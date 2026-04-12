// SPDX-License-Identifier: GPL-2.0
//
// kernel/ringbuf.c — Ring buffer: synchronization and producer/consumer races
//
// All locking lives here.  gpu_telem_core.c never touches the spinlock.
//
// Synchronization model:
//   ring_push()        — spin_lock_irqsave: serialises the single writer
//                        (collector kthread) and guards head_seq.
//   ring_consume()     — spin_lock_irqsave: serialises each reader's
//                        next_seq update against concurrent ring_push().
//   ring_data_available() — lockless READ_ONCE on head_seq; used as the
//                        condition in wait_event_interruptible so it must
//                        never sleep or acquire a lock.

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/kernel.h>

#include "ringbuf.h"

int ring_init(struct ring_buf *r, u32 size)
{
	if (!size || (size & (size - 1)) || size > 4096)
		return -EINVAL;

	r->samples = vzalloc(size * sizeof(struct gpu_sample));
	if (!r->samples)
		return -ENOMEM;

	r->size     = size;
	r->mask     = size - 1;
	r->head_seq = 0;
	r->dropped  = 0;
	spin_lock_init(&r->lock);
	return 0;
}

void ring_destroy(struct ring_buf *r)
{
	vfree(r->samples);
	r->samples = NULL;
}

void ring_push(struct ring_buf *r, const struct gpu_sample *s)
{
	unsigned long flags;
	u32 slot;

	spin_lock_irqsave(&r->lock, flags);
	slot = (u32)(r->head_seq & r->mask);
	r->samples[slot] = *s;
	/*
	 * Store head_seq last.  Any reader that sees the new head_seq via
	 * READ_ONCE in ring_data_available() will then find a fully written
	 * sample at [slot] — the spinlock's release barrier makes this
	 * visible before the unlock completes.
	 */
	r->head_seq++;
	spin_unlock_irqrestore(&r->lock, flags);
}

bool ring_data_available(const struct ring_buf *r, u64 next_seq)
{
	/*
	 * READ_ONCE prevents the compiler from caching head_seq across a
	 * call to schedule() inside wait_event_interruptible.  No memory
	 * barrier is needed here: if we read a stale (lower) head_seq we
	 * just go back to sleep; the next wake_up will re-check.
	 */
	return READ_ONCE(r->head_seq) > next_seq;
}

u32 ring_consume(struct ring_buf *r, u64 *next_seq,
		 struct gpu_sample *out, u32 max)
{
	unsigned long flags;
	u64 head, behind;
	u32 to_copy, i;

	spin_lock_irqsave(&r->lock, flags);

	head = r->head_seq;
	if (head <= *next_seq) {
		spin_unlock_irqrestore(&r->lock, flags);
		return 0;
	}

	behind = head - *next_seq;
	if (behind > r->size) {
		/*
		 * Reader fell behind — advance to the oldest live sample.
		 * Increment dropped by the number of samples we skip so
		 * user space can detect this via a future ioctl.
		 */
		u64 skip = behind - r->size;
		r->dropped  += skip;
		*next_seq   += skip;
		behind       = r->size;
		pr_debug_ratelimited("gpu_telem: ringbuf overrun — skipped %llu samples\n",
				     skip);
	}

	to_copy = (u32)min_t(u64, behind, (u64)max);
	for (i = 0; i < to_copy; i++) {
		u32 slot = (u32)(*next_seq & r->mask);
		out[i] = r->samples[slot];
		(*next_seq)++;
	}

	spin_unlock_irqrestore(&r->lock, flags);
	return to_copy;
}

u64 ring_dropped(const struct ring_buf *r)
{
	unsigned long flags;
	u64 d;

	spin_lock_irqsave((spinlock_t *)&r->lock, flags);
	d = r->dropped;
	spin_unlock_irqrestore((spinlock_t *)&r->lock, flags);
	return d;
}
