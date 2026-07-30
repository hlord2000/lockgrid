/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lockgrid.h
 * @brief LockGrid - a low power, self-healing 2.4 GHz mesh.
 *
 * LockGrid is a scheduled-rendezvous mesh on the IEEE 802.15.4 PHY. Nodes
 * discover a subset of their neighbours by occasional scanning, then hold
 * BLE-like periodic connection events with them. All traffic between two nodes
 * is authenticated and encrypted with a link key established through a
 * certificate-backed handshake, so reachability implies authentication.
 *
 * The radio can be owned outright or arbitrated through MPSL timeslots, in
 * which case LockGrid coexists with a Bluetooth LE stack on the same chip.
 */

#ifndef LOCKGRID_INCLUDE_LOCKGRID_H_
#define LOCKGRID_INCLUDE_LOCKGRID_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Short network address of a node. */
typedef uint16_t lg_addr_t;

/** Not a usable address. */
#define LG_ADDR_INVALID   0x0000U
/** Deliver to every node in radio range of the sender (single hop). */
#define LG_ADDR_BROADCAST 0xFFFFU
/** Deliver to the closest reachable backhaul node. */
#define LG_ADDR_BACKHAUL  0xFFFEU

/** Largest application payload a single LockGrid message can carry. */
#define LG_PAYLOAD_MAX 64

/** Rank value meaning "no route to a backhaul". */
#define LG_RANK_INFINITE 0xFFFFU

/**
 * What a node is currently doing for the rest of the grid.
 *
 * Not a configuration item. A node works out at runtime whether it should be a
 * leaf or a router, and changes its mind when its surroundings change, because
 * the right answer depends on where it turns out to be rather than on anything
 * knowable when it was provisioned. See lg_role().
 *
 * Backhaul is the exception: terminating traffic requires somewhere to terminate
 * it, which is a fact about the hardware. The application declares it with
 * lg_backhaul_set(). None of it comes from the certificate, which admits a node to
 * the network and says nothing about what it should do once there.
 */
enum lg_role {
	/** Keeps one uplink, never forwards. Cheapest to run, and the default. */
	LG_ROLE_LEAF = 0,
	/** Forwards for others and beacons so new nodes can find the grid. */
	LG_ROLE_ROUTER = 1,
	/** Router that also terminates traffic; rank 0 of the routing tree. */
	LG_ROLE_BACKHAUL = 2,
};

/** @name Flags for lg_send()
 *  @{
 */
/** Retransmit until acknowledged by the next hop (default is one attempt). */
#define LG_TX_RELIABLE BIT(0)
/** Bring the next connection event forward instead of waiting for the anchor. */
#define LG_TX_URGENT   BIT(1)
/** @} */

/** Metadata for a received message. */
struct lg_rx_info {
	/** Node that originated the message. */
	lg_addr_t src;
	/** Neighbour the message arrived from. */
	lg_addr_t prev_hop;
	/** Application port. */
	uint8_t port;
	/** RSSI of the frame that carried it, in dBm. */
	int8_t rssi;
	/** Hops travelled so far. */
	uint8_t hops;
};

/** Link and topology events. */
enum lg_evt {
	/** An authenticated link to @p peer is up and scheduled. */
	LG_EVT_LINK_UP,
	/** The link to @p peer was lost (supervision timeout or teardown). */
	LG_EVT_LINK_DOWN,
	/** @p peer became this node's preferred uplink. */
	LG_EVT_PARENT_CHANGED,
	/** This node lost its last route to a backhaul. */
	LG_EVT_ROUTE_LOST,
	/** A join attempt from @p peer failed to authenticate. */
	LG_EVT_AUTH_FAILED,
	/** This node took on or gave up a role; read the new one with lg_role(). */
	LG_EVT_ROLE_CHANGED,
};

/**
 * @brief Called for each message delivered to this node.
 *
 * Runs from the LockGrid work queue. @p data is only valid for the duration of
 * the call.
 */
typedef void (*lg_rx_cb_t)(const struct lg_rx_info *info, const uint8_t *data, size_t len);

/**
 * @brief Called when a link or the routing topology changes.
 *
 * Runs from the LockGrid work queue.
 */
typedef void (*lg_evt_cb_t)(enum lg_evt evt, lg_addr_t peer);

/** Application hooks. Either member may be NULL. */
struct lg_callbacks {
	lg_rx_cb_t rx;
	lg_evt_cb_t evt;
};

/** Identity material for this node. */
struct lg_identity {
	/** Grid this node belongs to. Scopes addresses, keys and PHY filters. */
	uint32_t network_id;
	/**
	 * LockGrid certificate for this node.
	 *
	 * Admission to the network, and a binding between this node and its
	 * address. It carries no role: what the node does for the grid is decided
	 * at runtime.
	 */
	const uint8_t *cert;
	size_t cert_len;
	/** Raw P-256 private key matching the certificate's public key. */
	const uint8_t *priv_key;
	size_t priv_key_len;
	/** Uncompressed P-256 public key of the grid's certificate authority. */
	const uint8_t *ca_pub_key;
	size_t ca_pub_key_len;
	/**
	 * 16 byte grid key, shared by every node in the network.
	 *
	 * This protects beacons and lets a node reject an outsider's traffic
	 * cheaply. It is not what authenticates a peer: forming a link still
	 * requires a valid certificate, so leaking the grid key does not let
	 * anyone join.
	 */
	const uint8_t *net_key;
	size_t net_key_len;
};

/**
 * @brief Provide this node's identity.
 *
 * Must be called before lg_start(). The pointed-to buffers must stay valid
 * until lg_stop().
 */
int lg_provision(const struct lg_identity *id);

/**
 * @brief Register application callbacks.
 */
void lg_callbacks_register(const struct lg_callbacks *cb);

/**
 * @brief Bring the node up.
 *
 * Starts discovery. The node will scan for beacons, authenticate against a
 * neighbour, settle into the routing tree and decide for itself whether it should
 * be a leaf or a router, without further calls.
 *
 * There is deliberately no role argument. A node cannot know in advance whether
 * the grid will need it to forward: that depends on how many routers turn out to
 * be in range and whether anything nearby is left without a parent.
 *
 * A backhaul-capable node should call lg_backhaul_set() when its uplink comes up.
 */
int lg_start(void);

/**
 * @brief Declare whether this node's backhaul uplink is available.
 *
 * Backhaul is the one role a node cannot elect itself into, because it means
 * having somewhere to terminate traffic. Call this from whatever owns the uplink.
 * Losing it takes effect at once rather than waiting out the usual settling time,
 * so the node stops advertising itself as a sink it no longer is; it stays a
 * router, since it is still useful as a relay.
 *
 * @retval 0 Applied.
 */
int lg_backhaul_set(bool uplink_up);

/** @brief The role this node has currently taken on. */
enum lg_role lg_role(void);

/**
 * @brief Take the node down, tearing every link down and stopping the radio.
 */
int lg_stop(void);

/**
 * @brief Queue a message for delivery.
 *
 * @param dst  Destination: a node address, ::LG_ADDR_BACKHAUL or
 *             ::LG_ADDR_BROADCAST.
 * @param port Application port, delivered verbatim to the peer's rx callback.
 * @param buf  Payload, at most ::LG_PAYLOAD_MAX bytes.
 * @param len  Payload length.
 * @param flags Bitwise OR of ::LG_TX_RELIABLE and ::LG_TX_URGENT.
 *
 * @retval 0        Queued.
 * @retval -ENOTCONN No route towards @p dst right now.
 * @retval -ENOMEM  Transmit queue full.
 * @retval -EMSGSIZE @p len exceeds ::LG_PAYLOAD_MAX.
 */
int lg_send(lg_addr_t dst, uint8_t port, const void *buf, size_t len, uint8_t flags);

/**
 * Duty-cycle parameters that can be changed while the node is running.
 *
 * These are the knobs that decide steady-state current, so they are the ones a
 * product needs to move without a rebuild - to trade latency for battery when a
 * device is on its last 10%, or to tighten up when it is on mains. The Kconfig
 * values are the defaults; anything set here overrides them from the next event
 * onwards.
 *
 * Existing links keep the interval they negotiated. A new interval applies to
 * links established afterwards, because changing the interval of a live link
 * needs both ends to agree at a shared event counter.
 */
struct lg_tuning {
	/** Connection interval for new links, in microseconds. */
	uint32_t conn_interval_us;
	/**
	 * Largest subrate factor an idle link may back off to.
	 *
	 * The single biggest lever on current: a skipped event costs nothing at
	 * all, and the fixed cost of starting the radio is a sixth of an idle
	 * event, so skipping is worth much more than the air time it avoids. It is
	 * clamped against the supervision timeout, since a link whose events are
	 * further apart than the timeout can never prove it is alive.
	 */
	uint8_t subrate_max;
	/** Supervision timeout for new links, in microseconds. */
	uint32_t sup_timeout_us;
	/** Scan period while unattached with nothing tracked. */
	uint32_t scan_unattached_us;
	/** Scan period once routed but short of the link target. */
	uint32_t scan_seeking_us;
	/** Scan period once attached and at the link target. */
	uint32_t scan_attached_us;
	/** Length of one blind scan window. */
	uint32_t scan_window_us;
	/** Nominal beacon interval for routers. */
	uint32_t beacon_interval_us;
	/** Multiplier applied to that when no joiner has been heard. */
	uint8_t beacon_idle_factor;
	/** Longest a router may go without opening a join window. */
	uint32_t join_window_floor_us;
};

/** @brief Read the parameters currently in force. */
void lg_tuning_get(struct lg_tuning *out);

/**
 * @brief Change the duty-cycle parameters.
 *
 * Values are clamped to what the protocol can honour - notably the subrate
 * ceiling against the supervision timeout - so the effect can be read back with
 * lg_tuning_get().
 *
 * @retval 0 Applied.
 * @retval -EINVAL @p t is NULL.
 */
int lg_tuning_set(const struct lg_tuning *t);

/** @brief This node's address, derived from its certificate. */
lg_addr_t lg_local_addr(void);

/** @brief Cost of this node's best path to a backhaul, ::LG_RANK_INFINITE if none. */
uint16_t lg_rank(void);

/** @brief Current preferred uplink, ::LG_ADDR_INVALID if unattached. */
lg_addr_t lg_parent(void);

/** @brief Whether this node can currently reach a backhaul. */
bool lg_has_route(void);

/** @brief Number of neighbours currently authenticated and on a schedule. */
uint8_t lg_link_count(void);

/** Per-neighbour view exposed for diagnostics. */
struct lg_neighbor_info {
	lg_addr_t addr;
	/** True once the certificate handshake completed and the link is scheduled. */
	bool linked;
	/** True if this neighbour is in the parent set. */
	bool parent;
	enum lg_role role;
	/** Neighbour's advertised rank. */
	uint16_t peer_rank;
	/** Cost of this link alone, in the same units as the rank. */
	uint16_t link_cost;
	/** Filtered RSSI in dBm. */
	int8_t rssi;
	/** Expected transmissions per delivered frame, in 1/16ths. */
	uint16_t etx_q4;
	/** Packet error rate over this link, in tenths of a percent. */
	uint16_t per_permille;
	/** Negotiated connection interval. */
	uint32_t interval_us;
	/** Events actually used out of every @c subrate. */
	uint8_t subrate;
	/** Current receive window, showing the effect of clock drift. */
	uint32_t rx_window_us;
};

/**
 * @brief Visit every known neighbour.
 *
 * @param cb Called once per neighbour; return false to stop early.
 */
void lg_neighbor_foreach(bool (*cb)(const struct lg_neighbor_info *info, void *user),
			 void *user);

/** Radio energy split by what the node was doing. */
struct lg_power_report {
	/** Wall time the accounting covers. */
	uint64_t elapsed_us;
	/**
	 * Total time the radio was on: receiving, transmitting or ramping.
	 *
	 * This is the headline number. Everything else in this structure exists
	 * to explain it.
	 */
	uint64_t radio_on_us;
	/** Charge drawn by the radio, in nano-coulombs. */
	uint64_t total_nc;
	/** Time in receive, split by purpose. */
	uint64_t rx_us_scan;
	uint64_t rx_us_link;
	uint64_t rx_us_join;
	uint64_t rx_us_chan_eval;
	/** Time transmitting, split by purpose. */
	uint64_t tx_us_beacon;
	uint64_t tx_us_link;
	uint64_t tx_us_join;
	/**
	 * Fixed cost of starting the radio, and of turning it round inside an
	 * event: pre-processing, crystal ramp, settle, radio start, the transmit
	 * to receive switch and the standby between the two halves.
	 *
	 * Charged whether the event carries a payload or nothing at all, which is
	 * why skipping an event is worth so much more than its air time alone.
	 */
	uint64_t fixed_us;
	uint64_t fixed_nc;
	uint32_t radio_starts;
	uint32_t turnarounds;
	/** Deprecated alias for @c fixed_us. */
	uint64_t ramp_us;
	/** Average current over @c elapsed_us, in nanoamps. */
	uint64_t avg_na;
	/**
	 * Average current with scanning and joining removed.
	 *
	 * Those are paid once at power-on. Battery life is set by what is left
	 * afterwards, so this is the figure to design against.
	 */
	uint64_t steady_na;
	/** Fraction of wall time the radio was on, in tenths of a percent. */
	uint32_t duty_permille;
};

/** @brief Snapshot the energy accounting. */
void lg_power_report_get(struct lg_power_report *out);

/** @brief As lg_power_report_get(), and also fill in @c steady_na. */
void lg_power_report_get_steady(struct lg_power_report *out);

/** @brief Log the energy accounting at info level. */
void lg_power_report_print(void);

/** Protocol counters, useful for tests and field diagnostics. */
struct lg_stats {
	uint32_t beacons_tx;
	uint32_t beacons_rx;
	uint32_t scans;
	/** Narrow windows opened at a predicted beacon rather than blind. */
	uint32_t tracked_beacons;
	uint32_t joins_ok;
	uint32_t joins_failed;
	uint32_t link_events;
	uint32_t link_events_skipped;
	uint32_t frames_tx;
	uint32_t frames_rx;
	uint32_t frames_retx;
	uint32_t crc_errors;
	uint32_t rx_timeouts;
	uint32_t auth_failures;
	uint32_t replays_dropped;
	uint32_t msgs_forwarded;
	uint32_t msgs_dropped_no_route;
	uint32_t parent_switches;
	uint32_t supervision_timeouts;
	uint32_t role_changes;
	uint32_t chan_map_updates;
	uint32_t channels_blacklisted;
	/** Timeslots the radio arbiter refused, only non-zero on the MPSL backend. */
	uint32_t slots_blocked;
};

/** @brief Snapshot the protocol counters. */
void lg_stats_get(struct lg_stats *out);

/** @brief Log the protocol counters and the channel map at info level. */
void lg_stats_print(void);

/**
 * @brief Emit a trace snapshot of this node's state and energy so far.
 *
 * Does nothing unless CONFIG_LOCKGRID_TRACE is set. Call it periodically from the
 * application: the visualisation uses these samples to plot radio on-time and to
 * show a node's rank and parent at any point on the timeline, neither of which can
 * be reconstructed from events alone once a run has been going for a while.
 */
void lg_trace_snapshot(void);

/** @brief Bitmap of channels currently considered usable. Bit 0 is channel 11. */
uint16_t lg_channel_map(void);

#ifdef __cplusplus
}
#endif

#endif /* LOCKGRID_INCLUDE_LOCKGRID_H_ */
