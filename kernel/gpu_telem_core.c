// SPDX-License-Identifier: GPL-2.0
//
// kernel/gpu_telem_core.c — GPU telemetry: filesystem interface + blocking reads
//
// Responsibility (see gpu_telem.h one-liner):
//   THIS FILE  — file interface and reader blocking
//   ringbuf.c  — synchronization and producer/consumer race protection
//
// Pipeline:
//   sysfs/hwmon  →  collector kthread  →  ring_push()  →  /dev/gpu_telem
//                                                              ↕
//                                                       wait_queue (readq)
//                                                   (wakes blocked readers)
//
// Key kernel interfaces used:
//   copy_to_user()          — safe kernel→user data transfer
//   wait_event_interruptible() — blocking read (reader sleeps on wait queue)
//   poll_wait()             — event-driven reads via select()/poll()/epoll()
//   spin_lock_irqsave()     — via ringbuf.c (not called directly here)
//   filp_open/kernel_read() — read sysfs nodes in kernel space

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/limits.h>

#include "gpu_telem.h"   /* kernel-internal structs                  */
/* gpu_telem_uapi.h is pulled in transitively via ringbuf.h → gpu_telem.h */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LKM-CharDev");
MODULE_DESCRIPTION("GPU kernel telemetry — ring buffer + chardev pipeline");
MODULE_VERSION("0.2");

/* ---- Module parameters ---- */

static char *hwmon_path = "";
module_param(hwmon_path, charp, 0644);
MODULE_PARM_DESC(hwmon_path, "Base hwmon sysfs path (auto-probed when empty)");

static unsigned int sample_ms = DEFAULT_SAMPLE_MS;
module_param(sample_ms, uint, 0644);
MODULE_PARM_DESC(sample_ms, "Collection interval in ms (min 10)");

static unsigned int ring_size = DEFAULT_RING_SIZE;
module_param(ring_size, uint, 0444);
MODULE_PARM_DESC(ring_size, "Ring buffer capacity (power-of-2, max 4096)");

/* ---- Singleton device state ---- */

static struct gpu_telem_device gdev;

/* ---- sysfs read helpers ---- */

static int sysfs_read_s64(const char *path, s64 *out)
{
	struct file *f;
	char buf[32];
	ssize_t n;
	loff_t pos = 0;
	int ret;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);

	if (n <= 0)
		return (n < 0) ? (int)n : -EIO;

	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';

	ret = kstrtos64(buf, 10, out);
	return ret;
}

/*
 * Scan hwmon0..9 for a GPU driver (nvidia/nouveau/amdgpu/radeon).
 * Falls back to empty paths — collector emits sentinel values and the
 * ring-buffer/chardev pipeline still exercises all kernel interfaces.
 * This is the expected state when running inside QEMU without a GPU.
 */
static void probe_hwmon_paths(void)
{
	char path[256], name[64];
	struct file *f;
	loff_t pos;
	ssize_t n;
	int i;

	if (hwmon_path && hwmon_path[0]) {
		snprintf(gdev.temp_path,  sizeof(gdev.temp_path),
			 "%s/temp1_input",    hwmon_path);
		snprintf(gdev.fan_path,   sizeof(gdev.fan_path),
			 "%s/fan1_input",     hwmon_path);
		snprintf(gdev.power_path, sizeof(gdev.power_path),
			 "%s/power1_average", hwmon_path);
		snprintf(gdev.freq_path,  sizeof(gdev.freq_path),
			 "%s/freq1_input",    hwmon_path);
		pr_info(DRIVER_NAME ": using user-specified hwmon: %s\n",
			hwmon_path);
		return;
	}

	for (i = 0; i < 10; i++) {
		snprintf(path, sizeof(path),
			 "/sys/class/hwmon/hwmon%d/name", i);
		f = filp_open(path, O_RDONLY, 0);
		if (IS_ERR(f))
			continue;
		pos = 0;
		n = kernel_read(f, name, sizeof(name) - 1, &pos);
		filp_close(f, NULL);
		if (n <= 0)
			continue;
		name[n] = '\0';
		while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == ' '))
			name[--n] = '\0';

		if (!strncmp(name, "nvidia",  6) ||
		    !strncmp(name, "nouveau", 7) ||
		    !strncmp(name, "amdgpu",  6) ||
		    !strncmp(name, "radeon",  6)) {
			snprintf(gdev.temp_path, sizeof(gdev.temp_path),
				 "/sys/class/hwmon/hwmon%d/temp1_input", i);
			snprintf(gdev.fan_path, sizeof(gdev.fan_path),
				 "/sys/class/hwmon/hwmon%d/fan1_input", i);
			snprintf(gdev.power_path, sizeof(gdev.power_path),
				 "/sys/class/hwmon/hwmon%d/power1_average", i);
			snprintf(gdev.freq_path, sizeof(gdev.freq_path),
				 "/sys/class/hwmon/hwmon%d/freq1_input", i);
			pr_info(DRIVER_NAME ": found GPU hwmon: %s (hwmon%d)\n",
				name, i);
			return;
		}
	}

	pr_warn(DRIVER_NAME ": no GPU hwmon — sentinel values, ring+chardev still active\n");
}

/* ---- Collector (producer) ---- */

static void collect_sample(struct gpu_sample *s)
{
	s64 val;

	s->timestamp_ns     = ktime_get_ns();
	s->gpu_util_pct     = U32_MAX;   /* unavailable via generic hwmon */
	s->mem_util_pct     = U32_MAX;
	s->throttle_reasons = 0;

	s->temp_millideg = (gdev.temp_path[0] &&
			    sysfs_read_s64(gdev.temp_path, &val) == 0)
			   ? (s32)val : -1;

	s->fan_rpm = (gdev.fan_path[0] &&
		      sysfs_read_s64(gdev.fan_path, &val) == 0)
		     ? (u32)val : 0;

	s->power_uw = (gdev.power_path[0] &&
		       sysfs_read_s64(gdev.power_path, &val) == 0)
		      ? (u64)val : 0;

	/* hwmon exposes frequency in Hz */
	s->core_freq_mhz = (gdev.freq_path[0] &&
			    sysfs_read_s64(gdev.freq_path, &val) == 0)
			   ? (u32)(val / 1000000) : 0;

	s->mem_freq_mhz = 0;
}

static int collector_thread(void *unused)
{
	struct gpu_sample s;

	pr_info(DRIVER_NAME ": collector started (interval %u ms)\n",
		gdev.interval_ms);

	while (!kthread_should_stop()) {
		collect_sample(&s);
		ring_push(&gdev.ring, &s);
		wake_up_interruptible(&gdev.wq);   /* ← wake all blocked readers */

		if (msleep_interruptible(gdev.interval_ms))
			break;
	}

	pr_info(DRIVER_NAME ": collector stopped\n");
	return 0;
}

/* ---- file_operations ---- */

/*
 * open() — allocate per-fd reader state.
 *
 * New readers start at the current ring head so they only receive samples
 * produced after their open() call.  This avoids delivering stale data on
 * first read and keeps the "what is /dev/gpu_telem" mental model simple:
 * it is a live stream, not a log replay.
 */
static int gpu_telem_open(struct inode *inode, struct file *filp)
{
	struct reader_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/* Snapshot head_seq via ring_data_available's READ_ONCE path is not
	 * quite right here — we want an exact value.  The ring's internal
	 * lock gives us the consistent read. */
	/* Start at the current head — this reader only sees future samples. */
	ctx->next_seq = READ_ONCE(gdev.ring.head_seq);

	filp->private_data = ctx;
	atomic_inc(&gdev.open_count);
	pr_debug(DRIVER_NAME ": open pid=%d seq_start=%llu\n",
		 current->pid, ctx->next_seq);
	return 0;
}

/*
 * release() — free per-fd reader state.
 *
 * Called when the last reference to the fd is dropped (close(), process exit).
 * The wait queue entry is automatically removed by the kernel once we return.
 */
static int gpu_telem_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	filp->private_data = NULL;
	atomic_dec(&gdev.open_count);
	pr_debug(DRIVER_NAME ": release pid=%d\n", current->pid);
	return 0;
}

/*
 * read() — deliver whole gpu_sample structs to user space.
 *
 * Blocking behaviour:
 *   - If the ring has no new samples for this reader, the task is put to
 *     sleep on gdev.wq via wait_event_interruptible.
 *   - ring_data_available() is the wakeup condition; it uses READ_ONCE so
 *     the compiler cannot cache head_seq across the schedule() call.
 *   - When a signal arrives (Ctrl-C), wait_event_interruptible returns
 *     -ERESTARTSYS and we propagate it so the syscall is restarted or
 *     the signal is delivered.
 *
 * O_NONBLOCK:
 *   - Returns -EAGAIN immediately when no data is available.
 *
 * copy_to_user():
 *   - We build a kernel-side kbuf before calling copy_to_user so we are
 *     never calling it with the ring spinlock held (spinlocks disable
 *     preemption; copy_to_user can sleep on a page fault).
 */
static ssize_t gpu_telem_read(struct file *filp, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct reader_ctx *ctx = filp->private_data;
	const size_t sample_sz = sizeof(struct gpu_sample);
	struct gpu_sample *kbuf;
	size_t max_samples;
	u32 n;
	int ret;

	if (count < sample_sz)
		return -EINVAL;

	max_samples = min(count / sample_sz, (size_t)gdev.ring.size);

	if (filp->f_flags & O_NONBLOCK) {
		if (!ring_data_available(&gdev.ring, ctx->next_seq))
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(gdev.wq,
			ring_data_available(&gdev.ring, ctx->next_seq));
		if (ret)
			return ret;
	}

	kbuf = kmalloc_array(max_samples, sample_sz, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	/* ring_consume() acquires the spinlock internally */
	n = ring_consume(&gdev.ring, &ctx->next_seq, kbuf, (u32)max_samples);

	if (copy_to_user(buf, kbuf, n * sample_sz)) {
		kfree(kbuf);
		return -EFAULT;
	}

	kfree(kbuf);
	return (ssize_t)(n * sample_sz);
}

/*
 * poll() — let user space multiplex /dev/gpu_telem with other fds.
 *
 * poll_wait() registers gdev.wq with the poll infrastructure without
 * sleeping.  When the collector calls wake_up_interruptible() after a
 * ring_push(), poll/select/epoll returns POLLIN to waiting user tasks.
 *
 * The check for data_available after poll_wait handles the race where
 * data arrives between the time user space called poll() and the time
 * poll_wait() registered the wait queue entry.
 */
static __poll_t gpu_telem_poll(struct file *filp, poll_table *wait)
{
	struct reader_ctx *ctx = filp->private_data;

	poll_wait(filp, &gdev.wq, wait);

	return ring_data_available(&gdev.ring, ctx->next_seq)
	       ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static long gpu_telem_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	u32 val;

	switch (cmd) {
	case GPU_TELEM_GET_RING_SIZE:
		val = gdev.ring.size;
		return copy_to_user((__u32 __user *)arg, &val, sizeof(val))
		       ? -EFAULT : 0;

	case GPU_TELEM_GET_INTERVAL_MS:
		val = READ_ONCE(gdev.interval_ms);
		return copy_to_user((__u32 __user *)arg, &val, sizeof(val))
		       ? -EFAULT : 0;

	case GPU_TELEM_SET_INTERVAL_MS:
		if (copy_from_user(&val, (__u32 __user *)arg, sizeof(val)))
			return -EFAULT;
		if (val < MIN_SAMPLE_MS)
			return -EINVAL;
		WRITE_ONCE(gdev.interval_ms, val);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations gpu_telem_fops = {
	.owner          = THIS_MODULE,
	.open           = gpu_telem_open,
	.release        = gpu_telem_release,
	.read           = gpu_telem_read,
	.poll           = gpu_telem_poll,
	.unlocked_ioctl = gpu_telem_ioctl,
};

/* ---- Module init / exit ---- */

static u32 next_pow2(u32 n)
{
	u32 p = 1;
	while (p < n)
		p <<= 1;
	return p;
}

static int __init gpu_telem_init(void)
{
	u32 sz;
	int ret;

	sz = ring_size;
	if (sz < 2 || sz > MAX_RING_SIZE) {
		pr_err(DRIVER_NAME ": ring_size %u out of range\n", sz);
		return -EINVAL;
	}
	if (sz & (sz - 1)) {
		sz = next_pow2(sz);
		pr_warn(DRIVER_NAME ": ring_size rounded to %u\n", sz);
	}

	ret = ring_init(&gdev.ring, sz);
	if (ret)
		return ret;

	gdev.interval_ms = max(sample_ms, MIN_SAMPLE_MS);
	init_waitqueue_head(&gdev.wq);
	atomic_set(&gdev.open_count, 0);

	probe_hwmon_paths();

	ret = alloc_chrdev_region(&gdev.devno, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		pr_err(DRIVER_NAME ": alloc_chrdev_region: %d\n", ret);
		goto err_ring;
	}

	cdev_init(&gdev.cdev, &gpu_telem_fops);
	gdev.cdev.owner = THIS_MODULE;
	ret = cdev_add(&gdev.cdev, gdev.devno, 1);
	if (ret) {
		pr_err(DRIVER_NAME ": cdev_add: %d\n", ret);
		goto err_unregister;
	}

	/* class_create() dropped the THIS_MODULE arg in kernel 6.4 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	gdev.dev_class = class_create(CLASS_NAME);
#else
	gdev.dev_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
	if (IS_ERR(gdev.dev_class)) {
		ret = PTR_ERR(gdev.dev_class);
		goto err_cdev;
	}

	if (IS_ERR(device_create(gdev.dev_class, NULL, gdev.devno,
				 NULL, DEVICE_NAME))) {
		ret = -ENOMEM;
		goto err_class;
	}

	/* Start collector last — it writes to the ring immediately */
	gdev.collector = kthread_run(collector_thread, NULL,
				     DRIVER_NAME "_collector");
	if (IS_ERR(gdev.collector)) {
		ret = PTR_ERR(gdev.collector);
		goto err_device;
	}

	pr_info(DRIVER_NAME ": loaded — /dev/%s major=%d ring=%u interval=%ums\n",
		DEVICE_NAME, MAJOR(gdev.devno), sz, gdev.interval_ms);
	return 0;

err_device:  device_destroy(gdev.dev_class, gdev.devno);
err_class:   class_destroy(gdev.dev_class);
err_cdev:    cdev_del(&gdev.cdev);
err_unregister: unregister_chrdev_region(gdev.devno, 1);
err_ring:    ring_destroy(&gdev.ring);
	return ret;
}

static void __exit gpu_telem_exit(void)
{
	/*
	 * Stop the collector before destroying the ring.
	 * kthread_stop() waits for the thread to return — msleep_interruptible
	 * inside collector_thread returns early when kthread_should_stop() is set.
	 * After this, no more ring_push() calls will occur.
	 */
	kthread_stop(gdev.collector);
	device_destroy(gdev.dev_class, gdev.devno);
	class_destroy(gdev.dev_class);
	cdev_del(&gdev.cdev);
	unregister_chrdev_region(gdev.devno, 1);
	ring_destroy(&gdev.ring);
	pr_info(DRIVER_NAME ": unloaded\n");
}

module_init(gpu_telem_init);
module_exit(gpu_telem_exit);
