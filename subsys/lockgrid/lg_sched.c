/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_sched.c
 * @brief The single-event radio scheduler.
 *
 * LockGrid has exactly one radio event outstanding at any moment. Everything
 * that wants the radio - each link's connection event, the beacon, the scan
 * window, each handshake slot, the channel noise measurement - publishes the
 * absolute time it next needs it, and the earliest one wins. There is no polling
 * loop and no periodic tick: between events the CPU has nothing scheduled and
 * the radio is off, which is the only way the average current gets near the
 * sleep figure.
 *
 * Choosing an event always happens on the work queue, never in an interrupt,
 * because choosing a link event also means encrypting the frame that event will
 * send. By the time the arbiter grants the radio, the frame is a buffer and a
 * length, and the interrupt path only has to point the radio at it.
 *
 * A single event slot is a real constraint: two links whose intervals collide
 * cannot both run. The loser is skipped, its anchor advances, and the miss is
 * charged to its metrics exactly like a lost frame. That is deliberate - a node
 * holding more links than it can schedule should look worse to the routing
 * metric, so the topology spreads out instead of piling onto one router.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "lg_internal.h"
#include "lg_trace.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* Never ask the arbiter for time that starts sooner than this. */
#define LG_SCHED_MIN_HORIZON_US 400

/* Radio time reserved for a beacon event: three copies plus the gaps. */
#define LG_BEACON_LEN_US 6000

/* Radio time reserved for one noise measurement. */
#define LG_CHAN_EVAL_LEN_US 1500

/* Nothing to do: check back at this rate so a schedule change is never missed
 * by more than this, even if a kick is lost.
 */
#define LG_SCHED_IDLE_POLL_US (1ULL * USEC_PER_SEC)

static struct k_work sched_work;
static struct k_work finish_work;
/* Re-kicks the scheduler when nothing at all is due, so a schedule change can
 * never be missed by more than one period even if a kick is lost. Kept separate
 * from the channel evaluation timer, which it used to share: borrowing that one
 * pushed the real evaluation period out every time the node went idle.
 */
static struct k_work_delayable idle_work;

/* Buffer the PHY receives into, sized for the largest PDU. Only one event runs
 * at a time so one buffer serves them all.
 */
static uint8_t sched_rxbuf[LG_PDU_MAX];

static struct lg_neigh *finish_neigh;
static enum lg_ev_kind finish_kind;
static bool running;

/* ------------------------------------------------------------------------- */
/* Candidate selection                                                        */
/* ------------------------------------------------------------------------- */

/**
 * Priority classes.
 *
 * Selection is by time, but a purely time-ordered scheduler starves whatever is
 * unlucky. A handshake is the case that matters: it needs four radio events 40 ms
 * apart, and every one it loses to a beacon or a scan costs it two slots of its
 * retry budget. In a dense grid that was enough to make most handshakes run out of
 * slots and fail, which showed up as a mesh that would not finish converging.
 *
 * So among candidates that are close enough together to contend, the higher class
 * wins and the loser's occurrence is dropped - which for a link event is charged as
 * a miss, and for a beacon or scan costs nothing but latency.
 */
#define LG_PRIO_SCAN     0
#define LG_PRIO_NORMAL   1
#define LG_PRIO_PARENT   2
#define LG_PRIO_HANDSHAKE 3

/**
 * How close two candidates must be before priority decides between them.
 *
 * Slightly more than a handshake slot, so a handshake step can displace anything
 * that would otherwise run during it.
 */
#define LG_SCHED_CONTEND_US 45000

struct candidate {
	enum lg_ev_kind kind;
	uint64_t anchor_us;
	struct lg_neigh *neigh;
	uint8_t prio;
};

/* Every link, plus beacon, scan, track, join window and channel evaluation. */
#define LG_CAND_MAX (CONFIG_LOCKGRID_MAX_NEIGHBORS + 5)

static void add_candidate(struct candidate *list, uint8_t *count, enum lg_ev_kind kind,
			  uint64_t at_us, struct lg_neigh *n, uint8_t prio)
{
	if (at_us == UINT64_MAX || *count >= LG_CAND_MAX) {
		return;
	}
	list[*count].kind = kind;
	list[*count].anchor_us = at_us;
	list[*count].neigh = n;
	list[*count].prio = prio;
	(*count)++;
}

/**
 * Earliest candidate, except that a higher priority one starting within the
 * contention window beats it.
 */
static const struct candidate *select_candidate(const struct candidate *list, uint8_t count)
{
	const struct candidate *best = NULL;

	for (uint8_t i = 0; i < count; i++) {
		if (!best || list[i].anchor_us < best->anchor_us) {
			best = &list[i];
		}
	}
	if (!best) {
		return NULL;
	}

	for (uint8_t i = 0; i < count; i++) {
		const struct candidate *c = &list[i];

		if (c->anchor_us > best->anchor_us + LG_SCHED_CONTEND_US) {
			continue;
		}
		if (c->prio > best->prio ||
		    (c->prio == best->prio && c->anchor_us < best->anchor_us)) {
			best = c;
		}
	}
	return best;
}

/** Fill in the timing and reservation for the chosen candidate. */
static void event_plan(struct lg_event *ev, const struct candidate *c)
{
	memset(ev, 0, sizeof(*ev));
	ev->kind = c->kind;
	ev->anchor_us = c->anchor_us;
	ev->neigh = c->neigh;
	ev->rxbuf = sched_rxbuf;

	switch (c->kind) {
	case LG_EV_LINK: {
		struct lg_neigh *n = c->neigh;
		uint32_t win = lg_link_window_us(n);

		ev->chan = lg_link_next_chan(n);
		ev->ops = &lg_link_ev_ops;
		ev->window_us = win;
		ev->len_us = lg_frame_event_budget_us(win * 2U);
		/* The responder starts listening half a window early, so its
		 * radio time has to begin before the anchor.
		 */
		ev->at_us = n->initiator ? c->anchor_us - LG_SCHED_LEAD_US
					 : c->anchor_us - win - LG_SCHED_LEAD_US;
		ev->urgent = n->is_parent;
		break;
	}

	case LG_EV_BEACON:
		ev->chan = lg_disc_rdv_chan(0);
		ev->ops = &lg_beacon_ev_ops;
		ev->len_us = LG_BEACON_LEN_US;
		ev->at_us = c->anchor_us - LG_SCHED_LEAD_US;
		break;

	case LG_EV_TRACK:
		/* One specific router's beacon at the time it promised. Only copy
		 * zero is targeted, which is always on rendezvous slot zero.
		 */
		ev->chan = lg_disc_rdv_chan(0);
		ev->ops = &lg_track_ev_ops;
		ev->close_us = c->anchor_us + LG_TRACK_MARGIN_US;
		ev->len_us = 2U * LG_TRACK_MARGIN_US + LG_SCHED_LEAD_US + 2000;
		ev->at_us = c->anchor_us - LG_TRACK_MARGIN_US - LG_SCHED_LEAD_US;
		break;

	case LG_EV_SCAN:
		ev->chan = lg_disc_rdv_chan(lg.scan_chan_idx);
		ev->ops = &lg_scan_ev_ops;
		ev->close_us = c->anchor_us + lg.tune.scan_window_us;
		ev->len_us = lg.tune.scan_window_us + LG_SCHED_LEAD_US + 1000;
		ev->at_us = c->anchor_us - LG_SCHED_LEAD_US;
		break;

	case LG_EV_JOIN_TX:
		ev->chan = lg_disc_rdv_chan(0);
		ev->ops = &lg_join_tx_ev_ops;
		ev->len_us = LG_JOIN_SLOT_LEN_US;
		ev->at_us = c->anchor_us - LG_SCHED_LEAD_US;
		break;

	case LG_EV_JOIN_RX: {
		/* A window with no neighbour attached is the router's initial
		 * listen: it has to span every sub-slot a joiner might pick.
		 * Once a handshake is under way the peer's timing is known and
		 * one slot is enough.
		 */
		uint32_t span = c->neigh ? LG_JOIN_SLOT_LEN_US : LG_JOIN_WINDOW_LEN_US;

		ev->chan = lg_disc_rdv_chan(0);
		ev->ops = &lg_join_rx_ev_ops;
		ev->close_us = c->anchor_us + span;
		ev->len_us = span + LG_SCHED_LEAD_US + 2000;
		ev->at_us = c->anchor_us - LG_SCHED_LEAD_US - 2000;
		break;
	}

	case LG_EV_CHAN_EVAL:
		ev->chan = lg_chan_next_eval();
		ev->ops = &lg_chan_eval_ev_ops;
		ev->len_us = LG_CHAN_EVAL_LEN_US;
		ev->at_us = c->anchor_us - LG_SCHED_LEAD_US;
		break;

	default:
		break;
	}
}

static void sched_pick(struct k_work *work);

static void sched_idle(struct k_work *work)
{
	ARG_UNUSED(work);
	sched_pick(NULL);
}

static void sched_pick(struct k_work *work)
{
	struct candidate list[LG_CAND_MAX];
	uint8_t count = 0;
	const struct candidate *chosen;
	struct candidate best;
	uint64_t now;
	uint64_t floor_us;
	int err;

	ARG_UNUSED(work);

	/* An event that has been requested or is running owns the slot. Leaving
	 * it alone is what keeps lg.ev safe to touch from the interrupt path.
	 */
	if (!running || lg.ev.kind != LG_EV_NONE || lg_ts_granted() || lg_phy_busy()) {
		return;
	}

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		struct lg_neigh *n = &lg.neigh[i];

		switch (n->state) {
		case LG_NS_LINKED:
			add_candidate(list, &count, LG_EV_LINK, lg_link_next_event_us(n), n,
				      n->is_parent ? LG_PRIO_PARENT : LG_PRIO_NORMAL);
			break;
		case LG_NS_JOINING:
			add_candidate(list, &count,
				      lg_join_slot_is_tx() ? LG_EV_JOIN_TX : LG_EV_JOIN_RX,
				      lg_join_next_us(n), n, LG_PRIO_HANDSHAKE);
			break;
		default:
			break;
		}
	}

	add_candidate(list, &count, LG_EV_JOIN_RX, lg_join_win_next_us(), NULL, LG_PRIO_NORMAL);
	add_candidate(list, &count, LG_EV_BEACON, lg_disc_next_beacon_us(), NULL,
		      LG_PRIO_NORMAL);
	add_candidate(list, &count, LG_EV_SCAN, lg_disc_next_scan_us(), NULL, LG_PRIO_SCAN);
	add_candidate(list, &count, LG_EV_CHAN_EVAL, lg.next_chan_eval_us, NULL, LG_PRIO_SCAN);

	{
		struct lg_neigh *tracked;
		uint64_t at = lg_disc_next_track_us(&tracked);

		add_candidate(list, &count, LG_EV_TRACK, at, tracked, LG_PRIO_NORMAL);
	}

	chosen = select_candidate(list, count);
	best = chosen ? *chosen
		      : (struct candidate){.kind = LG_EV_NONE, .anchor_us = UINT64_MAX};

	if (best.kind == LG_EV_NONE) {
		/* Nothing wants the radio. Come back later rather than spinning. */
		k_work_reschedule_for_queue(lg_workq(), &idle_work,
					    K_USEC(LG_SCHED_IDLE_POLL_US));
		LOG_DBG("nothing scheduled, idling");
		return;
	}

	LOG_DBG("pick kind %u anchor %llu peer 0x%04x", best.kind, best.anchor_us,
		best.neigh ? best.neigh->addr : 0);

	/* A link event needs its frame encrypted before the interrupt path can
	 * use it, and this is the only context where that may happen.
	 */
	if (best.kind == LG_EV_LINK && !best.neigh->tx_pending) {
		if (!lg_link_stage_tx(best.neigh)) {
			/* Cannot talk to this neighbour; skip its event so it
			 * does not block everyone else.
			 */
			lg_link_advance(best.neigh);
			k_work_submit_to_queue(lg_workq(), &sched_work);
			return;
		}
	}

	event_plan(&lg.ev, &best);

	now = lg_ts_now_us();
	floor_us = now + LG_SCHED_MIN_HORIZON_US;
	if (lg.ev.at_us < floor_us) {
		/* We are already late. Rather than transmit at the wrong time and
		 * desynchronise a peer, drop this occurrence. The slot has to be
		 * released before retrying, or the guard at the top of this
		 * function will refuse to look again.
		 */
		lg.ev.kind = LG_EV_NONE;

		if (best.kind == LG_EV_LINK) {
			lg_link_advance(best.neigh);
			lg.stats.link_events_skipped++;
		} else if (best.kind == LG_EV_BEACON) {
			lg_disc_beacon_finish();
		} else if (best.kind == LG_EV_SCAN) {
			lg_disc_scan_finish();
		} else if (best.kind == LG_EV_TRACK) {
			lg_disc_track_result(best.neigh, false);
		} else if (best.kind == LG_EV_CHAN_EVAL) {
			lg.next_chan_eval_us =
				now + CONFIG_LOCKGRID_CHAN_EVAL_PERIOD_MS * 1000ULL;
		} else {
			lg_join_event_finish();
		}
		k_work_submit_to_queue(lg_workq(), &sched_work);
		return;
	}

	err = lg_ts_request(lg.ev.at_us, lg.ev.len_us, lg.ev.urgent, &lg.ev);
	if (err) {
		LOG_DBG("radio request for kind %u at %llu refused locally: %d", lg.ev.kind,
			lg.ev.at_us, err);
		lg.ev.kind = LG_EV_NONE;
		k_work_submit_to_queue(lg_workq(), &sched_work);
	}
}

void lg_sched_kick(void)
{
	if (running) {
		k_work_submit_to_queue(lg_workq(), &sched_work);
	}
}

void lg_sched_abort(enum lg_ev_kind kind)
{
	if (lg.ev.kind == kind && kind != LG_EV_NONE) {
		lg_sched_event_done();
	}
}

/* ------------------------------------------------------------------------- */
/* Event completion                                                           */
/* ------------------------------------------------------------------------- */

/**
 * Settle the finished event on the work queue: advance schedules, restage
 * frames, and pick the next event.
 */
static void sched_finish(struct k_work *work)
{
	ARG_UNUSED(work);

	switch (finish_kind) {
	case LG_EV_LINK:
		if (finish_neigh) {
			lg_link_event_finish(finish_neigh);
		}
		break;
	case LG_EV_BEACON:
		lg_disc_beacon_finish();
		lg_role_evaluate();
		break;
	case LG_EV_SCAN:
		lg_disc_scan_finish();
		/* Regular enough to serve as the periodic role review, without
		 * needing a timer of its own.
		 */
		lg_role_evaluate();
		break;
	case LG_EV_TRACK:
		/* Nothing to reschedule: the outcome either produced a fresh
		 * prediction from the beacon we heard, or cleared the stale one.
		 */
		break;
	case LG_EV_JOIN_TX:
	case LG_EV_JOIN_RX:
		lg_join_event_finish();
		break;
	case LG_EV_CHAN_EVAL:
		if (lg_chan_reevaluate()) {
			/* Tell every link about the new map, taking effect at a
			 * counter both ends can compute.
			 */
			for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
				struct lg_neigh *n = &lg.neigh[i];

				if (n->state == LG_NS_LINKED && n->initiator &&
				    !n->pending_ctrl) {
					n->pending_chan_map = lg.chan_map;
					n->instant = n->counter + 6;
					n->ctrl_acked = false;
					n->pending_ctrl |= BIT(LG_CTRL_CHAN_MAP);
				}
			}
		}
		lg.next_chan_eval_us =
			lg_ts_now_us() + CONFIG_LOCKGRID_CHAN_EVAL_PERIOD_MS * 1000ULL;
		break;
	default:
		break;
	}

	finish_kind = LG_EV_NONE;
	finish_neigh = NULL;
	sched_pick(NULL);
}

void lg_sched_event_done(void)
{
	lg_phy_stop();
	lg_ts_release();

	finish_kind = lg.ev.kind;
	finish_neigh = lg.ev.neigh;
	lg.ev.kind = LG_EV_NONE;

	if (running) {
		k_work_submit_to_queue(lg_workq(), &finish_work);
	}
}

/* ------------------------------------------------------------------------- */
/* Arbiter and PHY callbacks, all ISR context                                 */
/* ------------------------------------------------------------------------- */

static void ts_granted(uint64_t at_us, void *user)
{
	struct lg_event *ev = user;

	ARG_UNUSED(at_us);

	if (ev != &lg.ev || ev->kind == LG_EV_NONE || !ev->ops || !ev->ops->start) {
		lg_ts_release();
		return;
	}
	ev->ops->start(ev);
}

static void ts_failed(enum lg_ts_fail reason, void *user)
{
	struct lg_event *ev = user;

	ARG_UNUSED(reason);

	/* Counted here rather than in the per-kind handler so that every refusal
	 * shows up, not just the ones that happened to land on a link event. A node
	 * with no links yet is exactly the one whose refusals matter most.
	 */
	lg.stats.slots_blocked++;

	if (ev != &lg.ev || ev->kind == LG_EV_NONE) {
		return;
	}
	if (ev->ops && ev->ops->aborted) {
		ev->ops->aborted(ev);
	}
	lg_sched_event_done();
}

static void phy_tx_done(uint64_t air_end_us)
{
	struct lg_event *ev = &lg.ev;

	if (ev->kind != LG_EV_NONE && ev->ops && ev->ops->tx_done) {
		ev->ops->tx_done(ev, air_end_us);
	} else {
		lg_sched_event_done();
	}
}

static void phy_rx_done(const struct lg_phy_rx *rx)
{
	struct lg_event *ev = &lg.ev;

	if (ev->kind != LG_EV_NONE && ev->ops && ev->ops->rx_done) {
		ev->ops->rx_done(ev, rx);
	} else {
		lg_sched_event_done();
	}
}

static void phy_rx_failed(enum lg_phy_rx_fail reason)
{
	struct lg_event *ev = &lg.ev;

	if (ev->kind != LG_EV_NONE && ev->ops && ev->ops->rx_fail) {
		ev->ops->rx_fail(ev, reason);
	} else {
		lg_sched_event_done();
	}
}

static const struct lg_ts_cb ts_cb = {
	.granted = ts_granted,
	.failed = ts_failed,
};

static const struct lg_phy_cb phy_cb = {
	.tx_done = phy_tx_done,
	.rx_done = phy_rx_done,
	.rx_failed = phy_rx_failed,
};

/* ------------------------------------------------------------------------- */
/* Channel evaluation event                                                   */
/* ------------------------------------------------------------------------- */

static void chan_eval_start(struct lg_event *ev)
{
	int8_t dbm;

	lg_power_activity(LG_PWR_ACT_CHAN_EVAL);

	/* One energy detect sweep with the receiver on. Short, and it is the
	 * only way a retired channel ever gets to come back.
	 */
	dbm = lg_phy_noise_floor(ev->chan, 512);
	if (dbm != INT8_MIN) {
		lg_chan_noise(ev->chan, dbm);
	}

	lg_sched_event_done();
}

static void chan_eval_aborted(struct lg_event *ev)
{
	ARG_UNUSED(ev);
}

const struct lg_ev_ops lg_chan_eval_ev_ops = {
	.start = chan_eval_start,
	.tx_done = NULL,
	.rx_done = NULL,
	.rx_fail = NULL,
	.aborted = chan_eval_aborted,
};

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

int lg_sched_init(void)
{
	int err;

	k_work_init(&sched_work, sched_pick);
	k_work_init(&finish_work, sched_finish);
	k_work_init_delayable(&idle_work, sched_idle);

	err = lg_phy_init(&phy_cb);
	if (err) {
		return err;
	}

	err = lg_ts_init(&ts_cb);
	if (err) {
		return err;
	}

	lg.ev.kind = LG_EV_NONE;
	running = true;
	return 0;
}

void lg_sched_stop(void)
{
	running = false;
	k_work_cancel_delayable(&idle_work);
	lg_phy_stop();
	lg_ts_release();
	lg_ts_uninit();
	lg.ev.kind = LG_EV_NONE;
}
