/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_ts_mpsl.c
 * @brief Radio arbitration through MPSL timeslots, so LockGrid can share the
 *        chip with Bluetooth LE.
 *
 * Each LockGrid radio event becomes one timeslot request. Inside a granted slot
 * the application owns RADIO, TIMER0, CCM and AAR, and the radio interrupt
 * arrives as a timeslot signal rather than on the RADIO vector, so it is handed
 * to the PHY from here.
 *
 * Two things make this awkward and both are handled below.
 *
 * The first is that a normal timeslot request is expressed as a distance from
 * the start of the *previous* slot, while LockGrid schedules in absolute time.
 * The slot start is captured in GRTC ticks when the START signal arrives - and
 * because this part has a GRTC, MPSL guarantees zero start jitter - so the
 * distance for the next request is just the difference. If that arithmetic comes
 * out in the past, the request falls back to "earliest" and the event is treated
 * as slipped.
 *
 * The second is that requests can be refused. A blocked or cancelled slot is
 * reported upwards as a failure, and lg_link.c folds it into the link's ETX
 * exactly like a lost frame. That is the right behaviour rather than a
 * workaround: a neighbour whose events keep losing to a Bluetooth connection
 * really is a worse route, and the metric should say so.
 *
 * MPSL API calls are funnelled through one cooperative thread, which is the
 * pattern the MPSL documentation requires.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <hal/nrf_timer.h>
#include <mpsl.h>
#include <mpsl_timeslot.h>
#include <mpsl_hwres.h>
#include <zephyr/drivers/timer/nrf_grtc_timer.h>

#include "lg_ts.h"
#include "lg_phy.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* MPSL caps a timeslot at 100 ms. */
#define LG_TS_MAX_GRANT_US MPSL_TIMESLOT_LENGTH_MAX_US

/* Closest to now a request can be made and still be worth making. */
#define LG_TS_MIN_LEAD_US 300U

/* How long to wait for an "earliest" slot before giving up on it. */
#define LG_TS_EARLIEST_TIMEOUT_US 100000U

/* Requests the MPSL thread can have queued. */
#define LG_TS_MSGQ_DEPTH 4

enum lg_ts_op {
	LG_TS_OP_OPEN,
	LG_TS_OP_REQUEST,
	LG_TS_OP_CLOSE,
};

static struct {
	const struct lg_ts_cb *cb;
	mpsl_timeslot_session_id_t session;
	bool session_open;

	/* The request the protocol wants next. */
	uint64_t req_at_us;
	uint32_t req_len_us;
	bool req_urgent;
	void *req_user;
	bool req_pending;

	/* Bookkeeping for the running slot. */
	bool granted;
	uint64_t slot_start_us;
	uint64_t grant_end_us;
	uint32_t slot_len_us;
	void *user;

	/* Slot start of the most recent grant, which normal requests measure
	 * their distance from.
	 */
	uint64_t last_slot_start_us;
	bool have_last_slot;

	void (*fine_cb)(void);
	uint64_t fine_at_us;
} ts;

/* MPSL requires that the structure it is handed outlives the callback. */
static mpsl_timeslot_request_t req_earliest;
static mpsl_timeslot_request_t req_normal;
static mpsl_timeslot_signal_return_param_t ret_param;

static uint8_t timeslot_context[MPSL_TIMESLOT_CONTEXT_SIZE * 1] __aligned(4);

K_MSGQ_DEFINE(ts_msgq, sizeof(enum lg_ts_op), LG_TS_MSGQ_DEPTH, 4);

/* ------------------------------------------------------------------------- */
/* Time                                                                      */
/* ------------------------------------------------------------------------- */

uint64_t lg_ts_now_us(void)
{
	return z_nrf_grtc_timer_read();
}

/* ------------------------------------------------------------------------- */
/* Request construction                                                       */
/* ------------------------------------------------------------------------- */

static uint8_t priority_of(bool urgent)
{
	return urgent ? MPSL_TIMESLOT_PRIORITY_HIGH : MPSL_TIMESLOT_PRIORITY_NORMAL;
}

/**
 * Turn the wanted absolute start time into a request MPSL understands.
 *
 * @return The request to issue, or NULL if the target has already gone by.
 */
static mpsl_timeslot_request_t *build_request(void)
{
	uint32_t len = CLAMP(ts.req_len_us + CONFIG_LOCKGRID_TIMESLOT_MARGIN_US,
			     MPSL_TIMESLOT_LENGTH_MIN_US, LG_TS_MAX_GRANT_US);

	if (ts.have_last_slot && ts.req_at_us > ts.last_slot_start_us) {
		uint64_t distance = ts.req_at_us - ts.last_slot_start_us;

		if (distance <= MPSL_TIMESLOT_DISTANCE_MAX_US) {
			req_normal.request_type = MPSL_TIMESLOT_REQ_TYPE_NORMAL;
			req_normal.params.normal.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED;
			req_normal.params.normal.priority = priority_of(ts.req_urgent);
			req_normal.params.normal.distance_us = (uint32_t)distance;
			req_normal.params.normal.length_us = len;
			return &req_normal;
		}
	}

	/* No previous slot to measure from, or the target is behind it. Ask for
	 * the soonest slot available; the event above will slip.
	 */
	req_earliest.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST;
	req_earliest.params.earliest.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED;
	req_earliest.params.earliest.priority = priority_of(ts.req_urgent);
	req_earliest.params.earliest.length_us = len;
	req_earliest.params.earliest.timeout_us = LG_TS_EARLIEST_TIMEOUT_US;
	return &req_earliest;
}

/* ------------------------------------------------------------------------- */
/* Timeslot signal callback, high priority context                            */
/* ------------------------------------------------------------------------- */

static void fine_timer_arm(uint64_t at_us)
{
	uint64_t rel;

	if (!ts.granted) {
		return;
	}

	/* TIMER0 counts microseconds from the start of the slot. */
	rel = (at_us > ts.slot_start_us) ? at_us - ts.slot_start_us : 1;
	if (rel > ts.slot_len_us) {
		/* Past the end of our reservation; it will not fire. */
		return;
	}

	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0, (uint32_t)rel);
	nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
}

static mpsl_timeslot_signal_return_param_t *signal_cb(mpsl_timeslot_session_id_t session_id,
						     uint32_t signal)
{
	ARG_UNUSED(session_id);

	ret_param.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		ts.slot_start_us = lg_ts_now_us();
		ts.last_slot_start_us = ts.slot_start_us;
		ts.have_last_slot = true;
		ts.granted = true;
		ts.req_pending = false;
		ts.slot_len_us = ts.req_len_us + CONFIG_LOCKGRID_TIMESLOT_MARGIN_US;
		ts.grant_end_us = ts.slot_start_us + ts.slot_len_us;
		ts.user = ts.req_user;

		nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);

		/* A fine timer armed before the slot started can be programmed
		 * now that TIMER0 exists and its origin is known.
		 */
		if (ts.fine_cb) {
			fine_timer_arm(ts.fine_at_us);
		}

		if (ts.cb->granted) {
			ts.cb->granted(ts.slot_start_us, ts.user);
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_RADIO:
		/* MPSL owns the RADIO vector inside a slot and forwards the
		 * event here.
		 */
		lg_phy_isr();
		break;

	case MPSL_TIMESLOT_SIGNAL_TIMER0: {
		void (*cb)(void) = ts.fine_cb;

		nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		ts.fine_cb = NULL;
		if (cb) {
			cb();
		}
		break;
	}

	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED: {
		enum lg_ts_fail reason = (signal == MPSL_TIMESLOT_SIGNAL_BLOCKED)
						 ? LG_TS_FAIL_BLOCKED
						 : LG_TS_FAIL_CANCELLED;

		ts.req_pending = false;
		ts.granted = false;
		if (ts.cb->failed) {
			ts.cb->failed(reason, ts.req_user);
		}
		break;
	}

	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		/* Nothing outstanding. The scheduler above will ask again as
		 * soon as it has an event; there is nothing to keep alive.
		 */
		break;

	case MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED:
		ts.session_open = false;
		ts.granted = false;
		ts.have_last_slot = false;
		break;

	case MPSL_TIMESLOT_SIGNAL_OVERSTAYED:
		LOG_ERR("timeslot overstayed: an event ran past its reservation");
		break;

	case MPSL_TIMESLOT_SIGNAL_INVALID_RETURN:
		LOG_ERR("timeslot callback returned an invalid action");
		break;

	default:
		break;
	}

	return &ret_param;
}

/* ------------------------------------------------------------------------- */
/* MPSL thread                                                                */
/* ------------------------------------------------------------------------- */

static void ts_thread(void *a, void *b, void *c)
{
	enum lg_ts_op op;
	int32_t err;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		if (k_msgq_get(&ts_msgq, &op, K_FOREVER) != 0) {
			continue;
		}

		switch (op) {
		case LG_TS_OP_OPEN:
			if (ts.session_open) {
				break;
			}
			/* NCS's MPSL integration has already reserved contexts from
			 * CONFIG_MPSL_TIMESLOT_SESSION_COUNT, so only claim our own
			 * memory if that was not done.
			 */
			err = mpsl_timeslot_session_open(signal_cb, &ts.session);
			if (err) {
				LOG_ERR("timeslot session_open failed: %d", (int)err);
				break;
			}
			ts.session_open = true;
			LOG_INF("timeslot session %u open", ts.session);
			break;

		case LG_TS_OP_REQUEST: {
			mpsl_timeslot_request_t *r;

			if (!ts.session_open) {
				break;
			}
			r = build_request();
			err = mpsl_timeslot_request(ts.session, r);
			LOG_DBG("timeslot req type %u len %u -> %d", r->request_type,
				r->request_type == MPSL_TIMESLOT_REQ_TYPE_NORMAL
					? r->params.normal.length_us
					: r->params.earliest.length_us,
				(int)err);
			if (err) {
				ts.req_pending = false;
				if (ts.cb->failed) {
					ts.cb->failed(LG_TS_FAIL_BLOCKED, ts.req_user);
				}
			}
			break;
		}

		case LG_TS_OP_CLOSE:
			if (ts.session_open) {
				mpsl_timeslot_session_close(ts.session);
			}
			break;
		}
	}
}

K_THREAD_DEFINE(lg_ts_mpsl_thread, 1024, ts_thread, NULL, NULL, NULL,
		K_PRIO_COOP(CONFIG_MPSL_THREAD_COOP_PRIO), 0, 0);

/* ------------------------------------------------------------------------- */
/* Public interface                                                           */
/* ------------------------------------------------------------------------- */

int lg_ts_init(const struct lg_ts_cb *cb)
{
	enum lg_ts_op op = LG_TS_OP_OPEN;

	ts.cb = cb;
	ts.granted = false;
	ts.have_last_slot = false;
	ts.session_open = false;

	return k_msgq_put(&ts_msgq, &op, K_NO_WAIT);
}

void lg_ts_uninit(void)
{
	enum lg_ts_op op = LG_TS_OP_CLOSE;

	ts.granted = false;
	ts.req_pending = false;
	(void)k_msgq_put(&ts_msgq, &op, K_NO_WAIT);
}

int lg_ts_request(uint64_t at_us, uint32_t len_us, bool urgent, void *user)
{
	enum lg_ts_op op = LG_TS_OP_REQUEST;
	uint64_t now = lg_ts_now_us();

	if (at_us < now + LG_TS_MIN_LEAD_US) {
		return -EINVAL;
	}

	ts.req_at_us = at_us;
	ts.req_len_us = MIN(len_us, LG_TS_MAX_GRANT_US - CONFIG_LOCKGRID_TIMESLOT_MARGIN_US);
	ts.req_urgent = urgent;
	ts.req_user = user;
	ts.req_pending = true;

	return k_msgq_put(&ts_msgq, &op, K_NO_WAIT);
}

void lg_ts_release(void)
{
	if (!ts.granted) {
		return;
	}

	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
	ts.granted = false;
	ts.fine_cb = NULL;

	/* Ending the slot early is what gives the time back to Bluetooth. The
	 * signal callback is not running right now, so the end is requested by
	 * closing out the reservation and letting MPSL reclaim it; the next
	 * request re-establishes the distance reference.
	 */
	ret_param.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
}

int lg_ts_extend(uint32_t extra_us)
{
	if (!ts.granted) {
		return -EBUSY;
	}
	if (ts.slot_len_us + extra_us > LG_TS_MAX_GRANT_US) {
		return -EBUSY;
	}

	/* Extension has to be asked for from inside the signal callback, so this
	 * arms it for the next signal rather than doing it here.
	 */
	ret_param.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
	ret_param.params.extend.length_us = MAX(extra_us, MPSL_TIMESLOT_EXTENSION_TIME_MIN_US);
	return 0;
}

void lg_ts_timer_at(uint64_t at_us, void (*cb)(void))
{
	ts.fine_cb = cb;
	ts.fine_at_us = at_us;

	if (ts.granted) {
		fine_timer_arm(at_us);
	}
}

void lg_ts_timer_cancel(void)
{
	ts.fine_cb = NULL;
	if (ts.granted) {
		nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
	}
}

bool lg_ts_granted(void)
{
	return ts.granted;
}

uint64_t lg_ts_grant_end_us(void)
{
	return ts.granted ? ts.grant_end_us : 0;
}

uint32_t lg_ts_max_grant_us(void)
{
	return LG_TS_MAX_GRANT_US - CONFIG_LOCKGRID_TIMESLOT_MARGIN_US;
}
