/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Tracks container processes registered by the user-space supervisor,
 * enforces soft and hard memory limits via periodic RSS checks, and
 * cleans up stale entries when processes exit.
 *
 * Build:  make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
 * Load:   sudo insmod monitor.ko
 * Device: /dev/container_monitor
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * Linked-list node for each monitored container process.
 *
 * Tracks PID, container ID, soft/hard limits, and whether
 * the soft-limit warning has already been emitted.
 * ============================================================== */
struct monitored_process {
    pid_t          pid;
    char           container_id[MONITOR_NAME_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            soft_warned;
    struct list_head list;
};

/* ==============================================================
 * Global monitored list and spinlock.
 *
 * Spinlock is used because the timer callback runs in softirq
 * context where sleeping (mutex_lock) is not permitted.  All
 * list accesses — from ioctl (process context) and timer
 * (softirq context) — use the same spinlock for consistency.
 * ============================================================== */
static LIST_HEAD(monitored_list);
static DEFINE_SPINLOCK(monitored_lock);

/* --- Internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t             dev_num;
static struct cdev       c_dev;
static struct class     *cl;

/* ---------------------------------------------------------------
 * RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Soft-limit helper — log a warning when a process first exceeds
 * the soft limit.
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d "
           "rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Hard-limit helper — kill a process when it exceeds the hard
 * limit.
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d "
           "rss=%ld limit=%lu — process killed\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback — fires every CHECK_INTERVAL_SEC seconds.
 *
 * Iterates through all monitored entries under the spinlock,
 * removes stale (exited) processes, checks RSS against limits,
 * and enforces soft/hard policies.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    struct monitored_process *entry, *tmp;

    (void)t;

    spin_lock(&monitored_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        long rss = get_rss_bytes(entry->pid);

        /* Process has exited — remove stale entry. */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] Removing stale entry: "
                   "container=%s pid=%d (process exited)\n",
                   entry->container_id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Check hard limit first (higher priority). */
        if ((unsigned long)rss > entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Check soft limit (warn once). */
        if (!entry->soft_warned &&
            (unsigned long)rss > entry->soft_limit_bytes) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_warned = 1;
        }
    }

    spin_unlock(&monitored_lock);

    /* Re-arm the timer. */
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 *
 * MONITOR_REGISTER:   add a new PID with soft + hard limits
 * MONITOR_UNREGISTER: remove a tracked PID
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitored_process *entry, *tmp;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d "
               "soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* Validate limits. */
        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_ERR
                   "[container_monitor] Invalid limits: soft > hard "
                   "for container=%s\n", req.container_id);
            return -EINVAL;
        }

        /* Allocate and initialize a new entry. */
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned = 0;
        memset(entry->container_id, 0, sizeof(entry->container_id));
        strncpy(entry->container_id, req.container_id,
                sizeof(entry->container_id) - 1);

        /* Insert into the list under the lock. */
        spin_lock(&monitored_lock);
        list_add_tail(&entry->list, &monitored_list);
        spin_unlock(&monitored_lock);

        return 0;
    }

    /* cmd == MONITOR_UNREGISTER */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    spin_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        if (entry->pid == req.pid &&
            strncmp(entry->container_id, req.container_id,
                    sizeof(entry->container_id)) == 0) {
            list_del(&entry->list);
            spin_unlock(&monitored_lock);
            kfree(entry);
            printk(KERN_INFO
                   "[container_monitor] Unregistered container=%s pid=%d\n",
                   req.container_id, req.pid);
            return 0;
        }
    }
    spin_unlock(&monitored_lock);

    return -ENOENT;
}

/* --- File operations --- */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO
           "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* --- Module Exit --- */
static void __exit monitor_exit(void)
{
    struct monitored_process *entry, *tmp;

    del_timer_sync(&monitor_timer);

    /* Free all remaining monitored entries. */
    spin_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        printk(KERN_INFO
               "[container_monitor] Cleanup: freeing entry container=%s "
               "pid=%d\n", entry->container_id, entry->pid);
        list_del(&entry->list);
        kfree(entry);
    }
    spin_unlock(&monitored_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
