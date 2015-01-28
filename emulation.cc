/*
 * emulation.cc
 *
 *  Created on: June 24, 2014
 *      Author: aousterh
 */

#include "emulation.h"
#include "api.h"
#include "api_impl.h"
#include "admitted.h"
#include "endpoint_group.h"
#include "router.h"
#include "drivers/EndpointDriver.h"
#include "drivers/RouterDriver.h"
#include "output.h"
#include "../protocol/topology.h"
#include "../graph-algo/fp_ring.h"
#include "../graph-algo/platform.h"

#include <assert.h>
#include <stdexcept>

emu_state *g_state; /* global emulation state */

EmulationCore::EmulationCore(struct emu_state *state,
		EndpointDriver **epg_drivers, RouterDriver **router_drivers,
		uint16_t n_epgs, uint16_t n_rtrs, uint16_t core_index)
{
	Dropper *dropper;
	uint32_t i;

	/* initialize the output and dropper for this core */
	m_out = new EmulationOutput(state->q_admitted_out,
			state->admitted_traffic_mempool, state->packet_mempool,
			&m_stat);
	dropper = new Dropper(*m_out, &state->queue_bank_stats);

	m_n_epgs = n_epgs;
	m_n_rtrs = n_rtrs;

	/* only 1 core for now - must handle all endpoints and routers */
	for (i = 0; i < n_epgs; i++) {
		m_endpoint_drivers[i] = epg_drivers[i];
		m_endpoint_drivers[i]->assign_to_core(m_out, &m_stat);
	}

	for (i = 0; i < n_rtrs; i++) {
		m_router_drivers[i] = router_drivers[i];
		m_router_drivers[i]->assign_to_core(dropper, &m_stat);
	}

	/* TODO: do this properly by making the APIs accessible from C, or moving
	 * the log core to C++ */
	state->core_stats[core_index] = &m_stat;
}

void EmulationCore::step() {
	uint32_t i;

	/* push/pull at endpoints and routers must be done in a specific order to
	 * ensure that packets pushed in one timeslot cannot be pulled until the
	 * next. */

	/* push new packets from the network to endpoints */
	for (i = 0; i < m_n_epgs; i++)
		m_endpoint_drivers[i]->step();

	/* emulate one timeslot at each router (push and pull) */
	for (i = 0; i < m_n_rtrs; i++)
		m_router_drivers[i]->step();

	m_out->flush();
}

void EmulationCore::cleanup() {
	uint32_t i;

	/* free all endpoints */
	for (i = 0; i < EMU_NUM_ENDPOINT_GROUPS; i++) {
		m_endpoint_drivers[i]->cleanup();
		delete m_endpoint_drivers[i];
	}

	/* free all routers */
	for (i = 0; i < EMU_NUM_ROUTERS; i++) {
		m_router_drivers[i]->cleanup();
		delete m_router_drivers[i];
	}

	delete m_out;
}

#if defined(SINGLE_RACK_TOPOLOGY)
/* construct a topology with 1 rack - 1 router connected to all endpoints */
inline void construct_single_rack_topology(struct emu_state *state,
		struct fp_ring **packet_queues, EndpointDriver **endpoint_drivers,
		RouterDriver **router_drivers, RouterType r_type, void *r_args,
		EndpointType e_type, void *e_args)
{
	uint32_t pq;
	struct fp_ring	*q_router_egress[EMU_MAX_OUTPUTS_PER_RTR];
	uint64_t rtr_masks[EMU_MAX_OUTPUTS_PER_RTR];
	struct fp_ring *q_router_ingress;
	EndpointGroup *epg;
	Router *rtr;
	struct topology_args topo_args;

	printf("SINGLE RACK topology with %d routers and %d endpoints\n",
			EMU_NUM_ROUTERS, EMU_NUM_ENDPOINTS);

	/* initialize rings for routers and endpoints */
	pq = 0;
	q_router_ingress = packet_queues[pq++];
	q_router_egress[0] = packet_queues[pq++];

	/* initialize the routers */
	topo_args.func = TOR_ROUTER;
	topo_args.rack_index = 0;
	rtr = RouterFactory::NewRouter(r_type, r_args, &topo_args, 0,
			&state->queue_bank_stats);
	assert(rtr != NULL);
	rtr_masks[0] = 0xFFFFFFFF; /* 32 ports */
	router_drivers[0] = new RouterDriver(rtr, q_router_ingress,
			&q_router_egress[0], &rtr_masks[0], 1);

	/* initialize all the endpoints in one endpoint group */
	epg = EndpointGroupFactory::NewEndpointGroup(e_type, EMU_NUM_ENDPOINTS, 0,
			e_args);
	assert(epg != NULL);
	endpoint_drivers[0] =
			new EndpointDriver(state->comm_state.q_epg_new_pkts[0],
					q_router_ingress, q_router_egress[0],
					state->comm_state.q_resets[0], epg);
}
#endif

#if defined(TWO_RACK_TOPOLOGY)
/* construct a topology with 2 racks and one core router connecting the tors */
inline void construct_two_rack_topology(struct emu_state *state,
		struct fp_ring **packet_queues, EndpointDriver **endpoint_drivers,
		RouterDriver **router_drivers, RouterType r_type, void *r_args,
		EndpointType e_type, void *e_args)
{
	uint32_t pq, i;
	struct fp_ring *q_epg_ingress[EMU_NUM_ENDPOINT_GROUPS];
	struct fp_ring *q_router_ingress[EMU_NUM_ROUTERS];
	EndpointGroup *epg;
	Router *rtr;
	struct topology_args topo_args;

	printf("TWO RACK topology with %d routers and %d endpoints\n",
			EMU_NUM_ROUTERS, EMU_NUM_ENDPOINTS);

	/* initialize rings for routers and endpoints */
	pq = 0;
	for (i = 0; i < EMU_NUM_ENDPOINT_GROUPS; i++)
		q_epg_ingress[i] = packet_queues[pq++];
	for (i = 0; i < EMU_NUM_ROUTERS; i++)
		q_router_ingress[i] = packet_queues[pq++];

	/* initialize all the endpoints */
	for (i = 0; i < EMU_NUM_ENDPOINT_GROUPS; i++) {
		epg = EndpointGroupFactory::NewEndpointGroup(e_type,
				EMU_ENDPOINTS_PER_RACK, i * EMU_ENDPOINTS_PER_RACK, e_args);
		assert(epg != NULL);
		endpoint_drivers[i] =
				new EndpointDriver(state->comm_state.q_epg_new_pkts[i],
						q_router_ingress[i], q_epg_ingress[i],
						state->comm_state.q_resets[i], epg);
	}

	/* initialize the ToRs. both have 32 ports facing down and 32 ports facing
	 * up to the core. */
	uint64_t rtr_masks[EMU_MAX_OUTPUTS_PER_RTR];
	struct fp_ring *q_router_egress[EMU_MAX_OUTPUTS_PER_RTR];
	topo_args.func = TOR_ROUTER;
	rtr_masks[0] = 0xFFFFFFFF;
	rtr_masks[1] = 0xFFFFFFFF00000000;
	q_router_egress[1] = q_router_ingress[2];
	for (i = 0; i < EMU_NUM_TORS; i++) {
		topo_args.rack_index = i;
		q_router_egress[0] = q_epg_ingress[i];

		rtr = RouterFactory::NewRouter(r_type, r_args, &topo_args, i, NULL);
		assert(rtr != NULL);
		router_drivers[i] = new RouterDriver(rtr, q_router_ingress[i],
				&q_router_egress[0], &rtr_masks[0], 2);
	}

	/* initialize the ToR. first 32 ports are for first ToR, next 32 are for
	 * second ToR. */
	topo_args.func = CORE_ROUTER;
	topo_args.links_per_tor = 32;
	q_router_egress[0] = q_router_ingress[0];
	q_router_egress[1] = q_router_ingress[1];
	rtr = RouterFactory::NewRouter(r_type, r_args, &topo_args, 2,
			&state->queue_bank_stats);
	assert(rtr != NULL);
	router_drivers[2] = new RouterDriver(rtr, q_router_ingress[2],
			&q_router_egress[0], &rtr_masks[0], 2);

}
#endif

/* configure the topology of endpoints and routers */
inline void construct_topology(struct emu_state *state,
		struct fp_ring **packet_queues, EndpointDriver **endpoint_drivers,
		RouterDriver **router_drivers, RouterType r_type, void *r_args,
		EndpointType e_type, void *e_args) {
#if defined(SINGLE_RACK_TOPOLOGY)
	/* construct topology: 1 router with 1 rack of endpoints */
	construct_single_rack_topology(state, packet_queues, endpoint_drivers,
			router_drivers, r_type, r_args, e_type, e_args);
#elif defined(TWO_RACK_TOPOLOGY)
	/* construct topology: 3 routers with 2 racks of endpoints */
	construct_two_rack_topology(state, packet_queues, endpoint_drivers,
			router_drivers, r_type, r_args, e_type, e_args);
#else
#error "unrecognized topology"
#endif
}

/* map drivers to cores based on number of cores available */
inline void assign_components_to_cores(struct emu_state *state,
		EndpointDriver **epg_drivers, RouterDriver **router_drivers) {
	uint16_t core_index = 0;
	uint16_t i;

#if (ALGO_N_CORES == (EMU_NUM_ROUTERS + EMU_NUM_ENDPOINT_GROUPS))
	/* put 1 router or endpoint group on each core */
	for (i = 0; i < EMU_NUM_ENDPOINT_GROUPS; i++) {
		state->cores[core_index] = new EmulationCore(state, &epg_drivers[i],
				NULL, 1, 0, core_index);
		core_index++;
	}
	for (i = 0; i < EMU_NUM_ROUTERS; i++) {
		state->cores[core_index] = new EmulationCore(state, NULL,
				&router_drivers[i], 0, 1, core_index);
		core_index++;
	}
#elif (ALGO_N_CORES == 1)
	/* assign everything to one core */
	state->cores[core_index] = new EmulationCore(state, epg_drivers,
			router_drivers, EMU_NUM_ENDPOINT_GROUPS, EMU_NUM_ROUTERS,
			core_index);
#else
#error "no specified way to assign this number of routers and endpoint groups to available cores"
#endif
}

void emu_init_state(struct emu_state *state,
		struct fp_mempool *admitted_traffic_mempool,
		struct fp_ring *q_admitted_out, struct fp_mempool *packet_mempool,
	    struct fp_ring **packet_queues, RouterType r_type, void *r_args,
		EndpointType e_type, void *e_args) {
	uint32_t i, pq;
	EndpointDriver	*endpoint_drivers[EMU_NUM_ENDPOINT_GROUPS];
	RouterDriver	*router_drivers[EMU_NUM_ROUTERS];
	EmulationOutput *out;
	Dropper *dropper;

	g_state = state;

	/* initialize global emulation state */
	pq = 0;
	state->admitted_traffic_mempool = admitted_traffic_mempool;
	state->q_admitted_out = q_admitted_out;
	state->packet_mempool = packet_mempool;
	memset(&state->queue_bank_stats, 0, sizeof(struct queue_bank_stats));

	/* initialize state used to communicate with comm cores */
	for (i = 0; i < EPGS_PER_COMM; i++) {
		state->comm_state.q_epg_new_pkts[i] = packet_queues[pq++];
		state->comm_state.q_resets[i] = packet_queues[pq++];
	}

	/* initialize the topology */
	construct_topology(state, &packet_queues[pq], &endpoint_drivers[0],
			&router_drivers[0], r_type, r_args, e_type, e_args);

	/* assign endpoints and routers to cores */
	assign_components_to_cores(state, endpoint_drivers, router_drivers);
}

void emu_cleanup(struct emu_state *state) {
	uint32_t i;
	struct emu_admitted_traffic *admitted;

	/* cleanup cores */
	for (i = 0; i < ALGO_N_CORES; i++) {
		state->cores[i]->cleanup();
		delete state->cores[i];
	}

	/* free queues to comm core */
	for (i = 0; i < EMU_NUM_ENDPOINT_GROUPS; i++) {
		/* free packet queues, return packets to mempool */
		free_packet_ring(state, state->comm_state.q_epg_new_pkts[i]);
		free_packet_ring(state, state->comm_state.q_resets[i]);
	}

	/* empty queue of admitted traffic, return structs to the mempool */
	while (fp_ring_dequeue(state->q_admitted_out, (void **) &admitted) == 0)
		fp_mempool_put(state->admitted_traffic_mempool, admitted);
	fp_free(state->q_admitted_out);

	fp_free(state->admitted_traffic_mempool);
	fp_free(state->packet_mempool);
}

void emu_emulate(struct emu_state *state) {
	uint32_t i;

	for (i = 0; i < ALGO_N_CORES; i++)
		state->cores[i]->step();
}

void free_packet_ring(struct emu_state *state, struct fp_ring *packet_ring)
{
	struct emu_packet *packet;

	while (fp_ring_dequeue(packet_ring, (void **) &packet) == 0) {
		free_packet(state, packet);
	}
	fp_free(packet_ring);
}

#ifdef NO_DPDK
void emu_alloc_init(struct emu_state* state, uint32_t admitted_mempool_size,
		uint32_t admitted_ring_size, uint32_t packet_mempool_size,
		uint32_t packet_ring_size)
{
	struct fp_mempool *admitted_traffic_mempool =
			fp_mempool_create(admitted_mempool_size,
					sizeof(struct emu_admitted_traffic));
	if (admitted_traffic_mempool == NULL)
		throw std::runtime_error("couldn't allocate admitted_traffic_mempool");

	struct fp_ring *q_admitted_out = fp_ring_create("q_admitted_out",
			admitted_ring_size, 0, 0);
	if (q_admitted_out == NULL)
		throw std::runtime_error("couldn't allocate q_admitted_out");

	struct fp_mempool *packet_mempool = fp_mempool_create(packet_mempool_size,
			EMU_ALIGN(sizeof(struct emu_packet)));
	if (packet_mempool == NULL)
		throw std::runtime_error("couldn't allocate packet_mempool");

	struct fp_ring *q_new_packets = fp_ring_create("q_new_packets",
			packet_ring_size, 0, 0);
	if (q_new_packets == NULL)
		throw std::runtime_error("couldn't allocate q_new_packets");

	struct fp_ring *q_resets = fp_ring_create("q_resets", packet_ring_size, 0,
			0);
	if (q_resets == NULL)
		throw std::runtime_error("couldn't allocate q_resets");

	struct fp_ring *packet_queues[EMU_NUM_PACKET_QS];
	packet_queues[2] = q_new_packets;
	packet_queues[3] = q_resets;

	emu_init_state(state, admitted_traffic_mempool, q_admitted_out,
			packet_mempool, packet_queues, R_DropTail, NULL,
			E_Simple, NULL);
}
#endif
