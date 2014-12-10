/*
 * drop_tail.c
 *
 *  Created on: August 30, 2014
 *      Author: aousterh
 */

#include "drop_tail.h"
#include "api.h"
#include "api_impl.h"

#define DROP_TAIL_PORT_CAPACITY 128

int drop_tail_router_init(struct emu_router *rtr, void *args) {
	struct drop_tail_router *rtr_priv;
	struct drop_tail_args *drop_tail_args;
	uint32_t i, port_capacity;

	/* get private state for this router */
	rtr_priv = (struct drop_tail_router *) router_priv(rtr);

	/* use args if supplied, otherwise use defaults */
	if (args != NULL) {
		drop_tail_args = (struct drop_tail_args *) args;
		port_capacity = drop_tail_args->port_capacity;
	} else
		port_capacity = DROP_TAIL_PORT_CAPACITY;

	for (i = 0; i < EMU_ROUTER_NUM_PORTS; i++)
		queue_create(&rtr_priv->output_queue[i], port_capacity);

	return 0;
}

void drop_tail_router_cleanup(struct emu_router *rtr) {
	struct drop_tail_router *rtr_priv;
	uint16_t i;
	struct emu_packet *packet;

	/* get private state for this router */
	rtr_priv = (struct drop_tail_router *) router_priv(rtr);

	/* free all queued packets */
	for (i = 0; i < EMU_ROUTER_NUM_PORTS; i++) {
		while (queue_dequeue(&rtr_priv->output_queue[i], &packet) == 0)
			free_packet(packet);
	}
}

void drop_tail_router_receive(struct emu_router *rtr, struct emu_packet *p) {
	struct drop_tail_router *rtr_priv;
	uint16_t output;
	struct packet_queue *output_q;

	/* get private state for this router */
	rtr_priv = (struct drop_tail_router *) router_priv(rtr);

	output = get_output_queue(rtr, p);
	output_q = &rtr_priv->output_queue[output];

	/* try to enqueue the packet */
	if (queue_enqueue(output_q, p) != 0) {
		/* no space to enqueue, drop this packet */
		adm_log_emu_router_dropped_packet(&g_state->stat);
		drop_packet(p);
	}
}

void drop_tail_router_send(struct emu_router *rtr, uint16_t output,
		struct emu_packet **packet) {
	struct drop_tail_router *rtr_priv;

	/* get private state for this router */
	rtr_priv = (struct drop_tail_router *) router_priv(rtr);

	/* dequeue one packet for this output if the queue is non-empty */
	*packet = NULL;
	queue_dequeue(&rtr_priv->output_queue[output], packet);
}

int drop_tail_endpoint_init(struct emu_endpoint *ep, void *args) {
	struct drop_tail_endpoint *ep_priv;
	struct drop_tail_args *drop_tail_args;
	uint32_t port_capacity;

	/* get private state for this endpoint */
	ep_priv = (struct drop_tail_endpoint *) endpoint_priv(ep);

	/* use args if supplied, otherwise use defaults */
	if (args != NULL) {
		drop_tail_args = (struct drop_tail_args *) args;
		port_capacity = drop_tail_args->port_capacity;
	} else
		port_capacity = DROP_TAIL_PORT_CAPACITY;

	queue_create(&ep_priv->output_queue, port_capacity);

	return 0;
};

void drop_tail_endpoint_reset(struct emu_endpoint *ep) {
	struct drop_tail_endpoint *ep_priv;
	struct emu_packet *packet;

	/* get private state for this endpoint */
	ep_priv = (struct drop_tail_endpoint *) endpoint_priv(ep);

	/* dequeue all queued packets */
	while (queue_dequeue(&ep_priv->output_queue, &packet) == 0)
		free_packet(packet);
}

void drop_tail_endpoint_cleanup(struct emu_endpoint *ep) {
	struct drop_tail_endpoint *ep_priv;
	struct emu_packet *packet;

	/* get private state for this endpoint */
	ep_priv = (struct drop_tail_endpoint *) endpoint_priv(ep);

	/* dequeue all queued packets */
	while (queue_dequeue(&ep_priv->output_queue, &packet) == 0)
		free_packet(packet);
}

void drop_tail_endpoint_rcv_from_app(struct emu_endpoint *ep,
		struct emu_packet *p) {
	struct drop_tail_endpoint *ep_priv;

	/* get private state for this endpoint */
	ep_priv = (struct drop_tail_endpoint *) endpoint_priv(ep);

	/* try to enqueue the packet */
	if (queue_enqueue(&ep_priv->output_queue, p) != 0) {
		/* no space to enqueue, drop this packet */
		adm_log_emu_endpoint_dropped_packet(&g_state->stat);
		drop_packet(p);
	}
}

void drop_tail_endpoint_rcv_from_net(struct emu_endpoint *ep,
		struct emu_packet *p) {
	struct drop_tail_endpoint *ep_priv;

	/* get private state for this endpoint */
	ep_priv = (struct drop_tail_endpoint *) endpoint_priv(ep);

	/* pass the packet up the stack */
	enqueue_packet_at_endpoint(ep, p);
}

void drop_tail_endpoint_send_to_net(struct emu_endpoint *ep,
		struct emu_packet **p) {
	struct drop_tail_endpoint *ep_priv;

	/* get private state for this endpoint */
	ep_priv = (struct drop_tail_endpoint *) endpoint_priv(ep);

	/* dequeue one incoming packet if the queue is non-empty */
	*p = NULL;
	queue_dequeue(&ep_priv->output_queue, p);
}
