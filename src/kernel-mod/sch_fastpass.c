/*
 * net/sched/sch_fastpass.c FastPass client
 *
 *  Copyright (C) 2013 Jonathan Perry <yonch@yonch.com>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/prefetch.h>
#include <linux/time.h>
#include <linux/bitops.h>
#include <linux/version.h>
#include <linux/ip.h>
#include <linux/inet.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/sch_generic.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(3,2,45)
#include "compat-3_2.h"
#endif

#include "sch_fastpass.h"
#include "sch_timeslot.h"
#include "fastpass_proto.h"
#include "../protocol/flags.h"
#include "../protocol/platform.h"
#include "../protocol/pacer.h"
#include "../protocol/window.h"
#include "../protocol/topology.h"
#include "../protocol/stat_print.h"

/*
 * FastPass client qdisc
 *
 * Invariants:
 *  - If a flow has unreq_tslots > 0, then it is linked to q->unreq_flows,
 *      otherwise flow->next is NULL.
 *
 *  	An exception is if flow->next == &do_not_schedule (which is used for
 *  	q->internal), then it is not linked to q->unreq_flows.
 *
 */

#define FASTPASS_HORIZON					64
#define FASTPASS_REQUEST_WINDOW_SIZE 		(1 << 13)

#define FASTPASS_CTRL_SOCK_WMEM				(64*1024*1024)

#define PROC_FILENAME_MAX_SIZE				64

enum {
	FLOW_UNQUEUED,
	FLOW_REQUEST_QUEUE,
};

/* module parameters */
static u32 req_cost = (2 << 20);
module_param(req_cost, uint, 0444);
MODULE_PARM_DESC(req_cost, "Cost of sending a request in ns, for request pacing");
EXPORT_SYMBOL_GPL(req_cost);

static u32 req_bucketlen = 4 * (2 << 20); /* 4 * req_cost */
module_param(req_bucketlen, uint, 0444);
MODULE_PARM_DESC(req_bucketlen, "Max bucket size in ns, for request pacing");
EXPORT_SYMBOL_GPL(req_bucketlen);

static u32 req_min_gap = 1000;
module_param(req_min_gap, uint, 0444);
MODULE_PARM_DESC(req_min_gap, "ns to wait from when data arrives to sending request");
EXPORT_SYMBOL_GPL(req_min_gap);

static char *ctrl_addr = "10.1.1.2";
module_param(ctrl_addr, charp, 0444);
MODULE_PARM_DESC(ctrl_addr, "IPv4 address of the controller");
EXPORT_SYMBOL_GPL(ctrl_addr);
static __be32 ctrl_addr_netorder;

static u32 reset_window_us = 2e6; /* 2 seconds */
module_param(reset_window_us, uint, 0444);
MODULE_PARM_DESC(reset_window_us, "the maximum time discrepancy (in us) to consider a reset valid");
EXPORT_SYMBOL_GPL(reset_window_us);

static u32 retrans_timeout_ns = 200000;
module_param(retrans_timeout_ns, uint, 0444);
MODULE_PARM_DESC(retrans_timeout_ns, "how long to wait for an ACK before retransmitting request");
EXPORT_SYMBOL_GPL(retrans_timeout_ns);

static u32 update_timer_ns = 2048;
module_param(update_timer_ns, uint, 0444);
MODULE_PARM_DESC(update_timer_ns, "how often to perform periodic tasks");
EXPORT_SYMBOL_GPL(update_timer_ns);

static bool proc_dump_dst = true;
module_param(proc_dump_dst, bool, 0444);
MODULE_PARM_DESC(proc_dump_dst, "should the proc file contain a dump of per-dst status");
EXPORT_SYMBOL_GPL(proc_dump_dst);

static u32 miss_threshold = 16;
module_param(miss_threshold, uint, 0444);
MODULE_PARM_DESC(miss_threshold, "how far in the past can the allocation be and still be accepted");
EXPORT_SYMBOL_GPL(miss_threshold);

static u32 max_preload = 64;
module_param(max_preload, uint, 0444);
MODULE_PARM_DESC(max_preload, "how futuristic can an allocation be and still be accepted");
EXPORT_SYMBOL_GPL(max_preload);

/*
 * Per flow structure, dynamically allocated
 */
struct fp_dst {
	u64		demand_tslots;		/* total needed timeslots */
	u64		requested_tslots;	/* highest requested timeslots */
	u64		acked_tslots;		/* highest requested timeslots that was acked*/
	u64		alloc_tslots;		/* total received allocations */
	u64		used_tslots;		/* timeslots in which packets moved */
	spinlock_t lock;
	uint8_t state;
};

/**
 *
 */
struct fp_sched_data {
	/* configuration paramters */
	u32		tslot_mul;					/* mul to calculate timeslot from nsec */
	u32		tslot_shift;				/* shift to calculate timeslot from nsec */

	/* state */
	u16 unreq_flows[MAX_FLOWS]; 		/* flows with unscheduled packets */
	u32 unreq_dsts_head;
	u32 unreq_dsts_tail;
	spinlock_t 				unreq_flows_lock;

	struct fp_dst dsts[MAX_FLOWS];

	struct tasklet_struct	maintenance_tasklet;
	struct hrtimer			maintenance_timer;
	struct tasklet_struct	retrans_tasklet;
	struct hrtimer			retrans_timer;

	spinlock_t 				pacer_lock;
	struct fp_pacer request_pacer;
	struct socket	*ctrl_sock;			/* socket to the controller */

	bool					is_destroyed;
	struct fpproto_conn		conn;
	spinlock_t 				conn_lock;

	struct proc_dir_entry *proc_entry;

	/* counters */
	atomic_t demand_tslots;		/* total needed timeslots */
	u64		requested_tslots;	/* highest requested timeslots */
	atomic_t alloc_tslots;		/* total received allocations */
	u64		acked_tslots;		/* total acknowledged requests */
	u64		used_tslots;

	/* statistics */
	struct fp_sched_stat stat;
};

static struct tsq_qdisc_entry *fastpass_tsq_entry;
static struct proc_dir_entry *fastpass_proc_entry;

static inline struct fp_dst *get_dst(struct fp_sched_data *q, u32 index) {
	struct fp_dst *ret = &q->dsts[index];
	fp_debug("get dst %u\n", index);
	spin_lock(&ret->lock);
	return ret;
}

static inline void release_dst(struct fp_sched_data *q, struct fp_dst *dst) {
	fp_debug("put dst %u\n", (u32)(dst - &q->dsts[0]));
	spin_unlock(&dst->lock);
}

/**
 *  computes the time when next request should go out
 *  @returns true if the timer was set, false if it was already set
 */
static inline bool trigger_tx(struct fp_sched_data* q)
{
	/* rate limit sending of requests */
	bool res;
	spin_lock_irq(&q->pacer_lock);
	res = pacer_trigger(&q->request_pacer, fp_monotonic_time_ns());
	spin_unlock_irq(&q->pacer_lock);
	return res;
}

void trigger_tx_voidp(void *param)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	trigger_tx(q);
}

static int cancel_retrans_timer(void *param)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	hrtimer_try_to_cancel(&q->retrans_timer);
	return 0;
}

static void set_retrans_timer(void *param, u64 when)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	hrtimer_start(&q->retrans_timer, ns_to_ktime(when), HRTIMER_MODE_ABS);
}

/**
 * Enqueues flow to the request queue, if it's not already in the retransmit
 *    queue
 * Assumes dst_id is already locked by caller.
 */
static void unreq_dsts_enqueue_if_not_queued(struct fp_sched_data *q, u32 dst_id,
		struct fp_dst *dst)
{
	if (dst->state != FLOW_UNQUEUED) {
		return;
	}

	/* enqueue */
	spin_lock(&q->unreq_flows_lock);
	q->unreq_flows[q->unreq_dsts_tail++ % MAX_FLOWS] = dst_id;
	spin_unlock(&q->unreq_flows_lock);
	dst->state = FLOW_REQUEST_QUEUE;

	/* update request timer if necessary */
	if (trigger_tx(q))
		fp_debug("set request timer to %llu\n", pacer_next_event(&q->request_pacer));
}

/* returns NULL if the dst queue is empty */
static struct fp_dst *unreq_dsts_dequeue_and_get(struct fp_sched_data* q, u32 *dst_id)
{
	struct fp_dst *res;

	/* get entry and remove from queue */
	spin_lock(&q->unreq_flows_lock);
	if (unlikely(q->unreq_dsts_head == q->unreq_dsts_tail)) {
		spin_unlock(&q->unreq_flows_lock);
		return NULL;
	}
	*dst_id = q->unreq_flows[q->unreq_dsts_head++ % MAX_FLOWS];
	spin_unlock(&q->unreq_flows_lock);
	res = get_dst(q, *dst_id);
	res->state = FLOW_UNQUEUED;

	return res;
}

static inline u32 n_unreq_dsts(struct fp_sched_data *q)
{
	return q->unreq_dsts_tail - q->unreq_dsts_head;
}

void flow_inc_used(struct fp_sched_data *q, struct fp_dst* dst, u64 amount) {
	dst->used_tslots += amount;
	q->used_tslots += amount;
}

/**
 * Increase the number of unrequested packets for the flow.
 *   Maintains the necessary invariants, e.g. adds the flow to the unreq_flows
 *   list if necessary
 */
static void flow_inc_demand(struct fp_sched_data *q, u32 dst_id,
		struct fp_dst *dst, u64 amount)
{
	dst->demand_tslots += amount;

	/* if flow not on scheduling queue yet, enqueue */
	unreq_dsts_enqueue_if_not_queued(q, dst_id, dst);

	atomic_add(amount, &q->demand_tslots);
}

/**
 * Performs a reset of all flows
 */
static void handle_reset(void *param)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;

	struct fp_dst *dst;
	u32 idx;
	u32 dst_id;
	u32 mask = MAX_FLOWS - 1;
	u32 base_idx = jhash_1word((__be32)fp_monotonic_time_ns(), 0) & mask;

	BUILD_BUG_ON_MSG(MAX_FLOWS & (MAX_FLOWS - 1), "MAX_FLOWS needs to be a power of 2");

	atomic_set(&q->demand_tslots, 0);
	q->requested_tslots = 0;	/* will remain 0 when we're done */
	atomic_set(&q->alloc_tslots, 0);		/* will remain 0 when we're done */
	q->acked_tslots = 0; 		/* will remain 0 when we're done */
	q->used_tslots = 0; 		/* will remain 0 when we're done */

	/* for each cell in hash table: */
	for (idx = 0; idx < MAX_FLOWS; idx++) {
		/* we start from a pseudo-random index 'base_idx' to have less
		 * discrimination towards the lower idx in unreq_dsts_enqueue, however
		 * this is not a perfectly fair scheme */
		dst_id = (idx + base_idx) & mask;
		dst = get_dst(q, dst_id);

		/* if flow was empty anyway, nothing more to do */
		if (likely(dst->demand_tslots == dst->used_tslots))
			goto release;

		/* has timeslots pending, rebase counters to 0 */
		dst->demand_tslots -= dst->used_tslots;
		dst->alloc_tslots = 0;
		dst->acked_tslots = 0;
		dst->requested_tslots = 0;
		dst->used_tslots = 0;

		atomic_add(dst->demand_tslots, &q->demand_tslots);

		fp_debug("rebased flow 0x%04X, new demand %llu timeslots\n",
				dst_id, dst->demand_tslots);

		/* add flow to request queue if it's not already there */
		unreq_dsts_enqueue_if_not_queued(q, dst_id, dst);
release:
		release_dst(q, dst);
	}
}

/**
 * Transmit or drop a single alloc to @dst_id with index @id, according to
 * @flags and the algorithm used (emulation, etc.).
 */
static void inline handle_single_alloc(struct fp_sched_data *q, u16 dst_id,
		u8 flags, u16 id)
{
#if (defined(EMULATION_ALGO))
	if (unlikely(flags == EMU_FLAGS_DROP)) {
		tsq_handle_now(q, dst_id, TSLOT_ACTION_DROP_BY_ID, id);
		q->stat.dropped_timeslots++;
	} else if (flags == EMU_FLAGS_NONE) {
		tsq_handle_now(q, dst_id, TSLOT_ACTION_ADMIT_BY_ID, id);
		q->stat.admitted_timeslots++;
	} else {
		/* unrecognized action, don't take any action */
		q->stat.unrecognized_action++;
	}
#else
	tsq_handle_now(q, dst_id, TSLOT_ACTION_ADMIT_HEAD, 0 /* ignored */);
	q->stat.admitted_timeslots++;
#endif
}

/**
 * Handles an ALLOC payload
 */
static void handle_alloc(void *param, u32 base_tslot, u16 *dst_ids,
		int n_dst, u8 *tslots, int n_tslots)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	int i;
	u8 spec;
	int dst_id_idx;
	u16 dst_id;
	u8 flags;
	u64 full_tslot;
	u64 now_real = fp_get_time_ns();
	u64 current_timeslot;
	u16 id = 0;

#if defined(EMULATION_ALGO)
	u16 *ids = (u16 *) (tslots + n_tslots);
#endif

	/* every alloc should be ACKed */
	trigger_tx(q);

	/* find full timeslot value of the ALLOC */
	current_timeslot = (now_real * q->tslot_mul) >> q->tslot_shift;

	full_tslot = current_timeslot - (1ULL << 18); /* 1/4 back, 3/4 front */
	full_tslot += ((u32)base_tslot - (u32)full_tslot) & 0xFFFFF; /* 20 bits */

	fp_debug("got ALLOC for timeslot %d (full %llu, current %llu), %d destinations, %d timeslots\n",
			base_tslot, full_tslot, current_timeslot, n_dst, n_tslots);

	/* these checks discard too many packets. perhaps they require that the switch
	 prioritize time sync traffic. */
	/* is packet of allocs too far in the past? */
	/*if (unlikely(time_before64(full_tslot, current_timeslot - miss_threshold))) {
		q->stat.alloc_too_late++;
		fp_debug("-X- already gone, dropping\n");
		return;
		}*/

	/* is packet of allocs too far in the future? */
	/*if (unlikely(time_after64(full_tslot, current_timeslot + max_preload))) {
		q->stat.alloc_premature++;
		fp_debug("-X- too futuristic, dropping\n");
		return;
		}*/

	for (i = 0; i < n_tslots; i++) {
		struct fp_dst *dst;

		/* upper 4 bits of specification encode the index of the dst,
		 * lower 4 bits encode flags */
		spec = tslots[i];
		dst_id_idx = spec >> 4;

		if (dst_id_idx == 0) {
			/* Skip instruction */
			fp_debug("ALLOC skip (no allocation)\n");
			continue;
		}

		if (dst_id_idx > n_dst) {
			/* destination index out of bounds */
			FASTPASS_CRIT("ALLOC tslot spec 0x%02X has illegal dst index %d (max %d)\n",
					spec, dst_id_idx, n_dst);
			return;
		}

		fp_debug("Timeslot %d (full %llu) to destination 0x%04x (%d)\n",
				base_tslot, full_tslot, dst_ids[dst_id_idx - 1], dst_ids[dst_id_idx - 1]);

		dst_id = dst_ids[dst_id_idx - 1];
		flags = spec & FLAGS_MASK;
#if defined(EMULATION_ALGO)
		id = ntohs(ids[i]);
		fp_debug("admitting timeslot to dst %d with flags %x and id %d\n",
				dst_id, flags, id);
#else
		fp_debug("admitting timeslot to dst %d with flags %x\n", dst_id,
				flags);
#endif

		dst = get_dst(q, dst_id);
		/* okay, allocate */
		if (dst->used_tslots != dst->demand_tslots) {

			flow_inc_used(q, dst, 1);
			dst->alloc_tslots++;
			release_dst(q, dst);

			handle_single_alloc(q, dst_id, flags, id);

			atomic_inc(&q->alloc_tslots);
			if (full_tslot > current_timeslot) {
				q->stat.early_enqueue++;
			} else {
				u64 tslot = current_timeslot;
				if (unlikely(full_tslot < tslot - (miss_threshold >> 1))) {
					if (unlikely(full_tslot < tslot - 3*(miss_threshold >> 2)))
						q->stat.late_enqueue4++;
					else
						q->stat.late_enqueue3++;
				} else {
					if (unlikely(full_tslot < tslot - (miss_threshold >> 2)))
						q->stat.late_enqueue2++;
					else
						q->stat.late_enqueue1++;
				}
			}

		} else {
			release_dst(q, dst);
			q->stat.unwanted_alloc++;
			fp_debug("got an allocation over demand, flow 0x%04X, demand %llu\n",
					dst_id, dst->demand_tslots);
		}
	}
}

static void handle_areq(void *param, u16 *dst_and_count, int n)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	struct fp_dst *dst;
	int i;
	u16 dst_id;
	u16 count_low;
	u64 count;

	trigger_tx(q);

	for (i = 0; i < n; i++) {
		dst_id = ntohs(dst_and_count[2*i]);
		count_low = ntohs(dst_and_count[2*i + 1]);

		dst = get_dst(q, dst_id);

		/* get full count */
		/* TODO: This is not perfectly safe. For example, if there is a big
		 * outage and the controller thinks it had produced many timeslots, this
		 * can go out of sync */
		count = dst->alloc_tslots - (1 << 15);
		count += (u16)(count_low - count);

		/* update counts */
		if ((s64)(count - dst->alloc_tslots) > 0) {
			u64 n_lost = count - dst->alloc_tslots;

			if (unlikely((s64)(count - dst->requested_tslots) > 0)) {
				release_dst(q, dst);
				FASTPASS_WARN("got an alloc report for dst %d larger than requested (%llu > %llu), will reset\n",
						dst_id, count, dst->requested_tslots);
				q->stat.alloc_report_larger_than_requested++;
				/* This corrupts the status; will force a reset */
				spin_lock(&q->conn_lock);
				fpproto_force_reset(&q->conn);
				spin_unlock(&q->conn_lock);
				handle_reset((void *)q); /* manually call callback since fpproto won't call it */
				return;
			}

			fp_debug("controller allocated %llu our allocated %llu, will increase demand by %llu\n",
					count, dst->alloc_tslots, n_lost);

			dst->alloc_tslots += n_lost;
			flow_inc_used(q, dst, n_lost);
			flow_inc_demand(q, dst_id, dst, n_lost);

			atomic_add(n_lost, &q->alloc_tslots);
			q->stat.timeslots_assumed_lost += n_lost;
		}

		release_dst(q, dst);
	}
}

static void handle_ack(void *param, struct fpproto_pktdesc *pd)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	int i;
	u64 new_acked;
	u64 delta;

	for (i = 0; i < pd->n_areq; i++) {
		u32 dst_id = pd->areq[i].src_dst_key;
		/* this node made pd, so no need to check bounds */
		struct fp_dst *dst = get_dst(q, dst_id);

		new_acked = pd->areq[i].tslots;
		if (dst->acked_tslots < new_acked) {
			FASTPASS_BUG_ON(new_acked > dst->demand_tslots);
			delta = new_acked - dst->acked_tslots;
			q->acked_tslots += delta;
			dst->acked_tslots = new_acked;
			fp_debug("acked request of %llu additional slots, flow 0x%04X, total %llu slots\n",
					delta, dst_id, new_acked);

			/* the demand-limiting window might be in effect, re-enqueue flow */
			if (unlikely(dst->requested_tslots != dst->demand_tslots))
				unreq_dsts_enqueue_if_not_queued(q, dst_id, dst);
		}

		release_dst(q, dst);
	}
}

static void handle_neg_ack(void *param, struct fpproto_pktdesc *pd)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	int i;
	u64 req_tslots;

	for (i = 0; i < pd->n_areq; i++) {
		u32 dst_id = pd->areq[i].src_dst_key;
		/* this node made pd, so no need to check bounds */
		struct fp_dst *dst = get_dst(q, dst_id);

		req_tslots = pd->areq[i].tslots;
		/* don't need to resend if got ack >= req_tslots */
		if (req_tslots <= dst->acked_tslots) {
			fp_debug("nack for request of %llu for flow 0x%04X, but already acked %llu\n",
							req_tslots, dst_id, dst->acked_tslots);
			goto release;
		}

		/* add to retransmit queue */
		unreq_dsts_enqueue_if_not_queued(q, dst_id, dst);

		fp_debug("nack for request of %llu for flow 0x%04X (%llu acked), added to retransmit queue\n",
						req_tslots, dst_id, dst->acked_tslots);
release:
		release_dst(q, dst);
	}
}

/**
 * Send a request packet to the controller
 */
static void send_request(struct fp_sched_data *q)
{
	u64 now_monotonic = fp_monotonic_time_ns();

	struct fp_kernel_pktdesc *kern_pd;
	struct fpproto_pktdesc *pd;
	u64 new_requested;

	fp_debug("start: unreq_flows=%u, unreq_tslots=%llu, now_mono=%llu, scheduled=%llu, diff=%lld, next_seq=%08llX\n",
			n_unreq_dsts(q), atomic_read(&q->demand_tslots) - q->requested_tslots, now_monotonic,
			pacer_next_event(&q->request_pacer),
			(s64 )now_monotonic - (s64 )pacer_next_event(&q->request_pacer),
			q->conn.next_seqno);
	FASTPASS_BUG_ON(!q->ctrl_sock);

	/* allocate packet descriptor */
	kern_pd = fpproto_pktdesc_alloc();
	if (!kern_pd)
		goto alloc_err;
	pd = &kern_pd->pktdesc;

	pd->n_areq = 0;

	spin_lock_irq(&q->conn_lock);
	if (unlikely(q->is_destroyed == true))
		goto out_conn_destroyed;
	/* nack the tail of the outwnd if it has not been nacked or acked */
	fpproto_prepare_to_send(&q->conn);
	spin_unlock_irq(&q->conn_lock);

	while (pd->n_areq < FASTPASS_PKT_MAX_AREQ) {
		/* get entry */
		u32 dst_id;
		struct fp_dst *dst = unreq_dsts_dequeue_and_get(q, &dst_id);
		if (dst == NULL)
			break;

		new_requested = min_t(u64, dst->demand_tslots,
				dst->acked_tslots + FASTPASS_REQUEST_WINDOW_SIZE - 1);
		if(new_requested <= dst->acked_tslots) {
			q->stat.queued_flow_already_acked++;
			fp_debug("flow 0x%04X was in queue, but already fully acked\n",
					dst_id);
			release_dst(q, dst);
			continue;
		}

		q->requested_tslots += (new_requested - dst->requested_tslots);
		dst->requested_tslots = new_requested;
		release_dst(q, dst);

		pd->areq[pd->n_areq].src_dst_key = dst_id;
		pd->areq[pd->n_areq].tslots = new_requested;

		pd->n_areq++;
	}

	if(pd->n_areq == 0) {
		q->stat.request_with_empty_flowqueue++;
		fp_debug("was called with no flows pending (could be due to bad packets?)\n");
	}
	fp_debug("end: unreq_flows=%u, unreq_tslots=%llu\n",
			n_unreq_dsts(q), atomic_read(&q->demand_tslots) - q->requested_tslots);

	spin_lock_irq(&q->conn_lock);
	if (unlikely(q->is_destroyed == true))
		goto out_conn_destroyed;
	fpproto_commit_packet(&q->conn, pd, now_monotonic);
	spin_unlock_irq(&q->conn_lock);

	/* let fpproto send the pktdesc */
	fpproto_send_pktdesc(q->ctrl_sock->sk, kern_pd);

	/* set timer for next request, if a request would be required */
	if (q->requested_tslots != atomic_read(&q->demand_tslots)) {
	  /* have more requests to send */
	  trigger_tx(q);
	}

	return;

out_conn_destroyed:
	free_kernel_pktdesc_no_refcount(kern_pd);
	spin_unlock_irq(&q->conn_lock);
	return;

alloc_err:
	q->stat.req_alloc_errors++;
	fp_debug("request allocation failed\n");
	trigger_tx(q); /* try again */
}


static enum hrtimer_restart maintenance_timer_func(struct hrtimer *timer)
{
	struct fp_sched_data *q =
			container_of(timer, struct fp_sched_data, maintenance_timer);

	/* schedule tasklet to write request */
	tasklet_schedule(&q->maintenance_tasklet);

	hrtimer_forward_now(timer, ns_to_ktime(update_timer_ns));
	return HRTIMER_RESTART;
}

static void maintenance_tasklet_func(unsigned long int param)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	u64 now_monotonic = fp_monotonic_time_ns();
	bool should_send = false;
//	u64 now_real;


	/* move skbs from flow queues to the prequeue */
//	now_real = fp_get_time_ns();
//	fpproto_maintenance_lock(q->ctrl_sock->sk);
//	update_current_timeslot(sch, now_real);
//	fpproto_maintenance_unlock(q->ctrl_sock->sk);

	/* now is also a good opportunity to send a request, if allowed */
	spin_lock_irq(&q->pacer_lock);
	if (pacer_is_triggered(&q->request_pacer) &&
		time_after_eq64(now_monotonic, pacer_next_event(&q->request_pacer))) {
		/* update request credits */
		pacer_reset(&q->request_pacer);
		/* mark that we need to send */
		should_send = true;
	}
	spin_unlock_irq(&q->pacer_lock);

	if (should_send)
		send_request(q);
}

static void retrans_tasklet_func(unsigned long int param)
{
	struct fp_sched_data *q = (struct fp_sched_data *)param;
	u64 now_monotonic = fp_monotonic_time_ns();

	spin_lock_irq(&q->conn_lock);
	if (likely(q->is_destroyed == false))
		fpproto_handle_timeout(&q->conn, now_monotonic);
	spin_unlock_irq(&q->conn_lock);
}

static enum hrtimer_restart retrans_timer_func(struct hrtimer *timer)
{
	struct fp_sched_data *q =
			container_of(timer, struct fp_sched_data, retrans_timer);
	tasklet_schedule(&q->retrans_tasklet);
	return HRTIMER_NORESTART;
}

void ctrl_rcv_handler(void *priv, u8 *pkt, u32 len, __be32 saddr, __be32 daddr)
{
	struct fp_sched_data *q = (struct fp_sched_data *)priv;
	bool ret = false;
	u64 in_seq;

	spin_lock_irq(&q->conn_lock);
	if (likely(q->is_destroyed == false))
		ret = fpproto_handle_rx_packet(&q->conn, pkt, len, saddr, daddr, &in_seq);
	spin_unlock_irq(&q->conn_lock);
	if (!ret)
		return;

	ret = fpproto_perform_rx_callbacks(&q->conn, pkt, len);
	if (!ret)
		return;

	spin_lock_irq(&q->conn_lock);
	if (likely(q->is_destroyed == false))
		fpproto_successful_rx(&q->conn, in_seq);
	spin_unlock_irq(&q->conn_lock);
}

struct fpproto_ops fastpass_sch_proto_ops = {
	.handle_reset	= &handle_reset,
	.handle_alloc	= &handle_alloc,
	.handle_ack		= &handle_ack,
	.handle_neg_ack	= &handle_neg_ack,
	.handle_areq	= &handle_areq,
	.trigger_request= &trigger_tx_voidp,
	.set_timer		= &set_retrans_timer,
	.cancel_timer	= &cancel_retrans_timer,
};

/* reconnects the control socket to the controller */
static int connect_ctrl_socket(struct fp_sched_data *q, struct net *qdisc_net)
{
	struct sock *sk;
	int opt;
	int rc;
	struct sockaddr_in sock_addr = {
			.sin_family = AF_INET,
			.sin_port = FASTPASS_DEFAULT_PORT_NETORDER
	};

	FASTPASS_BUG_ON(q->ctrl_sock != NULL);

	/* create socket */
	rc = __sock_create(qdisc_net, AF_INET, SOCK_DGRAM, IPPROTO_FASTPASS,
			&q->ctrl_sock, 1);
	if (rc != 0) {
		FASTPASS_WARN("Error %d creating socket\n", rc);
		q->ctrl_sock = NULL;
		return rc;
	}

	/* we need a larger-than-default wmem, so we don't run out. ask for a lot,
	 * the call will not fail if it's too much */
	opt = FASTPASS_CTRL_SOCK_WMEM;
	rc = kernel_setsockopt(q->ctrl_sock, SOL_SOCKET, SO_SNDBUF, (char *)&opt,
			sizeof(opt));
	if (rc != 0)
		FASTPASS_WARN("Could not set socket wmem size\n");

	sk = q->ctrl_sock->sk;

	FASTPASS_BUG_ON(sk->sk_priority != TC_PRIO_CONTROL);
	FASTPASS_BUG_ON(sk->sk_allocation != GFP_ATOMIC);

	/* give socket a reference to this qdisc for watchdog */
	((struct fastpass_sock *)sk)->rcv_handler = ctrl_rcv_handler;
	fpproto_set_priv(sk, (void *)q);

	/* connect */
	sock_addr.sin_addr.s_addr = ctrl_addr_netorder;
	rc = kernel_connect(q->ctrl_sock, (struct sockaddr *)&sock_addr,
			sizeof(sock_addr), 0);
	if (rc != 0)
		goto err_release;

	return 0;

err_release:
	FASTPASS_WARN("Error %d trying to connect to addr 0x%X (in netorder)\n",
			rc, ctrl_addr_netorder);
	sock_release(q->ctrl_sock);
	q->ctrl_sock = NULL;
	return rc;
}

/*
 * Prints flow status
 */
static void dump_flow_info(struct seq_file *seq, struct fp_sched_data *q,
		bool only_active)
{
	struct fp_dst *dst;
	u32 flow_id;
	u32 num_printed = 0;

	printk(KERN_DEBUG "fastpass flows (only_active=%d):\n", only_active);

	for (flow_id = 0; flow_id < MAX_FLOWS; flow_id++) {
		dst = &q->dsts[flow_id];

		if (dst->demand_tslots == dst->used_tslots && only_active)
			continue;

		num_printed++;
		printk(KERN_DEBUG "flow 0x%04X demand %llu requested %llu acked %llu alloc %llu used %llu state %d\n",
				flow_id, dst->demand_tslots, dst->requested_tslots,
				dst->acked_tslots, dst->alloc_tslots, dst->used_tslots,
				dst->state);
	}

	printk(KERN_DEBUG "fastpass printed %u flows\n", num_printed);
}

static int fastpass_proc_show(struct seq_file *seq, void *v)
{
	struct fp_sched_data *q = (struct fp_sched_data *)seq->private;
	u64 now_real = fp_get_time_ns();
	struct fp_sched_stat *scs = &q->stat;

	/* time */
	seq_printf(seq, "  fp_sched_data *p = %p ", q);
	seq_printf(seq, ", timestamp 0x%llX ", now_real);

	/* configuration */
	seq_printf(seq, "\n  req_cost %u ", req_cost);
	seq_printf(seq, ", req_bucketlen %u", req_bucketlen);
	seq_printf(seq, ", req_min_gap %u", req_min_gap);
	seq_printf(seq, ", ctrl_addr %s", ctrl_addr);
	seq_printf(seq, ", reset_window_us %u", reset_window_us);
	seq_printf(seq, ", retrans_timeout_ns %u", retrans_timeout_ns);
	seq_printf(seq, ", update_timer_ns %u", update_timer_ns);
	seq_printf(seq, ", proc_dump_dst %u", proc_dump_dst);
	seq_printf(seq, ", miss_threshold %u", miss_threshold);
	seq_printf(seq, ", max_preload %u", max_preload);
#if defined(EMULATION_ALGO)
	seq_printf(seq, ", algo emulation");
#elif defined(PIPELINED_ALGO)
	seq_printf(seq, ", algo sequential");
#endif

	/* timeslot statistics */
	seq_printf(seq, " (%llu %llu %llu %llu behind, %llu fast)", scs->late_enqueue4,
			scs->late_enqueue3, scs->late_enqueue2, scs->late_enqueue1,
			scs->early_enqueue);
	seq_printf(seq, ", %llu assumed_lost", scs->timeslots_assumed_lost);
	seq_printf(seq, "  (%llu late", scs->alloc_too_late);
	seq_printf(seq, ", %llu premature)", scs->alloc_premature);

	/* total since reset */
	seq_printf(seq, "\n  since reset: ");
	seq_printf(seq, " demand %u", atomic_read(&q->demand_tslots));
	seq_printf(seq, ", requested %llu", q->requested_tslots);
	seq_printf(seq, " (%u yet unrequested)", (u32)(atomic_read(&q->demand_tslots) - q->requested_tslots));
	seq_printf(seq, ", acked %llu", q->acked_tslots);
	seq_printf(seq, ", allocs %u", atomic_read(&q->alloc_tslots));
	seq_printf(seq, ", used %llu", q->used_tslots);
	seq_printf(seq, ", admitted %llu", scs->admitted_timeslots);
	seq_printf(seq, ", dropped %llu", scs->dropped_timeslots);

	seq_printf(seq, "\n  %llu requests w/no a-req", scs->request_with_empty_flowqueue);

	/* protocol state */
	fpproto_update_internal_stats(&q->conn);
	fpproto_print_stats(&q->conn.stat, seq);
	/* socket state */
	fpproto_print_socket_stats(q->ctrl_sock->sk, seq);

	/* error statistics */
	seq_printf(seq, "\n errors:");
	if (scs->req_alloc_errors)
		seq_printf(seq, "\n  %llu could not allocate pkt_desc for request", scs->req_alloc_errors);
	if (scs->alloc_report_larger_than_requested)
		seq_printf(seq, "\n  %llu alloc report larger than requested_timeslots (causes a reset)",
				scs->alloc_report_larger_than_requested);

	fpproto_print_errors(&q->conn.stat, seq);
	fpproto_print_socket_errors(q->ctrl_sock->sk, seq);

	/* warnings */
	seq_printf(seq, "\n warnings:");
	if (scs->queued_flow_already_acked)
		seq_printf(seq, "\n  %llu acked flows in flowqueue (possible ack just after timeout)",
				scs->queued_flow_already_acked);
	if (scs->unwanted_alloc)
		seq_printf(seq, "\n  %llu timeslots allocated beyond the demand of the flow (could happen due to reset / controller timeouts)",
				scs->unwanted_alloc);
	if (scs->unrecognized_action)
		seq_printf(seq, "\n  %llu timeslots with unrecognized actions (packet encoding error?)",
				scs->unrecognized_action);
	if (scs->alloc_too_late)
		seq_printf(seq, "\n  %llu late allocations (something wrong with time-sync?)",
				scs->alloc_too_late);
	if (scs->alloc_premature)
		seq_printf(seq, "\n  %llu premature allocations (something wrong with time-sync?)\n",
				scs->alloc_premature);

	fpproto_print_warnings(&q->conn.stat, seq);

	/* flow info */
	if (proc_dump_dst)
		dump_flow_info(seq, q, true);

	return 0;
}

static int fastpass_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, fastpass_proc_show, PDE_DATA(inode));
}

static const struct file_operations fastpass_proc_fops = {
	.owner	 = THIS_MODULE,
	.open	 = fastpass_proc_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};

static int fastpass_proc_init(struct fp_sched_data *q)
{
	char fname[PROC_FILENAME_MAX_SIZE];
	snprintf(fname, PROC_FILENAME_MAX_SIZE, "fastpass/stats-%p", q);
	q->proc_entry = proc_create_data(fname, S_IRUGO, NULL, &fastpass_proc_fops, q);
	if (q->proc_entry == NULL)
		return -1; /* error */
	return 0; /* success */
}

static void fastpass_proc_cleanup(struct fp_sched_data *q)
{
	proc_remove(q->proc_entry);
}

static int fpq_new_qdisc(void *priv, struct net *qdisc_net, u32 tslot_mul,
		u32 tslot_shift)
{
	struct fp_sched_data *q = (struct fp_sched_data *)priv;

	u64 now_monotonic = fp_monotonic_time_ns();
	int err;
	int i;

	q->unreq_dsts_head = q->unreq_dsts_tail = 0;
	q->tslot_mul		= tslot_mul;
	q->tslot_shift		= tslot_shift;

	spin_lock_init(&q->unreq_flows_lock);

	for (i = 0; i < MAX_FLOWS; i++)
		spin_lock_init(&q->dsts[i].lock);

	spin_lock_init(&q->pacer_lock);
	pacer_init_full(&q->request_pacer, now_monotonic, req_cost,
			req_bucketlen, req_min_gap);

	err = fastpass_proc_init(q);
	if (err != 0)
		goto out;

	/* initialize the fastpass protocol (before initializing socket) */
	q->is_destroyed = false;
	spin_lock_init(&q->conn_lock);
	fpproto_init_conn(&q->conn, &fastpass_sch_proto_ops, (void *)q,
			(u64)reset_window_us * NSEC_PER_USEC, retrans_timeout_ns);

	/* connect socket */
	q->ctrl_sock		= NULL;
	err = connect_ctrl_socket(q, qdisc_net);
	if (err != 0)
		goto out_destroy_conn;

	tasklet_init(&q->maintenance_tasklet, &maintenance_tasklet_func, (unsigned long int)q);

	hrtimer_init(&q->maintenance_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	q->maintenance_timer.function = maintenance_timer_func;
	hrtimer_start(&q->maintenance_timer, ns_to_ktime(update_timer_ns),
			HRTIMER_MODE_REL);

	/* initialize retransmission timer */
	tasklet_init(&q->retrans_tasklet, &retrans_tasklet_func, (unsigned long int)q);
	hrtimer_init(&q->retrans_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	q->retrans_timer.function = retrans_timer_func;

	return err;

out_destroy_conn:
	fpproto_destroy_conn(&q->conn);
	fastpass_proc_cleanup(q);
out:
	pr_info("%s: error creating new qdisc err=%d\n", __func__, err);
	return err;
}

static void fpq_stop_qdisc(void *priv) {
	struct fp_sched_data *q = (struct fp_sched_data *)priv;

	/**
	 * To get clean destruction we assume at this point that no more calls
	 *    to fpq_add_timeslot are made at this point
	 */

	/* there is no race with others setting the maintenance timer, cancel it */
	hrtimer_cancel(&q->maintenance_timer);
	tasklet_kill(&q->maintenance_tasklet);

	/* send_request is not going to run anymore, can close the control socket */
	fp_debug("closing control socket\n");
	sock_release(q->ctrl_sock);
	q->ctrl_sock = NULL;

	spin_lock(&q->conn_lock);
	q->is_destroyed = true;
	spin_unlock(&q->conn_lock);

	/**
	 * At this point:
	 * 1. control packets stop arriving (ctrl_rcv_handler)
	 * 2. no new control packets are committed (send_request)
	 * 3. no retransmission timers are handled (retrans_tasklet_func)
	 * So none of the fpproto_conn API is called after this point.
	 */

	fpproto_destroy_conn(&q->conn);

	/* no race with other code setting the timer, so we can cancel retrans_timer */
	hrtimer_cancel(&q->retrans_timer);
	tasklet_kill(&q->retrans_tasklet);

	fastpass_proc_cleanup(q);
}

static void fpq_add_timeslot(void *priv, u64 dst_id)
{
	struct fp_sched_data *q = (struct fp_sched_data *)priv;
	struct fp_dst *dst = get_dst(q, dst_id);
	flow_inc_demand(q, dst_id, dst, 1);
	release_dst(q, dst);
}

static struct tsq_ops fastpass_tsq_ops __read_mostly = {
	.id		=	"fastpass",
	.priv_size	=	sizeof(struct fp_sched_data),

	.new_qdisc = fpq_new_qdisc,
	.stop_qdisc = fpq_stop_qdisc,
	.add_timeslot = fpq_add_timeslot,
};

static int __init fastpass_module_init(void)
{
	int ret = -ENOSYS;
	const char *ctrl_addr_end;

	pr_info("%s: initializing\n", __func__);

	ret = in4_pton(ctrl_addr, -1, (u8*)&ctrl_addr_netorder, -1, &ctrl_addr_end);
	if (ret != 1) {
		FASTPASS_CRIT("could not parse controller's IP address (got %s)\n", ctrl_addr);
		goto out;
	}
	pr_info("%s: controller address is %s, parsed as 0x%X (netorder)\n",
			__func__, ctrl_addr, ctrl_addr_netorder);

	fastpass_proc_entry = proc_mkdir("fastpass", NULL);
	if (fastpass_proc_entry == NULL)
		goto out;

	ret = fpproto_register();
	if (ret)
		goto out_remove_proc;

	ret = tsq_init();
	if (ret != 0)
		goto out_unregister_fpproto;

	fastpass_tsq_entry = tsq_register_qdisc(&fastpass_tsq_ops);
	if (fastpass_tsq_entry == NULL)
		goto out_exit;

	pr_info("%s: success\n", __func__);
	return 0;
out_exit:
	tsq_exit();
out_unregister_fpproto:
	fpproto_unregister();
out_remove_proc:
	proc_remove(fastpass_proc_entry);
out:
	pr_info("%s: failed, ret=%d\n", __func__, ret);
	return ret;
}

static void __exit fastpass_module_exit(void)
{
	proc_remove(fastpass_proc_entry);
	tsq_unregister_qdisc(fastpass_tsq_entry);
	tsq_exit();
	fpproto_unregister(); /* TODO: verify this is safe */
}

module_init(fastpass_module_init)
module_exit(fastpass_module_exit)
MODULE_AUTHOR("Jonathan Perry");
MODULE_LICENSE("Dual MIT/GPL");
