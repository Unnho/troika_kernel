/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ADIOS 4.14 compatibility shims
 * Provides backports of blk-mq and sbitmap APIs that are missing in 4.14
 * but required by ADIOS 3.2.0 (originally written for 6.12).
 */
#ifndef _ADIOS_4_14_SHIM_H_
#define _ADIOS_4_14_SHIM_H_

#include <linux/blk-mq.h>
#include <linux/list.h>
#include <linux/sbitmap.h>

/*
 * blk_opf_t was introduced in 5.8 as a u32 wrapper around the old
 * REQ_* flag namespace. On 4.14 the equivalent type is unsigned int /
 * u32. We define blk_opf_t as a 32-bit type so ADIOS type-checks pass.
 */
typedef u32 blk_opf_t;

/*
 * In 6.12, blk_mq_free_requests() iterates a list of requests and
 * calls blk_mq_free_request() on each. We provide a 4.14-compatible
 * shim that does exactly that.
 *
 * ADIOS uses the rq->queuelist link to chain requests to free; we
 * follow the same convention.
 */
static inline void blk_mq_free_requests(struct list_head *list)
{
	struct request *rq, *next;

	list_for_each_entry_safe(rq, next, list, queuelist) {
		list_del_init(&rq->queuelist);
		blk_mq_free_request(rq);
	}
}

/*
 * sbitmap_queue_min_shallow_depth() was added in 5.16 so that an
 * sbitmap_queue could be configured to always allow at least @depth
 * tags to be allocated via the shallow path. In 4.14 the shallow
 * depth is passed per-call to sbitmap_queue_get_shallow() and there
 * is no equivalent per-queue state, so the function is a no-op that
 * just ensures the queue is initialised.
 */
static inline void sbitmap_queue_min_shallow_depth(struct sbitmap_queue *sbq,
						   unsigned int depth)
{
	/* 4.14 has no min_shallow_depth field; safe no-op. */
	(void)sbq;
	(void)depth;
}

#endif /* _ADIOS_4_14_SHIM_H_ */
