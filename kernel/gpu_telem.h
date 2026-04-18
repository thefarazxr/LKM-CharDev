/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kernel/gpu_telem.h — Kernel-internal device structs and constants
 *
 * NOT included by user space.  User/kernel shared definitions live in
 * include/gpu_telem_uapi.h.
 */

#ifndef GPU_TELEM_KERN_H
#define GPU_TELEM_KERN_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/kthread.h>

#include "ringbuf.h"

#define DRIVER_NAME    "gpu_telem"
#define DEVICE_NAME    "gpu_telem"
#define CLASS_NAME     "gpu_telem"

#define DEFAULT_RING_SIZE  256u
#define MAX_RING_SIZE      4096u
#define DEFAULT_SAMPLE_MS  500u
#define MIN_SAMPLE_MS      10u

/*
 * struct reader_ctx — per-fd reader state, allocated in gpu_telem_open().
 *
 * Tracks this file descriptor's independent read position in the ring buffer.
 * Multiple open() calls each get their own reader_ctx → broadcast semantics:
 * every reader sees every sample independently (like a log, not a pipe).
 *
 * next_seq is only written by ring_consume() while the ring spinlock is held,
 * and only read by ring_data_available() / ring_consume() on this fd's thread.
 * Concurrent reads on the same fd are not defined (standard driver behaviour).
 */
struct reader_ctx {
	u64 next_seq;
};

/*
 * struct gpu_telem_device — singleton device state for the entire driver.
 *
 * Fields:
 *   ring       — the shared ring buffer (synchronised inside ringbuf.c)
 *   wq         — wait queue; readers block here when the ring is empty
 *   collector  — kthread that periodically samples sysfs/hwmon
 *   cdev       — character device registered with the VFS
 *   dev_class  — sysfs class that triggers udev to create /dev/gpu_telem
 *   devno      — allocated major:minor pair
 *   open_count — number of active file descriptors (informational)
 *   interval_ms— collection interval; readable/writable via ioctl
 *   *_path     — resolved sysfs attribute paths; empty = attribute absent
 */
struct gpu_telem_device {
	struct ring_buf       ring;
	wait_queue_head_t     wq;
	struct task_struct   *collector;
	struct cdev           cdev;
	struct class         *dev_class;
	dev_t                 devno;
	atomic_t              open_count;
	unsigned int          interval_ms;

	char temp_path[256];
	char fan_path[256];
	char power_path[256];
	char freq_path[256];
};

#endif /* GPU_TELEM_KERN_H */
