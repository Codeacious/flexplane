/*
 * pim_admission_core.c
 *
 *  Created on: May 18, 2014
 *      Author: aousterh
 */

#include "pim_admission_core.h"

#include <rte_errno.h>
#include <rte_string_fns.h>

#include "admission_core_common.h"
#include "admission_log.h"
#include "../grant-accept/pim.h"
#include "../graph-algo/algo_config.h"

#define		Q_NEW_DEMANDS_RING_SIZE      (64 * 1024)
#define		Q_READY_PARTITIONS_RING_SIZE (2 * N_PARTITIONS)

struct pim_state g_pim_state;

struct admission_log admission_core_logs[RTE_MAX_LCORE];
struct rte_ring *q_new_demands[N_ADMISSION_CORES];
struct rte_ring *q_ready_partitions[N_ADMISSION_CORES];

void pim_admission_init_global(struct rte_ring *q_admitted_out,
		struct rte_mempool *admitted_traffic_mempool)
{
	int i;
	char s[64];
	struct rte_mempool *bin_mempool;

	/* allocate bin_mempool */
	uint32_t pool_index = 0;
	uint32_t socketid = 0;
	snprintf(s, sizeof(s), "bin_pool_%d", pool_index);
	bin_mempool =
		rte_mempool_create(s,
			BIN_MEMPOOL_SIZE, /* num elements */
			bin_num_bytes(SMALL_BIN_SIZE), /* element size */
			BIN_MEMPOOL_CACHE_SIZE, /* cache size */
			0, NULL, NULL, NULL, NULL, /* custom initialization, disabled */
			socketid, 0);
	if (bin_mempool == NULL)
		rte_exit(EXIT_FAILURE,
				"Cannot init bin mempool on socket %d: %s\n", socketid,
				rte_strerror(rte_errno));
	else
		RTE_LOG(INFO, ADMISSION, "Allocated bin mempool on socket %d - %lu bufs\n",
				socketid, (uint64_t)BIN_MEMPOOL_SIZE);

	/* init log */
	for (i = 0; i < RTE_MAX_LCORE; i++)
		admission_log_init(&admission_core_logs[i]);

	/* init q_new_demands */
	for (i = 0; i < N_ADMISSION_CORES; i++) {
		snprintf(s, sizeof(s), "q_new_demands_%d", i);
		q_new_demands[i] = rte_ring_create(s, Q_NEW_DEMANDS_RING_SIZE, 0,
                                                   RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (q_new_demands[i] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot init q_new_demands[%d]: %s\n",
                                 i, rte_strerror(rte_errno));
	}

	/* init q_ready_partitions */
	for (i = 0; i < N_ADMISSION_CORES; i++) {
		snprintf(s, sizeof(s), "q_ready_partitions_%d", i);
		q_ready_partitions[i] = rte_ring_create(s, Q_READY_PARTITIONS_RING_SIZE,
							0, RING_F_SC_DEQ);
		if (q_ready_partitions[i] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot init q_ready_partitions[%d]: %s\n",
                                 i, rte_strerror(rte_errno));
	}

	/* init pim_state */
	pim_init_state(&g_pim_state, q_new_demands, q_admitted_out,
                       bin_mempool, admitted_traffic_mempool, q_ready_partitions);
}

void pim_admission_init_core(uint16_t lcore_id)
{
        /* TODO: is this function necessary? */
	int pool_index;
	int socketid;
	char s[64];

	socketid = rte_lcore_to_socket_id(lcore_id);
}

int exec_pim_admission_core(void *void_cmd_p)
{
	struct admission_core_cmd *cmd = (struct admission_core_cmd *)void_cmd_p;
	uint32_t core_ind = cmd->admission_core_index;
	uint64_t logical_timeslot = cmd->start_timeslot;
	uint64_t start_time_first_timeslot;

	ADMISSION_DEBUG("core %d admission %d starting allocations\n",
			rte_lcore_id(), core_ind);

	/* do allocation loop */
	while (1) {
		/* TODO: skip timeslots */
		
		/* perform allocation */
		admission_log_allocation_begin(logical_timeslot,
				start_time_first_timeslot);

                /* reset per-timeslot state */
                pim_prepare(&g_pim_state, core_ind);

                /* run multiple iterations of pim and print out accepted edges */
                pim_do_grant_first_it(&g_pim_state, core_ind);
                pim_do_accept(&g_pim_state, core_ind);
                pim_process_accepts(&g_pim_state, core_ind);
                uint8_t i;
                for (i = 1; i < NUM_ITERATIONS; i++) {
                        pim_do_grant(&g_pim_state, core_ind);
                        pim_do_accept(&g_pim_state, core_ind);
                        pim_process_accepts(&g_pim_state, core_ind);
                }

                pim_complete_timeslot(&g_pim_state, core_ind);

		admission_log_allocation_end(logical_timeslot);

		logical_timeslot += 1;
	}

	return 0;
}
