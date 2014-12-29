/*
 * Platform-independent FastPass protocol
 */

#ifndef FPPROTO_H_
#define FPPROTO_H_

#if (defined(FASTPASS_CONTROLLER) && defined(FASTPASS_ENDPOINT))
#error "Both FASTPASS_CONTROLLER and FASTPASS_ENDPOINT are defined"
#endif
#if !(defined(FASTPASS_CONTROLLER) || defined(FASTPASS_ENDPOINT))
#error "Neither FASTPASS_CONTROLLER or FASTPASS_ENDPOINT is defined"
#endif

#ifdef FASTPASS_CONTROLLER
#include <rte_ip.h>
#endif

#include "platform/generic.h"
#include "platform/debug.h"
#include "window.h"

/* FASTPASS_PR_DEBUG defined in platform.h */
#ifdef CONFIG_IP_FASTPASS_DEBUG
extern bool fastpass_debug;
#define fp_debug(format, a...)	  FASTPASS_PR_DEBUG(fastpass_debug, format, ##a)
#else
#define fp_debug(format, a...)
#endif

#define IPPROTO_FASTPASS 222

#define FASTPASS_TO_CONTROLLER_SEQNO_OFFSET		0
#define FASTPASS_TO_ENDPOINT_SEQNO_OFFSET		(0xDEADBEEF)

#ifdef FASTPASS_ENDPOINT
#define IS_ENDPOINT						true
#define FASTPASS_EGRESS_SEQNO_OFFSET	FASTPASS_TO_CONTROLLER_SEQNO_OFFSET
#define FASTPASS_INGRESS_SEQNO_OFFSET	FASTPASS_TO_ENDPOINT_SEQNO_OFFSET
#else
#define IS_ENDPOINT						false
#define FASTPASS_EGRESS_SEQNO_OFFSET	FASTPASS_TO_ENDPOINT_SEQNO_OFFSET
#define FASTPASS_INGRESS_SEQNO_OFFSET	FASTPASS_TO_CONTROLLER_SEQNO_OFFSET
#endif


#define FASTPASS_BAD_PKT_RESET_THRESHOLD	10
#define FASTPASS_RESET_WINDOW_NS	(1000*1000*1000)

#define FASTPASS_PKT_HDR_LEN			8
#define FASTPASS_PKT_RESET_LEN			8

#ifdef FASTPASS_CONTROLLER
/* CONTROLLER */
#define FASTPASS_PKT_MAX_ALLOC_TSLOTS	64
#define FASTPASS_PKT_ALLOC_LEN			(2 + 2 * 15 + FASTPASS_PKT_MAX_ALLOC_TSLOTS)
#else
/* END NODE */
#define FASTPASS_PKT_MAX_ALLOC_TSLOTS	0
#define FASTPASS_PKT_ALLOC_LEN			0
#endif

/* COMMON TO END_NODE AND CONTROLLER */
#define FASTPASS_PKT_MAX_AREQ			10
#define FASTPASS_PKT_AREQ_LEN			(2 + 4 * FASTPASS_PKT_MAX_AREQ)

#define FASTPASS_MAX_PAYLOAD		(FASTPASS_PKT_HDR_LEN + \
									FASTPASS_PKT_RESET_LEN + \
									FASTPASS_PKT_AREQ_LEN + \
									FASTPASS_PKT_ALLOC_LEN)

#define FASTPASS_PTYPE_PADDING		0x0
#define FASTPASS_PTYPE_RESET 		0x1
#define FASTPASS_PTYPE_AREQ			0x2
#define FASTPASS_PTYPE_ALLOC		0x3
#define FASTPASS_PTYPE_ACK			0x4

/**
 * An allocation request (to the arbiter) or report (from the arbiter) for a
 * single destination
 * @src_dst_key: the key for the flow
 * @tslots: the total number of tslots
 */
struct fpproto_areq_desc {
	u64		src_dst_key;
	u64		tslots;
};

/**
 * The state encoded in a full packet sent to or from the arbiter. Stored
 * 	temporarily to help with ACKs and timeouts.
 * @sent_timestamp: a timestamp when the request was sent
 * @seqno: sequence number (allocated by the protocol)
 * @ack_seq: sequence number of ACK
 * @ack_vec: ACK vector
 * @send_reset: whether or not a reset needs to be sent
 * @reset_timestamp: timestamp of a reset, if applicable
 * @n_areq: number of filled in destinations for A-REQ
 * @areq: an array of allocation requests
 * @alloc_tslot: number of allocs in this packet
 * @base_tslot: timeslot of the first alloc (to avoid using very old allocs)
 * @n_dsts: number of destinations with allocs in this packet
 * @dsts: the destinations that have allocs
 * @dst_counts: number of ALLOCs per dst in dsts
 * @tslot_desc: description of each allocation
 */
struct fpproto_pktdesc {
	/* state for tracking timeouts */
	u64							sent_timestamp;

	/* protocol header */
	u64							seqno;
	u64							ack_seq;
	u16							ack_vec;

	/* payload - reset */
	bool						send_reset;
	u64							reset_timestamp;

	/* payload - allocation request totals (areq) */
	u16							n_areq;
	struct fpproto_areq_desc	areq[FASTPASS_PKT_MAX_AREQ];

	/* payload - allocations */
#ifdef FASTPASS_CONTROLLER
	u16							alloc_tslot;
	u16							base_tslot;
	u16							n_dsts;
	u16							dsts[15];
	u16							dst_counts[15];
	u8							tslot_desc[FASTPASS_PKT_MAX_ALLOC_TSLOTS];
#endif
};

/**
 * Operations executed by the protocol
 */
struct fpproto_ops {
	void 	(*handle_reset)(void *param);

	/**
	 * Called when an ack is received for a sent packet.
	 * @note: this function becomes responsible for freeing the memory of @pd
	 */
	void	(*handle_ack)(void *param, struct fpproto_pktdesc *pd);

	/**
	 * Called when a sent packet is deemed as probably lost.
	 * @note: this function becomes responsible for freeing the memory of @pd
	 */
	void	(*handle_neg_ack)(void *param, struct fpproto_pktdesc *pd);

	/**
	 * The protocol needs to send information to the controller -- the user
	 *    should send a packet, so that information can piggy back.
	 */
	void	(*trigger_request)(void *param);

	/**
	 * Called for an ALLOC payload
	 */
	void	(*handle_alloc)(void *param, u32 base_tslot,
			u16 *dst, int n_dst, u8 *tslots, int n_tslots);

	/**
	 * Called for every A-REQ payload
	 * @dst_and_count: a 16-bit destination, then a 16-bit demand count, in
	 *   network byte-order
	 * @n: the number of dst+count pairs
	 */
	void	(*handle_areq)(void *param, u16 *dst_and_count, int n);

	/**
	 * Sets a timer for the connection
	 */
	void	(*set_timer)(void *param, u64 when);

	/**
	 * Cancels the timer for the connection
	 */
	int		(*cancel_timer)(void *param);

};

#define FASTPASS_PROTOCOL_STATS_VERSION 2

/* Control socket statistics */
struct fp_proto_stat {
	__u32 version;

	/* ACK/NACK-related */
	__u64 out_max_seqno;
	__u64 timeout_handler_runs;
	__u64 ack_payloads;
	__u64 too_early_ack;
	__u64 acked_packets;
	__u64 timeout_pkts;
	__u64 informative_ack_payloads;
	__u64 reprogrammed_timer;
	__u64 earliest_unacked;
	__u64 committed_pkts;
	__u64 never_acked_pkts;
	__u64 next_timeout_seqno;
	__u16 tx_num_unacked;

	/* send-related */
	__u64 fall_off_outwnd;

	/* rx-related */
	__u64 rx_pkts;
	__u64 rx_too_short;
	__u64 rx_unknown_payload;
	__u64 rx_incomplete_reset;
	__u64 rx_incomplete_alloc;
	__u64 rx_incomplete_ack;
	__u64 rx_incomplete_areq;
	__u64 rx_dup_pkt;
	__u64 rx_out_of_order;
	__u64 rx_checksum_error;
	__u64 in_max_seqno;
	__u64 inwnd_jumped;
	__u64 seqno_before_inwnd;
	__u16 consecutive_bad_pkts;
	__u64 inwnd;

	/* reset */
	__u32 in_sync:1;
	__u64 last_reset_time;
	__u64 reset_payloads;
	__u64 proto_resets;
	__u64 redundant_reset;
	__u64 reset_both_recent_last_reset_wins;
	__u64 reset_both_recent_payload_wins;
	__u64 reset_last_recent_payload_old;
	__u64 reset_last_old_payload_recent;
	__u64 reset_both_old;
	__u64 no_reset_because_recent;
	__u64 reset_from_bad_pkts;
	__u64 forced_reset;
};

/**
 * @last_reset_time: the time used in the last sent reset
 * @rst_win_ns: time window within which resets are accepted, in nanoseconds
 * @send_timeout_ns: number of ns after which a tx packet is deemed lost
 * @bin_mask: a mask for each bin, 1 if it has not been acked yet.
 * @bins: pointers to the packet descriptors of each bin
 * @earliest_unacked: sequence number of the earliest unacked packet in the
 * 		outwnd. only valid if the outwnd is not empty.
 */
struct fpproto_conn {
	u64						last_reset_time;
	u64						next_seqno;
	u64						in_max_seqno;
	u32						in_sync:1;
	struct fpproto_ops		*ops;
	void 					*ops_param;

	u64 					rst_win_ns;
	u32						send_timeout;
	u32						consecutive_bad_pkts;

	/* outwnd */
	struct fp_window		outwnd;
	struct fpproto_pktdesc	*unacked_pkts[(1 << FASTPASS_WND_LOG)];
	u64						next_timeout_seqno;

	/* inwnd */
	u64						inwnd;

	/* statistics */
	struct fp_proto_stat	stat;

};


/* initializes conn */
void fpproto_init_conn(struct fpproto_conn *conn, struct fpproto_ops *ops,
		void *ops_param, u64 rst_win_ns, u64 send_timeout_us);

/* destroys conn */
void fpproto_destroy_conn(struct fpproto_conn *conn);

/* dumps statistics to @stat, run fpproto_update_internal_stats first */
void fpproto_dump_stats(struct fpproto_conn *conn, struct fp_proto_stat *stat);

/* updates internal statistics struct conn->stat with up-to-date statistics */
void fpproto_update_internal_stats(struct fpproto_conn *conn);

/**
 * Forces a reset (maybe a reset needed due to application failure).
 * Note: the caller should reset application state beforehand, the reset
 *    callback will NOT be called.
 */
void fpproto_force_reset(struct fpproto_conn *conn);

/*** TIMER CALLBACK ***/
void fpproto_handle_timeout(struct fpproto_conn *conn, u64 now);

/*** RX ***/
/* parses payloads and manipulates ack state. returns true if packet should
 *   be processed further for payloads, false otherwise. If returned true
 *   and parsing was successful, in_seq should be passed to
 *   fpproto_successful_rx()*/
bool fpproto_handle_rx_packet(struct fpproto_conn *conn, u8 *pkt, u32 len,
		__be32 saddr, __be32 daddr, u64 *in_seq);
/* performs appropriate callbacks to ops */
bool fpproto_perform_rx_callbacks(struct fpproto_conn *conn, u8 *pkt, u32 len);
/* marks packet as successfully received */
void fpproto_successful_rx(struct fpproto_conn *conn, u64 in_seq);

/* performs all RX functions in sequence */
void fpproto_handle_rx_complete(struct fpproto_conn *conn, u8 *pkt, u32 len,
		__be32 saddr, __be32 daddr);

/*** TX ***/
void fpproto_prepare_to_send(struct fpproto_conn *conn);
void fpproto_commit_packet(struct fpproto_conn *conn,
		struct fpproto_pktdesc *pkt, u64 timestamp);

/**
 * Encodes @pd into the buffer @data.
 * Returns the number of used bytes (not to exceed @max_len)
 */
int fpproto_encode_packet(struct fpproto_pktdesc *pd, u8 *data, u32 max_len,
		__be32 saddr, __be32 daddr, u32 min_size);

#endif /* FPPROTO_H_ */
