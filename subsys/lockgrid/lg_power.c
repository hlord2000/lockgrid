/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_power.c
 * @brief Radio energy accounting, split by what the radio was doing.
 *
 * The point of this module is to make the protocol's central claim checkable:
 * that receive time dominates, and that the scheduling decisions - narrow
 * windows, drift-driven widening, early close, subrating, gossip instead of
 * scanning - actually move the number.
 *
 * Air time is the easy part. The part that is easy to get wrong, and that this
 * model originally did get wrong, is the fixed cost of turning the radio on:
 *
 *     pre-processing      13 us @ 2.2 mA     29 nC
 *     crystal ramp       380 us @ 1.0 mA    380 nC
 *     settle             500 us @ 0.6 mA    300 nC
 *     start radio        100 us @ 2.4 mA    240 nC
 *     turnaround         143 us @ 1.2 mA    172 nC
 *     standby between     58 us @ 2.9 mA    168 nC
 *                                        --------
 *                                          1289 nC
 *
 * Against roughly 3.8 uC for the air time of a keepalive, that is a sixth of an
 * idle connection event - and it is charged whether the event carries a payload
 * or nothing at all. Two things follow. Skipping an event saves the fixed cost
 * as well as the air time, which makes subrating far more valuable than air time
 * alone suggests. And shortening the connection interval is disproportionately
 * expensive, because the fixed cost is paid more often while the payload does
 * not change.
 *
 * The crystal ramp and settle are the largest single item at 680 nC, 53% of the
 * fixed cost. CONFIG_LOCKGRID_PWR_HFXO_ALWAYS_ON drops them, which is the right
 * model only if something else keeps the crystal running.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "lg_internal.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

#if defined(CONFIG_LOCKGRID_POWER_ACCOUNTING)

/** Charge in nanocoulombs for a phase given in microamps and microseconds. */
#define PHASE_NC(ua, us) (((uint64_t)(ua) * (us)) / 1000U)

/* Cost of bringing the radio up for an event. */
#define LG_NC_PRE    PHASE_NC(CONFIG_LOCKGRID_PWR_PRE_UA, CONFIG_LOCKGRID_PWR_PRE_US)
#define LG_NC_XTAL   PHASE_NC(CONFIG_LOCKGRID_PWR_XTAL_UA, CONFIG_LOCKGRID_PWR_XTAL_US)
#define LG_NC_SETTLE PHASE_NC(CONFIG_LOCKGRID_PWR_SETTLE_UA, CONFIG_LOCKGRID_PWR_SETTLE_US)
#define LG_NC_START  PHASE_NC(CONFIG_LOCKGRID_PWR_START_UA, CONFIG_LOCKGRID_PWR_START_US)

/* Cost of turning the radio round inside an event. */
#define LG_NC_SWITCH  PHASE_NC(CONFIG_LOCKGRID_PWR_SWITCH_UA, CONFIG_LOCKGRID_PWR_SWITCH_US)
#define LG_NC_STANDBY PHASE_NC(CONFIG_LOCKGRID_PWR_STANDBY_UA, CONFIG_LOCKGRID_PWR_STANDBY_US)

#if defined(CONFIG_LOCKGRID_PWR_HFXO_ALWAYS_ON)
#define LG_NC_EVENT_START (LG_NC_PRE + LG_NC_START)
#define LG_US_EVENT_START (CONFIG_LOCKGRID_PWR_PRE_US + CONFIG_LOCKGRID_PWR_START_US)
#else
#define LG_NC_EVENT_START (LG_NC_PRE + LG_NC_XTAL + LG_NC_SETTLE + LG_NC_START)
#define LG_US_EVENT_START                                                                          \
	(CONFIG_LOCKGRID_PWR_PRE_US + CONFIG_LOCKGRID_PWR_XTAL_US +                                \
	 CONFIG_LOCKGRID_PWR_SETTLE_US + CONFIG_LOCKGRID_PWR_START_US)
#endif

#define LG_NC_TURNAROUND (LG_NC_SWITCH + LG_NC_STANDBY)
#define LG_US_TURNAROUND (CONFIG_LOCKGRID_PWR_SWITCH_US + CONFIG_LOCKGRID_PWR_STANDBY_US)

static struct {
	uint64_t start_us;
	enum lg_pwr_act act;
	uint64_t total_nc;
	uint64_t rx_us[5];
	uint64_t tx_us[5];
	/* Fixed overhead, kept separate so it can be seen rather than buried. */
	uint64_t fixed_nc;
	uint64_t fixed_us;
	uint32_t starts;
	uint32_t turnarounds;
	uint64_t radio_on_us;
} pw;

void lg_power_init(void)
{
	memset(&pw, 0, sizeof(pw));
	pw.start_us = lg_ts_now_us();
	pw.act = LG_PWR_ACT_LINK;
}

void lg_power_activity(enum lg_pwr_act act)
{
	pw.act = act;
}

void lg_power_event_start(void)
{
	pw.starts++;
	pw.fixed_nc += LG_NC_EVENT_START;
	pw.fixed_us += LG_US_EVENT_START;
	pw.total_nc += LG_NC_EVENT_START;
	pw.radio_on_us += LG_US_EVENT_START;
}

void lg_power_turnaround(void)
{
	pw.turnarounds++;
	pw.fixed_nc += LG_NC_TURNAROUND;
	pw.fixed_us += LG_US_TURNAROUND;
	pw.total_nc += LG_NC_TURNAROUND;
	pw.radio_on_us += LG_US_TURNAROUND;
}

void lg_power_charge(enum lg_phy_state state, uint32_t us)
{
	uint32_t ua;

	switch (state) {
	case LG_PHY_STATE_TX:
		ua = CONFIG_LOCKGRID_PWR_TX_UA;
		pw.tx_us[pw.act] += us;
		break;
	case LG_PHY_STATE_RX:
		ua = CONFIG_LOCKGRID_PWR_RX_UA;
		pw.rx_us[pw.act] += us;
		break;
	default:
		/* Ramp and settle are accounted as part of the fixed cost of an
		 * event start, not as a state the radio passes through.
		 */
		return;
	}

	pw.radio_on_us += us;
	pw.total_nc += ((uint64_t)ua * us) / 1000U;
}

void lg_power_report_get(struct lg_power_report *out)
{
	uint64_t elapsed = lg_ts_now_us() - pw.start_us;
	uint64_t sleep_nc;

	memset(out, 0, sizeof(*out));
	out->elapsed_us = elapsed;
	out->radio_on_us = pw.radio_on_us;

	out->rx_us_scan = pw.rx_us[LG_PWR_ACT_SCAN];
	out->rx_us_link = pw.rx_us[LG_PWR_ACT_LINK];
	out->rx_us_join = pw.rx_us[LG_PWR_ACT_JOIN];
	out->rx_us_chan_eval = pw.rx_us[LG_PWR_ACT_CHAN_EVAL];
	out->tx_us_beacon = pw.tx_us[LG_PWR_ACT_BEACON];
	out->tx_us_link = pw.tx_us[LG_PWR_ACT_LINK];
	out->tx_us_join = pw.tx_us[LG_PWR_ACT_JOIN];

	out->fixed_us = pw.fixed_us;
	out->fixed_nc = pw.fixed_nc;
	out->radio_starts = pw.starts;
	out->turnarounds = pw.turnarounds;
	/* Retained for callers that only want a single "overhead" figure. */
	out->ramp_us = pw.fixed_us;

	/* Charge the sleep current for the time the radio was off, so the average
	 * is comparable to a datasheet figure.
	 *
	 * Nanoamps times microseconds is femtocoulombs, so the divisor is a
	 * million to land in nanocoulombs - unlike the radio phases, which are in
	 * microamps and only need a thousand. Getting this wrong put the sleep term
	 * three orders of magnitude high, which swamped every radio cost and made
	 * the whole report read as milliamps.
	 */
	sleep_nc = elapsed > pw.radio_on_us
			   ? ((elapsed - pw.radio_on_us) * CONFIG_LOCKGRID_PWR_SLEEP_NA) / 1000000U
			   : 0;
	out->total_nc = pw.total_nc + sleep_nc;

	if (elapsed) {
		/* nanocoulombs / microseconds = milliamps; scale to nanoamps. */
		out->avg_na = (out->total_nc * 1000000U) / elapsed;
		out->duty_permille = (uint32_t)((pw.radio_on_us * 1000U) / elapsed);
	}
}

void lg_power_report_get_steady(struct lg_power_report *out)
{
	uint64_t one_off_nc;

	lg_power_report_get(out);

	/* Scanning and joining are paid at power-on, not forever. A product's
	 * battery life is set by what is left once the node has settled, so that
	 * figure is reported separately rather than being averaged in with the
	 * cost of getting there.
	 */
	one_off_nc = ((uint64_t)out->rx_us_scan * CONFIG_LOCKGRID_PWR_RX_UA) / 1000U +
		     ((uint64_t)out->rx_us_join * CONFIG_LOCKGRID_PWR_RX_UA) / 1000U +
		     ((uint64_t)out->tx_us_join * CONFIG_LOCKGRID_PWR_TX_UA) / 1000U;

	if (out->elapsed_us && out->total_nc > one_off_nc) {
		out->steady_na = ((out->total_nc - one_off_nc) * 1000000U) / out->elapsed_us;
	} else {
		out->steady_na = out->avg_na;
	}
}

void lg_power_report_print(void)
{
	struct lg_power_report r;
	uint64_t rx_total, tx_total;

	lg_power_report_get_steady(&r);
	rx_total = r.rx_us_scan + r.rx_us_link + r.rx_us_join + r.rx_us_chan_eval;
	tx_total = r.tx_us_beacon + r.tx_us_link + r.tx_us_join;

	LOG_INF("radio on %llu us of %llu ms = %u.%u%% duty", r.radio_on_us,
		r.elapsed_us / 1000U, r.duty_permille / 10U, r.duty_permille % 10U);
	LOG_INF("  current: %llu.%03llu uA including discovery, %llu.%03llu uA steady state",
		r.avg_na / 1000U, r.avg_na % 1000U, r.steady_na / 1000U, r.steady_na % 1000U);
	LOG_INF("  rx %llu us: link %llu, scan %llu, join %llu, chan %llu", rx_total,
		r.rx_us_link, r.rx_us_scan, r.rx_us_join, r.rx_us_chan_eval);
	LOG_INF("  tx %llu us: link %llu, beacon %llu, join %llu", tx_total, r.tx_us_link,
		r.tx_us_beacon, r.tx_us_join);
	LOG_INF("  fixed %llu us / %llu nC over %u starts and %u turnarounds", r.fixed_us,
		r.fixed_nc, r.radio_starts, r.turnarounds);
	LOG_INF("  charge %llu nC total", r.total_nc);
	if (r.total_nc) {
		LOG_INF("  fixed overhead is %llu%% of all charge",
			(r.fixed_nc * 100U) / r.total_nc);
	}
}

#else /* !CONFIG_LOCKGRID_POWER_ACCOUNTING */

void lg_power_init(void)
{
}

void lg_power_activity(enum lg_pwr_act act)
{
	ARG_UNUSED(act);
}

void lg_power_event_start(void)
{
}

void lg_power_turnaround(void)
{
}

void lg_power_charge(enum lg_phy_state state, uint32_t us)
{
	ARG_UNUSED(state);
	ARG_UNUSED(us);
}

void lg_power_report_get(struct lg_power_report *out)
{
	memset(out, 0, sizeof(*out));
}

void lg_power_report_get_steady(struct lg_power_report *out)
{
	memset(out, 0, sizeof(*out));
}

void lg_power_report_print(void)
{
}

#endif /* CONFIG_LOCKGRID_POWER_ACCOUNTING */
