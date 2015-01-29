/*
 * hull.cc
 *
 * Created on: January 29, 2015
 *     Author: hari
 */

#include "queue_managers/dctcp.h"
#include "queue_managers/hull.h"
#include <limits>

HULLQueueManager::HULLQueueManager(PacketQueueBank *bank,
                                 struct hull_args *hull_params)
    : m_bank(bank), m_hull_params(*hull_params), m_phantom_len(0), m_last_phantom_update_time(0)
{
    // HULL_QUEUE_CAPACITY should be smaller than MAXINT/HULL_ATOM_SIZE
    assert(HULL_QUEUE_CAPACITY < std::numeric_limits<int>::max()/HULL_ATOM_SIZE);
    if (bank == NULL)
        throw std::runtime_error("bank should be non-NULL");
    /* initialize other state */
}

void HULLQueueManager::enqueue(struct emu_packet *pkt,
                               uint32_t port, uint32_t queue, uint64_t time)
{
    uint32_t qlen = m_bank->occupancy(port, queue);
    if (qlen >= m_hull_params.q_capacity) {
        /* no space to enqueue, drop this packet */
        adm_log_emu_router_dropped_packet(m_stat);
        m_dropper->drop(pkt, port);
	return;
    }

    m_phantom_len -= (time - m_last_phantom_update_time) * m_hull_params.GAMMA;
    if (m_phantom_len < 0) {
        m_phantom_len = 0;
    }
    m_phantom_len += HULL_ATOM_SIZE;

    if (m_phantom_len > m_hull_params.mark_threshold) {
      /* Set ECN mark on packet, then drop into enqueue */
        adm_log_emu_router_marked_packet(m_stat);
        packet_mark_ecn(pkt);
    }

    m_bank->enqueue(port, queue, pkt);
}


/**
 * All ports of a HULLRouter run HULL. We don't currently support routers with 
 * different ports running different QMs or schedulers.
 */
HULLRouter::HULLRouter(uint16_t id, struct hull_args *hull_params,
		struct queue_bank_stats *stats)
    : m_bank(EMU_ENDPOINTS_PER_RACK, 1, HULL_QUEUE_CAPACITY, stats),
      m_rt(16, 0, EMU_ENDPOINTS_PER_RACK, 0),
	  m_cla(),
      m_qm(&m_bank, hull_params),
      m_sch(&m_bank),
      HULLRouterBase(&m_rt, &m_cla, &m_qm, &m_sch, EMU_ENDPOINTS_PER_RACK)
{}

HULLRouter::~HULLRouter() {}

void HULLRouter::assign_to_core(Dropper *dropper,
                                struct emu_admission_core_statistics *stat) {
    m_qm.assign_to_core(dropper, stat);
}

