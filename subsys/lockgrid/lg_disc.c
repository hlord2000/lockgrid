/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_disc.c
 * @brief Beaconing and duty-cycled scanning.
 *
 * Discovery is the one part of a mesh that cannot be scheduled, because two
 * nodes that have never met share no timebase. It is therefore the expensive
 * part, and the design pushes as much of the cost as possible onto the side that
 * can afford it.
 *
 * Transmitting is cheap and receiving is not, so routers advertise and joiners
 * listen. A beacon event puts the same beacon on all three rendezvous channels
 * back to back, which means a scanner parked on any one of them will hear it.
 * The rendezvous channels come from the network id alone - not from the channel
 * map and not from a clock - because a node that has not joined yet has neither.
 *
 * Blind scanning is duty cycled, and the duty collapses as the node settles:
 *
 *   no route, nothing tracked   over half the time in receive, because a node
 *                               with no route has nothing better to spend
 *                               energy on and wants to be attached
 *   tracking a router           occasional, since a predicted beacon finds the
 *                               same router for a tenth of the receive time
 *   routed, short of links      occasional, redundancy is worth having but not
 *                               worth a high duty cycle
 *   routed and at target        one window a minute, as a sanity check
 *
 * The settled case can be that low because a routed node learns the topology
 * from route advertisements carried on links it already has, at no extra receive
 * cost. When gossip says a materially better uplink exists, lg_route.c sets a
 * wanted address; scanning goes aggressive briefly and stops the moment that
 * address is heard. Open-ended scanning is never how a settled node finds a
 * neighbour.
 *
 * Each beacon points at a join window a few milliseconds later, so a joiner does
 * not have to keep receiving between hearing a beacon and being able to talk.
 * The offset is measured from the beacon copy the joiner actually heard, so all
 * three copies point at the same absolute window.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <string.h>
#include "lg_internal.h"
#include "lg_trace.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/** Rendezvous channels. Three of the four that sit between the Wi-Fi channels. */
#define LG_RDV_SLOTS 3

/** Gap between the three copies of a beacon, enough to retune the radio. */
#define LG_BEACON_COPY_GAP_US 600

/** Delay from the last beacon copy to the join window. */
#define LG_JOIN_OFFSET_US 5000
/**
 * How long the beaconer keeps the join window open: one train per sub-slot, plus
 * slack for the joiner's clock offset.
 *
 * This is a contention window, not just a rendezvous. The router has to cover all
 * of it, which is the most expensive single receive in the protocol - which is why
 * lg_disc.c only opens one when a joiner is plausibly present.
 */
#define LG_JOIN_WINDOW_US (LG_JOIN_CONTEND_SLOTS * LG_JOIN_TRAIN_US + 2000)

/**
 * Air time of the longest slot-0 message: two full fragments plus the gap.
 */
#define LG_JOIN_TRAIN_US 9500

/**
 * Discrete sub-slots a joiner may start in.
 *
 * A joiner picks one at random and starts exactly on its boundary, so two joiners
 * that chose different sub-slots cannot overlap at all. Picking a uniformly random
 * microsecond inside a window barely wider than one train does not work: with a
 * 9.5 ms train the two are almost certain to collide, and both then fail, back off
 * and try again - which is what made a 25 node grid take minutes to converge and
 * produce twice as many failed handshakes as successful ones.
 */
#define LG_JOIN_CONTEND_SLOTS 4

/** Staged beacon copies, sealed on the work queue before the event runs. */
static struct {
	uint8_t pdu[LG_PDU_MAX];
	uint8_t len;
	uint8_t chan;
} beacon_copy[LG_RDV_SLOTS];

static uint8_t beacon_copy_idx;

/* ------------------------------------------------------------------------- */
/* Rendezvous channels                                                        */
/* ------------------------------------------------------------------------- */

uint8_t lg_disc_rdv_chan(uint8_t slot)
{
	/* The four channels that fall between the three non-overlapping Wi-Fi
	 * channels. Offsetting by the network id lets two grids in the same
	 * space avoid picking identical rendezvous channels.
	 */
	static const uint8_t pref[4] = {15, 20, 25, 26};

	return pref[(lg.network_id + slot) & 3U];
}

/* ------------------------------------------------------------------------- */
/* Scheduling                                                                 */
/* ------------------------------------------------------------------------- */

/** Uniform jitter of +/- 25% so nearby nodes do not fall into lockstep. */
static uint32_t jitter(uint32_t nominal_us)
{
	uint32_t span = nominal_us / 2U;

	if (!span) {
		return nominal_us;
	}
	return nominal_us - span / 2U + (sys_rand32_get() % span);
}

static bool should_beacon(void);
static bool worth_tracking(const struct lg_neigh *n);

/** True if some neighbour's next beacon is predicted and still useful. */
static bool tracking_someone(void)
{
	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		if (worth_tracking(&lg.neigh[i])) {
			return true;
		}
	}
	return false;
}

/**
 * Scan period, in three tiers.
 *
 * The jump between them is what the whole power argument rests on. A node with
 * no route spends most of its time receiving, because nothing else it could do
 * with that energy is worth anything. A node that has a route but would like a
 * second one looks occasionally - it is already useful, so paying a high duty
 * cycle for redundancy would be a poor trade, and with only one router in range
 * an aggressive rate would never succeed anyway. A node that is fully attached
 * scans once a minute purely to notice a grid that has changed shape around it.
 */
static uint32_t scan_interval_us(void)
{
	/* A targeted scan is short and ends the moment it hears its address, so
	 * it can afford the aggressive rate.
	 */
	if (lg.want_addr != LG_ADDR_INVALID && lg_ts_now_us() < lg.want_until_us) {
		return lg.tune.scan_unattached_us;
	}
	/* A live prediction is worth far more than a blind window: it finds the
	 * same router for a tenth of the receive time. While one is in hand,
	 * blind scanning is only needed to discover routers we have never heard,
	 * so it drops to the occasional rate even when unattached.
	 */
	if (tracking_someone()) {
		return lg.tune.scan_seeking_us;
	}
	if (!lg_has_route()) {
		return lg.tune.scan_unattached_us;
	}
	if (lg_neigh_linked_count() < CONFIG_LOCKGRID_MIN_LINKS_TARGET) {
		return lg.tune.scan_seeking_us;
	}
	return lg.tune.scan_attached_us;
}

void lg_disc_init(void)
{
	uint64_t now = lg_ts_now_us();

	lg.next_scan_us = now + jitter(20000);
	lg.next_beacon_us = should_beacon() ? now + jitter(lg.tune.beacon_interval_us)
					    : UINT64_MAX;
	lg.scan_chan_idx = 0;
	lg.beacons_heard_this_window = 0;
	lg.join_win_us = 0;
	lg.last_join_window_us = 0;
	lg.link_change_us = lg_ts_now_us();
	lg.beacon_epoch = 0;
	beacon_copy_idx = 0;
}

void lg_disc_retune(void)
{
	uint64_t now = lg_ts_now_us();
	uint32_t interval = scan_interval_us();

	/* Pull the next scan in or push it out to match the new posture, but
	 * never move it later than one full interval away.
	 */
	if (lg.next_scan_us > now + interval) {
		lg.next_scan_us = now + jitter(interval);
	}

	if (!should_beacon()) {
		lg.next_beacon_us = UINT64_MAX;
	} else if (lg.next_beacon_us == UINT64_MAX) {
		lg.next_beacon_us = now + jitter(lg.tune.beacon_interval_us);
	}

	if (lg.want_addr != LG_ADDR_INVALID && now >= lg.want_until_us) {
		lg.want_addr = LG_ADDR_INVALID;
	}
}

/**
 * Whether this node should be advertising at all.
 *
 * Routers and backhauls advertise so others can attach to them. A node with no
 * route advertises too, for the opposite reason: it is asking to be adopted.
 * Without that, an unattached leaf is invisible - it does not beacon, so no router
 * can tell it needs a parent, and in a settled grid no router has any reason to
 * open a join window. It would sit there hearing beacons it cannot act on.
 */
static bool should_beacon(void)
{
	return lg.role != LG_ROLE_LEAF || !lg_has_route();
}

uint64_t lg_disc_next_beacon_us(void)
{
	if (!should_beacon()) {
		return UINT64_MAX;
	}
	return lg.next_beacon_us;
}

uint64_t lg_disc_next_scan_us(void)
{
	return lg.next_scan_us;
}

/**
 * Is this neighbour's beacon still worth waking up for?
 *
 * Only if we might act on it. A neighbour we already have a link with beacons
 * for other people's benefit, not ours, and listening to it would be pure waste.
 */
static bool worth_tracking(const struct lg_neigh *n)
{
	if (!n->next_beacon_us || n->state == LG_NS_LINKED || n->state == LG_NS_JOINING) {
		return false;
	}
	if (n->join_retries >= 3) {
		return false;
	}
	/* Unattached, or this neighbour could improve on what we have. */
	return !lg_has_route() || lg_neigh_linked_count() < CONFIG_LOCKGRID_MIN_LINKS_TARGET ||
	       (n->peer_rank != LG_RANK_INFINITE && n->peer_rank + 4U < lg.rank);
}

uint64_t lg_disc_next_track_us(struct lg_neigh **out)
{
	uint64_t best = UINT64_MAX;
	uint64_t now = lg_ts_now_us();

	*out = NULL;

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		struct lg_neigh *n = &lg.neigh[i];

		if (!worth_tracking(n)) {
			continue;
		}
		/* A prediction that has already passed is stale; drop it so the
		 * node falls back to a blind scan rather than chasing it.
		 */
		if (n->next_beacon_us < now) {
			n->next_beacon_us = 0;
			continue;
		}
		if (n->next_beacon_us < best) {
			best = n->next_beacon_us;
			*out = n;
		}
	}
	return best;
}

void lg_disc_track_result(struct lg_neigh *n, bool heard)
{
	if (!n) {
		return;
	}
	if (heard) {
		n->track_misses = 0;
		return;
	}

	/* A prediction that keeps failing is worse than none, because it costs a
	 * window every interval and delivers nothing.
	 */
	if (++n->track_misses >= 3) {
		n->next_beacon_us = 0;
		n->track_misses = 0;
	} else {
		n->next_beacon_us = 0;
	}
}

/* ------------------------------------------------------------------------- */
/* Beacon construction                                                        */
/* ------------------------------------------------------------------------- */

static bool join_window_worthwhile(void);

/**
 * Build and seal the three copies of a beacon.
 *
 * Runs on the work queue because sealing needs AES. Each copy carries a
 * different join offset so that, whichever one a joiner hears, adding the offset
 * to that frame's arrival time lands on the same join window.
 */
static void beacon_stage(void)
{
	struct lg_beacon b;
	uint32_t total_air;
	uint8_t linked = lg_neigh_linked_count();

	b.network_id = lg.network_id;
	b.epoch = lg.beacon_epoch++;
	b.rank = lg.rank;
	b.chan_map = lg.chan_map;
	b.role = (uint8_t)lg.role;
	b.link_count = linked;
	b.link_capacity = (linked < CONFIG_LOCKGRID_MAX_LINKS)
				  ? (uint8_t)(CONFIG_LOCKGRID_MAX_LINKS - linked)
				  : 0;
	/* Committed here so the beacon cannot promise what beacon_tx_done will not
	 * deliver. A router with nothing to offer advertises a zero-length window,
	 * which a joiner reads as "not listening" and skips without cost.
	 */
	lg.beacon_offers_join = (linked < CONFIG_LOCKGRID_MAX_LINKS) && join_window_worthwhile();
	b.join_window_us = lg.beacon_offers_join ? LG_JOIN_WINDOW_US : 0;

	for (int i = 0; i < LG_RDV_SLOTS; i++) {
		uint32_t copy_off =
			(uint32_t)i * (LG_AIRTIME_US(LG_HDR_LEN + sizeof(b) + LG_BEACON_MIC_LEN) +
				       LG_BEACON_COPY_GAP_US);
		uint8_t hdr_len;
		uint8_t len;

		/* Air time from the start of copy i to the end of the last copy,
		 * so every copy points at the same window.
		 */
		total_air = 0;
		for (int j = i; j < LG_RDV_SLOTS; j++) {
			total_air += LG_AIRTIME_US(LG_HDR_LEN + sizeof(b) + LG_BEACON_MIC_LEN);
			if (j < LG_RDV_SLOTS - 1) {
				total_air += LG_BEACON_COPY_GAP_US;
			}
		}
		b.join_offset_us = (uint16_t)(total_air + LG_JOIN_OFFSET_US);

		/* Where the next beacon event starts, measured from this copy, so
		 * a listener that hears any copy can predict it.
		 */
		b.next_beacon_offset_us =
			(uint32_t)(lg.beacon_planned_us - (lg.next_beacon_us + copy_off));

		/* Beacons are broadcast and unencrypted: a node that has not
		 * joined has to be able to read one. They are authenticated with
		 * the grid key, which every provisioned node holds, so a beacon
		 * from outside the grid is discarded without a handshake.
		 */
		hdr_len = lg_frame_hdr_put(beacon_copy[i].pdu, lg.netid16, LG_FT_BEACON, 0,
					   LG_ADDR_BROADCAST, lg.addr);
		memcpy(&beacon_copy[i].pdu[hdr_len], &b, sizeof(b));
		len = hdr_len + sizeof(b);

		if (lg_sec_beacon_seal(beacon_copy[i].pdu, &len) != 0) {
			beacon_copy[i].len = 0;
			continue;
		}

		beacon_copy[i].len = len;
		beacon_copy[i].chan = lg_disc_rdv_chan(i);
	}
}

/* ------------------------------------------------------------------------- */
/* Beacon event                                                               */
/* ------------------------------------------------------------------------- */

/** How long an unlinked node's beacon keeps join windows open. */
#define LG_JOINER_HINT_LIFETIME_US (5ULL * USEC_PER_SEC)

/**
 * How long a node short of its link target keeps offering join windows after the
 * last change to its links.
 *
 * Long enough to cover a grid forming - a hundred nodes take a couple of minutes -
 * and short enough that a router with no further neighbours in range stops paying
 * for the possibility of one. Every link that comes or goes restarts it, because
 * that is evidence the neighbourhood is still moving.
 */
#define LG_JOIN_OFFER_GRACE_US (90ULL * USEC_PER_SEC)

/**
 * Is it worth spending receive time on a join window?
 *
 * Yes if this node is short of links itself, or if somebody unlinked has been
 * beaconing nearby recently - and in any case at least occasionally.
 *
 * That last clause is not belt-and-braces, it is the only way a leaf can ever
 * join a settled grid. Leaves do not beacon, so they cannot produce the hint,
 * and a router that already has everything it wants would otherwise stop opening
 * windows altogether. A newly powered leaf would then hear the beacons perfectly,
 * compute exactly when the window should be, transmit into it, and never be
 * heard. The floor costs one 20 ms window per period - about 0.07% duty at the
 * default - and guarantees there is always a way in.
 */
/**
 * Whether to hold a receive window open after the next beacon.
 *
 * This is the single most expensive decision in the protocol. The window is tens
 * of milliseconds of receive, and a router that opens one after every beacon
 * spends more on the chance of a joiner than on every link it holds: measured at
 * 122 uA of a router's 125 uA before this was gated properly.
 *
 * So it needs direct evidence that a window would be used. "I would like more
 * links" is deliberately not such evidence - that is this node's own appetite, and
 * it is satisfied by scanning and by initiating a handshake. The worse-ranked end
 * initiates, so a node short of links gains nothing at all from listening.
 */
static bool join_window_worthwhile(void)
{
	uint64_t now = lg_ts_now_us();

	/* Somebody was heard who might use a window. */
	if (lg.joiner_hint_us && now - lg.joiner_hint_us < LG_JOINER_HINT_LIFETIME_US) {
		return true;
	}
	/* Without a route there is nothing to offer: a joiner that attached here
	 * would believe it had a path and would not have one.
	 */
	if (!lg_has_route()) {
		return false;
	}
	/*
	 * Short of our own link target, and the neighbourhood has changed recently
	 * enough that somebody may still be looking for us.
	 *
	 * The grace is what makes this affordable. Listening while short of links is
	 * what forms a grid in the first place, so it cannot simply be dropped - it
	 * was, and a hundred-node grid managed twelve links. But in a clustered
	 * layout plenty of routers never reach the target because there is nobody
	 * left within range to reach it with, and for them "still forming" never
	 * ended: that is what cost 122 uA of a router's 125 uA. Bounding it keeps
	 * formation intact and lets a settled grid go quiet.
	 */
	if (lg_neigh_linked_count() < CONFIG_LOCKGRID_MIN_LINKS_TARGET &&
	    now - lg.link_change_us < LG_JOIN_OFFER_GRACE_US) {
		return true;
	}
	/* Otherwise occasionally, so a node that arrives long after everything has
	 * settled is not locked out indefinitely.
	 */
	return now - lg.last_join_window_us >= lg.tune.join_window_floor_us;
}

static void beacon_start(struct lg_event *ev)
{
	lg_power_activity(LG_PWR_ACT_BEACON);
	beacon_copy_idx = 0;

	/* Suppression is decided here rather than at scheduling time so the
	 * count reflects everything heard right up to the event.
	 */
	if (lg.beacons_heard_this_window >= CONFIG_LOCKGRID_BEACON_SUPPRESS_THRESHOLD) {
		LOG_DBG("beacon suppressed, %u heard nearby", lg.beacons_heard_this_window);
		lg.beacons_heard_this_window = 0;
		lg_sched_event_done();
		return;
	}

	if (!beacon_copy[0].len ||
	    lg_phy_tx_at(ev->anchor_us, beacon_copy[0].chan, beacon_copy[0].pdu,
			 beacon_copy[0].len) != 0) {
		lg_sched_event_done();
	}
}

static void beacon_tx_done(struct lg_event *ev, uint64_t air_end_us)
{
	ARG_UNUSED(ev);

	lg.stats.beacons_tx++;
	LG_TRACE_FRAME("beacontx", "ch=%u copy=%u", beacon_copy[beacon_copy_idx].chan,
		       beacon_copy_idx);
	beacon_copy_idx++;

	if (beacon_copy_idx < LG_RDV_SLOTS && beacon_copy[beacon_copy_idx].len) {
		if (lg_phy_tx_at(air_end_us + LG_BEACON_COPY_GAP_US,
				 beacon_copy[beacon_copy_idx].chan,
				 beacon_copy[beacon_copy_idx].pdu,
				 beacon_copy[beacon_copy_idx].len) == 0) {
			return;
		}
	}

	/* Open a join window where the beacons said we would, but only when there
	 * is a reason to. Listening for 20 ms after every beacon would cost more
	 * than every link this router holds.
	 */
	/* Honour exactly what the beacon just promised. */
	if (lg.beacon_offers_join) {
		lg.join_win_us = air_end_us + LG_JOIN_OFFSET_US;
		lg.last_join_window_us = lg_ts_now_us();
	}

	lg_sched_event_done();
}

static void beacon_rx_fail(struct lg_event *ev, enum lg_phy_rx_fail reason)
{
	ARG_UNUSED(ev);
	ARG_UNUSED(reason);
	lg_sched_event_done();
}

static void beacon_aborted(struct lg_event *ev)
{
	ARG_UNUSED(ev);
	/* A missed beacon costs nothing but a little discovery latency. */
}

/**
 * Whether there is a reason to keep beaconing at the nominal rate.
 *
 * Deliberately a different question from whether to open a join window, though
 * the two were once answered by the same predicate. Transmitting a beacon is an
 * announcement and costs about a millisecond; holding a join window open is
 * listening and costs tens. So a node announces itself whenever it needs
 * something, but only listens when somebody has been heard who might use it.
 *
 * Conflating them broke bootstrapping in a way worth remembering: gating the
 * beacon rate on the join-window test meant an unrouted node - which has nothing
 * to offer a joiner, so does not want a window - stretched its beacons by the idle
 * factor. But an unrouted node's beacon is exactly how it asks to be adopted, so
 * at one beacon every eight seconds it was never heard, and a hundred-node grid
 * formed twelve links instead of a hundred.
 */
static bool beacon_urgent(void)
{
	uint64_t now = lg_ts_now_us();

	/* We need something: announce ourselves so a router can act on it. */
	if (!lg_has_route() || lg_neigh_linked_count() < CONFIG_LOCKGRID_MIN_LINKS_TARGET) {
		return true;
	}
	/* Somebody nearby needs something: keep the rate up until they are settled. */
	return lg.joiner_hint_us && now - lg.joiner_hint_us < LG_JOINER_HINT_LIFETIME_US;
}

/** Called by the scheduler after a beacon event to set up the next one. */
/**
 * Beacon interval, backed off when nobody appears to need us.
 *
 * In a converged grid beaconing becomes the largest single consumer of radio
 * time: three copies every interval, forever, for the benefit of nodes that have
 * all already joined. Backing off when nothing needs us removes most of that, and
 * the hint brings the rate straight back when somebody turns up.
 */
static uint32_t beacon_interval_us(void)
{
	uint32_t nominal = lg.tune.beacon_interval_us;

	if (beacon_urgent()) {
		return nominal;
	}
	return nominal * lg.tune.beacon_idle_factor;
}

void lg_disc_beacon_finish(void)
{
	uint32_t interval = beacon_interval_us();
	uint64_t now = lg_ts_now_us();

	/* The beacon just sent told listeners when the next one would be, so use
	 * that time rather than picking a fresh one - otherwise every listener's
	 * prediction is wrong and the tracking is worthless. Only fall back to a
	 * fresh time if the promised one has already gone by.
	 */
	if (lg.beacon_planned_us > now + 2000) {
		lg.next_beacon_us = lg.beacon_planned_us;
	} else {
		lg.next_beacon_us = now + jitter(interval);
	}
	lg.beacon_planned_us = lg.next_beacon_us + jitter(interval);

	lg.beacons_heard_this_window = 0;
	beacon_stage();
}

const struct lg_ev_ops lg_beacon_ev_ops = {
	.start = beacon_start,
	.tx_done = beacon_tx_done,
	.rx_done = NULL,
	.rx_fail = beacon_rx_fail,
	.aborted = beacon_aborted,
};

/* ------------------------------------------------------------------------- */
/* Scan event                                                                 */
/* ------------------------------------------------------------------------- */

static void scan_start(struct lg_event *ev)
{
	lg_power_activity(LG_PWR_ACT_SCAN);
	lg.stats.scans++;

	if (lg_phy_rx_window(ev->anchor_us, ev->close_us, ev->chan, ev->rxbuf) != 0) {
		lg_sched_event_done();
	}
}

static void scan_rx_done(struct lg_event *ev, const struct lg_phy_rx *rx)
{
	/* Checking the beacon's authenticity needs AES, so the frame goes to the
	 * work queue. The source address is readable without it, which is enough
	 * to decide whether a targeted scan is finished.
	 */
	lg_rx_post(rx->pdu, rx->len, rx->rssi, ev->chan, rx->air_start_us);

	/* A targeted scan is done the moment it hears what it came for. */
	if (lg.want_addr != LG_ADDR_INVALID &&
	    lg_get_le16(&rx->pdu[LG_OFF_SRC]) == lg.want_addr) {
		lg.want_addr = LG_ADDR_INVALID;
		lg_sched_event_done();
		return;
	}

	/* Otherwise keep listening for the rest of the window: a second beacon
	 * from a different router is worth having, and the radio is already on.
	 */
	if (lg_ts_now_us() + LG_SYNC_WINDOW_US < ev->close_us) {
		if (lg_phy_rx_window(lg_ts_now_us() + LG_RAMPUP_US + 20, ev->close_us, ev->chan,
				     ev->rxbuf) == 0) {
			return;
		}
	}
	lg_sched_event_done();
}

static void scan_rx_fail(struct lg_event *ev, enum lg_phy_rx_fail reason)
{
	/* A filtered or corrupt frame still leaves window time; keep listening. */
	if (reason != LG_PHY_RX_TIMEOUT && lg_ts_now_us() + LG_SYNC_WINDOW_US < ev->close_us) {
		if (lg_phy_rx_window(lg_ts_now_us() + LG_RAMPUP_US + 20, ev->close_us, ev->chan,
				     ev->rxbuf) == 0) {
			return;
		}
	}
	lg_sched_event_done();
}

static void scan_aborted(struct lg_event *ev)
{
	ARG_UNUSED(ev);
}

/** Called by the scheduler after a scan window to set up the next one. */
void lg_disc_scan_finish(void)
{
	lg.scan_chan_idx = (lg.scan_chan_idx + 1) % LG_RDV_SLOTS;
	lg.next_scan_us = lg_ts_now_us() + jitter(scan_interval_us());
}

/* ------------------------------------------------------------------------- */
/* Tracked beacon event                                                       */
/* ------------------------------------------------------------------------- */

static void track_start(struct lg_event *ev)
{
	lg_power_activity(LG_PWR_ACT_SCAN);
	lg.stats.tracked_beacons++;

	if (lg_phy_rx_window(ev->anchor_us - LG_TRACK_MARGIN_US, ev->close_us, ev->chan,
			     ev->rxbuf) != 0) {
		lg_sched_event_done();
	}
}

static void track_rx_done(struct lg_event *ev, const struct lg_phy_rx *rx)
{
	lg_rx_post(rx->pdu, rx->len, rx->rssi, ev->chan, rx->air_start_us);
	lg_disc_track_result(ev->neigh, true);
	lg_sched_event_done();
}

static void track_rx_fail(struct lg_event *ev, enum lg_phy_rx_fail reason)
{
	ARG_UNUSED(reason);
	lg_disc_track_result(ev->neigh, false);
	lg_sched_event_done();
}

static void track_aborted(struct lg_event *ev)
{
	lg_disc_track_result(ev->neigh, false);
}

const struct lg_ev_ops lg_track_ev_ops = {
	.start = track_start,
	.tx_done = NULL,
	.rx_done = track_rx_done,
	.rx_fail = track_rx_fail,
	.aborted = track_aborted,
};

const struct lg_ev_ops lg_scan_ev_ops = {
	.start = scan_start,
	.tx_done = NULL,
	.rx_done = scan_rx_done,
	.rx_fail = scan_rx_fail,
	.aborted = scan_aborted,
};

/* ------------------------------------------------------------------------- */
/* Beacon reception                                                           */
/* ------------------------------------------------------------------------- */

/**
 * Should this node start a handshake towards the beacon's sender?
 *
 * Rank decides who initiates, and that is not just an optimisation. There is one
 * handshake context per node, so if both ends decide to be the joiner at the same
 * time neither can act as the router and both attempts die. Requiring the
 * initiator to have the worse rank makes the choice unambiguous from information
 * both sides already have, with the address as a tie-break when ranks are equal.
 */
static bool candidate_is_useful(const struct lg_beacon *b, lg_addr_t src, int8_t rssi)
{
	if (b->role == LG_ROLE_LEAF || !b->link_capacity) {
		return false;
	}
	if (rssi < CONFIG_LOCKGRID_CHAN_NOISE_FLOOR_DBM - 10) {
		return false;
	}
	/* A node with no route of its own has nothing to offer as a parent. */
	if (b->rank == LG_RANK_INFINITE) {
		return false;
	}
	/* A backhaul is already at rank 0; it accepts links but never seeks them. */
	if (lg.role == LG_ROLE_BACKHAUL) {
		return false;
	}

	/* Unattached: any routed neighbour is an improvement on nothing. */
	if (!lg_has_route()) {
		return true;
	}

	/* Attached but short of the link target: take on a peer that is at least
	 * as well placed as we are, for redundancy.
	 */
	if (lg_neigh_linked_count() < CONFIG_LOCKGRID_MIN_LINKS_TARGET) {
		if (b->rank == lg.rank) {
			return lg.addr > src;
		}
		return b->rank < lg.rank;
	}

	/* Otherwise only a materially better parent is worth a handshake. */
	return b->rank + 4U < lg.rank;
}

void lg_disc_beacon_rx(const uint8_t *pdu, uint8_t len, int8_t rssi, uint64_t air_start_us)
{
	uint8_t work[LG_PDU_MAX];
	struct lg_beacon b;
	struct lg_neigh *n;
	lg_addr_t src;
	uint8_t body_len;

	if (len < LG_HDR_LEN + sizeof(b) + LG_BEACON_MIC_LEN) {
		return;
	}
	if (lg_frame_type(pdu) != LG_FT_BEACON) {
		return;
	}
	if (lg_get_le16(&pdu[LG_OFF_NETID]) != lg.netid16) {
		return;
	}

	memcpy(work, pdu, len);
	body_len = len;
	if (lg_sec_beacon_open(work, &body_len) != 0) {
		/* Wrong grid key, or a forgery. Either way not ours. */
		LOG_DBG("beacon rejected by the grid key");
		lg.stats.auth_failures++;
		return;
	}

	src = lg_get_le16(&work[LG_OFF_SRC]);
	if (src == lg.addr || src == LG_ADDR_INVALID) {
		return;
	}

	memcpy(&b, &work[LG_HDR_LEN], sizeof(b));
	if (b.network_id != lg.network_id) {
		return;
	}

	lg.stats.beacons_rx++;

	/* A neighbour advertising no route is asking for a parent. That is the
	 * signal that makes a leaf consider promoting itself, and it is why an
	 * unattached node beacons at all.
	 */
	if (b.rank == LG_RANK_INFINITE) {
		lg_role_note_unrouted();
		lg_role_evaluate();
	}

	LG_TRACE("beaconrx", "peer=%04x rank=%u role=%u cap=%u rssi=%d", src, b.rank, b.role,
		 b.link_capacity, rssi);
	LOG_DBG("beacon from 0x%04x: rank %u role %u cap %u rssi %d", src, b.rank, b.role,
		b.link_capacity, rssi);

	/* Trickle-style density control: only beacons from nodes at least as
	 * well connected as us count towards suppressing our own.
	 */
	if (b.rank <= lg.rank && lg.beacons_heard_this_window < UINT8_MAX) {
		lg.beacons_heard_this_window++;
	}

	n = lg_neigh_find(src);
	if ((!n || n->state != LG_NS_LINKED) && b.rank >= lg.rank &&
	    b.link_count < CONFIG_LOCKGRID_MIN_LINKS_TARGET) {
		/*
		 * Somebody nearby is advertising, is not linked to us, would be the
		 * joiner if a link were formed, and says it is short of links. That is
		 * worth opening a join window for after our next beacon.
		 *
		 * All three conditions are load-bearing, and each was learned by
		 * getting it wrong:
		 *
		 * Without the rank test every router in earshot refreshed every other
		 * router's hint just by beaconing, so in a dense grid the hint never
		 * expired, the window never closed, and listening for joiners cost
		 * 122 uA of a router's 125 uA. Equal rank is included because which
		 * end initiates is then settled by address, so we may well be the
		 * responder; excluding it cost the second link that redundancy
		 * depends on, and two peer routers would wait for the periodic floor
		 * to introduce them.
		 *
		 * The link count is what lets the hint expire in a settled grid. Once
		 * every neighbour holds its target, nobody is asking for anything, and
		 * windows drop back to the floor.
		 */
		lg.joiner_hint_us = lg_ts_now_us();
	}

	if (!n) {
		if (!candidate_is_useful(&b, src, rssi)) {
			return;
		}
		n = lg_neigh_alloc(src);
		if (!n) {
			/* Table full. Displace the least useful entry, but only
			 * if it is worse than this router looks.
			 */
			if (!lg_neigh_make_room(LG_ETX_ONE)) {
				return;
			}
			n = lg_neigh_alloc(src);
			if (!n) {
				return;
			}
		}
	}

	n->role = b.role;
	n->peer_rank = b.rank;
	n->peer_chan_map = b.chan_map;
	n->peer_links = b.link_count;
	n->peer_capacity = b.link_capacity;

	/* Remember when this router says it will beacon next, so the next look at
	 * it costs a few milliseconds of receive rather than a blind window.
	 */
	if (b.next_beacon_offset_us && b.next_beacon_offset_us < 60U * USEC_PER_SEC) {
		n->next_beacon_us = air_start_us + b.next_beacon_offset_us;
		n->track_misses = 0;
	}
	lg_neigh_rx_rssi(n, rssi);
	lg_neigh_update_cost(n);

	if (n->state == LG_NS_LINKED || n->state == LG_NS_JOINING) {
		return;
	}
	if (!candidate_is_useful(&b, src, rssi)) {
		return;
	}

	/* This beacon is not offering a window, so there is nothing to aim at.
	 * Skipping costs nothing; attempting would waste a handshake and count a
	 * failure against a router that simply was not listening.
	 */
	if (b.join_window_us == 0) {
		return;
	}

	/* Aim the handshake at the join window this beacon advertised, measured
	 * from this copy's arrival so all three copies agree.
	 */
	n->join_next_us = air_start_us + b.join_offset_us;

	/* Pick one of the advertised window's sub-slots at random and start on its
	 * boundary, so a joiner that chose a different sub-slot cannot overlap us at
	 * all. The handshake takes its slot origin from the frame itself, so this
	 * costs nothing in agreement between the two ends.
	 */
	if (b.join_window_us >= 2U * LG_JOIN_TRAIN_US) {
		uint32_t slots = b.join_window_us / LG_JOIN_TRAIN_US;

		n->join_spread_us = (sys_rand32_get() % slots) * LG_JOIN_TRAIN_US;
	} else {
		n->join_spread_us = 0;
	}

	if (lg_join_start(n) == 0) {
		LOG_DBG("join towards 0x%04x at %llu (+%u us), rank %u, rssi %d", src,
			n->join_next_us, b.join_offset_us, b.rank, rssi);

		/* The scan has found what it was for. Its window can easily be
		 * wider than the gap to the join window it just pointed us at, so
		 * end it now rather than let it swallow the first handshake slot.
		 */
		lg_sched_abort(LG_EV_SCAN);
		lg_sched_kick();
	}
}
