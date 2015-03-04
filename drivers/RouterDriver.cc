/*
 * RouterDriver.cc
 *
 *  Created on: Dec 21, 2014
 *      Author: yonch
 */

#include "RouterDriver.h"
#include <assert.h>
#include <stdint.h>
#include <time.h> /* for seeding the random number generator */
#include "../config.h"
#include "../emulation.h"
#include "../router.h"
#include "../packet_impl.h"
#include "../graph-algo/fp_ring.h"
#include "../graph-algo/platform.h"
#include "../graph-algo/random.h"

#define ROUTER_MAX_BURST	(EMU_ENDPOINTS_PER_RACK)


RouterDriver::RouterDriver(Router *router, struct fp_ring *q_to_router,
		struct fp_ring **q_from_router, uint64_t *masks, uint16_t n_neighbors,
		struct fp_mempool *packet_mempool)
	: m_router(router),
	  m_q_to_router(q_to_router),
	  m_neighbors(n_neighbors),
	  m_cur_time(0),
	  m_packet_mempool(packet_mempool)
{
	uint16_t i;

	seed_random(&m_random, time(NULL));
	for (i = 0; i < n_neighbors; i++) {
		m_q_from_router[i] = q_from_router[i];
		m_port_masks[i] = masks[i];
	}
}

void RouterDriver::assign_to_core(Dropper *dropper,
		struct emu_admission_core_statistics *stat, uint16_t core_index) {
	m_stat = stat;
	m_router->assign_to_core(dropper, stat);
	m_core_index = core_index;
}

struct queue_bank_stats *RouterDriver::get_queue_bank_stats() {
	return m_router->get_queue_bank_stats();
}

struct port_drop_stats *RouterDriver::get_port_drop_stats() {
	return m_router->get_port_drop_stats();
}

void RouterDriver::cleanup() {
	free_packet_ring(m_q_to_router, m_packet_mempool);

	delete m_router;
}

/**
 * Emulate a timeslot at a single router
 */
void RouterDriver::step() {
	uint32_t i, j, n_pkts;
	struct emu_packet *pkt_ptrs[ROUTER_MAX_BURST];
	assert(ROUTER_MAX_BURST >= EMU_ENDPOINTS_PER_RACK);

	/* fetch packets to send from router to endpoints */
	for (j = 0; j < m_neighbors; j++) {
#ifdef EMU_NO_BATCH_CALLS
		n_pkts = 0;
		for (uint32_t i = 0; i < ROUTER_MAX_BURST; i++) {
			pkt_ptrs[n_pkts] = m_router->pull(i);

			if (pkt_ptrs[n_pkts] != NULL)
				n_pkts++;
		}
#else
		n_pkts = m_router->pull_batch(pkt_ptrs, ROUTER_MAX_BURST,
				&m_port_masks[j]);
#endif

#ifdef CONFIG_IP_FASTPASS_DEBUG
		for (i = 0; i < n_pkts; i++) {
			assert(pkt_ptrs[i] != NULL);
		}
#endif
		assert(n_pkts <= ROUTER_MAX_BURST);
		/* send packets to endpoint groups */
#ifdef DROP_ON_FAILED_ENQUEUE
		if (n_pkts > 0 && fp_ring_enqueue_bulk(m_q_from_router[j],
				(void **) &pkt_ptrs[0], n_pkts) == -ENOBUFS) {
			/* no space in ring. log but don't retry. */
			adm_log_emu_send_packets_failed(m_stat, n_pkts);
			for (i = 0; i < n_pkts; i++)
				free_packet(pkt_ptrs[i], m_packet_mempool);
		} else {
			adm_log_emu_router_driver_pulled(m_stat, n_pkts);
		}
#else
		while (n_pkts > 0 && fp_ring_enqueue_bulk(m_q_from_router[j],
				(void **) &pkt_ptrs[0], n_pkts) == -ENOBUFS) {
			/* no space in ring. log and retry. */
			adm_log_emu_send_packets_failed(m_stat, n_pkts);
		}
		adm_log_emu_router_sent_packets(m_stat, n_pkts);
		adm_log_emu_router_driver_pulled(m_stat, n_pkts);
#endif

#ifdef CONFIG_IP_FASTPASS_DEBUG
		printf("RouterDriver on core %d pulled %d packets with mask %llx\n",
			m_core_index, n_pkts, m_port_masks[j]);
#endif
	}

	/* fetch a batch of packets from the network */
	n_pkts = fp_ring_dequeue_burst(m_q_to_router,
			(void **) &pkt_ptrs, ROUTER_MAX_BURST);
	assert(n_pkts <= ROUTER_MAX_BURST);
	/* shuffle packets to ensure router doesn't discriminate against some
	 * endpoints */
	for (uint32_t i = n_pkts; i > 1; i--) {
		/* decide which packet should be in pkts_ptrs[i] between 0..i-1*/
		uint32_t j = random_int(&m_random, i);
		/* swap i and j, works okay even when i == j */
		struct emu_packet *tmp = pkt_ptrs[i-1];
		pkt_ptrs[i-1] = pkt_ptrs[j];
		pkt_ptrs[j] = tmp;
	}

	/* pass all incoming packets to the router */
#ifdef EMU_NO_BATCH_CALLS
	for (i = 0; i < n_pkts; i++) {
		m_router->push(pkt_ptrs[i]);
	}
#else
	m_router->push_batch(&pkt_ptrs[0], n_pkts, m_cur_time);
#endif
	adm_log_emu_router_driver_pushed(m_stat, n_pkts);

#ifdef CONFIG_IP_FASTPASS_DEBUG
	printf("RouterDriver on core %d pushed %d packets\n", m_core_index,
			n_pkts);
#endif

	m_cur_time++;
}


