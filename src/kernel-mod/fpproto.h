/*
 * Platform-independent FastPass protocol
 */

#ifndef FPPROTO_H_
#define FPPROTO_H_

#include "fp_statistics.h"

#define IPPROTO_FASTPASS 222

#define FASTPASS_TO_CONTROLLER_SEQNO_OFFSET		0
#define FASTPASS_TO_ENDPOINT_SEQNO_OFFSET		(0xDEADBEEF)

/**
 * The log of the size of outgoing packet window waiting for ACKs or timeout
 *    expiry. Setting this at < 6 is a bit wasteful since a full word has 64
 *    bits, and the algorithm works with word granularity
 */
#define FASTPASS_OUTWND_LOG			8
#define FASTPASS_OUTWND_LEN			(1 << FASTPASS_OUTWND_LOG)

#define FASTPASS_BAD_PKT_RESET_THRESHOLD	10
#define FASTPASS_RESET_WINDOW_NS	(1000*1000*1000)

#define FASTPASS_PKT_MAX_AREQ		10

#define FASTPASS_PTYPE_RSTREQ		0x0
#define FASTPASS_PTYPE_RESET 		0x1
#define FASTPASS_PTYPE_AREQ			0x2
#define FASTPASS_PTYPE_ALLOC		0x3
#define FASTPASS_PTYPE_ACK			0x4


/**
 * An A-REQ for a single destination
 * @src_dst_key: the key for the flow
 * @tslots: the total number of tslots requested
 */
struct fpproto_areq_desc {
	u64		src_dst_key;
	u64		tslots;
};

/**
 * A full packet sent to the controller
 * @n_areq: number of filled in destinations for A-REQ
 * @sent_timestamp: a timestamp when the request was sent
 */
struct fpproto_pktdesc {
	u16							n_areq;
	struct fpproto_areq_desc	areq[FASTPASS_PKT_MAX_AREQ];

	u64							sent_timestamp;
	u64							seqno;
	u64							ack_seq;
	u16							ack_vec;
	bool						send_reset;
	u64							reset_timestamp;
};

/**
 * Operations executed by the protocol
 */
struct fpproto_ops {
	void 	(*handle_reset)(void *param);

	void	(*handle_alloc)(void *param, u32 base_tslot,
			u16 *dst, int n_dst, u8 *tslots, int n_tslots);

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
	void	(*trigger_request)(void *param, u64 when);

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
	u32						send_timeout_us;
	u32						consecutive_bad_pkts;

	/* outwnd */
	unsigned long			bin_mask[BITS_TO_LONGS(2 * FASTPASS_OUTWND_LEN)];
	struct fpproto_pktdesc	*bins[FASTPASS_OUTWND_LEN];
	u32						tx_num_unacked;

	u64						earliest_unacked;

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

/*** TIMER CALLBACK ***/
void fpproto_handle_timeout(struct fpproto_conn *conn, u64 now);

/*** RX ***/
/* parses payloads and performs appropriate callbacks to ops */
void fpproto_handle_rx_packet(struct fpproto_conn *conn, u8 *data, u32 len,
		__be32 saddr, __be32 daddr);

/*** TX ***/
void fpproto_prepare_to_send(struct fpproto_conn *conn);
void fpproto_commit_packet(struct fpproto_conn *conn,
		struct fpproto_pktdesc *pkt, u64 timestamp);

struct fpproto_pktdesc *fpproto_pktdesc_alloc(void);
void fpproto_pktdesc_free(struct fpproto_pktdesc *pd);


/**
 * Encodes @pd into the buffer @data.
 * Returns the number of used bytes (not to exceed @max_len)
 */
int fpproto_encode_packet(struct fpproto_conn *conn,
		struct fpproto_pktdesc *pd, u8 *data, u32 max_len, __be32 saddr,
		__be32 daddr);

#endif /* FPPROTO_H_ */