/*
 * pim_test.c
 *
 *  Created on: May 13, 2014
 *      Author: aousterh
 */

#include "pim.h"
#include "pim_admissible_traffic.h"

#include <time.h> /* for seeding srand */

#define ADMITTED_TRAFFIC_MEMPOOL_SIZE           (N_PARTITIONS)
#define ADMITTED_OUT_RING_LOG_SIZE		16
#define BIN_MEMPOOL_SIZE                        (10*N_PARTITIONS)
#define NEW_DEMANDS_Q_SIZE                      16
#define READY_PARTITIONS_Q_SIZE                 2

/**
 * Simple test of pim for a few timeslots
 */
int main() {
        /* initialize rand */
        srand(time(NULL));

        /* initialize state */
        struct fp_ring *q_new_demands[N_PARTITIONS];
        struct fp_ring *q_admitted_out;
        struct fp_mempool *bin_mempool;
        struct fp_mempool *admitted_traffic_mempool;
        struct fp_ring *q_ready_partitions[N_PARTITIONS];

        uint16_t i;
        for (i = 0; i < N_PARTITIONS; i++) {
                q_new_demands[i] = fp_ring_create("", 1 << NEW_DEMANDS_Q_SIZE, 0, 0);
                q_ready_partitions[i] = fp_ring_create("", 1 << READY_PARTITIONS_Q_SIZE, 0, 0);
        }
        bin_mempool = fp_mempool_create("",BIN_MEMPOOL_SIZE, bin_num_bytes(SMALL_BIN_SIZE),
        		0, 0, 0);
        q_admitted_out = fp_ring_create("", 1 << ADMITTED_OUT_RING_LOG_SIZE, 0, 0);
        admitted_traffic_mempool = fp_mempool_create("",
        		ADMITTED_TRAFFIC_MEMPOOL_SIZE, sizeof(struct admitted_traffic),
				0, 0, 0);
        struct pim_state *state = pim_create_state(&q_new_demands[0], q_admitted_out,
                                                   bin_mempool,
                                                   admitted_traffic_mempool,
                                                   q_ready_partitions);

        /* add some test edges */
        struct ga_edge test_edges[] = {{1, 3}, {4, 5}, {1, 5}};
        for (i = 0; i < sizeof(test_edges) / sizeof(struct ga_edge); i++) {
                uint16_t src = test_edges[i].src;
                uint16_t dst = test_edges[i].dst;
                pim_add_backlog(state, src, dst, 0x2UL);
        }
        pim_flush_backlog(state);

        uint8_t NUM_TIMESLOTS = 3;
        for (i = 0; i < NUM_TIMESLOTS; i++) {
                pim_get_admissible_traffic(state);

                if (!pim_is_valid_admitted_traffic(state))
                        printf("invalid admitted traffic\n");

                /* return admitted to mempool */
                uint16_t p;
                for (p = 0; p < N_PARTITIONS; p++) {
                        struct admitted_traffic *admitted;
                        fp_ring_dequeue(state->q_admitted_out, (void **) &admitted);
                        fp_mempool_put(state->admitted_traffic_mempool, admitted);
                }
        }
}
