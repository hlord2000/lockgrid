/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_link.c
 * @brief Scheduled connection events between two authenticated neighbours.
 *
 * A link is a periodic rendezvous. One end is the initiator and owns the
 * timing; the other synchronises to it. Every interval the two exchange exactly
 * one frame each:
 *
 *     initiator:  [ tx ] --- inter-frame space --- [ rx window ]
 *     responder:  [ rx window ] --- inter-frame space --- [ tx ]
 *
 * Receive time is the whole ball game, so five things keep it small:
 *
 *  1. The responder's window is only as wide as the clock uncertainty demands.
 *     Every successful frame is timestamped from the radio's END event and the
 *     known air time, so the anchor is re-derived exactly and the window
 *     collapses back to its floor. Widening is proportional to time since the
 *     last successful sync, not to a worst case.
 *
 *  2. The initiator's window stays at the floor permanently. The responder
 *     replies at a time it computed from a frame it just measured, so its error
 *     is a microsecond or two. Asymmetry is deliberate: only one end pays.
 *
 *  3. An event ends the moment its single exchange is over. Nothing sits in
 *     receive waiting for traffic that is not coming.
 *
 *  4. When both ends are idle the initiator raises a subrate factor, so only
 *     every Nth event actually uses the radio. The skipped events cost nothing;
 *     the kept ones hold the synchronisation that keeps the windows narrow.
 *
 *  5. More data does not extend the event. It expedites the next one instead,
 *     which keeps every grant short - important when the radio is shared with
 *     Bluetooth through MPSL, where a long request is more likely to be refused.
 *
 * Acknowledgement is implicit. Because the exchange within an event is a strict
 * ping-pong, receiving the peer's frame confirms that ours arrived, and there is
 * nothing to put in a header. That also means the header can be authenticated
 * as a whole, with no field mutated after encryption.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "lg_internal.h"
#include "lg_trace.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* Uncertainty that is not clock drift: interrupt latency, timestamp
 * quantisation and the peer's transmit chain.
 */
#define LG_WINDOW_JITTER_US 40

/* Consecutive empty events before the link is treated as degraded and the
 * subrate is dropped back so it can recover quickly.
 */
#define LG_MISSES_BEFORE_RESYNC 2

/* How far ahead an expedited event is placed. Enough for the work queue to
 * stage the frame and for the arbiter to accept the request.
 */
#define LG_EXPEDITE_LEAD_US 3000

/** State of the event currently running, valid only between start and done. */
static struct {
	bool tx_first;
	bool got_rx;
	bool sent_tx;
	uint64_t tx_air_end_us;
} evst;

/* ------------------------------------------------------------------------- */
/* Schedule arithmetic                                                        */
/* ------------------------------------------------------------------------- */

uint32_t lg_link_window_us(struct lg_neigh *n)
{
	uint64_t now = lg_ts_now_us();
	uint64_t elapsed;
	uint32_t combined_ppm;
	uint64_t drift_us;
	uint32_t half;

	if (n->initiator) {
		/* The peer replies off a frame it measured; its error is tiny. */
		return CONFIG_LOCKGRID_RX_WINDOW_MIN_US / 2U;
	}

	elapsed = (n->last_sync_us && now > n->last_sync_us) ? now - n->last_sync_us : 0;
	combined_ppm = CONFIG_LOCKGRID_CLOCK_ACCURACY_PPM + n->peer_sca_ppm;

	/* Both clocks can drift in opposite directions, so the window has to
	 * cover the sum of the two accuracies over the time since last sync.
	 */
	drift_us = (elapsed * combined_ppm) / 1000000ULL;

	half = (uint32_t)MIN(drift_us + LG_WINDOW_JITTER_US +
				     CONFIG_LOCKGRID_RX_WINDOW_MIN_US / 2U,
			     (uint64_t)CONFIG_LOCKGRID_RX_WINDOW_MAX_US / 2U);

	n->rx_window_us = half * 2U;
	return half;
}

/**
 * Event number of the anchor currently loaded.
 *
 * Derived from time rather than counted, so an event this node skipped and the
 * peer did not cannot put the two on different channels.
 */
static uint16_t counter_of(const struct lg_neigh *n)
{
	if (!n->interval_us || n->anchor_us < n->epoch_us) {
		return 0;
	}
	return (uint16_t)(((n->anchor_us - n->epoch_us) + n->interval_us / 2U) / n->interval_us);
}

/**
 * Move the anchor to event @p target.
 *
 * Computed from the epoch rather than by addition, so an end that has skipped
 * events lands on exactly the same event as one that has not.
 */
static void anchor_set(struct lg_neigh *n, uint16_t target)
{
	n->anchor_us = n->epoch_us + (uint64_t)target * n->interval_us;
	n->counter = target;
}

/**
 * Next event both ends will agree is active.
 *
 * Subrating selects events whose counter is a multiple of the factor, rather
 * than stepping forward by the factor. The difference matters: stepping keeps a
 * phase that depends on where each end started counting, so one skipped event
 * puts the two on disjoint sets of events forever, each listening while the
 * other sleeps. A modulus has no phase to lose.
 */
static uint16_t next_active(const struct lg_neigh *n, uint16_t after)
{
	uint16_t step = MAX(n->subrate, 1);

	return (uint16_t)(((uint32_t)(after / step) + 1U) * step);
}

/** Advance the anchor past any event whose time has already gone. */
static void anchor_catch_up(struct lg_neigh *n)
{
	uint64_t now = lg_ts_now_us();
	uint64_t lead = LG_SCHED_LEAD_US + lg_link_window_us(n) + 200;
	uint16_t step = MAX(n->subrate, 1);

	if (!n->interval_us) {
		return;
	}

	/* Land on an active event even if the current anchor is not one, which
	 * happens the moment a new subrate takes effect.
	 */
	if (counter_of(n) % step) {
		anchor_set(n, next_active(n, counter_of(n)));
	}

	while (n->anchor_us < now + lead) {
		anchor_set(n, next_active(n, counter_of(n)));
		lg.stats.link_events_skipped++;
	}
}

uint64_t lg_link_next_event_us(struct lg_neigh *n)
{
	if (n->state != LG_NS_LINKED || !n->interval_us) {
		return UINT64_MAX;
	}
	anchor_catch_up(n);
	return n->anchor_us;
}

uint8_t lg_link_next_chan(struct lg_neigh *n)
{
	return lg_chan_hop(n->chan_map, n->hop_seed, counter_of(n));
}

void lg_link_advance(struct lg_neigh *n)
{
	anchor_set(n, next_active(n, counter_of(n)));

	/* Apply a parameter change at the counter both ends agreed on.
	 *
	 * The side that received the change applies it unconditionally: having
	 * received it is proof of agreement. The side that originated it applies
	 * it only once the peer has replied to the frame that carried it. An
	 * unconfirmed change is abandoned rather than applied, because applying it
	 * would split the two ends onto different event sets permanently.
	 */
	if (n->pending_ctrl && n->counter >= n->instant) {
		if (!n->initiator || n->ctrl_acked) {
			if (n->pending_ctrl & BIT(LG_CTRL_CHAN_MAP)) {
				n->chan_map = n->pending_chan_map;
			}
			if (n->pending_ctrl & BIT(LG_CTRL_SUBRATE)) {
				n->subrate = MAX(n->pending_subrate, 1);
			}
		} else {
			LOG_DBG("0x%04x: parameter change unconfirmed, abandoned", n->addr);
		}
		n->pending_ctrl = 0;
		n->ctrl_acked = false;
	}
}

/* ------------------------------------------------------------------------- */
/* Establishment and teardown                                                 */
/* ------------------------------------------------------------------------- */

int lg_link_establish(struct lg_neigh *n, const struct lg_link_params *pp, bool initiator)
{
	uint32_t interval = CLAMP(pp->interval_us, CONFIG_LOCKGRID_CONN_INTERVAL_MIN_MS * 1000U,
				  CONFIG_LOCKGRID_CONN_INTERVAL_MAX_MS * 1000U);

	n->initiator = initiator;
	n->interval_us = interval;
	n->epoch_us = n->anchor_us;
	n->counter = 0;
	n->chan_map = pp->chan_map ? pp->chan_map : lg.chan_map;
	n->hop_seed = pp->hop_seed;
	n->subrate = 1;
	n->subrate_max = MIN(pp->subrate_max,
			     MIN(lg.tune.subrate_max,
				 lg_subrate_ceiling(interval, (uint32_t)pp->sup_timeout_cs * 10000U)));
	n->peer_sca_ppm = pp->sca_ppm;
	n->sup_timeout_us = (uint32_t)pp->sup_timeout_cs * 10000U;
	n->peer_rank = pp->rank;
	n->role = pp->role;
	n->pending_ctrl = 0;
	n->consec_misses = 0;
	n->tx_pending = false;
	n->last_sync_us = lg_ts_now_us();
	n->last_rx_us = n->last_sync_us;
	n->state = LG_NS_LINKED;

	/* The anchor was set by the caller from the timestamp of the frame that
	 * carried these parameters, plus pp->anchor_offset_us.
	 */
	anchor_catch_up(n);

	lg.link_count = lg_neigh_linked_count();
	lg_neigh_update_cost(n);

	LOG_INF("link up with 0x%04x: %s, interval %u us, map 0x%04x, seed 0x%04x", n->addr,
		initiator ? "initiator" : "responder", n->interval_us, n->chan_map, n->hop_seed);
	LG_TRACE("linkup", "peer=%04x init=%u int=%u map=%04x seed=%04x rank=%u", n->addr,
		 initiator ? 1U : 0U, n->interval_us, n->chan_map, n->hop_seed, n->peer_rank);

	if (lg_route_recompute()) {
		lg_notify(LG_EVT_PARENT_CHANGED, lg_parent());
	}
	lg.link_change_us = lg_ts_now_us();
	lg_notify(LG_EVT_LINK_UP, n->addr);
	lg_role_evaluate();
	lg_disc_retune();
	lg_sched_kick();
	return 0;
}

void lg_link_down(struct lg_neigh *n, enum lg_teardown_reason reason)
{
	lg_addr_t addr = n->addr;

	if (n->state != LG_NS_LINKED) {
		return;
	}

	LOG_INF("link down with 0x%04x, reason %u", addr, reason);
	LG_TRACE("linkdown", "peer=%04x reason=%u", addr, (unsigned)reason);

	/* Keep the long term key so re-attaching is one round trip rather than
	 * a full certificate handshake. That is what makes healing fast.
	 */
	memcpy(n->ltk, n->link_key, LG_KEY_LEN);
	n->has_ltk = true;
	lg_sec_forget_key(n);

	n->state = LG_NS_LOST;
	n->is_parent = false;
	n->interval_us = 0;
	n->tx_pending = false;
	n->join_step = LG_JOIN_IDLE;

	lg_route_flush_via(addr);
	lg.link_count = lg_neigh_linked_count();

	if (lg_route_recompute()) {
		lg_notify(LG_EVT_PARENT_CHANGED, lg_parent());
	}
	lg.link_change_us = lg_ts_now_us();
	lg_notify(LG_EVT_LINK_DOWN, addr);
	lg_role_evaluate();
	lg_disc_retune();
	lg_sched_kick();
}

/** Check supervision after an event that produced nothing. */
void lg_link_event_miss(struct lg_neigh *n)
{
	uint64_t now = lg_ts_now_us();

	lg_neigh_tx_result(n, false);
	lg_chan_result(lg_link_next_chan(n), false);

	/*
	 * Deliberately no subrate change here. Lowering it needs a frame to reach
	 * the peer, and this path runs precisely because frames are not reaching
	 * the peer - so the request would take effect on this side only, and this
	 * side would then transmit on events the peer is asleep for. That is worse
	 * than doing nothing: it burns energy and cannot recover. Supervision is
	 * what ends a link that has genuinely failed.
	 */

	LG_TRACE_FRAME("miss", "peer=%04x ch=%u", n->addr, lg_link_next_chan(n));

	if (n->last_rx_us && now - n->last_rx_us > n->sup_timeout_us) {
		lg.stats.supervision_timeouts++;
		LG_TRACE("supervision", "peer=%04x", n->addr);
		lg_link_down(n, LG_TD_POOR_QUALITY);
	}
}

/* ------------------------------------------------------------------------- */
/* Frame staging                                                              */
/* ------------------------------------------------------------------------- */

/** Choose and build the frame this link will send at its next event. */
bool lg_link_stage_tx(struct lg_neigh *n)
{
	uint8_t *pdu = n->txbuf;
	uint8_t fcf = LG_FCF_SEC;
	uint8_t len;
	uint8_t hdr_len;
	struct lg_txmsg *m;
	bool more = false;

	if (n->state != LG_NS_LINKED || !n->has_key) {
		return false;
	}

	/* Control traffic first: a channel map or subrate update that is late
	 * costs the link far more than a delayed data frame.
	 */
	if (n->pending_ctrl) {
		uint8_t body[16];
		uint8_t body_len = 0;

		if (n->pending_ctrl & BIT(LG_CTRL_CHAN_MAP)) {
			body[0] = LG_CTRL_CHAN_MAP;
			lg_put_le16(&body[1], n->pending_chan_map);
			lg_put_le16(&body[3], n->instant);
			body_len = 5;
		} else if (n->pending_ctrl & BIT(LG_CTRL_SUBRATE)) {
			body[0] = LG_CTRL_SUBRATE;
			body[1] = n->pending_subrate;
			lg_put_le16(&body[2], n->instant);
			body_len = 4;
		}

		if (body_len) {
			hdr_len = lg_frame_hdr_put(pdu, lg.netid16, fcf | LG_FT_CTRL, n->tx_seq++,
						   n->addr, lg.addr);
			memcpy(&pdu[hdr_len], body, body_len);
			len = hdr_len + body_len;
			n->tx_was_ctrl = true;
			goto seal;
		}
	}
	n->tx_was_ctrl = false;

	/* A route advertisement, if this node's rank has just changed or the peer
	 * has not had one recently. Gossip is how a settled node learns the topology
	 * without spending receive time; the urgent case exists because a rank that
	 * has gone to infinity has to overtake the timer, or a neighbour keeps
	 * forwarding towards a sink that is no longer there.
	 */
	if ((lg.adv_urgent && !n->adv_urgent_done) ||
	    lg_ts_now_us() - n->route_adv_us > 30ULL * USEC_PER_SEC) {
		uint8_t body[4 + 6 * 4];
		uint8_t body_len = lg_route_build_adv(body, sizeof(body));

		if (body_len) {
			hdr_len = lg_frame_hdr_put(pdu, lg.netid16, fcf | LG_FT_CTRL, n->tx_seq++,
						   n->addr, lg.addr);
			memcpy(&pdu[hdr_len], body, body_len);
			len = hdr_len + body_len;
			n->route_adv_us = lg_ts_now_us();

			/* The flag clears once every linked neighbour has had the
			 * news, not on the first one told.
			 */
			n->adv_urgent_done = true;
			lg.adv_urgent = false;
			for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
				const struct lg_neigh *o = &lg.neigh[i];

				if (o->state == LG_NS_LINKED && !o->adv_urgent_done) {
					lg.adv_urgent = true;
					break;
				}
			}
			goto seal;
		}
	}

	m = lg_txq_peek(n->addr);
	if (m) {
		struct lg_data_hdr dh;

		hdr_len = lg_frame_hdr_put(pdu, lg.netid16, fcf | LG_FT_DATA, n->tx_seq++, n->addr,
					   lg.addr);
		dh.flags = m->flags;
		dh.origin = m->origin;
		dh.final = m->final;
		dh.hops = m->hops;
		dh.port = m->port;
		dh.msg_id = m->msg_id;

		memcpy(&pdu[hdr_len], &dh, sizeof(dh));
		memcpy(&pdu[hdr_len + sizeof(dh)], m->payload, m->len);
		len = hdr_len + sizeof(dh) + m->len;

		/* Released once the event confirms the peer heard us. */
		n->tx_needs_ack = true;
		more = lg_txq_pending(n->addr);
	} else {
		/* Nothing to say. The keepalive is what holds synchronisation,
		 * and it is the smallest frame the protocol has.
		 */
		hdr_len = lg_frame_hdr_put(pdu, lg.netid16, fcf | LG_FT_ACK, n->tx_seq++, n->addr,
					   lg.addr);
		len = hdr_len;
		n->tx_needs_ack = false;
	}

seal:
	if (more) {
		pdu[LG_OFF_FCF] |= LG_FCF_MD;
	}

	if (lg_sec_encrypt(n, pdu, LG_HDR_LEN_SEC, &len) != 0) {
		n->tx_pending = false;
		return false;
	}

	n->txlen = len;
	n->tx_pending = true;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Subrating                                                                  */
/* ------------------------------------------------------------------------- */

/** Raise or lower the subrate factor when the link's traffic changes. */
static void subrate_update(struct lg_neigh *n)
{
	uint8_t want;

	if (!n->initiator || n->subrate_max <= 1) {
		return;
	}

	if (lg_txq_pending(n->addr) || n->pending_ctrl) {
		want = 1;
	} else if (n->consec_misses) {
		want = 1;
	} else {
		/* Idle: back off, but only one step at a time so the window
		 * widening that comes with it stays gradual.
		 */
		want = MIN(n->subrate * 2U, n->subrate_max);
	}

	if (want == n->subrate || n->pending_ctrl) {
		return;
	}

	n->pending_subrate = want;
	/* Far enough ahead that the change has several events to be delivered and
	 * confirmed in before it is due to take effect.
	 */
	n->instant = counter_of(n) + 6;
	n->ctrl_acked = false;
	n->pending_ctrl |= BIT(LG_CTRL_SUBRATE);
}

/* ------------------------------------------------------------------------- */
/* Event execution, all in ISR context                                        */
/* ------------------------------------------------------------------------- */

static void link_start(struct lg_event *ev)
{
	struct lg_neigh *n = ev->neigh;
	uint32_t win = ev->window_us;

	evst.got_rx = false;
	evst.sent_tx = false;
	evst.tx_first = n->initiator;
	lg.stats.link_events++;

	lg_power_activity(LG_PWR_ACT_LINK);
	LOG_DBG("ev %s peer 0x%04x ctr %u ch %u anchor %llu win %u", n->initiator ? "I" : "R",
		n->addr, counter_of(n), ev->chan, ev->anchor_us, win);

	if (evst.tx_first) {
		if (lg_phy_tx_at(ev->anchor_us, ev->chan, n->txbuf, n->txlen) != 0) {
			lg_sched_event_done();
		}
	} else {
		if (lg_phy_rx_window(ev->anchor_us - win, ev->anchor_us + win + LG_SYNC_WINDOW_US,
				     ev->chan, ev->rxbuf) != 0) {
			lg_sched_event_done();
		}
	}
}

static void link_tx_done(struct lg_event *ev, uint64_t air_end_us)
{
	uint64_t expect;
	uint32_t win;

	evst.sent_tx = true;
	evst.tx_air_end_us = air_end_us;
	lg.stats.frames_tx++;
	LG_TRACE_FRAME("tx", "peer=%04x ch=%u type=%u len=%u", ev->neigh->addr, ev->chan,
		       ev->neigh->txbuf[LG_OFF_FCF] & LG_FCF_TYPE_MASK, ev->neigh->txlen);

	if (!evst.tx_first) {
		/* Responder: our reply was the last frame of the event. */
		lg_sched_event_done();
		return;
	}

	/* Initiator: listen for the reply. The window is at its floor because
	 * the peer derives its timing from the frame we just sent.
	 */
	expect = air_end_us + LG_IFS_US;
	win = CONFIG_LOCKGRID_RX_WINDOW_MIN_US / 2U;

	if (lg_phy_rx_window(expect - win, expect + win + LG_SYNC_WINDOW_US, ev->chan,
			     ev->rxbuf) != 0) {
		lg_sched_event_done();
	}
}

static void link_rx_done(struct lg_event *ev, const struct lg_phy_rx *rx)
{
	struct lg_neigh *n = ev->neigh;
	uint64_t now = lg_ts_now_us();

	evst.got_rx = true;
	lg.stats.frames_rx++;
	LG_TRACE_FRAME("rx", "peer=%04x ch=%u type=%u len=%u rssi=%d", n->addr, ev->chan,
		       rx->pdu[LG_OFF_FCF] & LG_FCF_TYPE_MASK, rx->len, rx->rssi);
	LOG_DBG("ev rx peer 0x%04x ch %u err %lld", n->addr, ev->chan,
		(long long)((int64_t)rx->air_start_us - (int64_t)ev->anchor_us));

	lg_neigh_rx_rssi(n, rx->rssi);
	n->last_rx_us = now;
	lg_chan_result(ev->chan, true);

	/* Hand the frame to the work queue: decryption and anything it leads to
	 * must not run here.
	 */
	lg_rx_post(rx->pdu, rx->len, rx->rssi, ev->chan, rx->air_start_us);

	if (evst.tx_first) {
		/* Initiator: the reply confirms our frame was delivered, which for
		 * a parameter change is what licenses it to take effect.
		 */
		lg_neigh_tx_result(n, true);
		if (n->tx_was_ctrl) {
			n->ctrl_acked = true;
		}
		lg_sched_event_done();
		return;
	}

	/* The exchange worked, which is what the quality metrics are actually
	 * measuring. The initiator learns this from the reply; the responder
	 * learns it from the frame it just received, and without recording it here
	 * a responder would report a 100% error rate on a perfectly good link and
	 * routing would avoid it.
	 */
	lg_neigh_tx_result(n, true);

	/* Responder: re-derive the anchor from the measured frame start. This is
	 * the moment the receive window collapses back to its floor. The epoch
	 * moves by the same amount, so correcting for drift never changes which
	 * event number this is, and therefore never changes the channel.
	 */
	n->sync_err_us = (int32_t)((int64_t)rx->air_start_us - (int64_t)ev->anchor_us);
	n->epoch_us = (uint64_t)((int64_t)n->epoch_us + n->sync_err_us);
	n->anchor_us = rx->air_start_us;
	n->last_sync_us = now;
	n->consec_misses = 0;

	if (n->tx_pending) {
		if (lg_phy_tx_at(rx->air_end_us + LG_IFS_US, ev->chan, n->txbuf, n->txlen) == 0) {
			return;
		}
	}
	lg_sched_event_done();
}

static void link_rx_fail(struct lg_event *ev, enum lg_phy_rx_fail reason)
{
	struct lg_neigh *n = ev->neigh;

	LOG_DBG("ev fail peer 0x%04x reason %u ch %u", n->addr, reason, ev->chan);

	switch (reason) {
	case LG_PHY_RX_CRC:
		lg.stats.crc_errors++;
		lg_chan_result(ev->chan, false);
		break;
	case LG_PHY_RX_TIMEOUT:
		lg.stats.rx_timeouts++;
		lg_chan_result(ev->chan, false);
		break;
	default:
		break;
	}

	if (evst.tx_first) {
		/* Our frame went out but nothing came back. */
		lg_neigh_tx_result(n, false);
	}

	lg_sched_event_done();
}

static void link_aborted(struct lg_event *ev)
{
	struct lg_neigh *n = ev->neigh;

	/* Radio time we asked for and did not get. From the link's point of view
	 * this is indistinguishable from a lost frame, and folding it into the
	 * metrics is what lets routing move away from a node whose events keep
	 * losing to Bluetooth. The refusal itself is counted by the scheduler.
	 */
	lg_link_event_miss(n);
}

/**
 * Called by the scheduler once the event is over, on the work queue, to settle
 * the bookkeeping that could not run in the interrupt.
 */
void lg_link_event_finish(struct lg_neigh *n)
{
	if (!evst.got_rx) {
		lg_link_event_miss(n);
	} else if (n->tx_needs_ack && evst.sent_tx) {
		struct lg_txmsg *m = lg_txq_peek(n->addr);

		if (m) {
			lg_txq_free(m);
		}
		n->tx_needs_ack = false;
	}

	if (n->state != LG_NS_LINKED) {
		return;
	}

	lg_link_advance(n);
	subrate_update(n);
	lg_link_stage_tx(n);
}

const struct lg_ev_ops lg_link_ev_ops = {
	.start = link_start,
	.tx_done = link_tx_done,
	.rx_done = link_rx_done,
	.rx_fail = link_rx_fail,
	.aborted = link_aborted,
};

/* ------------------------------------------------------------------------- */
/* Inbound frame handling, on the work queue                                  */
/* ------------------------------------------------------------------------- */

static void handle_ctrl(struct lg_neigh *n, const uint8_t *body, uint8_t len)
{
	if (!len) {
		return;
	}

	switch (body[0]) {
	case LG_CTRL_CHAN_MAP:
		if (len >= 5) {
			n->pending_chan_map = lg_get_le16(&body[1]);
			n->instant = lg_get_le16(&body[3]);
			n->pending_ctrl |= BIT(LG_CTRL_CHAN_MAP);
		}
		break;
	case LG_CTRL_SUBRATE:
		if (len >= 4) {
			n->pending_subrate = body[1];
			n->instant = lg_get_le16(&body[2]);
			n->pending_ctrl |= BIT(LG_CTRL_SUBRATE);
		}
		break;
	case LG_CTRL_INTERVAL:
		if (len >= 7) {
			n->interval_us = CLAMP(lg_get_le32(&body[1]),
					       CONFIG_LOCKGRID_CONN_INTERVAL_MIN_MS * 1000U,
					       CONFIG_LOCKGRID_CONN_INTERVAL_MAX_MS * 1000U);
		}
		break;
	case LG_CTRL_ROUTE_ADV:
		lg_route_handle_adv(n, body, len);
		break;
	case LG_CTRL_ROUTE_REG:
		lg_route_handle_reg(n, body, len);
		break;
	case LG_CTRL_TEARDOWN:
		lg_link_down(n, len >= 2 ? (enum lg_teardown_reason)body[1] : LG_TD_LOCAL_CLOSE);
		break;
	default:
		break;
	}
}

static void handle_data(struct lg_neigh *n, const uint8_t *body, uint8_t len, int8_t rssi)
{
	struct lg_data_hdr dh;
	const uint8_t *payload;
	uint8_t payload_len;

	if (len < sizeof(dh)) {
		return;
	}
	memcpy(&dh, body, sizeof(dh));
	payload = body + sizeof(dh);
	payload_len = len - sizeof(dh);

	if (payload_len > LG_PAYLOAD_MAX) {
		return;
	}
	if (lg_route_is_duplicate(dh.origin, dh.msg_id)) {
		return;
	}

	/* Delivered here? */
	bool for_us = (dh.final == lg.addr) ||
		      ((dh.flags & LG_DATA_F_UPWARD) && lg.role == LG_ROLE_BACKHAUL) ||
		      (dh.flags & LG_DATA_F_BROADCAST);

	if (for_us) {
		struct lg_rx_info info = {
			.src = dh.origin,
			.prev_hop = n->addr,
			.port = dh.port,
			.rssi = rssi,
			.hops = dh.hops,
		};

		LG_TRACE("deliver", "origin=%04x via=%04x port=%u hops=%u len=%u", dh.origin,
			 n->addr, dh.port, dh.hops, payload_len);
		if (lg.cb.rx) {
			lg.cb.rx(&info, payload, payload_len);
		}
		if (!(dh.flags & LG_DATA_F_BROADCAST) || dh.final == lg.addr) {
			return;
		}
	}

	/* Forward it. Leaves never do, and a message that has bounced too many
	 * times is dropped rather than allowed to circulate.
	 */
	if (lg.role == LG_ROLE_LEAF || dh.hops >= LG_HOPS_MAX) {
		lg.stats.msgs_dropped_no_route++;
		return;
	}

	uint8_t flags = dh.flags;

	if (!(flags & LG_DATA_F_UPWARD) && dh.final != LG_ADDR_BACKHAUL) {
		/* Heading for a specific node. If we are above it in the tree
		 * the next hop is downward.
		 */
		lg_addr_t nh = lg_route_next_hop(dh.final, 0);

		if (nh == LG_ADDR_INVALID) {
			lg.stats.msgs_dropped_no_route++;
			return;
		}
		if (nh != lg_parent()) {
			flags |= LG_DATA_F_DOWNWARD;
		}
	}

	if (lg_send_forward(dh.origin, dh.final, dh.port, payload, payload_len, flags,
			    dh.hops + 1, dh.msg_id) == 0) {
		lg.stats.msgs_forwarded++;
		LG_TRACE("forward", "origin=%04x final=%04x from=%04x hops=%u", dh.origin, dh.final,
			 n->addr, dh.hops + 1);
	} else {
		lg.stats.msgs_dropped_no_route++;
	}
}

void lg_link_rx_frame(struct lg_neigh *n, uint8_t fcf, uint8_t seq, const uint8_t *payload,
		      uint8_t len, int8_t rssi)
{
	ARG_UNUSED(seq);

	switch (fcf & LG_FCF_TYPE_MASK) {
	case LG_FT_DATA:
		handle_data(n, payload, len, rssi);
		break;
	case LG_FT_CTRL:
		handle_ctrl(n, payload, len);
		break;
	case LG_FT_ACK:
		/* Keepalive. Its only job was to arrive. */
		break;
	default:
		break;
	}

	/* The peer says it has more queued; bring the next event forward rather
	 * than making it wait a whole interval.
	 */
	if (fcf & LG_FCF_MD) {
		lg_sched_expedite(n);
	}
}

void lg_sched_expedite(struct lg_neigh *n)
{
	uint64_t soon;

	if (n->state != LG_NS_LINKED) {
		return;
	}

	/* Only ever move an event earlier, and only the initiator may, since it
	 * owns the timing the responder is tracking.
	 */
	if (!n->initiator) {
		return;
	}

	soon = lg_ts_now_us() + LG_EXPEDITE_LEAD_US;
	if (n->anchor_us > soon) {
		n->anchor_us = soon;
		n->subrate = 1;
		lg_sched_kick();
	}
}
