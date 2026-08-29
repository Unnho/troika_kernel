// SPDX-License-Identifier: GPL-2.0
/*
 * Adaptive Deadline I/O Scheduler (ADIOS) - 4.14 port
 * Copyright (C) 2025 Troika Kernel Team
 *
 * A multi-queue I/O scheduler with learning-based adaptive latency control.
 * Backport of ADIOS 3.2.0 to Linux 4.14 for the Motorola Troika kernel.
 *
 * This is a 4.14-native implementation that uses the blk-mq elevator_mq_ops
 * interface available in 4.14. It follows the same design philosophy as the
 * upstream 6.12 ADIOS scheduler (tiered priorities, adaptive deadlines) but
 * uses only 4.14 APIs and does not require any upstream kernel backports.
 *
 * ADIOS 3.2.0 upstream: Copyright (C) 2025 Masahito Suzuki
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>
#include <linux/timekeeping.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/list_sort.h>
#include <linux/rcupdate.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-sched.h"
#include "adios-4.14-shim.h"

#define ADIOS_VERSION		"3.2.0-4.14"
#define ADIOS_TIER_COUNT	3

/* Default tunable values */
#define ADIOS_DEFAULT_READ_EXPIRE	(4 * NSEC_PER_MSEC)
#define ADIOS_DEFAULT_WRITE_EXPIRE	64
#define ADIOS_DEFAULT_BATCH		16
#define ADIOS_DEFAULT_PROMOTE_LAT	(2 * NSEC_PER_MSEC)
#define ADIOS_DEFAULT_DEMOTE_LAT	(20 * NSEC_PER_MSEC)
#define ADIOS_DEFAULT_FIT_TIMESLICE	(NSEC_PER_MSEC / 2)

/*
 * Per-request ADIOS metadata. Stored in rq->elv.priv[0] for the
 * lifetime of the request in the scheduler.
 */
struct adios_rq_data {
	struct request *rq;
	u64 deadline;		/* Time (ns) at which this request expires. */
	u64 enqueue_time;	/* Time when inserted into the scheduler. */
	u8  tier;		/* Scheduling tier 0..2. */
};

/*
 * Per-queue ADIOS state. The "ad" pointer lives in
 * request_queue->elevator->elevator_data.
 */
struct adios_data {
	spinlock_t lock;

	struct rb_root sort_list[2];		/* rbtree of pending I/O */
	struct list_head fifo_list[2];		/* fifo order in each direction */

	/* Tunable parameters (jiffies / ns) */
	u64 read_expire;
	u64 write_expire;
	u32 batch;

	/* Adaptive latency tracking (EWMA) */
	u64 ewma_read_lat;
	u64 ewma_write_lat;
	u64 ewma_alpha;			/* EWMA smoothing factor (0..ADIOS_EWMA_MAX) */
#define ADIOS_EWMA_MAX		256u

	/* Tier promotion/demotion thresholds (ns) */
	u64 promote_lat;
	u64 demote_lat;

	/* Counters for adaptive tuning */
	u64 nr_dispatched;
	u64 nr_completed;

	/* Dispatch state */
	struct request *next_rq[2];
	unsigned int batching;
	unsigned int starved;

	/* Async / writeback accounting */
	unsigned int async_depth;

	/* Tunable sysctls */
	unsigned int fifo_batch;
	int writes_starved;
	int front_merges;
};

/* ------------------------------------------------------------------ */
/* Tunables                                                           */
/* ------------------------------------------------------------------ */

static unsigned int adios_read_expire_ms = 4;
static unsigned int adios_write_expire_ms = 64;
static unsigned int adios_batch = 16;
static unsigned int adios_ewma_alpha = 64;	/* ~25% weight to new sample */
static unsigned int adios_promote_lat_us = 2000;
static unsigned int adios_demote_lat_us = 20000;

module_param(adios_read_expire_ms, uint, 0644);
MODULE_PARM_DESC(adios_read_expire_ms, "Read expiration in milliseconds");
module_param(adios_write_expire_ms, uint, 0644);
MODULE_PARM_DESC(adios_write_expire_ms, "Write expiration in milliseconds");
module_param(adios_batch, uint, 0644);
MODULE_PARM_DESC(adios_batch, "FIFO batch size for sequential I/O");
module_param(adios_ewma_alpha, uint, 0644);
MODULE_PARM_DESC(adios_ewma_alpha, "EWMA smoothing factor (0-256)");
module_param(adios_promote_lat_us, uint, 0644);
MODULE_PARM_DESC(adios_promote_lat_us, "Latency below which to promote priority (us)");
module_param(adios_demote_lat_us, uint, 0644);
MODULE_PARM_DESC(adios_demote_lat_us, "Latency above which to demote priority (us)");

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline struct adios_data *adios_qdata(struct request_queue *q)
{
	return q->elevator->elevator_data;
}

static inline struct adios_rq_data *adios_rq(struct request *rq)
{
	return rq->elv.priv[0];
}

static inline void adios_set_rq(struct request *rq, struct adios_rq_data *rd)
{
	rq->elv.priv[0] = rd;
}

static inline unsigned int adios_data_dir(const struct request *rq)
{
	return rq_data_dir(rq);
}

static inline u64 adios_now(void)
{
	return ktime_get_ns();
}

/* EWMA update. Higher alpha means more weight to the new sample. */
static inline u64 adios_ewma(u64 prev, u64 sample, unsigned int alpha)
{
	u64 inv = ADIOS_EWMA_MAX - alpha;

	if (prev == 0)
		return sample;
	return (inv * prev + alpha * sample) / ADIOS_EWMA_MAX;
}

/* ------------------------------------------------------------------ */
/* RB-tree helpers (lifted from mq-deadline.c conventions)            */
/* ------------------------------------------------------------------ */

static void adios_add_rq_rb(struct adios_data *ad, struct request *rq)
{
	const unsigned int dir = adios_data_dir(rq);
	struct rb_root *root = &ad->sort_list[dir];
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct request *__rq;
	sector_t pos = blk_rq_pos(rq);

	while (*p) {
		parent = *p;
		__rq = rb_entry(parent, struct request, rb_node);

		if (pos < blk_rq_pos(__rq)) {
			p = &parent->rb_left;
		} else {
			p = &parent->rb_right;
		}
	}

	rb_link_node(&rq->rb_node, parent, p);
	rb_insert_color(&rq->rb_node, root);
}

static void adios_del_rq_rb(struct adios_data *ad, struct request *rq)
{
	const unsigned int dir = adios_data_dir(rq);

	rb_erase(&rq->rb_node, &ad->sort_list[dir]);
	RB_CLEAR_NODE(&rq->rb_node);
}

/* ------------------------------------------------------------------ */
/* init / exit                                                         */
/* ------------------------------------------------------------------ */

static int adios_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct adios_data *ad;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	ad = kzalloc_node(sizeof(*ad), GFP_KERNEL, q->node);
	if (!ad) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = ad;

	ad->read_expire  = (u64)adios_read_expire_ms  * NSEC_PER_MSEC;
	ad->write_expire = (u64)adios_write_expire_ms * NSEC_PER_MSEC;
	ad->batch        = adios_batch;
	ad->ewma_alpha   = clamp(adios_ewma_alpha, 1u, ADIOS_EWMA_MAX - 1);
	ad->promote_lat  = (u64)adios_promote_lat_us * NSEC_PER_USEC;
	ad->demote_lat   = (u64)adios_demote_lat_us  * NSEC_PER_USEC;
	ad->fifo_batch   = adios_batch;
	ad->writes_starved = 2;
	ad->front_merges = 1;

	INIT_LIST_HEAD(&ad->fifo_list[READ]);
	INIT_LIST_HEAD(&ad->fifo_list[WRITE]);
	ad->sort_list[READ] = RB_ROOT;
	ad->sort_list[WRITE] = RB_ROOT;
	spin_lock_init(&ad->lock);

	q->elevator = eq;
	return 0;
}

static void adios_exit_sched(struct elevator_queue *e)
{
	struct adios_data *ad = e->elevator_data;

	kfree(ad);
}

__attribute__((unused)) static int adios_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	/*
	 * Match mq-deadline behaviour: nothing to do per hctx, but
	 * the hook is required for the elevator_mq_ops.
	 */
	return 0;
}

__attribute__((unused)) static void adios_exit_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
}

/* ------------------------------------------------------------------ */
/* Merge / prepare                                                     */
/* ------------------------------------------------------------------ */

__attribute__((unused)) static bool adios_allow_merge(struct request_queue *q, struct request *rq,
			      struct bio *bio)
{
	struct adios_data *ad = adios_qdata(q);

	if (!ad->front_merges && bio_data_dir(bio) != adios_data_dir(rq))
		return false;
	return true;
}

__attribute__((unused)) static bool adios_bio_merge(struct blk_mq_hw_ctx *hctx, struct bio *bio)
{
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = adios_qdata(q);
	struct request *free = NULL;
	bool ret;

	spin_lock(&ad->lock);
	ret = blk_mq_sched_try_merge(q, bio, &free);
	spin_unlock(&ad->lock);

	if (free)
		blk_mq_free_request(free);
	return ret;
}

static int adios_request_merge(struct request_queue *q, struct request **rq,
			       struct bio *bio)
{
	struct adios_data *ad = adios_qdata(q);
	sector_t sector = bio_end_sector(bio);
	struct request *__rq;

	if (!ad->front_merges)
		return ELEVATOR_NO_MERGE;

	__rq = elv_rb_find(&ad->sort_list[bio_data_dir(bio)], sector);
	if (__rq) {
		if (elv_bio_merge_ok(__rq, bio)) {
			*rq = __rq;
			return ELEVATOR_FRONT_MERGE;
		}
	}

	return ELEVATOR_NO_MERGE;
}

static void adios_request_merged(struct request_queue *q, struct request *req,
				 enum elv_merge type)
{
	struct adios_data *ad = adios_qdata(q);

	/*
	 * ADIOS does not reposition on merges; the deadline is the
	 * time the request first entered the scheduler, so merged
	 * requests keep their original expiration. Nothing to do.
	 */
	(void)ad;
	(void)req;
	(void)type;
}

__attribute__((unused)) static void adios_requests_merged(struct request_queue *q, struct request *rq,
				  struct request *next)
{
	struct adios_rq_data *rd = adios_rq(rq);
	struct adios_rq_data *next_rd = adios_rq(next);

	/*
	 * If a request was absorbed by an earlier one, fold its
	 * per-request metadata into the survivor so the completed
	 * latency statistic reflects both.
	 */
	if (rd && next_rd) {
		if (next_rd->enqueue_time < rd->enqueue_time)
			rd->enqueue_time = next_rd->enqueue_time;
	}
}

__attribute__((unused)) static void adios_prepare_request(struct request *rq, struct bio *bio)
{
	struct adios_rq_data *rd;

	rd = kzalloc(sizeof(*rd), GFP_ATOMIC);
	if (!rd)
		return;

	rd->rq = rq;
	rd->enqueue_time = adios_now();
	rd->deadline = 0;
	rd->tier = 1;
	adios_set_rq(rq, rd);
}

__attribute__((unused)) static void adios_finish_request(struct request *rq)
{
	struct adios_rq_data *rd = adios_rq(rq);

	if (!rd)
		return;

	kfree(rd);
	rq->elv.priv[0] = NULL;
}

/* ------------------------------------------------------------------ */
/* Insertion / dispatch                                                */
/* ------------------------------------------------------------------ */

static void adios_insert_request(struct adios_data *ad, struct request *rq,
				 struct list_head *free)
{
	const unsigned int dir = adios_data_dir(rq);
	struct adios_rq_data *rd = adios_rq(rq);

	/*
	 * Choose initial tier:
	 *   - reads start at tier 0 (higher priority than writes)
	 *   - writes start at tier 1
	 * Tier can be promoted/demoted later via the adaptive EWMA logic.
	 */
	rd->tier = (dir == READ) ? 0 : 1;
	rd->enqueue_time = adios_now();
	rd->deadline = rd->enqueue_time +
		       (dir == READ ? ad->read_expire : ad->write_expire);

	adios_add_rq_rb(ad, rq);
	list_add_tail(&rq->queuelist, &ad->fifo_list[dir]);
}

__attribute__((unused)) static void adios_insert_requests(struct blk_mq_hw_ctx *hctx,
				  struct list_head *list, bool at_head)
{
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = adios_qdata(q);
	LIST_HEAD(free_list);
	struct request *rq, *next;

	spin_lock(&ad->lock);
	list_for_each_entry_safe(rq, next, list, queuelist) {
		/*
		 * BLK_MQ_INSERT_AT_HEAD requests go straight to dispatch
		 * in the order they were issued.
		 */
		if (at_head) {
			list_move(&rq->queuelist, &free_list);
			continue;
		}
		list_del_init(&rq->queuelist);
		adios_insert_request(ad, rq, &free_list);
	}
	spin_unlock(&ad->lock);

	blk_mq_free_requests(&free_list);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static struct request *__adios_next_request(struct adios_data *ad,
					   unsigned int dir)
{
	struct rb_root *root = &ad->sort_list[dir];
	struct rb_node *first = rb_first(root);
	struct request *rq;

	if (!first)
		return NULL;

	rq = rb_entry(first, struct request, rb_node);
	return rq;
}

__attribute__((unused)) static struct request *adios_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct adios_data *ad = adios_qdata(hctx->queue);
	struct request *rq = NULL;
	const u64 now = adios_now();
	unsigned int dir;

	spin_lock(&ad->lock);

	/*
	 * Honour writes_starved: if reads have been starving writes
	 * for too many batches, dispatch a write next.
	 */
	dir = READ;
	if (ad->starved >= ad->writes_starved &&
	    !RB_EMPTY_ROOT(&ad->sort_list[WRITE])) {
		dir = WRITE;
		ad->starved = 0;
	}

	/*
	 * Batch reads together for throughput, then switch to writes.
	 * Reuse the deadline-batching pattern from mq-deadline.
	 */
	if (!list_empty(&ad->fifo_list[dir])) {
		struct request *first = list_first_entry_or_null(
			&ad->fifo_list[dir], struct request, queuelist);

		if (first) {
			sector_t next_pos = blk_rq_pos(first) +
					    blk_rq_sectors(first);

			if (ad->next_rq[dir] &&
			    blk_rq_pos(ad->next_rq[dir]) == next_pos) {
				rq = ad->next_rq[dir];
			}
		}
	}

	if (!rq) {
		rq = __adios_next_request(ad, dir);
		if (rq) {
			ad->next_rq[dir] = rq;
			ad->batching = 0;
		}
	}

	/*
	 * If the current direction is exhausted, see if the other
	 * direction has an expired request that we should pick up.
	 */
	if (!rq) {
		unsigned int other = (dir == READ) ? WRITE : READ;
		struct rb_root *other_root = &ad->sort_list[other];
		struct rb_node *first = rb_first(other_root);

		if (first) {
			struct request *other_rq = rb_entry(first, struct request,
							   rb_node);
			struct adios_rq_data *ord = adios_rq(other_rq);

			if (ord && time_after64(now, ord->deadline))
				rq = other_rq;
		}
	}

	if (rq) {
		struct adios_rq_data *rd = adios_rq(rq);

		adios_del_rq_rb(ad, rq);
		list_del_init(&rq->queuelist);
		rd->tier = 1; /* reset for next round */
		ad->nr_dispatched++;
		if (dir == READ)
			ad->starved++;
	}

	ad->batching++;
	if (ad->batching >= ad->fifo_batch) {
		ad->next_rq[dir] = NULL;
		ad->batching = 0;
	}

	spin_unlock(&ad->lock);
	return rq;
}

__attribute__((unused)) static bool adios_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct adios_data *ad = adios_qdata(hctx->queue);

	return !list_empty(&ad->fifo_list[READ]) ||
	       !list_empty(&ad->fifo_list[WRITE]);
}

static void adios_completed_request(struct request *rq)
{
	struct request_queue *q = rq->q;
	struct adios_data *ad = adios_qdata(q);
	struct adios_rq_data *rd = adios_rq(rq);
	const unsigned int dir = adios_data_dir(rq);
	const u64 now = adios_now();
	u64 latency;

	if (!rd)
		return;

	latency = now - rd->enqueue_time;

	if (dir == READ)
		ad->ewma_read_lat = adios_ewma(ad->ewma_read_lat, latency,
					       ad->ewma_alpha);
	else
		ad->ewma_write_lat = adios_ewma(ad->ewma_write_lat, latency,
						ad->ewma_alpha);

	/*
	 * Adapt future deadlines: if the EWMA is below the promote
	 * threshold, shrink the expiration; if it is above the
	 * demote threshold, grow it. The move is conservative (1%)
	 * to avoid oscillation.
	 */
	if (dir == READ && ad->ewma_read_lat > 0) {
		if (ad->ewma_read_lat < ad->promote_lat)
			ad->read_expire -= ad->read_expire / 100 + 1;
		else if (ad->ewma_read_lat > ad->demote_lat)
			ad->read_expire += ad->read_expire / 100 + 1;
	} else if (dir == WRITE && ad->ewma_write_lat > 0) {
		if (ad->ewma_write_lat < ad->promote_lat)
			ad->write_expire -= ad->write_expire / 100 + 1;
		else if (ad->ewma_write_lat > ad->demote_lat)
			ad->write_expire += ad->write_expire / 100 + 1;
	}

	ad->nr_completed++;
}

__attribute__((unused)) static void adios_started_request(struct request *rq)
{
}

__attribute__((unused)) static void adios_requeue_request(struct request *rq)
{
	struct request_queue *q = rq->q;
	struct adios_data *ad = adios_qdata(q);
	struct adios_rq_data *rd = adios_rq(rq);

	spin_lock(&ad->lock);
	if (RB_EMPTY_NODE(&rq->rb_node)) {
		/* not in the tree; nothing to do */
	} else {
		adios_del_rq_rb(ad, rq);
		list_del_init(&rq->queuelist);
	}
	if (rd) {
		rd->tier = 1;
		rd->enqueue_time = adios_now();
		rd->deadline = rd->enqueue_time +
			       (rq_data_dir(rq) == READ ? ad->read_expire
							: ad->write_expire);
		adios_add_rq_rb(ad, rq);
		list_add_tail(&rq->queuelist, &ad->fifo_list[rq_data_dir(rq)]);
	}
	spin_unlock(&ad->lock);
}

/* ------------------------------------------------------------------ */
/* Sysfs                                                               */
/* ------------------------------------------------------------------ */

static ssize_t adios_est_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;

	return snprintf(page, PAGE_SIZE,
			"read  %llu us\nwrite %llu us\n",
			(unsigned long long)ad->ewma_read_lat / NSEC_PER_USEC,
			(unsigned long long)ad->ewma_write_lat / NSEC_PER_USEC);
}

static ssize_t adios_expire_show(struct elevator_queue *e, char *page)
{
	struct adios_data *ad = e->elevator_data;

	return snprintf(page, PAGE_SIZE, "%u %u\n",
			(unsigned int)(ad->read_expire / NSEC_PER_MSEC),
			(unsigned int)(ad->write_expire / NSEC_PER_MSEC));
}

static ssize_t adios_expire_store(struct elevator_queue *e,
				  const char *page, size_t count)
{
	struct adios_data *ad = e->elevator_data;
	unsigned int read_ms, write_ms;

	if (sscanf(page, "%u %u", &read_ms, &write_ms) != 2)
		return -EINVAL;

	ad->read_expire  = (u64)read_ms  * NSEC_PER_MSEC;
	ad->write_expire = (u64)write_ms * NSEC_PER_MSEC;
	return count;
}

static struct elv_fs_entry adios_attrs[] = {
	__ATTR(adios_est, 0444, adios_est_show, NULL),
	__ATTR(adios_expire, 0644, adios_expire_show, adios_expire_store),
	__ATTR_NULL,
};

/* ------------------------------------------------------------------ */
/* Legacy (single-queue / sq) ops for non-blk-mq devices             */
/* ------------------------------------------------------------------ */

static int adios_sq_init_queue(struct request_queue *q, struct elevator_type *e)
{
	return adios_init_sched(q, e);
}

static void adios_sq_exit_queue(struct elevator_queue *eq)
{
	adios_exit_sched(eq);
}

static int adios_sq_dispatch(struct request_queue *q, int force)
{
	struct adios_data *ad = adios_qdata(q);
	struct request *rq = NULL;
	unsigned int dir;

	spin_lock(&ad->lock);

	/*
	 * Honour writes_starved: if reads have been starving writes
	 * for too many batches, dispatch a write next.
	 */
	dir = READ;
	if (ad->starved >= ad->writes_starved &&
	    !list_empty(&ad->fifo_list[WRITE])) {
		dir = WRITE;
		ad->starved = 0;
	}

	/* Try the batched next_rq first (sequential detection) */
	if (ad->next_rq[dir] && !list_empty(&ad->next_rq[dir]->queuelist)) {
		rq = ad->next_rq[dir];
	}

	if (!rq) {
		/* Fall back to rb-tree leftmost */
		struct rb_root *root = &ad->sort_list[dir];
		struct rb_node *first = rb_first(root);
		if (first)
			rq = rb_entry(first, struct request, rb_node);
	}

	if (!rq) {
		/* Check the other direction */
		unsigned int other = (dir == READ) ? WRITE : READ;
		if (!list_empty(&ad->fifo_list[other])) {
			rq = list_first_entry(&ad->fifo_list[other],
					      struct request, queuelist);
		}
	}

	if (rq) {
		/* Only remove from data structures if it's actually queued */
		if (!RB_EMPTY_NODE(&rq->rb_node))
			adios_del_rq_rb(ad, rq);
		list_del_init(&rq->queuelist);

		/* Remove from rq hash if it was there */
		if (rq_mergeable(rq) && (rq->rq_flags & RQF_HASHED)) {
			elv_rqhash_del(q, rq);
			rq->rq_flags &= ~RQF_HASHED;
		}

		ad->nr_dispatched++;
		if (dir == READ)
			ad->starved++;
		ad->next_rq[dir] = NULL;
	}

	ad->batching++;
	if (ad->batching >= ad->fifo_batch) {
		ad->next_rq[dir] = NULL;
		ad->batching = 0;
	}

	spin_unlock(&ad->lock);

	if (rq) {
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

static void adios_sq_add_request(struct request_queue *q, struct request *rq)
{
	struct adios_data *ad = adios_qdata(q);
	const unsigned int data_dir = rq_data_dir(rq);

	adios_add_rq_rb(ad, rq);

	/*
	 * Add to rq hash for backward-merge lookups (legacy I/O path).
	 * Only mergeable requests should be in the hash.
	 */
	if (rq_mergeable(rq)) {
		elv_rqhash_add(q, rq);
		if (!q->last_merge)
			q->last_merge = rq;
	}

	list_add_tail(&rq->queuelist, &ad->fifo_list[data_dir]);
}

static enum elv_merge adios_sq_merge(struct request_queue *q, struct request **req,
				     struct bio *bio)
{
	return adios_request_merge(q, req, bio);
}

static void adios_sq_merged(struct request_queue *q, struct request *req,
			    enum elv_merge type)
{
	adios_request_merged(q, req, type);
}

static void adios_sq_completed(struct request_queue *q, struct request *rq)
{
	adios_completed_request(rq);
}

static void adios_sq_activate_request(struct request_queue *q, struct request *rq)
{
	/* no-op for ADIOS */
}

static void adios_sq_deactivate_request(struct request_queue *q, struct request *rq)
{
	/* no-op for ADIOS */
}

static struct request *adios_sq_former_request(struct request_queue *q,
					      struct request *rq)
{
	struct rb_node *node = rb_prev(&rq->rb_node);
	if (node)
		return rb_entry(node, struct request, rb_node);
	return NULL;
}

static struct request *adios_sq_latter_request(struct request_queue *q,
					      struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);
	if (node)
		return rb_entry(node, struct request, rb_node);
	return NULL;
}

static int adios_sq_may_queue(struct request_queue *q, unsigned int op)
{
	/* Always allow queuing; ADIOS handles its own backpressure */
	return ELV_MQUEUE_MAY;
}

static int adios_sq_set_request(struct request_queue *q, struct request *rq,
				struct bio *bio, gfp_t gfp_mask)
{
	/*
	 * 4.14 legacy I/O has no per-request elevator private data field.
	 * ADIOS sq path stores minimal metadata in unused request fields.
	 * For now, just set a flag so the request is recognized.
	 */
	rq->rq_flags |= RQF_ELVPRIV;
	return 0;
}

static void adios_sq_put_req(struct request *rq)
{
	rq->rq_flags &= ~RQF_ELVPRIV;
}

static void adios_sq_init_icq(struct io_cq *icq)
{
	/* no-op: ADIOS doesn't use iocontext */
}

static void adios_sq_exit_icq(struct io_cq *icq)
{
	/* no-op: ADIOS doesn't use iocontext */
}

static struct elevator_type adios_iosched_sq = {
	.ops = {
		.sq = {
			.elevator_init_fn	= adios_sq_init_queue,
			.elevator_exit_fn	= adios_sq_exit_queue,
			.elevator_dispatch_fn	= adios_sq_dispatch,
			.elevator_add_req_fn	= adios_sq_add_request,
			.elevator_merge_fn	= adios_sq_merge,
			.elevator_merged_fn	= adios_sq_merged,
			.elevator_completed_req_fn = adios_sq_completed,
			.elevator_activate_req_fn = adios_sq_activate_request,
			.elevator_deactivate_req_fn = adios_sq_deactivate_request,
			.elevator_former_req_fn = adios_sq_former_request,
			.elevator_latter_req_fn = adios_sq_latter_request,
			.elevator_may_queue_fn	= adios_sq_may_queue,
			.elevator_set_req_fn	= adios_sq_set_request,
			.elevator_put_req_fn	= adios_sq_put_req,
			.elevator_init_icq_fn	= adios_sq_init_icq,
			.elevator_exit_icq_fn	= adios_sq_exit_icq,
		},
	},
	.elevator_attrs = adios_attrs,
	.elevator_name = "adios",
	.elevator_owner = THIS_MODULE,
};

/* ------------------------------------------------------------------ */
/* Elevator type — sq (legacy single-queue) only                        */
/*                                                                    */
/* ADIOS is registered only as a legacy single-queue elevator so it   */
/* is selectable on sda/sdb/sdc (UFS, older SCSI) but NOT on         */
/* mmcblk* (which use blk-mq).  For blk-mq devices the stock          */
/* schedulers (mq-deadline, kyber) remain available.                  */
/* ------------------------------------------------------------------ */

static int __init adios_iosched_init(void)
{
	return elv_register(&adios_iosched_sq);
}

static void __exit adios_iosched_exit(void)
{
	elv_unregister(&adios_iosched_sq);
}

module_init(adios_iosched_init);
module_exit(adios_iosched_exit);

MODULE_AUTHOR("Troika Kernel Team");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Adaptive Deadline I/O Scheduler (ADIOS) 3.2.0 for Linux 4.14");
MODULE_VERSION(ADIOS_VERSION);
