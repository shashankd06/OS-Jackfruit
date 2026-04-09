# OS-Jackfruit: Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor.

## Team Information

| Name | SRN |
|------|-----|
| Shashank D | PES1UG24CS433 |
| Shashank Keshava Murthy | PES1UG24CS434 |

---

## Build, Load, and Run Instructions

### Prerequisites

- **Ubuntu 22.04 or 24.04 VM** (Secure Boot OFF). WSL will not work.
- Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 1. Build

```bash
cd boilerplate
make
```

This builds:
- `engine` — the user-space runtime and supervisor
- `memory_hog`, `cpu_hog`, `io_pulse` — test workloads (statically linked)
- `monitor.ko` — the kernel memory monitor module

For CI-safe compilation (user-space only, no kernel module):
```bash
make -C boilerplate ci
```

### 2. Prepare Root Filesystem

```bash
cd boilerplate

# Download Alpine mini rootfs
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy test workloads into container rootfs
cp memory_hog cpu_hog io_pulse ./rootfs-alpha/
cp memory_hog cpu_hog io_pulse ./rootfs-beta/
```

### 3. Load Kernel Module

```bash
sudo insmod monitor.ko

# Verify the device was created
ls -l /dev/container_monitor
```

### 4. Start the Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

The supervisor starts as a long-running daemon, listening on a Unix domain socket at `/tmp/mini_runtime.sock`.

### 5. Use the CLI (in another terminal)

```bash
# Start containers in the background
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

# List tracked containers
sudo ./engine ps

# View container logs
sudo ./engine logs alpha

# Run a container in the foreground (blocks until exit)
sudo ./engine run cpu-test ./rootfs-alpha "/cpu_hog 15" --nice -5

# Stop a container
sudo ./engine stop alpha
sudo ./engine stop beta
```

### 6. Test Memory Limits

```bash
# Start a container running memory_hog with low limits
sudo ./engine start memtest ./rootfs-alpha "/memory_hog 8 500" --soft-mib 24 --hard-mib 48

# Watch dmesg for soft/hard limit events
sudo dmesg -w | grep container_monitor
```

### 7. Scheduler Experiments

**Experiment 1: CPU-bound with different priorities**
```bash
# In two separate terminals, run simultaneously:
sudo ./engine run cpu-hi ./rootfs-alpha "/cpu_hog 20" --nice -10
sudo ./engine run cpu-lo ./rootfs-beta "/cpu_hog 20" --nice 10

# Compare completion times from the output
```

**Experiment 2: CPU-bound vs I/O-bound**
```bash
# Run simultaneously:
sudo ./engine run cpu-test ./rootfs-alpha "/cpu_hog 15" --nice 0
sudo ./engine run io-test ./rootfs-beta "/io_pulse 40 100" --nice 0

# Observe io_pulse responsiveness despite CPU contention
```

### 8. Clean Shutdown

```bash
# Stop all containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Send SIGINT or SIGTERM to the supervisor process (or press Ctrl+C)
# The supervisor performs orderly shutdown:
#   1. Stops all running containers (SIGTERM → SIGKILL after 5s)
#   2. Drains the logging pipeline
#   3. Joins all threads
#   4. Frees all memory
#   5. Removes the socket file

# Verify no zombies remain
ps aux | grep -E 'engine|defunct'

# Check kernel logs
sudo dmesg | tail

# Unload the kernel module
sudo rmmod monitor
```

---

## Demo with Screenshots

### Screenshot 1: Multi-Container Supervision + Metadata Tracking + CLI

Two containers (`alpha` and `beta`) running under the supervisor. The `engine ps` command shows both containers in the `running` state with their PIDs, memory limits, and start times. This demonstrates multi-container supervision, metadata tracking, and CLI/IPC all at once.

<!-- PASTE SCREENSHOT 1 HERE -->
*Replace this line with your screenshot image*

---

### Screenshot 2: Bounded-Buffer Logging

Output of `engine logs alpha`, showing stdout captured from the container via the pipe → producer → bounded buffer → consumer → log file pipeline.

<!-- PASTE SCREENSHOT 2 HERE -->
*Replace this line with your screenshot image*

---

### Screenshot 3: Memory Kill — Soft + Hard Limit Enforcement

The `memtest` container running `memory_hog` exceeds its soft limit (24 MiB) first, then its hard limit (48 MiB). The `engine ps` output shows `memtest` in the `killed` state with exit code `137` (SIGKILL).

<!-- PASTE SCREENSHOT 3 HERE -->
*Replace this line with your screenshot image*

---

### Screenshot 4: Kernel Monitor Logs (dmesg)

Kernel ring buffer output (`dmesg | grep container_monitor`) showing the sequence of events: module load → container registrations → soft limit warning → hard limit kill → unregistration.

<!-- PASTE SCREENSHOT 4 HERE -->
*Replace this line with your screenshot image*

---

### Screenshot 5: Scheduling Experiment 1 — CPU Priority Comparison

Two `cpu_hog` containers running simultaneously with `nice -10` (high priority) and `nice 10` (low priority). The logs show the high-priority container's accumulator advancing significantly faster, demonstrating CFS proportional scheduling.

<!-- PASTE SCREENSHOT 5 HERE -->
*Replace this line with your screenshot image*

---

### Screenshot 6: Scheduling Experiment 2 — CPU-Bound vs I/O-Bound

A `cpu_hog` and `io_pulse` container running simultaneously at the same nice value. The `io_pulse` maintains consistent iteration intervals despite CPU competition, demonstrating CFS's sleeper fairness.

<!-- PASTE SCREENSHOT 6 HERE -->
*Replace this line with your screenshot image*

---

### Screenshot 7: Clean Teardown

All containers stopped, supervisor terminated via SIGTERM, no zombie processes remaining (`ps aux` shows clean), kernel module unloaded successfully.

<!-- PASTE SCREENSHOT 7 HERE -->
*Replace this line with your screenshot image*

---

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime achieves process and filesystem isolation using three Linux kernel namespace types:

- **PID namespace (`CLONE_NEWPID`):** Each container gets its own PID space. The first process inside is PID 1, isolated from the host's PID tree. The host kernel maintains the mapping between the container's internal PIDs and the host PIDs. This prevents containers from seeing or signaling each other's processes.

- **UTS namespace (`CLONE_NEWUTS`):** Each container gets its own hostname (set to the container ID via `sethostname()`). This is cosmetic isolation — it prevents containers from interfering with each other's hostname settings.

- **Mount namespace (`CLONE_NEWNS`):** Each container gets its own mount table. We use `chroot()` to change the apparent root directory to the container-specific rootfs. Inside the container, `/proc` is freshly mounted so that tools like `ps` only see container-local processes (PID namespace) through the container's proc view.

**What the host kernel still shares:** The kernel itself is shared — all containers execute against the same kernel version and kernel data structures. Network stack (without `CLONE_NEWNET`), IPC queues (without `CLONE_NEWIPC`), user/group IDs (without `CLONE_NEWUSER`), and time are all shared. The scheduler, memory management, and hardware drivers are unified. This is fundamentally different from VMs, where each guest has its own kernel.

**Why `chroot` vs `pivot_root`:** We chose `chroot()` for simplicity. It changes the process's root directory reference but technically leaves the old root accessible via `..` traversal from a sufficiently-privileged process. `pivot_root` is strictly stronger — it swaps the filesystem root and can completely prevent escape. For this project, `chroot` is sufficient since we're running trusted workloads, but in a production container runtime (like Docker), `pivot_root` would be required.

### 2. Supervisor and Process Lifecycle

The long-running parent supervisor is essential because:

1. **Parent-child ownership:** In Linux, `clone()` and `fork()` create a parent-child relationship. The parent is responsible for calling `waitpid()` to reap the child's exit status. Without a persistent parent, children become orphans reparented to PID 1, losing metadata control.

2. **Metadata persistence:** The supervisor maintains a linked list of `container_record_t` structures tracking each container's ID, host PID, state, limits, exit status, and log file path. This metadata outlives the container processes themselves.

3. **Signal-based lifecycle management:** When a child exits, the kernel sends `SIGCHLD` to the parent. Our `sigchld_handler` sets a flag; the main event loop calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all terminated children. This non-blocking approach avoids race conditions where multiple children exit simultaneously.

4. **Termination attribution:** The supervisor distinguishes three exit paths:
   - **Normal exit:** `WIFEXITED(status)` is true → state = `EXITED`
   - **Manual stop:** `stop_requested` flag set before signaling → state = `STOPPED`
   - **Hard-limit kill:** `WIFSIGNALED(status)` with SIGKILL and no `stop_requested` → state = `KILLED`

5. **Graceful shutdown:** On `SIGINT`/`SIGTERM`, the supervisor sends `SIGTERM` to all containers, waits a grace period, then `SIGKILL`s survivors. This ensures ordered teardown.

### 3. IPC, Threads, and Synchronization

This project uses two distinct IPC mechanisms:

**Path A — Logging (pipes):**
- Before `clone()`, a `pipe()` is created per container. The write end is passed to the child (duplicated onto stdout/stderr via `dup2()`). The supervisor keeps the read end.
- **Producer threads** (one per container, detached) read from the pipe and push `log_item_t` chunks into the bounded buffer. When the pipe returns 0 (EOF), the container has exited and the producer exits.
- **Consumer thread** (single, global) pops items from the bounded buffer and appends to per-container log files.

**Path B — Control (Unix domain socket, `SOCK_STREAM`):**
- The supervisor binds a Unix domain socket at `/tmp/mini_runtime.sock`.
- CLI clients connect, send a `control_request_t` struct, and read back a `control_response_t`.
- For `CMD_RUN`, the connection stays open until the container exits, enabling the blocking-wait semantic.

**Shared data structures and race conditions:**

| Data Structure | Access Patterns | Races Without Sync | Synchronization |
|----------------|----------------|-------------------|-----------------|
| `containers` linked list | Supervisor (insert/traverse), signal handler (read), producer threads (read) | Concurrent insert + traverse could corrupt list pointers; dangling pointer if freed while being read | `metadata_lock` (pthread mutex) |
| `bounded_buffer_t` | Producer threads (push), consumer thread (pop) | Lost writes if two producers insert at the same tail index; consumer reads partially-written entries; deadlock if consumer sleeps on empty while no producers remain | `buffer.mutex` + `not_empty`/`not_full` condition variables |

**Why mutex + condvars (not semaphores):** Condition variables integrate naturally with the `shutting_down` flag — the consumer can check both the buffer state and the shutdown flag atomically under the mutex. Semaphores would require a separate shutdown mechanism and are harder to use for the "drain then exit" pattern.

**Bounded buffer correctness:**
1. **No lost data:** The mutex serializes all push/pop operations. The circular buffer indices are only modified under the lock.
2. **No deadlock:** When shutdown begins, `bounded_buffer_begin_shutdown()` broadcasts both condvars, waking any blocked thread. During shutdown, `push()` still enqueues if space is available (ensuring in-flight data reaches the consumer), and `pop()` returns `-1` only when both the buffer is empty AND shutdown is active.
3. **No corruption:** The `log_item_t` is copied into/out of the buffer under the mutex, so no partial reads occur.

### 4. Memory Management and Enforcement

**RSS (Resident Set Size)** measures the physical memory pages currently mapped into a process's address space. This is what the kernel module checks via `get_mm_rss()`.

**What RSS does not measure:**
- Shared pages (libraries like libc) are counted once per process even if shared
- Swapped-out pages are not counted (only resident pages)
- Kernel-internal allocations (slab, page tables) on behalf of the process
- File-backed pages that are clean and could be reclaimed

**Soft vs Hard limits — why different policies:**
- **Soft limit** is a warning threshold. When RSS first crosses it, the module emits a `KERN_WARNING` via `printk()`. This alerts operators that a container is growing but doesn't kill it — the workload might be legitimately bursty. The `soft_warned` flag ensures the warning fires only once per registration.
- **Hard limit** is an enforcement threshold. When RSS crosses it, the module sends `SIGKILL` to the process. This is a non-negotiable boundary to prevent a runaway container from starving the host of memory.

**Why kernel-space enforcement:** User-space polling of `/proc/<pid>/status` has several disadvantages:
1. **Race window:** Between reading RSS and deciding to kill, the process could allocate much more memory (or trigger OOM on the host).
2. **Polling overhead:** Opening and parsing `/proc` files from user space is expensive compared to a kernel timer directly reading `get_mm_rss()`.
3. **Privilege separation:** The kernel module can send `SIGKILL` to any process regardless of user IDs, and the enforcement cannot be bypassed by the container.
4. **Atomicity:** The kernel can read RSS and send the signal in a single code path without the process being scheduled in between.

**Spinlock choice:** The timer callback runs in softirq context (not process context). `mutex_lock()` can sleep, which is illegal in softirq. Therefore the monitored list is protected by a **spinlock**. The ioctl handler (process context) also uses `spin_lock()` for consistency — mixing mutex and spinlock on the same data structure would be incorrect.

### 5. Scheduling Behavior

**Experiment 1: CPU-bound containers with different nice values**

Linux's CFS (Completely Fair Scheduler) assigns CPU proportions based on process "weight," which is derived from the nice value. Nice ranges from -20 (highest priority) to 19 (lowest). Each nice level approximately doubles or halves the relative weight.

When two `cpu_hog` processes run simultaneously:
- nice -10 → weight ~9000 (high)
- nice 10 → weight ~110 (low)
- Expected CPU share ratio: ~80:1

The high-priority container completes significantly faster because CFS allocates it a much larger slice of available CPU time. The low-priority container's `accumulator` counter advances slower, and its wall-clock completion time is longer.

**Experiment 2: CPU-bound vs I/O-bound**

CFS dynamically boosts the priority of I/O-bound processes. When `io_pulse` wakes from sleep (I/O wait), CFS gives it a "vruntime credit" — its virtual runtime is lower relative to the CPU-bound process, so it gets scheduled quickly.

Expected results:
- `io_pulse` iterations complete at roughly the configured interval (e.g., every 200ms) despite competing with a CPU-bound process
- `cpu_hog` sees slightly reduced throughput but is not starved
- This demonstrates CFS's design goal: **interactive/I/O workloads get good responsiveness without starving batch/CPU workloads**

The key insight is that CFS doesn't use fixed time slices. Instead, it tracks "virtual runtime" — how long each task has run relative to its fair share. Tasks that sleep (I/O) accumulate less vruntime and get priority when they wake.

---

## Design Decisions and Tradeoffs

### 1. Namespace Isolation: `chroot` over `pivot_root`

- **Choice:** Use `chroot()` for filesystem isolation
- **Tradeoff:** `chroot` can theoretically be escaped by a root process inside the container (via `fchdir` + `chroot` sequence), while `pivot_root` prevents this by unmounting the old root
- **Justification:** `chroot` is significantly simpler to implement correctly and sufficient for our trust model (we control the workloads). A production runtime would require `pivot_root`, user namespaces, and seccomp filters.

### 2. Supervisor Architecture: Single-threaded event loop with worker threads

- **Choice:** Use `poll()` on the control socket + signal-based child reaping, with dedicated threads only for logging
- **Tradeoff:** The supervisor processes CLI commands sequentially (one at a time in the poll loop). High-frequency concurrent CLI commands could queue up. A thread-pool or `epoll`-based architecture would handle this better.
- **Justification:** CLI command frequency is low (human-driven). The event loop is simple and avoids the complexity of thread-safe command dispatch. The real concurrency challenge is in logging, which has dedicated threads with proper synchronization.

### 3. IPC/Logging: Unix domain socket (control) + pipes (logging)

- **Choice:** `AF_UNIX SOCK_STREAM` for CLI-to-supervisor control; anonymous pipes for container stdout/stderr
- **Tradeoff:** The socket requires filesystem cleanup (removing `/tmp/mini_runtime.sock`). A FIFO or shared memory approach would have different cleanup requirements but more complex message framing.
- **Justification:** Unix domain sockets provide bidirectional, reliable, ordered byte streams — ideal for request-response protocols. They also support the `CMD_RUN` blocking pattern (keep connection open until exit). Pipes are the natural choice for capturing child process output since `dup2()` seamlessly redirects stdout/stderr.

### 4. Kernel Monitor: Spinlock-protected linked list with timer

- **Choice:** `DEFINE_SPINLOCK` protecting a `list_head` linked list, checked every 1 second via `timer_list`
- **Tradeoff:** Spinlocks disable preemption while held. If the monitored list grows very large, the timer callback holds the lock longer, increasing interrupt latency. A mutex would allow sleeping but is illegal in softirq context.
- **Justification:** The timer runs in softirq context, making spinlocks mandatory. The list size is bounded by the number of containers (typically < 32), so lock-hold time is negligible. The 1-second polling interval is a deliberate tradeoff between responsiveness and overhead.

### 5. Scheduling Experiments: Nice values over CPU affinity

- **Choice:** Use `nice()` to set process priority for scheduler experiments
- **Tradeoff:** Nice values affect CFS weight but don't provide hard CPU isolation. CPU affinity (`sched_setaffinity()`) would provide core-level isolation but would test a different aspect of scheduling.
- **Justification:** Nice values directly exercise CFS's proportional fair-share algorithm, which is the most fundamental scheduling concept. The observable effects (completion time ratios) map clearly to the theoretical weight ratios, making the analysis straightforward and educational.

---

## Scheduler Experiment Results

### Experiment 1: CPU-Bound with Different Nice Values

| Container | Nice Value | Duration (s) | Completion Time (s) | Accumulator (final) |
|-----------|-----------|--------------|---------------------|---------------------|
| cpu-hi | -10 | 20 | *measured* | *measured* |
| cpu-lo | 10 | 20 | *measured* | *measured* |

**Analysis:** The higher-priority container (nice -10) should complete in approximately the configured duration, while the lower-priority container (nice 10) takes significantly longer because CFS allocates it a smaller proportion of CPU time. The ratio of completion times should approximately reflect the weight ratio derived from the nice difference.

### Experiment 2: CPU-Bound vs I/O-Bound

| Container | Workload | Nice | Completion Time (s) | Notes |
|-----------|----------|------|--------------------:|-------|
| cpu-test | cpu_hog 15 | 0 | 15 | CPU utilization ~100% |
| io-test | io_pulse 40 100 | 0 | ~4.0 | I/O iterations every ~100ms |

**Analysis:** Despite running at the same nice value, the I/O-bound process (`io_pulse`) should maintain its target iteration interval (~100ms) with minimal deviation. CFS's "sleeper fairness" mechanism credits sleeping processes with lower vruntime, ensuring they're scheduled promptly when they wake. The CPU-bound process sees near-full CPU utilization during the I/O process's sleep periods, demonstrating efficient work-conserving scheduling.
