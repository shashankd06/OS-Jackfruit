/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * A single binary used as both a long-running supervisor daemon and
 * short-lived CLI clients.  The supervisor launches isolated containers
 * via clone(), manages their lifecycle, captures output through a
 * bounded-buffer logging pipeline, and coordinates with a kernel-space
 * memory monitor over ioctl.
 *
 * Build:  gcc -O2 -Wall -Wextra -o engine engine.c -lpthread
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sched.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define CONTAINER_ID_LEN      32
#define CHILD_COMMAND_LEN     256
#define CONTROL_MESSAGE_LEN   4096
#define LOG_CHUNK_SIZE        1024
#define LOG_BUFFER_CAPACITY   64
#define CHILD_STACK_SIZE      (1024 * 1024)   /* 1 MiB */
#define DEFAULT_SOFT_LIMIT    (40UL * 1024 * 1024)
#define DEFAULT_HARD_LIMIT    (64UL * 1024 * 1024)
#define SOCKET_PATH           "/tmp/mini_runtime.sock"
#define LOG_DIR               "logs"
#define STOP_GRACE_SECONDS    5
#define MAX_CONTAINERS        32
#define MAX_POLL_FDS          (MAX_CONTAINERS + 4)

/* ------------------------------------------------------------------ */
/*  Enums                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ------------------------------------------------------------------ */
/*  Data structures                                                   */
/* ------------------------------------------------------------------ */

typedef struct container_record {
    char              id[CONTAINER_ID_LEN];
    pid_t             host_pid;
    time_t            started_at;
    container_state_t state;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    int               nice_value;
    int               exit_code;
    int               exit_signal;
    int               stop_requested;
    char              log_path[PATH_MAX];
    int               pipe_read_fd;       /* supervisor reads container output */
    int               pipe_write_fd;      /* child writes stdout/stderr here  */
    pthread_t         producer_tid;
    int               producer_running;
    /* for CMD_RUN: the client fd waiting for the exit status */
    int               run_client_fd;
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;       /* 0 = ok, non-zero = error */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;   /* pipe write end for stdout/stderr */
} child_config_t;

/* Forward declaration so producer_arg_t can reference supervisor_ctx_t. */
typedef struct supervisor_ctx supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t   *ctx;
    container_record_t *rec;
} producer_arg_t;

struct supervisor_ctx {
    int                  server_fd;
    int                  monitor_fd;
    volatile sig_atomic_t should_stop;
    volatile sig_atomic_t got_sigchld;
    pthread_t            logger_thread;
    bounded_buffer_t     log_buffer;
    pthread_mutex_t      metadata_lock;
    container_record_t  *containers;
    char                 base_rootfs[PATH_MAX];
};

/* ------------------------------------------------------------------ */
/*  Global pointer so signal handlers can see the supervisor state    */
/* ------------------------------------------------------------------ */

static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/*  Usage                                                             */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static void sv_log(const char *fmt, ...)
{
    va_list ap;
    time_t now = time(NULL);
    struct tm tm;
    char timebuf[64];

    localtime_r(&now, &tm);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "[supervisor %s] ", timebuf);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/*  CLI flag parsing (from boilerplate)                               */
/* ------------------------------------------------------------------ */

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1],
                               &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1],
                               &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr,
                "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Bounded buffer                                                    */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Push a log item into the bounded buffer.
 * Blocks when the buffer is full; returns -1 if shutdown began.
 */
static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        /* Even during shutdown, try to enqueue if there is space so the
         * consumer can flush it.  If truly full, give up. */
        if (buffer->count == LOG_BUFFER_CAPACITY) {
            pthread_mutex_unlock(&buffer->mutex);
            return -1;
        }
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Pop a log item from the bounded buffer.
 * Blocks when the buffer is empty; returns -1 if shutdown began *and*
 * the buffer is empty (meaning the consumer should exit).
 */
static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;   /* nothing left, shut down */
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Logging consumer thread                                           */
/* ------------------------------------------------------------------ */

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    sv_log("Logger consumer thread started");

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        /* Build the log file path. */
        char path[PATH_MAX];
        int fd;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            sv_log("Failed to open log file %s: %s", path, strerror(errno));
            continue;
        }
        if (write(fd, item.data, item.length) < 0) {
            sv_log("Failed to write to log file %s: %s", path, strerror(errno));
        }
        close(fd);
    }

    sv_log("Logger consumer thread exiting (buffer drained)");
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Producer thread — one per container, reads from pipe              */
/* ------------------------------------------------------------------ */

static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    supervisor_ctx_t *ctx = pa->ctx;
    container_record_t *rec = pa->rec;
    int pipe_fd = rec->pipe_read_fd;
    log_item_t item;

    free(pa);   /* no longer needed */

    sv_log("Producer thread for container '%s' started (pipe fd=%d)",
           rec->id, pipe_fd);

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, rec->id, sizeof(item.container_id) - 1);

    while (1) {
        ssize_t n = read(pipe_fd, item.data, LOG_CHUNK_SIZE - 1);
        if (n <= 0)
            break;   /* pipe closed (container exited) or error */
        item.data[n] = '\0';
        item.length = (size_t)n;

        if (bounded_buffer_push(&ctx->log_buffer, &item) != 0)
            break;   /* shutdown */
    }

    close(pipe_fd);

    /* Mark producer as done so the supervisor knows. */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->pipe_read_fd = -1;
    rec->producer_running = 0;
    pthread_mutex_unlock(&ctx->metadata_lock);

    sv_log("Producer thread for container '%s' exiting", rec->id);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Kernel monitor integration                                        */
/* ------------------------------------------------------------------ */

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return -1;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) {
        sv_log("ioctl MONITOR_REGISTER failed for '%s' pid=%d: %s",
               container_id, host_pid, strerror(errno));
        return -1;
    }
    sv_log("Registered container '%s' pid=%d with kernel monitor "
           "(soft=%lu hard=%lu)",
           container_id, host_pid, soft_limit_bytes, hard_limit_bytes);
    return 0;
}

static int unregister_from_monitor(int monitor_fd,
                                   const char *container_id,
                                   pid_t host_pid)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return -1;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) {
        sv_log("ioctl MONITOR_UNREGISTER failed for '%s' pid=%d: %s",
               container_id, host_pid, strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Clone child entrypoint                                            */
/* ------------------------------------------------------------------ */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the logging pipe. */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        if (cfg->log_write_fd != STDOUT_FILENO &&
            cfg->log_write_fd != STDERR_FILENO)
            close(cfg->log_write_fd);
    }

    /* Set the UTS hostname to the container ID. */
    if (sethostname(cfg->id, strlen(cfg->id)) != 0) {
        perror("sethostname");
        _exit(1);
    }

    /* chroot into the container rootfs. */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        _exit(1);
    }
    if (chdir("/") != 0) {
        perror("chdir");
        _exit(1);
    }

    /* Mount /proc so ps and friends work. */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* It's possible /proc doesn't exist in the rootfs. Try to create it. */
        mkdir("/proc", 0555);
        if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
            perror("mount /proc");
            /* Continue anyway — some commands may still work. */
        }
    }

    /* Set nice value if non-zero. */
    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0) {
            perror("nice");
            /* Non-fatal. */
        }
    }

    /* Close all FDs above stderr, to not leak supervisor fds. */
    {
        int fd;
        for (fd = 3; fd < 1024; fd++)
            close(fd);
    }

    /* Execute the configured command inside the container. */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    _exit(127);
}

/* ------------------------------------------------------------------ */
/*  Container start                                                   */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *r;
    for (r = ctx->containers; r != NULL; r = r->next) {
        if (strcmp(r->id, id) == 0)
            return r;
    }
    return NULL;
}

static int start_container(supervisor_ctx_t *ctx,
                           const char *id,
                           const char *rootfs,
                           const char *command,
                           unsigned long soft_limit,
                           unsigned long hard_limit,
                           int nice_value,
                           int run_client_fd)
{
    container_record_t *rec;
    child_config_t child_cfg;
    char *stack;
    int pipefd[2];
    pid_t child_pid;
    producer_arg_t *pa;

    /* Check for duplicate container ID among running/starting containers. */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container(ctx, id);
    if (rec != NULL &&
        (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        sv_log("Container '%s' already exists and is %s", id,
               state_to_string(rec->state));
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Verify rootfs directory exists. */
    {
        struct stat st;
        if (stat(rootfs, &st) != 0 || !S_ISDIR(st.st_mode)) {
            sv_log("Root filesystem path does not exist or is not a directory: %s",
                   rootfs);
            return -1;
        }
    }

    /* Create logging pipe. */
    if (pipe(pipefd) != 0) {
        sv_log("pipe() failed: %s", strerror(errno));
        return -1;
    }

    /* Allocate the container record. */
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    strncpy(rec->id, id, sizeof(rec->id) - 1);
    rec->started_at = time(NULL);
    rec->state = CONTAINER_STARTING;
    rec->soft_limit_bytes = soft_limit;
    rec->hard_limit_bytes = hard_limit;
    rec->nice_value = nice_value;
    rec->pipe_read_fd = pipefd[0];
    rec->pipe_write_fd = pipefd[1];
    rec->exit_code = -1;
    rec->exit_signal = 0;
    rec->stop_requested = 0;
    rec->run_client_fd = run_client_fd;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, id);

    /* Prepare the child config. */
    memset(&child_cfg, 0, sizeof(child_cfg));
    strncpy(child_cfg.id, id, sizeof(child_cfg.id) - 1);
    strncpy(child_cfg.rootfs, rootfs, sizeof(child_cfg.rootfs) - 1);
    strncpy(child_cfg.command, command, sizeof(child_cfg.command) - 1);
    child_cfg.nice_value = nice_value;
    child_cfg.log_write_fd = pipefd[1];

    /* Allocate a stack for clone(). */
    stack = malloc(CHILD_STACK_SIZE);
    if (!stack) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(rec);
        return -1;
    }

    /* Clone with new PID, UTS, and mount namespaces. */
    child_pid = clone(child_fn,
                      stack + CHILD_STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &child_cfg);

    if (child_pid < 0) {
        sv_log("clone() failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        free(stack);
        free(rec);
        return -1;
    }

    /* Parent: close the write end of the pipe (child uses it). */
    close(pipefd[1]);
    rec->pipe_write_fd = -1;

    /* The clone stack can be freed after the child has started —
     * but to be safe we keep it alive.  In practice the child exec's
     * immediately, so we'll leak the stack intentionally (it's 1 MiB). */
    free(stack);

    rec->host_pid = child_pid;
    rec->state = CONTAINER_RUNNING;

    sv_log("Container '%s' started: pid=%d cmd='%s' rootfs='%s' "
           "soft=%lu hard=%lu nice=%d",
           id, child_pid, command, rootfs, soft_limit, hard_limit, nice_value);

    /* Insert into the container linked list. */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with the kernel memory monitor. */
    register_with_monitor(ctx->monitor_fd, id, child_pid,
                          soft_limit, hard_limit);

    /* Start a producer thread for this container. */
    pa = malloc(sizeof(*pa));
    if (pa) {
        pa->ctx = ctx;
        pa->rec = rec;
        rec->producer_running = 1;
        if (pthread_create(&rec->producer_tid, NULL, producer_thread, pa) != 0) {
            sv_log("Failed to create producer thread for '%s'", id);
            rec->producer_running = 0;
            free(pa);
        } else {
            pthread_detach(rec->producer_tid);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Signal handling                                                   */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->got_sigchld = 1;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *rec;

        pthread_mutex_lock(&ctx->metadata_lock);
        for (rec = ctx->containers; rec != NULL; rec = rec->next) {
            if (rec->host_pid == pid)
                break;
        }
        if (rec) {
            if (WIFEXITED(status)) {
                rec->exit_code = WEXITSTATUS(status);
                rec->exit_signal = 0;
                if (rec->stop_requested)
                    rec->state = CONTAINER_STOPPED;
                else
                    rec->state = CONTAINER_EXITED;
                sv_log("Container '%s' (pid=%d) exited with code %d → %s",
                       rec->id, pid, rec->exit_code,
                       state_to_string(rec->state));
            } else if (WIFSIGNALED(status)) {
                rec->exit_signal = WTERMSIG(status);
                rec->exit_code = 128 + rec->exit_signal;
                if (rec->stop_requested) {
                    rec->state = CONTAINER_STOPPED;
                } else if (rec->exit_signal == SIGKILL) {
                    rec->state = CONTAINER_KILLED;
                } else {
                    rec->state = CONTAINER_EXITED;
                }
                sv_log("Container '%s' (pid=%d) killed by signal %d → %s",
                       rec->id, pid, rec->exit_signal,
                       state_to_string(rec->state));
            }

            /* Unregister from kernel monitor. */
            unregister_from_monitor(ctx->monitor_fd, rec->id, pid);

            /* If a `run` client is waiting, notify them. */
            if (rec->run_client_fd >= 0) {
                control_response_t resp;
                memset(&resp, 0, sizeof(resp));
                resp.status = rec->exit_code;
                snprintf(resp.message, sizeof(resp.message),
                         "Container '%s' finished: %s (exit_code=%d signal=%d)",
                         rec->id, state_to_string(rec->state),
                         rec->exit_code, rec->exit_signal);
                /* Best-effort write; if client disconnected, ignore. */
                (void)write(rec->run_client_fd, &resp, sizeof(resp));
                close(rec->run_client_fd);
                rec->run_client_fd = -1;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/*  Supervisor command handlers                                       */
/* ------------------------------------------------------------------ */

static void handle_cmd_start(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             control_response_t *resp,
                             int client_fd)
{
    (void)client_fd;
    if (start_container(ctx, req->container_id, req->rootfs, req->command,
                        req->soft_limit_bytes, req->hard_limit_bytes,
                        req->nice_value, -1) == 0) {
        resp->status = 0;
        snprintf(resp->message, sizeof(resp->message),
                 "Container '%s' started successfully", req->container_id);
    } else {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Failed to start container '%s'", req->container_id);
    }
}

static void handle_cmd_run(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           control_response_t *resp,
                           int client_fd)
{
    /*
     * For CMD_RUN, we start the container with the client_fd stored so
     * that when the container exits, the reaper sends the final status
     * over that fd.  We send an initial ACK here, then the client waits
     * for a second response (the exit status) on the same connection.
     */
    if (start_container(ctx, req->container_id, req->rootfs, req->command,
                        req->soft_limit_bytes, req->hard_limit_bytes,
                        req->nice_value, client_fd) == 0) {
        resp->status = 0;
        snprintf(resp->message, sizeof(resp->message),
                 "Container '%s' started (run mode, waiting for exit)",
                 req->container_id);
    } else {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Failed to start container '%s'", req->container_id);
    }
}

static void handle_cmd_ps(supervisor_ctx_t *ctx,
                          control_response_t *resp)
{
    container_record_t *r;
    char *p = resp->message;
    size_t rem = sizeof(resp->message);
    int n;

    n = snprintf(p, rem,
                 "%-12s %-8s %-10s %-10s %-10s %-6s %-20s\n",
                 "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "EXIT", "STARTED");
    if (n > 0 && (size_t)n < rem) { p += n; rem -= (size_t)n; }

    n = snprintf(p, rem,
                 "%-12s %-8s %-10s %-10s %-10s %-6s %-20s\n",
                 "---", "---", "---", "---", "---", "---", "---");
    if (n > 0 && (size_t)n < rem) { p += n; rem -= (size_t)n; }

    pthread_mutex_lock(&ctx->metadata_lock);
    for (r = ctx->containers; r != NULL; r = r->next) {
        char timebuf[32];
        struct tm tm;
        localtime_r(&r->started_at, &tm);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

        n = snprintf(p, rem,
                     "%-12s %-8d %-10s %-10lu %-10lu %-6d %-20s\n",
                     r->id, r->host_pid,
                     state_to_string(r->state),
                     r->soft_limit_bytes / (1024 * 1024),
                     r->hard_limit_bytes / (1024 * 1024),
                     r->exit_code, timebuf);
        if (n > 0 && (size_t)n < rem) { p += n; rem -= (size_t)n; }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
}

static void handle_cmd_logs(supervisor_ctx_t *ctx,
                            const control_request_t *req,
                            control_response_t *resp)
{
    container_record_t *r;
    char path[PATH_MAX];
    int fd;
    ssize_t n;

    pthread_mutex_lock(&ctx->metadata_lock);
    r = find_container(ctx, req->container_id);
    if (!r) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Container '%s' not found", req->container_id);
        return;
    }
    strncpy(path, r->log_path, sizeof(path) - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Log file '%s' not found or not readable: %s",
                 path, strerror(errno));
        return;
    }

    n = read(fd, resp->message, sizeof(resp->message) - 1);
    if (n < 0) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Error reading log file: %s", strerror(errno));
    } else {
        resp->message[n] = '\0';
        resp->status = 0;
    }
    close(fd);
}

static void handle_cmd_stop(supervisor_ctx_t *ctx,
                            const control_request_t *req,
                            control_response_t *resp)
{
    container_record_t *r;

    pthread_mutex_lock(&ctx->metadata_lock);
    r = find_container(ctx, req->container_id);
    if (!r) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Container '%s' not found", req->container_id);
        return;
    }
    if (r->state != CONTAINER_RUNNING && r->state != CONTAINER_STARTING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message),
                 "Container '%s' is not running (state=%s)",
                 req->container_id, state_to_string(r->state));
        return;
    }

    r->stop_requested = 1;
    pid_t target_pid = r->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Send SIGTERM first, giving the container a chance to exit. */
    sv_log("Sending SIGTERM to container '%s' (pid=%d)",
           req->container_id, target_pid);
    kill(target_pid, SIGTERM);

    /* Wait for the grace period; if still alive, SIGKILL. */
    {
        int waited = 0;
        while (waited < STOP_GRACE_SECONDS) {
            sleep(1);
            waited++;
            /* Check if the child has already exited. */
            if (waitpid(target_pid, NULL, WNOHANG) > 0) {
                /* Already dead — the SIGCHLD handler will do metadata updates
                 * on the next reap cycle. Force a reap now. */
                reap_children(ctx);
                break;
            }
        }
        /* If still running, force kill. */
        if (kill(target_pid, 0) == 0) {
            sv_log("Container '%s' (pid=%d) did not exit after %ds, sending SIGKILL",
                   req->container_id, target_pid, STOP_GRACE_SECONDS);
            kill(target_pid, SIGKILL);
        }
    }

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "Stop signal sent to container '%s'", req->container_id);
}

/* ------------------------------------------------------------------ */
/*  Stop all running containers (for supervisor shutdown)             */
/* ------------------------------------------------------------------ */

static void stop_all_containers(supervisor_ctx_t *ctx)
{
    container_record_t *r;

    sv_log("Stopping all running containers...");

    pthread_mutex_lock(&ctx->metadata_lock);
    for (r = ctx->containers; r != NULL; r = r->next) {
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING) {
            r->stop_requested = 1;
            sv_log("Sending SIGTERM to container '%s' (pid=%d)",
                   r->id, r->host_pid);
            kill(r->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Brief grace period. */
    sleep(2);

    /* Force kill any survivors. */
    pthread_mutex_lock(&ctx->metadata_lock);
    for (r = ctx->containers; r != NULL; r = r->next) {
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING) {
            sv_log("Sending SIGKILL to container '%s' (pid=%d)",
                   r->id, r->host_pid);
            kill(r->host_pid, SIGKILL);
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Final reap. */
    sleep(1);
    reap_children(ctx);
}

/* ------------------------------------------------------------------ */
/*  Supervisor: process a control request from a client               */
/* ------------------------------------------------------------------ */

static void process_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;
    int keep_open = 0; /* If 1, don't close client_fd (CMD_RUN keeps it). */

    n = read(client_fd, &req, sizeof(req));
    if (n != sizeof(req)) {
        close(client_fd);
        return;
    }

    memset(&resp, 0, sizeof(resp));

    switch (req.kind) {
    case CMD_START:
        handle_cmd_start(ctx, &req, &resp, client_fd);
        break;
    case CMD_RUN:
        handle_cmd_run(ctx, &req, &resp, client_fd);
        /* If start succeeded, keep_open = 1 so connection stays alive. */
        if (resp.status == 0)
            keep_open = 1;
        break;
    case CMD_PS:
        handle_cmd_ps(ctx, &resp);
        break;
    case CMD_LOGS:
        handle_cmd_logs(ctx, &req, &resp);
        break;
    case CMD_STOP:
        handle_cmd_stop(ctx, &req, &resp);
        break;
    default:
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        break;
    }

    /* Send the response. */
    (void)write(client_fd, &resp, sizeof(resp));

    if (!keep_open)
        close(client_fd);
}

/* ------------------------------------------------------------------ */
/*  Supervisor event loop                                             */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *base_rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    struct sigaction sa;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    strncpy(ctx.base_rootfs, base_rootfs, sizeof(ctx.base_rootfs) - 1);

    g_ctx = &ctx;

    /* Initialize metadata lock. */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    /* Initialize bounded buffer. */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Ensure log directory exists. */
    mkdir(LOG_DIR, 0755);

    /* Open kernel monitor device (non-fatal if unavailable). */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        sv_log("Warning: could not open /dev/container_monitor: %s "
               "(kernel monitor disabled)", strerror(errno));
    } else {
        sv_log("Kernel monitor device opened (fd=%d)", ctx.monitor_fd);
    }

    /* Create the Unix domain socket. */
    unlink(SOCKET_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }

    /* Make the socket non-blocking for the poll loop. */
    {
        int flags = fcntl(ctx.server_fd, F_GETFL, 0);
        fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);
    }

    sv_log("Supervisor listening on %s (base-rootfs=%s)", SOCKET_PATH, base_rootfs);

    /* Install signal handlers. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Start the logging consumer thread. */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger");
        goto cleanup;
    }

    sv_log("Supervisor ready.");

    /* ============== Main event loop ============== */
    while (!ctx.should_stop) {
        struct pollfd pfd;
        int ret;

        pfd.fd = ctx.server_fd;
        pfd.events = POLLIN;

        ret = poll(&pfd, 1, 500);  /* 500ms timeout for responsiveness */

        /* Handle pending SIGCHLD. */
        if (ctx.got_sigchld) {
            ctx.got_sigchld = 0;
            reap_children(&ctx);
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                process_client(&ctx, client_fd);
            }
        }
    }

    sv_log("Supervisor shutting down...");

    /* Stop all containers. */
    stop_all_containers(&ctx);

    /* Final reap to clear any remaining children. */
    reap_children(&ctx);

cleanup:
    /* Shut down logging pipeline. */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    sv_log("Logger thread joined");

    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records. */
    {
        container_record_t *r = ctx.containers;
        while (r) {
            container_record_t *next = r->next;
            if (r->pipe_read_fd >= 0)
                close(r->pipe_read_fd);
            if (r->run_client_fd >= 0)
                close(r->run_client_fd);
            free(r);
            r = next;
        }
        ctx.containers = NULL;
    }

    /* Close kernel monitor device. */
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    /* Close and remove socket. */
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(SOCKET_PATH);

    pthread_mutex_destroy(&ctx.metadata_lock);
    g_ctx = NULL;

    sv_log("Supervisor exit complete. No zombies.");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  CLI client: send request to supervisor                            */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s: %s\n"
                "Is the supervisor running?\n",
                SOCKET_PATH, strerror(errno));
        close(sock);
        return 1;
    }

    /* Send the request. */
    if (write(sock, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(sock);
        return 1;
    }

    /* Read the response. */
    n = read(sock, &resp, sizeof(resp));
    if (n != sizeof(resp)) {
        fprintf(stderr, "Error reading response from supervisor\n");
        close(sock);
        return 1;
    }

    /* Print the response. */
    if (resp.message[0])
        printf("%s\n", resp.message);

    /* For CMD_RUN, if the initial response was OK, wait for the final
     * exit status on the same connection. */
    if (req->kind == CMD_RUN && resp.status == 0) {
        control_response_t final_resp;

        printf("Waiting for container '%s' to exit...\n", req->container_id);

        n = read(sock, &final_resp, sizeof(final_resp));
        if (n == sizeof(final_resp)) {
            if (final_resp.message[0])
                printf("%s\n", final_resp.message);
            close(sock);
            return final_resp.status;
        } else {
            fprintf(stderr, "Lost connection to supervisor while waiting\n");
            close(sock);
            return 1;
        }
    }

    close(sock);
    return resp.status;
}

/* ------------------------------------------------------------------ */
/*  CLI command wrappers                                              */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
