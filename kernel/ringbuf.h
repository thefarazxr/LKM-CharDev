/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kernel/ringbuf.h — Lock-protected circular ring buffer
 *
 * Responsibility split:
 *   ringbuf.c   — synchronization and producer/consumer race conditions
 *   gpu_telem_core.c — filesystem interface, blocking reads, wait queues
 *
 * Design:
 *   - Indexed by a monotonically increasing u64 sequence number (head_seq).
 *   - Each reader tracks its own next_seq independently → broadcast semantics:
 *     multiple open() fds each see the complete stream.
 *   - The spinlock is entirely internal; callers never acquire it.
 *   - Overrun policy: when a slow reader falls more than ring->size samples
 *     behind, its next_seq is advanced to the oldest live sample and the
 *     dropped counter is incremented.
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <linux/spinlock.h>
#include <linux/types.h>
#include "../include/gpu_telem_uapi.h"

struct ring_buf {
	struct gpu_sample *samples; /* vzalloc'd, size entries              */
	u32  size;                  /* capacity — always a power of 2       */
	u32  mask;                  /* size - 1, for fast modular index     */
	u64  head_seq;              /* sequence of the next slot to write   */
	u64  dropped;               /* samples skipped due to slow readers  */
	spinlock_t lock;
};

/*
 * ring_init — allocate sample storage and initialise all fields.
 * @size must be a power of 2 and ≤ 4096.
 * Returns 0 on success, -EINVAL / -ENOMEM on failure.
 */
int  ring_init(struct ring_buf *r, u32 size);

/* ring_destroy — release sample storage (safe to call if init failed). */
void ring_destroy(struct ring_buf *r);

/*
 * ring_push — append one sample (called by the producer / collector thread).
 * Acquires and releases the spinlock internally.
 * Never blocks; if the ring is full the oldest slot is silently overwritten
 * (readers that were pointing at it will detect overrun on next ring_consume).
 */
void ring_push(struct ring_buf *r, const struct gpu_sample *s);

/*
 * ring_data_available — lockless check for use as wait_event condition.
 *
 * Returns true if at least one sample is available at *next_seq.
 * Uses READ_ONCE so the compiler cannot cache head_seq across a schedule().
 * It is safe to call this from any context without holding any lock.
 */
bool ring_data_available(const struct ring_buf *r, u64 next_seq);

/*
 * ring_consume — copy up to @max samples into @out[] and advance *next_seq.
 *
 * Acquires the spinlock internally.  If the reader has fallen behind by more
 * than ring->size samples, *next_seq is fast-forwarded to the oldest live
 * entry and r->dropped is incremented.
 *
 * Returns the number of samples written to @out[].  Never blocks.
 */
u32  ring_consume(struct ring_buf *r, u64 *next_seq,
		  struct gpu_sample *out, u32 max);

/* ring_dropped — total samples lost to slow-reader overrun. */
u64  ring_dropped(const struct ring_buf *r);

#endif /* RINGBUF_H */
