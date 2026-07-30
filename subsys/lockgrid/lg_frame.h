/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_frame.h
 * @brief LockGrid over-the-air formats.
 *
 * The PDU is carried directly in the 802.15.4 PSDU, after the length byte.
 * Everything is little endian.
 *
 *   0        2        3        4        6        8       12
 *   +--------+--------+--------+--------+--------+--------+---------------+
 *   | netid  |  fcf   |  seq   |  dst   |  src   | frame  |  payload ...  |
 *   |  (2)   |  (1)   |  (1)   |  (2)   |  (2)   | ctr(4) |               |
 *   +--------+--------+--------+--------+--------+--------+---------------+
 *                                                 ^ present only when FCF_SEC
 *
 * followed by CONFIG_LOCKGRID_MIC_LEN bytes of AES-CCM tag when FCF_SEC is set.
 *
 * @c netid is the low 16 bits of the network id. It sits first so a receiver
 * can reject a foreign grid after two bytes, which is what
 * CONFIG_LOCKGRID_EARLY_ABORT keys off.
 */

#ifndef LOCKGRID_LG_FRAME_H_
#define LOCKGRID_LG_FRAME_H_

#include <stdint.h>
#include <zephyr/sys/util.h>
#include <lockgrid/lockgrid.h>

/** Bytes before the frame counter: netid, fcf, seq, dst, src. */
#define LG_HDR_LEN 8
/** Frame counter length. */
#define LG_FCTR_LEN 4
/** Header length of a secured frame. */
#define LG_HDR_LEN_SEC (LG_HDR_LEN + LG_FCTR_LEN)
/** Total overhead of a secured frame. */
#define LG_SEC_OVERHEAD (LG_HDR_LEN_SEC + CONFIG_LOCKGRID_MIC_LEN)

/**
 * Largest PDU the PHY carries: the 802.15.4 PSDU length field allows 127
 * including the two CRC bytes.
 */
#define LG_PDU_MAX 125

/** Byte offsets into a LockGrid PDU. */
#define LG_OFF_NETID 0
#define LG_OFF_FCF   2
#define LG_OFF_SEQ   3
#define LG_OFF_DST   4
#define LG_OFF_SRC   6
#define LG_OFF_FCTR  8

/** @name Frame control field
 *  @{
 */
#define LG_FCF_TYPE_MASK 0x0FU
/** Payload is encrypted and the whole frame is authenticated. */
#define LG_FCF_SEC       BIT(4)
/** Sender has more to send in this event; keep the exchange going. */
#define LG_FCF_MD        BIT(5)
/** Sender wants this frame acknowledged. */
#define LG_FCF_AREQ      BIT(6)
/** This frame acknowledges the peer's last frame. */
#define LG_FCF_ACK       BIT(7)
/** @} */

/** Frame types. */
enum lg_frame_type {
	/** Unsolicited advertisement of a grid, sent by routers. */
	LG_FT_BEACON = 0,
	/** Certificate handshake, joiner to router. */
	LG_FT_JOIN_REQ = 1,
	/** Certificate handshake, router to joiner. */
	LG_FT_JOIN_RSP = 2,
	/** Certificate handshake, joiner's proof of possession. */
	LG_FT_JOIN_CFM = 3,
	/** Certificate handshake complete, carries grid parameters. */
	LG_FT_JOIN_ACK = 4,
	/** One round trip re-attach using a cached link key. */
	LG_FT_RESUME_REQ = 5,
	LG_FT_RESUME_RSP = 6,
	/** Negotiate the schedule of a link. */
	LG_FT_LINK_REQ = 7,
	LG_FT_LINK_RSP = 8,
	/** Application traffic. */
	LG_FT_DATA = 9,
	/** Bare acknowledgement, doubles as the keepalive that holds sync. */
	LG_FT_ACK = 10,
	/** Link and topology control, see @ref lg_ctrl_op. */
	LG_FT_CTRL = 11,
};

/** Link and topology control opcodes, first byte of an ::LG_FT_CTRL payload. */
enum lg_ctrl_op {
	/** New channel map, applied at a shared event counter. */
	LG_CTRL_CHAN_MAP = 0,
	/** New subrate factor, applied at a shared event counter. */
	LG_CTRL_SUBRATE = 1,
	/** Change the connection interval at a shared event counter. */
	LG_CTRL_INTERVAL = 2,
	/** Sender's rank, role and neighbour costs. Feeds routing without scanning. */
	LG_CTRL_ROUTE_ADV = 4,
	/** Addresses reachable through the sender, so parents can route downward. */
	LG_CTRL_ROUTE_REG = 5,
	/** Sender is dropping the link. */
	LG_CTRL_TEARDOWN = 6,
	/** Reserved. */
	LG_CTRL_TIME = 7,
};

/** Reasons carried by ::LG_CTRL_TEARDOWN. */
enum lg_teardown_reason {
	LG_TD_LOCAL_CLOSE = 0,
	LG_TD_TOO_MANY_LINKS = 1,
	LG_TD_POOR_QUALITY = 2,
	LG_TD_ROLE_CHANGE = 3,
	LG_TD_AUTH_EXPIRED = 4,
};

/**
 * @brief Beacon payload.
 *
 * Sent unsecured so an unprovisioned joiner can act on it, and MIC'd with the
 * grid key on top of that so nodes already in the grid can tell a real beacon
 * from a replayed one. A joiner therefore trusts a beacon only far enough to
 * start a handshake, which authenticates properly.
 */
struct lg_beacon {
	/** Full network id, so a joiner can check the low 16 bits were not a collision. */
	uint32_t network_id;
	/**
	 * Monotonic beacon counter, for freshness only.
	 *
	 * Rendezvous channels are derived from the network id alone, not from a
	 * clock, because a node that has not joined has no shared timebase.
	 */
	uint32_t epoch;
	/** Sender's cost to a backhaul. */
	uint16_t rank;
	/** Channels the sender currently considers usable. */
	uint16_t chan_map;
	/** Sender's role, see ::lg_role. */
	uint8_t role;
	/** Links the sender already has, so joiners can prefer a less loaded router. */
	uint8_t link_count;
	/** Links the sender can still accept. */
	uint8_t link_capacity;
	/**
	 * Microseconds from the end of this beacon to the join window the
	 * sender will open. Lets a joiner stop receiving in between.
	 */
	uint16_t join_offset_us;
	/** Length of that join window. */
	uint16_t join_window_us;
	/**
	 * Microseconds from this frame's first bit to the first bit of the
	 * sender's next beacon event.
	 *
	 * This is the single most valuable field in the protocol for energy. A
	 * node that has heard one beacon knows when the next one is, so instead
	 * of a blind scan window sized against the beacon interval it opens one
	 * sized against clock drift - milliseconds instead of tens of
	 * milliseconds. It is also what makes re-attaching to a router that was
	 * lost cheap, because its schedule is already known.
	 */
	uint32_t next_beacon_offset_us;
} __packed;

/** Beacon MIC length. Short on purpose; the beacon is not a security boundary. */
#define LG_BEACON_MIC_LEN 4

/** @brief Payload of ::LG_FT_DATA, ahead of the application bytes. */
struct lg_data_hdr {
	/** See ::LG_DATA_F_*. */
	uint8_t flags;
	/** Node that produced the message. */
	uint16_t origin;
	/** Final destination, or ::LG_ADDR_BACKHAUL. */
	uint16_t final;
	/** Incremented at every forward, used to bound looping. */
	uint8_t hops;
	/** Application port. */
	uint8_t port;
	/** Origin-scoped id, used to drop duplicates arriving over two parents. */
	uint16_t msg_id;
} __packed;

/** Route towards the nearest backhaul rather than a specific address. */
#define LG_DATA_F_UPWARD    BIT(0)
/** Deliver to every neighbour of the sender, do not forward. */
#define LG_DATA_F_BROADCAST BIT(1)
/** Travelling down the tree using the downward routing table. */
#define LG_DATA_F_DOWNWARD  BIT(2)

/** Hops a message may take before it is dropped. */
#define LG_HOPS_MAX 12

/** @brief Payload of ::LG_FT_LINK_REQ and ::LG_FT_LINK_RSP. */
struct lg_link_params {
	/** Connection interval in microseconds. */
	uint32_t interval_us;
	/**
	 * Microseconds from the first bit of the frame carrying this structure
	 * to the anchor of event 0.
	 *
	 * Expressed as an offset rather than an absolute time because the two
	 * nodes' counters started independently: the receiver timestamped this
	 * frame's arrival in its own clock, so an offset needs no shared epoch.
	 */
	uint32_t anchor_offset_us;
	/** Channels both ends will hop over. */
	uint16_t chan_map;
	/** Seed for the hop sequence, so the two ends agree without messaging. */
	uint16_t hop_seed;
	/** Supervision timeout in units of 10 ms. */
	uint16_t sup_timeout_cs;
	/** Largest subrate factor the sender will honour. */
	uint8_t subrate_max;
	/** Sleep clock accuracy in ppm, drives the peer's window widening. */
	uint16_t sca_ppm;
	/** Sender's role. */
	uint8_t role;
	/** Sender's rank at the time of the request. */
	uint16_t rank;
} __packed;

/** @brief Build the fixed part of a PDU header. Returns bytes written. */
uint8_t lg_frame_hdr_put(uint8_t *pdu, uint16_t netid, uint8_t fcf, uint8_t seq,
			 lg_addr_t dst, lg_addr_t src);

/** @brief Read the header of a received PDU. Returns false if malformed. */
bool lg_frame_hdr_get(const uint8_t *pdu, uint8_t len, uint16_t netid, uint8_t *fcf,
		      uint8_t *seq, lg_addr_t *dst, lg_addr_t *src);

static inline uint8_t lg_frame_type(const uint8_t *pdu)
{
	return pdu[LG_OFF_FCF] & LG_FCF_TYPE_MASK;
}

#endif /* LOCKGRID_LG_FRAME_H_ */
