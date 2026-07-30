/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_internal.h
 * @brief Shared state and cross-module prototypes.
 *
 * Threading model: the radio runs entirely from ISR context, driven by the
 * scheduler in lg_sched.c. Anything that needs more than a few microseconds -
 * public key operations, routing decisions, application callbacks - is deferred
 * to the LockGrid work queue. Frames a link needs to send are built and
 * encrypted ahead of the event so the interrupt path only has to point the
 * radio at a buffer.
 */

#ifndef LOCKGRID_LG_INTERNAL_H_
#define LOCKGRID_LG_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <lockgrid/lockgrid.h>
#include "lg_frame.h"
#include "lg_phy.h"
#include "lg_ts.h"

/** ETX is carried in sixteenths, so a link that never retransmits is 16. */
#define LG_ETX_ONE 16U

/** Sizes of the certificate primitives, all raw P-256 / SHA-256. */
#define LG_EC_PUB_LEN  64
#define LG_EC_PRIV_LEN 32
#define LG_SIG_LEN     64
#define LG_HASH_LEN    32
#define LG_KEY_LEN     16
#define LG_NONCE_LEN   8

/** A LockGrid certificate: the signed part, then the CA's signature over it. */
#define LG_CERT_BODY_LEN (2 + 1 + 1 + 4 + 4 + LG_EC_PUB_LEN)
#define LG_CERT_LEN      (LG_CERT_BODY_LEN + LG_SIG_LEN)

/** Offsets into a certificate. */
#define LG_CERT_OFF_ADDR    0  /* uint16, becomes the node's short address */
#define LG_CERT_OFF_ROLE    2  /* uint8, highest role the node may claim */
#define LG_CERT_OFF_FLAGS   3
#define LG_CERT_OFF_NOTBEF  4  /* uint32 seconds */
#define LG_CERT_OFF_NOTAFT  8
#define LG_CERT_OFF_PUB    12
#define LG_CERT_OFF_SIG    (LG_CERT_BODY_LEN)

/** Where a neighbour is in its lifecycle. */
enum lg_neigh_state {
	/** Slot unused. */
	LG_NS_FREE = 0,
	/** Beacon heard, nothing established. */
	LG_NS_HEARD,
	/** Certificate handshake or resumption in progress. */
	LG_NS_JOINING,
	/** Authenticated, keyed and on a schedule. */
	LG_NS_LINKED,
	/** Was linked, supervision expired. Link key kept for a fast resume. */
	LG_NS_LOST,
};

/** Steps of the join exchange, each one its own radio event. */
enum lg_join_step {
	LG_JOIN_IDLE = 0,
	LG_JOIN_SEND_REQ,
	LG_JOIN_WAIT_RSP,
	LG_JOIN_SEND_CFM,
	LG_JOIN_WAIT_ACK,
	/* Responder side */
	LG_JOIN_WAIT_REQ,
	LG_JOIN_SEND_RSP,
	LG_JOIN_WAIT_CFM,
	LG_JOIN_SEND_ACK,
	/* Resumption */
	LG_JOIN_SEND_RESUME,
	LG_JOIN_WAIT_RESUME_RSP,
	LG_JOIN_WAIT_RESUME,
	LG_JOIN_DONE,
	LG_JOIN_FAILED,
};

/** Largest handshake message, split across frames inside one event. */
#define LG_XFER_MAX 288

/** Reassembly and staging buffer for a handshake message. */
struct lg_xfer {
	uint8_t buf[LG_XFER_MAX];
	uint16_t len;
	uint16_t off;
	uint8_t frags_seen;
	bool complete;
};

/** Everything LockGrid knows about one other node. */
struct lg_neigh {
	enum lg_neigh_state state;
	lg_addr_t addr;

	/* --- What the neighbour advertises --- */
	uint8_t role;
	uint16_t peer_rank;
	uint16_t peer_chan_map;
	uint8_t peer_links;
	uint8_t peer_capacity;

	/* --- Link schedule --- */
	bool initiator;
	uint64_t anchor_us;
	/**
	 * Anchor of event zero.
	 *
	 * The event counter is derived from the distance between @c anchor_us and
	 * this, rather than being incremented locally. Both ends hop and apply
	 * parameter changes by counter, so a counter that depended on how many
	 * events each side happened to skip would diverge the moment one of them
	 * missed one - and diverge permanently, because they would then be on
	 * different channels. Re-anchoring shifts this by the same amount as
	 * @c anchor_us, so correcting for drift never changes which event it is.
	 */
	uint64_t epoch_us;
	uint32_t interval_us;
	uint16_t counter;
	uint16_t chan_map;
	uint16_t hop_seed;
	uint8_t subrate;
	uint8_t subrate_max;
	/** Event counter at which a pending parameter change takes effect. */
	uint16_t instant;
	uint8_t pending_ctrl;
	uint16_t pending_chan_map;
	uint8_t pending_subrate;
	/**
	 * Set once the peer has been seen to reply after our parameter change
	 * went out.
	 *
	 * A change the initiator originates must not take effect until it is
	 * known to have arrived. Applying it on schedule regardless means that a
	 * change carried by a frame the peer never received leaves the two ends
	 * selecting different events - one listening while the other sleeps - with
	 * no way to repair it, because repairing it needs a frame to get through.
	 */
	bool ctrl_acked;
	/** The staged frame is the parameter change, so a reply confirms it. */
	bool tx_was_ctrl;

	/* --- Synchronisation --- */
	uint64_t last_sync_us;
	uint16_t peer_sca_ppm;
	uint32_t rx_window_us;
	int32_t sync_err_us;

	/* --- Supervision --- */
	uint64_t last_rx_us;
	uint32_t sup_timeout_us;
	uint8_t consec_misses;

	/* --- Quality --- */
	int8_t rssi;
	uint16_t etx_q4;
	uint16_t per_permille;
	uint16_t link_cost;
	uint32_t tx_attempts;
	uint32_t tx_acked;

	/* --- Sequencing and keys --- */
	uint8_t tx_seq;
	uint8_t rx_seq;
	bool rx_seq_valid;
	uint32_t tx_ctr;
	uint32_t rx_ctr;
	uint32_t replay_mask;
	uint8_t link_key[LG_KEY_LEN];
	/** PSA key identifier for @c link_key, imported once when the link comes up. */
	uint32_t key_id;
	bool has_key;
	/** Kept across a loss so re-attaching costs one round trip. */
	uint8_t ltk[LG_KEY_LEN];
	bool has_ltk;

	/* --- Handshake --- */
	enum lg_join_step join_step;
	uint64_t join_next_us;
	uint8_t join_retries;
	/** Attempts are refused until this time, so a bad router is not hammered. */
	uint64_t join_backoff_us;
	/** Random offset into the router's contention window for our slot 0. */
	uint32_t join_spread_us;
	uint8_t nonce_local[LG_NONCE_LEN];
	uint8_t nonce_peer[LG_NONCE_LEN];
	uint8_t eph_pub_peer[LG_EC_PUB_LEN];
	uint8_t peer_static_pub[LG_EC_PUB_LEN];
	uint8_t transcript[LG_HASH_LEN];

	/* --- Staged outbound frame for the next event --- */
	uint8_t txbuf[LG_PDU_MAX];
	uint8_t txlen;
	bool tx_pending;
	bool tx_needs_ack;
	/** Set when the staged frame is a retransmission. */
	uint8_t tx_tries;

	/* --- Beacon tracking --- */
	/**
	 * When this neighbour's next beacon is due, from its own advertisement.
	 * Zero when unknown, in which case only a blind scan can find it.
	 */
	uint64_t next_beacon_us;
	/** Consecutive predicted beacons that did not arrive. */
	uint8_t track_misses;

	/* --- Routing bookkeeping --- */
	bool is_parent;
	uint64_t route_adv_us;
	/** Whether this neighbour has been told about the latest rank change. */
	bool adv_urgent_done;
};

/** A destination reachable through one of our neighbours. */
struct lg_route {
	lg_addr_t dst;
	lg_addr_t next_hop;
	uint8_t hops;
	uint64_t expiry_us;
};

/** Nodes we have only heard about second hand, over an existing link. */
#define LG_GOSSIP_SIZE 4

/**
 * A candidate neighbour learned from a linked peer's route advertisement.
 *
 * Gossip is how a settled node finds out that a better uplink exists without
 * paying for receive time. It cannot establish the link on its own - the
 * candidate's schedule is unknown - but it turns open-ended scanning into a
 * short scan that stops as soon as the wanted address is heard.
 */
struct lg_gossip {
	lg_addr_t addr;
	uint16_t rank;
	uint64_t heard_us;
};

/** A message waiting to go out. */
struct lg_txmsg {
	bool used;
	lg_addr_t final;
	lg_addr_t origin;
	uint8_t flags;
	uint8_t port;
	uint8_t hops;
	uint16_t msg_id;
	uint8_t len;
	uint8_t retries;
	uint64_t enqueued_us;
	uint8_t payload[LG_PAYLOAD_MAX];
};

/** Per-channel quality, indexed by channel number minus ::LG_CHAN_FIRST. */
struct lg_chan_stat {
	uint16_t frames_ok;
	uint16_t frames_bad;
	uint16_t per_permille;
	int8_t noise_dbm;
	bool blacklisted;
	/** Set while the channel is being retried after having been retired. */
	bool probing;
	uint64_t last_eval_us;
};

/** What a granted radio event is for. */
enum lg_ev_kind {
	LG_EV_NONE = 0,
	/** Periodic exchange with a linked neighbour. */
	LG_EV_LINK,
	/** Transmit a beacon on a rendezvous channel. */
	LG_EV_BEACON,
	/** Listen for beacons on a rendezvous channel. */
	LG_EV_SCAN,
	/**
	 * Listen for one specific neighbour's beacon at the time it said it
	 * would send it. Costs a few milliseconds instead of a blind window.
	 */
	LG_EV_TRACK,
	/** One step of a handshake, transmit side. */
	LG_EV_JOIN_TX,
	/** One step of a handshake, receive side. */
	LG_EV_JOIN_RX,
	/** Sample the noise floor on one channel. */
	LG_EV_CHAN_EVAL,
};

struct lg_event;

/**
 * Per-kind handlers for a granted radio event.
 *
 * The scheduler owns the single event slot and forwards the PHY's interrupt
 * callbacks to whichever kind is running, so each kind gets a small state
 * machine of its own without the scheduler knowing anything about it.
 */
struct lg_ev_ops {
	/** Radio time has started; command the first transmission or window. */
	void (*start)(struct lg_event *ev);
	void (*tx_done)(struct lg_event *ev, uint64_t air_end_us);
	void (*rx_done)(struct lg_event *ev, const struct lg_phy_rx *rx);
	void (*rx_fail)(struct lg_event *ev, enum lg_phy_rx_fail reason);
	/** The arbiter refused or withdrew the slot; no radio time happened. */
	void (*aborted)(struct lg_event *ev);
};

/** The single scheduled radio event. */
struct lg_event {
	enum lg_ev_kind kind;
	/** When the radio time must start, which is what the arbiter is asked for. */
	uint64_t at_us;
	/** Protocol anchor: when the first bit of the first frame is due on air. */
	uint64_t anchor_us;
	/** For receive events, when the window must close. */
	uint64_t close_us;
	/**
	 * Half-width of the receive window, fixed when the event was planned.
	 *
	 * Recomputing this at event start would give a slightly wider window,
	 * because the drift term grows with time, and the wider window would open
	 * before the radio time that was reserved for it - so the event would be
	 * dropped as late. Planning it once is what keeps that from happening.
	 */
	uint32_t window_us;
	uint32_t len_us;
	uint8_t chan;
	/** Neighbour this event belongs to, NULL for scan, beacon and eval. */
	struct lg_neigh *neigh;
	bool urgent;
	const struct lg_ev_ops *ops;
	/** Buffer the PHY receives into for this event. */
	uint8_t *rxbuf;
};

/**
 * Lead time between the arbiter granting the radio and the earliest moment the
 * radio has to be commanded. Covers ramp-up, the transmit chain delay and a
 * little slack for the interrupt path.
 */
#define LG_SCHED_LEAD_US (LG_RAMPUP_US + 60)

/** Node-wide state. */
struct lg_ctx {
	bool started;
	uint32_t network_id;
	uint16_t netid16;
	lg_addr_t addr;
	/** The role currently taken on, decided at runtime by lg_role.c. */
	enum lg_role role;
	/**
	 * Whether the application says this node has a backhaul uplink and it is
	 * up.
	 *
	 * Not a certificate matter. The certificate admits a node to the network and
	 * nothing more; whether a node can terminate traffic depends on whether it
	 * has somewhere to terminate it, which is a fact about the hardware that
	 * only the application knows.
	 */
	bool backhaul_active;
	/** Last time a neighbour with no route of its own was heard. */
	uint64_t unrouted_seen_us;
	/** When the role last changed, so a marginal node cannot oscillate. */
	uint64_t role_changed_us;

	/* Identity */
	const uint8_t *cert;
	size_t cert_len;
	const uint8_t *priv_key;
	size_t priv_key_len;
	const uint8_t *ca_pub;
	size_t ca_pub_len;
	uint8_t net_key[LG_KEY_LEN];

	/* Routing */
	uint16_t rank;
	uint8_t link_count;
	uint16_t msg_id_next;

	struct lg_neigh neigh[CONFIG_LOCKGRID_MAX_NEIGHBORS];
	struct lg_route routes[CONFIG_LOCKGRID_MAX_ROUTES];
	struct lg_txmsg txq[CONFIG_LOCKGRID_TX_QUEUE_SIZE];
	struct lg_chan_stat chan[LG_CHAN_COUNT];
	uint16_t chan_map;

	/* Discovery */
	uint64_t next_beacon_us;
	uint64_t next_scan_us;
	uint64_t next_chan_eval_us;
	uint8_t beacons_heard_this_window;
	uint8_t scan_chan_idx;
	uint8_t beacon_chan_idx;
	struct lg_gossip gossip[LG_GOSSIP_SIZE];
	/** Address a targeted scan is looking for, ::LG_ADDR_INVALID if none. */
	lg_addr_t want_addr;
	uint64_t want_until_us;
	/** When this node will open a join window, 0 if none is pending. */
	uint64_t join_win_us;
	/**
	 * Last time we heard a beacon from a node we have no link with.
	 *
	 * A join window is 20 ms of receive, so opening one after every beacon
	 * would cost a router more than all its links put together. It is only
	 * worth opening when there is reason to think somebody is trying to
	 * join, and an unlinked node beaconing nearby is that reason.
	 */
	uint64_t joiner_hint_us;
	/**
	 * When the set of links last changed, either way.
	 *
	 * Bounds how long a node short of its link target keeps listening for
	 * joiners. While the neighbourhood is still settling that listening is
	 * exactly what forms the grid, but a router that simply has no more
	 * neighbours within range would otherwise listen for the rest of its life
	 * for somebody who is never coming.
	 */
	uint64_t link_change_us;
	/**
	 * Set when this node's own rank has changed and neighbours have not been
	 * told yet.
	 *
	 * Route advertisements are otherwise sent on a 30 s timer, which is the
	 * right steady-state cost - gossip over an existing link buys topology
	 * knowledge for no extra receive time. But a timer alone means a rank
	 * change takes up to 30 s per hop to propagate, and the change that matters
	 * most is a rank going to infinity: until the news arrives, neighbours go on
	 * forwarding towards a sink that can no longer terminate anything. So a
	 * change is pushed on the next link event instead of waiting.
	 */
	bool adv_urgent;
	/**
	 * Whether the beacon currently staged promises a join window.
	 *
	 * The decision has to be made when the beacon is built, not when it is
	 * sent, because it is what the beacon tells listeners. Advertising a window
	 * and then not opening it is worse than not advertising one at all: a joiner
	 * aims at it, hears nothing, and counts a failed handshake against a router
	 * that was never listening - eventually backing off from a perfectly good
	 * neighbour.
	 */
	bool beacon_offers_join;
	/**
	 * When this node last opened a join window.
	 *
	 * Backstops the hint above. A leaf never beacons, so it can never produce
	 * the hint, and a router that has everything it wants would otherwise
	 * never open a window - leaving a newly powered leaf permanently unable to
	 * get in even though it can hear the beacons perfectly.
	 */
	uint64_t last_join_window_us;
	uint32_t beacon_epoch;
	/** Anchor advertised as the successor of the beacon currently staged. */
	uint64_t beacon_planned_us;

	/* Current event */
	struct lg_event ev;

	struct lg_callbacks cb;
	struct lg_stats stats;
	/** Live duty-cycle parameters; Kconfig supplies the initial values. */
	struct lg_tuning tune;
};

extern struct lg_ctx lg;

/** Duplicate suppression window for forwarded messages. */
#define LG_DUP_CACHE_SIZE 16

/*
 * ---------------------------------------------------------------------------
 * lg_core.c
 * ---------------------------------------------------------------------------
 */

/** @brief Run @p fn on the LockGrid work queue. Safe from ISR context. */
void lg_defer(struct k_work *work);

/** @brief The LockGrid work queue, for modules that own their own work items. */
struct k_work_q *lg_workq(void);

/** @brief Report an event to the application from the work queue. */
void lg_notify(enum lg_evt evt, lg_addr_t peer);

/**
 * @brief Hand a received frame to the work queue for decryption and dispatch.
 *
 * Called from the radio interrupt. Public key work and application callbacks
 * must not run there, and even AES-CCM is kept out so an event's timing cannot
 * be disturbed by it.
 */
void lg_rx_post(const uint8_t *pdu, uint8_t len, int8_t rssi, uint8_t chan,
		uint64_t air_start_us);

/** @brief Load the Kconfig defaults into lg.tune. */
void lg_tuning_init(void);

/**
 * @brief Largest subrate that still lets a link prove it is alive.
 *
 * Events must be frequent enough that a couple of losses do not exhaust the
 * supervision timeout, so the ceiling depends on both the interval and the
 * timeout rather than being a free choice.
 */
uint8_t lg_subrate_ceiling(uint32_t interval_us, uint32_t sup_timeout_us);

/** @brief Half-width of the receive window @p n currently needs. */
uint32_t lg_link_window_us(struct lg_neigh *n);

/** @brief Find a queued message whose next hop is @p peer, NULL if none. */
struct lg_txmsg *lg_txq_peek(lg_addr_t peer);

/** @brief Release a queued message. */
void lg_txq_free(struct lg_txmsg *m);

/** @brief True if anything is queued for @p peer. */
bool lg_txq_pending(lg_addr_t peer);

/*
 * ---------------------------------------------------------------------------
 * lg_sched.c
 * ---------------------------------------------------------------------------
 */

int lg_sched_init(void);
void lg_sched_stop(void);

/**
 * @brief Recompute which event is next and ask the arbiter for it.
 *
 * Idempotent, and safe to call from either context. Every module calls this
 * after changing anything that affects timing.
 */
void lg_sched_kick(void);

/** @brief Bring a link's next event forward because traffic is waiting. */
void lg_sched_expedite(struct lg_neigh *n);

/**
 * @brief Declare the running event finished.
 *
 * Releases the radio and asks for the next event. Safe from ISR context, which
 * is where it is normally called from.
 */
void lg_sched_event_done(void);

/**
 * @brief End the running event early.
 *
 * Used when an event has already achieved its purpose and is now standing
 * between the protocol and something more important - a scan window that has
 * just heard the beacon it was looking for, for instance.
 */
void lg_sched_abort(enum lg_ev_kind kind);

/** @brief Handlers for each event kind, installed by the scheduler. */
extern const struct lg_ev_ops lg_link_ev_ops;
extern const struct lg_ev_ops lg_beacon_ev_ops;
extern const struct lg_ev_ops lg_scan_ev_ops;
extern const struct lg_ev_ops lg_track_ev_ops;
extern const struct lg_ev_ops lg_join_tx_ev_ops;
extern const struct lg_ev_ops lg_join_rx_ev_ops;
extern const struct lg_ev_ops lg_chan_eval_ev_ops;

/*
 * ---------------------------------------------------------------------------
 * lg_neigh.c
 * ---------------------------------------------------------------------------
 */

struct lg_neigh *lg_neigh_find(lg_addr_t addr);
struct lg_neigh *lg_neigh_alloc(lg_addr_t addr);
void lg_neigh_free(struct lg_neigh *n);
uint8_t lg_neigh_linked_count(void);

/** @brief Fold a transmission outcome into the neighbour's ETX and PER. */
void lg_neigh_tx_result(struct lg_neigh *n, bool acked);

/** @brief Fold a received frame's RSSI into the filtered value. */
void lg_neigh_rx_rssi(struct lg_neigh *n, int8_t rssi);

/** @brief Recompute ::lg_neigh.link_cost from ETX, RSSI and PHY. */
void lg_neigh_update_cost(struct lg_neigh *n);

/** @brief Drop the worst neighbour if the table is full and @p cand looks better. */
struct lg_neigh *lg_neigh_make_room(uint16_t cand_cost);

/*
 * ---------------------------------------------------------------------------
 * lg_link.c
 * ---------------------------------------------------------------------------
 */

/** @brief Put a freshly authenticated neighbour onto a schedule. */
int lg_link_establish(struct lg_neigh *n, const struct lg_link_params *p, bool initiator);

/** @brief Advance @p n past the events it just used or skipped. */
void lg_link_advance(struct lg_neigh *n);

/** @brief Absolute time of @p n's next event, accounting for subrating. */
uint64_t lg_link_next_event_us(struct lg_neigh *n);

/** @brief Channel @p n will use for its next event. */
uint8_t lg_link_next_chan(struct lg_neigh *n);

/** @brief Recompute the receive window from elapsed time and clock accuracy. */
void lg_link_update_window(struct lg_neigh *n);

/** @brief Run a granted link event. Called from ISR context. */
void lg_link_event_start(struct lg_neigh *n, uint64_t at_us);

/** @brief Note that a link event produced nothing and check supervision. */
void lg_link_event_miss(struct lg_neigh *n);

/** @brief Stage the next frame for @p n, encrypting it. Returns false if nothing to send. */
bool lg_link_stage_tx(struct lg_neigh *n);

/** @brief Tear a link down, keeping the long term key for a later resume. */
void lg_link_down(struct lg_neigh *n, enum lg_teardown_reason reason);

/** @brief Handle a decrypted frame that arrived on a link. */
void lg_link_rx_frame(struct lg_neigh *n, uint8_t fcf, uint8_t seq, const uint8_t *payload,
		      uint8_t len, int8_t rssi);

/** @brief Settle a finished link event: advance the schedule, restage. */
void lg_link_event_finish(struct lg_neigh *n);

/** @brief Queue a message that originated elsewhere, preserving its identity. */
int lg_send_forward(lg_addr_t origin, lg_addr_t final, uint8_t port, const void *buf, size_t len,
		    uint8_t flags, uint8_t hops, uint16_t msg_id);

/*
 * ---------------------------------------------------------------------------
 * lg_disc.c
 * ---------------------------------------------------------------------------
 */

void lg_disc_init(void);

/** @brief Absolute time of the next beacon, UINT64_MAX if this node does not beacon. */
uint64_t lg_disc_next_beacon_us(void);

/** @brief Absolute time of the next scan window. */
uint64_t lg_disc_next_scan_us(void);

/**
 * @brief Earliest predicted beacon worth waking for, UINT64_MAX if none.
 *
 * @param out Set to the neighbour whose beacon it is.
 */
uint64_t lg_disc_next_track_us(struct lg_neigh **out);

/** @brief Half-width of a tracked beacon window. */
#define LG_TRACK_MARGIN_US 1500

/** How long a targeted scan keeps looking before giving up. */
#define LG_WANT_TIMEOUT_US (20ULL * USEC_PER_SEC)

/**
 * How much better a gossiped node's rank must be before it is worth scanning for.
 *
 * A targeted scan is cheap but not free, and switching parents has its own cost,
 * so hearsay about a marginally better uplink is not worth acting on.
 */
#define LG_GOSSIP_SEEK_MARGIN 8U

/** @brief Note that a tracked beacon did or did not arrive. */
void lg_disc_track_result(struct lg_neigh *n, bool heard);

/** @brief Rendezvous channel for @p slot. Derived from the network id alone. */
uint8_t lg_disc_rdv_chan(uint8_t slot);

/** @brief Schedule the next beacon and stage its copies. Work queue only. */
void lg_disc_beacon_finish(void);

/** @brief Schedule the next scan window and rotate the channel. */
void lg_disc_scan_finish(void);

/** @brief Called from ISR context for every beacon received during a scan. */
void lg_disc_beacon_rx(const uint8_t *pdu, uint8_t len, int8_t rssi, uint64_t air_start_us);

/** @brief Reschedule discovery after the topology changed. */
void lg_disc_retune(void);

/*
 * ---------------------------------------------------------------------------
 * lg_chan.c
 * ---------------------------------------------------------------------------
 */

void lg_chan_init(void);

/** @brief Fold one frame outcome into channel @p chan's statistics. */
void lg_chan_result(uint8_t chan, bool ok);

/** @brief Record a noise floor measurement. */
void lg_chan_noise(uint8_t chan, int8_t dbm);

/** @brief Re-classify channels and return true if the map changed. */
bool lg_chan_reevaluate(void);

/** @brief Next channel due for a noise measurement. */
uint8_t lg_chan_next_eval(void);

/**
 * @brief Map an index into the used-channel list onto a channel number.
 *
 * Both ends of a link run this over the same map, so neither has to be told
 * which channel the other will use.
 */
uint8_t lg_chan_remap(uint16_t map, uint16_t index);

/** @brief Number of channels set in @p map. */
uint8_t lg_chan_map_count(uint16_t map);

/** @brief Channel for event @p counter of a link with seed @p seed. */
uint8_t lg_chan_hop(uint16_t map, uint16_t seed, uint16_t counter);

/*
 * ---------------------------------------------------------------------------
 * lg_route.c
 * ---------------------------------------------------------------------------
 */

void lg_route_init(void);

/** @brief Recompute rank and the parent set. Returns true if the parent changed. */
bool lg_route_recompute(void);

/** @brief Next hop for @p final, ::LG_ADDR_INVALID if unreachable. */
lg_addr_t lg_route_next_hop(lg_addr_t final, uint8_t flags);

/** @brief Learn that @p dst is reachable through @p via. */
void lg_route_add(lg_addr_t dst, lg_addr_t via, uint8_t hops);

/** @brief Forget everything that pointed through @p via. */
void lg_route_flush_via(lg_addr_t via);

/** @brief Build a route advertisement into @p buf. Returns its length. */
uint8_t lg_route_build_adv(uint8_t *buf, size_t cap);

/** @brief Consume a route advertisement from a neighbour. */
void lg_route_handle_adv(struct lg_neigh *n, const uint8_t *buf, uint8_t len);

/** @brief Consume a downward route registration from a neighbour. */
void lg_route_handle_reg(struct lg_neigh *n, const uint8_t *buf, uint8_t len);

/** @brief True if @p msg_id from @p origin was already forwarded. */
bool lg_route_is_duplicate(lg_addr_t origin, uint16_t msg_id);

/*
 * ---------------------------------------------------------------------------
 * lg_sec.c
 * ---------------------------------------------------------------------------
 */

int lg_sec_init(void);

/** @brief Fill @p out with @p len cryptographically random bytes. */
int lg_sec_rand(void *out, size_t len);

/**
 * @brief Encrypt and authenticate a staged frame in place.
 *
 * @param n       Link whose key and counter to use.
 * @param pdu     Frame starting at the header.
 * @param hdr_len Bytes of header to authenticate but not encrypt.
 * @param len     In: total length so far. Out: length including the tag.
 */
int lg_sec_encrypt(struct lg_neigh *n, uint8_t *pdu, uint8_t hdr_len, uint8_t *len);

/**
 * @brief Verify and decrypt a received frame in place.
 *
 * Also runs the replay filter, so a frame that authenticates but repeats a
 * counter is rejected here.
 *
 * @retval 0        Frame is authentic and @p len now covers the plaintext.
 * @retval -EBADMSG Tag mismatch.
 * @retval -EALREADY Replay.
 */
int lg_sec_decrypt(struct lg_neigh *n, uint8_t *pdu, uint8_t *len);

/** @brief Append the network-key MIC that protects a beacon. */
int lg_sec_beacon_seal(uint8_t *pdu, uint8_t *len);

/** @brief Check a beacon's MIC. Returns 0 if it verifies. */
int lg_sec_beacon_open(uint8_t *pdu, uint8_t *len);

/** @brief SHA-256 over an iovec-like list. */
int lg_sec_hash(const uint8_t *const *parts, const size_t *lens, size_t n, uint8_t *out);

/** @brief Generate an ephemeral P-256 key pair. */
int lg_sec_ecdh_keypair(uint8_t *pub, uint8_t *priv);

/** @brief Raw ECDH, writing the 32 byte shared X coordinate. */
int lg_sec_ecdh(const uint8_t *priv, const uint8_t *peer_pub, uint8_t *secret);

/** @brief Derive a 16 byte key from a shared secret and a transcript. */
int lg_sec_kdf(const uint8_t *secret, size_t secret_len, const uint8_t *salt, size_t salt_len,
	       const char *info, uint8_t *key_out);

/** @brief ECDSA P-256 over SHA-256 with this node's static key. */
int lg_sec_sign(const uint8_t *hash, uint8_t *sig);

/** @brief Verify an ECDSA P-256 signature. */
int lg_sec_verify(const uint8_t *pub, const uint8_t *hash, const uint8_t *sig);

/** @brief Check a certificate against the grid CA and extract its fields. */
int lg_sec_cert_verify(const uint8_t *cert, size_t len, lg_addr_t *addr, uint8_t *role,
		       uint8_t *pub_out);

/** @brief Install a derived link key on @p n and reset its counters. */
int lg_sec_install_key(struct lg_neigh *n, const uint8_t *key);

/** @brief Drop @p n's link key. The long term key is left alone. */
void lg_sec_forget_key(struct lg_neigh *n);

/*
 * ---------------------------------------------------------------------------
 * lg_join.c
 * ---------------------------------------------------------------------------
 */

void lg_join_init(void);

/** @brief Start authenticating towards @p n, resuming if we still hold its key. */
int lg_join_start(struct lg_neigh *n);

/** @brief Time of @p n's next handshake step, UINT64_MAX when idle. */
uint64_t lg_join_next_us(struct lg_neigh *n);

/** @brief True when the slot the handshake is waiting for is ours to transmit in. */
bool lg_join_slot_is_tx(void);

/** @brief Time this node will open a join window, UINT64_MAX if none. */
uint64_t lg_join_win_next_us(void);

/** @brief Advance the handshake after one of its events finishes. */
void lg_join_event_finish(void);

/** @brief Feed a received handshake frame in. Runs on the work queue. */
void lg_join_rx_pdu(uint8_t *pdu, uint8_t len, int8_t rssi, uint64_t air_start_us);

/** @brief Length of one handshake slot, used when reserving radio time. */
#define LG_JOIN_SLOT_LEN_US 20000

/**
 * @brief Receive span of a router's initial join window.
 *
 * Wider than a handshake slot because it is a contention window covering several
 * possible joiner start positions, not a rendezvous with a known peer.
 */
#define LG_JOIN_WINDOW_LEN_US 40000

/*
 * ---------------------------------------------------------------------------
 * lg_role.c
 * ---------------------------------------------------------------------------
 */

/** @brief Take the cheapest role the certificate allows, or backhaul if seeded. */
void lg_role_init(void);

/**
 * @brief Reconsider whether this node should be a leaf or a router.
 *
 * Cheap and idempotent; called whenever something that feeds the decision may
 * have changed - a link coming or going, a beacon heard, a scan finishing.
 */
void lg_role_evaluate(void);

/** @brief Note that a neighbour with no route was heard. */
void lg_role_note_unrouted(void);

/*
 * ---------------------------------------------------------------------------
 * lg_power.c
 * ---------------------------------------------------------------------------
 */

/** @brief What the radio is currently being used for, for the energy split. */
enum lg_pwr_act {
	LG_PWR_ACT_SCAN,
	LG_PWR_ACT_LINK,
	LG_PWR_ACT_JOIN,
	LG_PWR_ACT_BEACON,
	LG_PWR_ACT_CHAN_EVAL,
};

void lg_power_init(void);

/** @brief Attribute subsequent radio time to @p act. */
void lg_power_activity(enum lg_pwr_act act);

/** @brief Account @p us microseconds in @p state. Called from ISR context. */
void lg_power_charge(enum lg_phy_state state, uint32_t us);

/**
 * @brief Charge the fixed cost of bringing the radio up for an event.
 *
 * Called once per granted event, on the first transmission or receive window.
 */
void lg_power_event_start(void);

/** @brief Charge the fixed cost of turning the radio round inside an event. */
void lg_power_turnaround(void);

/*
 * ---------------------------------------------------------------------------
 * lg_frame.c
 * ---------------------------------------------------------------------------
 */

/** @brief Air time budget for a whole event, used when reserving radio time. */
uint32_t lg_frame_event_budget_us(uint32_t rx_window_us);

/*
 * ---------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------
 */

static inline void lg_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline uint16_t lg_get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline void lg_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t lg_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/** Exponentially weighted moving average with weight 1/8 on the new sample. */
static inline int32_t lg_ewma(int32_t old, int32_t new_sample, bool seeded)
{
	return seeded ? (old * 7 + new_sample) / 8 : new_sample;
}

#endif /* LOCKGRID_LG_INTERNAL_H_ */
