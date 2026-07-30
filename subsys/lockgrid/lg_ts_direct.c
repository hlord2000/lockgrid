/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_ts_direct.c
 * @brief Radio arbitration when LockGrid owns the radio outright.
 *
 * Two GRTC compare channels: one fires at the start of each scheduled radio
 * event, the other handles the sub-event timing (turnarounds, window closes).
 * Requests are always granted, at exactly the requested microsecond, so this
 * backend is also the reference against which the MPSL backend's jitter and
 * refusals can be judged.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/timer/nrf_grtc_timer.h>

#include "lg_ts.h"
#include "lg_phy.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* Matches the MPSL backend's ceiling so the scheduler above cannot come to
 * depend on unbounded grants.
 */
#define LG_TS_MAX_GRANT_US 100000U

/* Closest to now a compare channel can be armed and still fire. */
#define LG_TS_MIN_LEAD_US 5U

static struct {
	const struct lg_ts_cb *cb;
	int32_t chan_grant;
	int32_t chan_fine;
	bool granted;
	uint64_t grant_end_us;
	void *user;
	void (*fine_cb)(void);
	bool req_pending;
	bool req_refuse;
	uint64_t req_at_us;
	uint32_t req_len_us;
} ts = {
	.chan_grant = -1,
	.chan_fine = -1,
};

/**
 * Whether to refuse this request, when the synthetic contention mode is on.
 *
 * Refusing at the moment the slot was due is the harshest case: the event above
 * loses its whole opportunity rather than being told early enough to reschedule.
 * That is deliberate - it is what a cancelled MPSL timeslot costs.
 */
static bool refuse_request(void)
{
	if (CONFIG_LOCKGRID_TS_REFUSE_PERMILLE == 0) {
		return false;
	}
	return (sys_rand32_get() % 1000U) < (uint32_t)CONFIG_LOCKGRID_TS_REFUSE_PERMILLE;
}

static void grant_handler(int32_t id, uint64_t expire_time, void *user_data)
{
	ARG_UNUSED(id);
	ARG_UNUSED(user_data);

	if (!ts.req_pending) {
		return;
	}
	ts.req_pending = false;

	if (ts.req_refuse) {
		ts.req_refuse = false;
		if (ts.cb->failed) {
			ts.cb->failed(LG_TS_FAIL_BLOCKED, ts.user);
		}
		return;
	}

	ts.granted = true;
	ts.grant_end_us = ts.req_at_us + ts.req_len_us;

	if (ts.cb->granted) {
		ts.cb->granted(expire_time, ts.user);
	}
}

static void fine_handler(int32_t id, uint64_t expire_time, void *user_data)
{
	void (*cb)(void) = ts.fine_cb;

	ARG_UNUSED(id);
	ARG_UNUSED(expire_time);
	ARG_UNUSED(user_data);

	ts.fine_cb = NULL;
	if (cb) {
		cb();
	}
}

static void radio_isr_trampoline(const void *arg)
{
	ARG_UNUSED(arg);
	lg_phy_isr();
}

int lg_ts_init(const struct lg_ts_cb *cb)
{
	ts.cb = cb;

	/* Owning the radio means owning its interrupt vector too. */
	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(radio)), 0, radio_isr_trampoline, NULL, 0);
	irq_enable(DT_IRQN(DT_NODELABEL(radio)));

	ts.chan_grant = z_nrf_grtc_timer_chan_alloc();
	if (ts.chan_grant < 0) {
		LOG_ERR("no GRTC channel for event scheduling");
		return -ENOMEM;
	}

	ts.chan_fine = z_nrf_grtc_timer_chan_alloc();
	if (ts.chan_fine < 0) {
		z_nrf_grtc_timer_chan_free(ts.chan_grant);
		ts.chan_grant = -1;
		LOG_ERR("no GRTC channel for sub-event timing");
		return -ENOMEM;
	}

	return 0;
}

void lg_ts_uninit(void)
{
	if (ts.chan_grant >= 0) {
		z_nrf_grtc_timer_abort(ts.chan_grant);
		z_nrf_grtc_timer_chan_free(ts.chan_grant);
		ts.chan_grant = -1;
	}
	if (ts.chan_fine >= 0) {
		z_nrf_grtc_timer_abort(ts.chan_fine);
		z_nrf_grtc_timer_chan_free(ts.chan_fine);
		ts.chan_fine = -1;
	}
	ts.granted = false;
	ts.req_pending = false;
}

uint64_t lg_ts_now_us(void)
{
	return z_nrf_grtc_timer_read();
}

int lg_ts_request(uint64_t at_us, uint32_t len_us, bool urgent, void *user)
{
	uint64_t now = lg_ts_now_us();
	int err;

	ARG_UNUSED(urgent);

	if (at_us < now + LG_TS_MIN_LEAD_US) {
		return -EINVAL;
	}

	z_nrf_grtc_timer_abort(ts.chan_grant);

	ts.req_at_us = at_us;
	ts.req_len_us = MIN(len_us, LG_TS_MAX_GRANT_US);
	ts.user = user;
	ts.req_pending = true;
	ts.req_refuse = refuse_request();

	err = z_nrf_grtc_timer_set(ts.chan_grant, at_us, grant_handler, NULL);
	if (err) {
		ts.req_pending = false;
		return err;
	}

	return 0;
}

void lg_ts_release(void)
{
	ts.granted = false;
	ts.grant_end_us = 0;
}

int lg_ts_extend(uint32_t extra_us)
{
	if (!ts.granted) {
		return -EBUSY;
	}
	if (ts.grant_end_us - ts.req_at_us + extra_us > LG_TS_MAX_GRANT_US) {
		return -EBUSY;
	}
	ts.grant_end_us += extra_us;
	return 0;
}

void lg_ts_timer_at(uint64_t at_us, void (*cb)(void))
{
	z_nrf_grtc_timer_abort(ts.chan_fine);
	ts.fine_cb = cb;

	if (z_nrf_grtc_timer_set(ts.chan_fine, at_us, fine_handler, NULL)) {
		/* Target already gone by; run it now rather than stalling the
		 * event that is waiting on it.
		 */
		ts.fine_cb = NULL;
		if (cb) {
			cb();
		}
	}
}

void lg_ts_timer_cancel(void)
{
	ts.fine_cb = NULL;
	z_nrf_grtc_timer_abort(ts.chan_fine);
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
	return LG_TS_MAX_GRANT_US;
}
