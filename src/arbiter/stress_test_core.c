#include "stress_test_core.h"

#include "comm_core.h"
#include <rte_log.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_memcpy.h>
#include <rte_string_fns.h>
#include <rte_errno.h>
#include <rte_mempool.h>
#include <ccan/list/list.h>
#include "control.h"
#include "main.h"
#include "../graph-algo/admissible_structures.h"
#include "../graph-algo/admissible_traffic.h"
#include "../graph-algo/generate_requests.h"
#include "admission_core.h"
#include "../protocol/stat_print.h"
#include "../protocol/topology.h"
#include "comm_log.h"

#define STRESS_TEST_MIN_LOOP_TIME_SEC		2e-6

/* logs */
struct stress_test_log {
	uint64_t processed_tslots;
	uint64_t occupied_node_tslots;
};
struct stress_test_log stress_test_core_logs[RTE_MAX_LCORE];
/* current log */
#define CL		(&stress_test_core_logs[rte_lcore_id()])

static inline void stress_test_log_init(struct stress_test_log *cl)
{
	memset(cl, 0, sizeof(*cl));
}

static inline void stress_test_log_got_admitted_tslot(uint16_t size, uint64_t timeslot) {
	(void)size;(void)timeslot;
	CL->processed_tslots++;
	CL->occupied_node_tslots += size;
}

static void flush_q_head_buffer(struct comm_core_state *core)
{
	uint32_t remaining = core->q_head_buf_len;
	void **edge_p = &core->q_head_write_buffer[0];
	int rc;

	while(remaining > 0) {
		rc = rte_ring_enqueue_burst(g_admissible_status.q_head,
				edge_p, remaining);
		if (unlikely(rc < 0))
			rte_exit(EXIT_FAILURE, "got negative value (%d) from rte_ring_enqueue_burst, should never happen\n", rc);
		remaining -= rc;
		edge_p += rc;
	}

	core->q_head_buf_len = 0;
}

static void add_backlog_buffered(struct comm_core_state *core,
		struct admissible_status *status, uint16_t src, uint16_t dst,
        uint32_t demand_tslots)
{
	void *edge;

	if (add_backlog_no_enqueue(status, src, dst, demand_tslots, &edge) == true) {
		/* need to add to q_head, will do so through buffer */
		core->q_head_write_buffer[core->q_head_buf_len++] = edge;

		if (unlikely(core->q_head_buf_len == Q_HEAD_WRITE_BUFFER_SIZE)) {
			flush_q_head_buffer(core);
		}
	}
}

static inline void process_allocated_traffic(struct comm_core_state *core,
		struct rte_ring *q_admitted)
{
	int rc;
	int i;
	struct admitted_traffic* admitted[MAX_ADMITTED_PER_LOOP];
	uint64_t current_timeslot;

	/* Process newly allocated timeslots */
	rc = rte_ring_dequeue_burst(q_admitted, (void **) &admitted[0],
								MAX_ADMITTED_PER_LOOP);
	if (unlikely(rc < 0)) {
		/* error in dequeuing.. should never happen?? */
		comm_log_dequeue_admitted_failed(rc);
		return;
	}

	for (i = 0; i < rc; i++) {
		current_timeslot = ++core->latest_timeslot;
		comm_log_got_admitted_tslot(admitted[i]->size, current_timeslot);
	}
	/* free memory */
	rte_mempool_put_bulk(admitted_traffic_pool[0], (void **) admitted, rc);
}

/**
 * Adds demands from 'num_srcs' sources, each to 'num_dsts_per_src'.
 *    Demand is for 'flow_size' tslots.
 */
static void add_initial_requests(struct comm_core_state *core,
		uint32_t num_srcs, uint32_t num_dsts_per_src, uint32_t flow_size)
{
	uint32_t src;
	uint32_t i;
	for (src = 0; src < num_srcs; src++)
		for (i = 0; i < num_dsts_per_src; i++)
			add_backlog_buffered(core, &g_admissible_status,
						src, (src + 1 + i) % num_srcs , flow_size);

	flush_q_head_buffer(core);
}

void exec_stress_test_core(struct stress_test_core_cmd * cmd,
		uint64_t first_time_slot)
{
	int i;
	const unsigned lcore_id = rte_lcore_id();
	uint64_t now;
	struct request_generator gen;
	struct request next_request;
	struct comm_core_state *core = &ccore_state[lcore_id];
	uint64_t min_next_iteration_time;
	uint64_t loop_minimum_iteration_time =
			rte_get_timer_hz() * STRESS_TEST_MIN_LOOP_TIME_SEC;
	uint64_t next_rate_increase_time;
	double next_mean_t_btwn_requests;

	core->latest_timeslot = first_time_slot - 1;
	core->q_head_buf_len = 0;
	stress_test_log_init(&stress_test_core_logs[lcore_id]);
	comm_log_init(&comm_core_logs[lcore_id]);

	/* Add initial demands */
	assert(cmd->num_initial_srcs <= cmd->num_nodes);
	assert(cmd->num_initial_dsts_per_src < cmd->num_initial_srcs);
	add_initial_requests(core, cmd->num_initial_srcs,
			cmd->num_initial_dsts_per_src, cmd->initial_flow_size);

	/* Initialize gen */
	next_mean_t_btwn_requests = cmd->mean_t_btwn_requests;

	while (rte_get_timer_cycles() < cmd->start_time);

	/* Generate first request */
	now = rte_get_timer_cycles();
	next_rate_increase_time = now;

	/* MAIN LOOP */
	while (now < cmd->end_time) {
		uint32_t n_processed_requests = 0;

		/* if need to change rate, do it */
		if (now >= next_rate_increase_time) {
			init_request_generator(&gen, next_mean_t_btwn_requests,
					now, cmd->num_nodes);
			get_next_request(&gen, &next_request);

			next_mean_t_btwn_requests /= STRESS_TEST_RATE_INCREASE_FACTOR;
			next_rate_increase_time +=
					rte_get_timer_hz() * STRESS_TEST_RATE_INCREASE_GAP_SEC;
		}

		/* if time to enqueue request, do so now */
		for (i = 0; i < MAX_ENQUEUES_PER_LOOP; i++) {
			if (next_request.time > now)
				break;

			/* enqueue the request */
			add_backlog_buffered(core, &g_admissible_status,
					next_request.src, next_request.dst, cmd->demand_tslots);
			comm_log_demand_increased(next_request.src, next_request.dst, 0,
					cmd->demand_tslots, cmd->demand_tslots);

			n_processed_requests++;

			/* generate the next request */
			get_next_request(&gen, &next_request);
		}

		comm_log_processed_batch(n_processed_requests, now);

		/* Process newly allocated timeslots */
		process_allocated_traffic(core, cmd->q_allocated);

		/* flush q_head's buffer into q_head */
		flush_q_head_buffer(core);

		/* wait until at least loop_minimum_iteration_time has passed from
		 * beginning of loop */
		min_next_iteration_time = now + loop_minimum_iteration_time;
		do {
			now = rte_get_timer_cycles();
		} while (now < min_next_iteration_time);
	}

	/* Dump some stats */
	printf("Stress test finished; %lu processed timeslots, %lu node-tslots\n",
			CL->processed_tslots, CL->occupied_node_tslots);
}