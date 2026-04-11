/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/gpu_telem_uapi.h — User/Kernel shared telemetry interface
 *
 * This header is the contract between:
 *   kernel/gpu_telem_core.c  (producer — writes into ring buffer)
 *   user/gpu_telem_reader.c  (C consumer — reads from /dev/gpu_telem)
 *   user/consumer.py         (Python consumer — same struct via struct.unpack)
 *
 * Any change to struct gpu_sample breaks binary compatibility with all
 * user-space readers — bump the size and update _pad accordingly.
 */

#ifndef GPU_TELEM_UAPI_H
#define GPU_TELEM_UAPI_H

#ifdef __KERNEL__
# include <linux/types.h>
# include <linux/ioctl.h>
#else
# include <stdint.h>
# include <sys/ioctl.h>
typedef uint8_t  __u8;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t  __s32;
#endif

/*
 * struct gpu_sample — one telemetry snapshot from the kernel pipeline.
 *
 * Fields use sentinel values when the underlying sysfs/hwmon attribute is
 * absent on the current hardware (common on NVIDIA with the closed driver).
 *
 * Memory layout (little-endian, 48 bytes, no compiler padding needed):
 *   offset  size  field
 *        0     8  timestamp_ns
 *        8     4  temp_millideg
 *       12     4  fan_rpm
 *       16     8  power_uw          ← 8-byte aligned
 *       24     4  core_freq_mhz
 *       28     4  mem_freq_mhz
 *       32     4  gpu_util_pct
 *       36     4  mem_util_pct
 *       40     1  throttle_reasons
 *       41     7  _pad
 *   total = 48 bytes
 */
struct gpu_sample {
	__u64 timestamp_ns;     /* ktime_get_ns() at collection time       */
	__s32 temp_millideg;    /* millidegrees C; -1 = unavailable        */
	__u32 fan_rpm;          /* RPM; 0 = unavailable                    */
	__u64 power_uw;         /* microwatts; 0 = unavailable             */
	__u32 core_freq_mhz;    /* GPU core clock; 0 = unavailable         */
	__u32 mem_freq_mhz;     /* memory clock;   0 = unavailable         */
	__u32 gpu_util_pct;     /* 0-100; ~0u = unavailable (NVIDIA: n/a) */
	__u32 mem_util_pct;     /* 0-100; ~0u = unavailable (NVIDIA: n/a) */
	__u8  throttle_reasons; /* bitmask — see GPU_THROTTLE_* below      */
	__u8  _pad[7];
};

/* throttle_reasons bitmask bits */
#define GPU_THROTTLE_POWER    (1u << 0)  /* power limit hit        */
#define GPU_THROTTLE_THERMAL  (1u << 1)  /* thermal limit hit      */
#define GPU_THROTTLE_CURRENT  (1u << 2)  /* current/voltage limit  */
#define GPU_THROTTLE_UTIL     (1u << 3)  /* utilisation-based      */

/*
 * ioctl interface — allows user space to inspect and tune the kernel pipeline
 * without reloading the module.
 *
 * ioctl number encoding (Linux _IOC macro):
 *   bits 31-30: direction (2=read, 1=write, 3=read+write)
 *   bits 29-16: size of the data argument
 *   bits 15-8:  magic byte (type)
 *   bits  7-0:  command number
 */
#define GPU_TELEM_MAGIC            'G'
#define GPU_TELEM_GET_RING_SIZE    _IOR(GPU_TELEM_MAGIC, 0, __u32)
#define GPU_TELEM_GET_INTERVAL_MS  _IOR(GPU_TELEM_MAGIC, 1, __u32)
#define GPU_TELEM_SET_INTERVAL_MS  _IOW(GPU_TELEM_MAGIC, 2, __u32)

#endif /* GPU_TELEM_UAPI_H */
