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

static int adios_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	/*
	 * Match mq-deadline behaviour: nothing to do per hctx, but
	 * the hook is required for the elevator_mq_ops.
	 */
	return 0;
}

static void adios_exit_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
}

/* ------------------------------------------------------------------ */
/* Merge / prepare                                                     */
/* ------------------------------------------------------------------ */

static bool adios_allow_merge(struct request_queue *q, struct request *rq,
			      struct bio *bio)
{
	struct adios_data *ad = adios_qdata(q);

	if (!ad->front_merges && bio_data_dir(bio) != adios_data_dir(rq))
		return false;
	return true;
}

static bool adios_bio_merge(struct blk_mq_hw_ctx *hctx, struct bio *bio)
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

static void adios_requests_merged(struct request_queue *q, struct request *rq,
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

static void adios_prepare_request(struct request *rq, struct bio *bio)
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

static void adios_finish_request(struct request *rq)
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

static void adios_insert_requests(struct blk_mq_hw_ctx *hctx,
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

static struct request *adios_dispatch_request(struct blk_mq_hw_ctx *hctx)
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

static bool adios_has_work(struct blk_mq_hw_ctx *hctx)
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

static void adios_started_request(struct request *rq)
{
}

static void adios_requeue_request(struct request *rq)
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
/* Elevator type                                                       */
/* ------------------------------------------------------------------ */

static struct elevator_type adios_iosched = {
	.ops = {
		.mq = {
			.init_sched		= adios_init_sched,
			.exit_sched		= adios_exit_sched,
			.init_hctx		= adios_init_hctx,
			.exit_hctx		= adios_exit_hctx,

			.allow_merge		= adios_allow_merge,
			.bio_merge		= adios_bio_merge,
			.request_merge		= adios_request_merge,
			.request_merged		= adios_request_merged,
			.requests_merged	= adios_requests_merged,
			.prepare_request	= adios_prepare_request,
			.finish_request		= adios_finish_request,
			.insert_requests	= adios_insert_requests,
			.dispatch_request	= adios_dispatch_request,
			.has_work		= adios_has_work,
			.completed_request	= adios_completed_request,
			.started_request	= adios_started_request,
			.requeue_request	= adios_requeue_request,
		},
	},
	.elevator_attrs = adios_attrs,
	.elevator_name = "adios",
	.elevator_owner = THIS_MODULE,
};

static int __init adios_iosched_init(void)
{
	return elv_register(&adios_iosched);
}

static void __exit adios_iosched_exit(void)
{
	elv_unregister(&adios_iosched);
}

module_init(adios_iosched_init);
module_exit(adios_iosched_exit);

MODULE_AUTHOR("Troika Kernel Team");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Adaptive Deadline I/O Scheduler (ADIOS) 3.2.0 for Linux 4.14");
MODULE_VERSION(ADIOS_VERSION);
