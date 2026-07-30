/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief LockGrid BabbleSim test node.
 *
 * One binary drives every scenario. Each node is told its address, role and what
 * it should be able to prove by the end of the run; when the check window
 * closes, it evaluates its expectations and fails the simulation through
 * bs_trace_error_line() if any of them did not hold. That makes a failure an
 * exit code the runner scripts already know how to notice, rather than something
 * a human has to spot in a log.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <lockgrid/lockgrid.h>
#include <lockgrid/lg_provision_sim.h>

#include <posix_native_task.h>
#include <nsi_main.h>
#include "bs_cmd_line.h"
#include "bs_dynargs.h"
#include "bs_tracing.h"
#include "bsim_args_runner.h"


LOG_MODULE_REGISTER(lgtest, LOG_LEVEL_INF);

#define LOCKGRID_NETWORK_ID 0x10CC6217UL
#define TEST_PORT           9

static uint32_t arg_addr;
static uint32_t arg_role = LG_ROLE_ROUTER;
static uint32_t arg_period_ms;
static uint32_t arg_stop_ms;
static uint32_t arg_delay_ms;
static uint32_t arg_check_ms = 40000;
static uint32_t arg_snap_ms;    /* trace snapshot period, 0 for none */
static uint32_t arg_uplink_ms;  /* withdraw a backhaul's uplink at this uptime */
static uint32_t arg_flap_ms;    /* and then keep losing it this often */
static uint32_t arg_flap_down_ms = 15000; /* staying down this long each time */
static uint32_t arg_report = 1; /* print the human-readable reports at check time */

/* Expectations, all optional. */
static uint32_t exp_route;      /* must have a route to a backhaul */
static uint32_t exp_links;      /* at least this many authenticated links */
static uint32_t exp_rx_from;    /* must have received from this many distinct nodes */
static uint32_t exp_joins;      /* at least this many successful handshakes */
static uint32_t exp_duty_max;   /* radio on-time ceiling, in tenths of a percent */
static uint32_t exp_no_authfail;/* no authentication failure may have occurred */
static uint32_t exp_min_blocked; /* radio requests that must have been refused */
static uint32_t exp_role;        /* role the node must have settled on, plus one */
static uint32_t exp_max_gap_ms;  /* longest tolerable stretch with no route */


/*
 * BabbleSim presets every unsigned command line option to UINT_MAX before
 * parsing, so "not given" is not zero. Anything with a meaningful default has to
 * be normalised before use, and the optional expectations have to distinguish
 * absent from zero.
 */
#define LG_ARG_UNSET UINT32_MAX

static uint32_t arg_or(uint32_t v, uint32_t fallback)
{
	return (v == LG_ARG_UNSET) ? fallback : v;
}

static bool arg_given(uint32_t v)
{
	return v != LG_ARG_UNSET && v != 0;
}

/* Observations. */
static uint32_t rx_total;
static lg_addr_t rx_sources[8];
static uint8_t rx_source_count;

static void note_source(lg_addr_t src)
{
	for (uint8_t i = 0; i < rx_source_count; i++) {
		if (rx_sources[i] == src) {
			return;
		}
	}
	if (rx_source_count < ARRAY_SIZE(rx_sources)) {
		rx_sources[rx_source_count++] = src;
	}
}

static void on_rx(const struct lg_rx_info *info, const uint8_t *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);

	rx_total++;
	note_source(info->src);
	LOG_INF("rx from 0x%04x via 0x%04x, %u hops", info->src, info->prev_hop, info->hops);
}

static void on_evt(enum lg_evt evt, lg_addr_t peer)
{
	static const char *const names[] = {
		"link up", "link down", "parent changed", "route lost", "auth failed",
		"role changed",
	};

	LOG_INF("%s 0x%04x, rank now %u", evt < ARRAY_SIZE(names) ? names[evt] : "?", peer,
		lg_rank());
}

static const struct lg_callbacks callbacks = {.rx = on_rx, .evt = on_evt};

/* ------------------------------------------------------------------------- */
/* Periodic work                                                             */
/* ------------------------------------------------------------------------- */

static void telemetry_fn(struct k_work *work);
static void check_fn(struct k_work *work);
static void stop_fn(struct k_work *work);
static void snap_fn(struct k_work *work);
static void uplink_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_fn);
static K_WORK_DELAYABLE_DEFINE(check_work, check_fn);
static K_WORK_DELAYABLE_DEFINE(stop_work, stop_fn);
static K_WORK_DELAYABLE_DEFINE(snap_work, snap_fn);
static K_WORK_DELAYABLE_DEFINE(uplink_work, uplink_fn);
static void route_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(route_poll_work, route_poll_fn);

/*
 * Withdraw the uplink, to model a backhaul losing its wired or cellular
 * connection while the grid is running.
 *
 * The node stays up and keeps its links; what changes is that it can no longer
 * terminate traffic, so it must stop advertising rank zero and let everything
 * that was reaching the grid through it find another way. This is the one role
 * transition that is not the node's own decision.
 *
 * With -lg_flap it keeps happening: the uplink comes back, goes again, and the
 * grid has to cope with its set of sinks changing indefinitely rather than once.
 * A backhaul on a cellular link is not a stable fact, and a grid that only
 * survives losing one is not much use.
 */
static void uplink_fn(struct k_work *work)
{
	static bool up = true;

	ARG_UNUSED(work);

	up = !up;
	LOG_WRN("uplink %s", up ? "restored" : "lost");
	(void)lg_backhaul_set(up);

	if (arg_flap_ms) {
		/* Down for a fixed spell, then up for the rest of the period, so
		 * "falls off once a minute" is what it looks like from outside.
		 */
		k_work_reschedule(&uplink_work,
				  K_MSEC(up ? arg_flap_ms - arg_flap_down_ms
					    : arg_flap_down_ms));
	}
}

/*
 * Periodic state dump for the visualisation. The event trace records what
 * happened, but the graph a node believes it is part of - its parent, its rank,
 * its neighbours and their costs - is state rather than an event, and there is no
 * moment at which emitting it would be natural. So it is sampled instead.
 */
static void snap_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	lg_trace_snapshot();
	k_work_reschedule(&snap_work, K_MSEC(arg_snap_ms));
}

static void telemetry_fn(struct k_work *work)
{
	static uint32_t seq;
	uint8_t payload[8];

	ARG_UNUSED(work);

	sys_put_le32(++seq, payload);
	sys_put_le32(lg_local_addr(), &payload[4]);
	(void)lg_send(LG_ADDR_BACKHAUL, TEST_PORT, payload, sizeof(payload), LG_TX_RELIABLE);

	k_work_reschedule(&telemetry_work, K_MSEC(arg_period_ms));
}

static void stop_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("node going dark to exercise healing");
	k_work_cancel_delayable(&telemetry_work);
	lg_stop();
}

/*
 * Longest stretch with no route, sampled.
 *
 * A grid whose backhauls keep dropping is never in one state long enough for an
 * end-of-run check to mean anything: a node could be routed at the moment it is
 * asked and have spent half the run stranded. What matters is the worst gap, so
 * that is what is measured.
 */
static uint32_t route_gap_ms;
static uint32_t route_gap_worst_ms;
static bool route_ever_had;

#define ROUTE_POLL_MS 500

static void route_poll_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (lg_has_route()) {
		route_ever_had = true;
		route_gap_ms = 0;
	} else if (route_ever_had) {
		/*
		 * Only gaps after the first route count. Every node starts without
		 * one, and counting that stretch measured how long the grid took to
		 * form rather than how long a disturbance stranded anybody - which
		 * made the figure look like a healing time when it was a convergence
		 * time, and hid the thing the test exists to measure.
		 */
		route_gap_ms += ROUTE_POLL_MS;
		if (route_gap_ms > route_gap_worst_ms) {
			route_gap_worst_ms = route_gap_ms;
		}
	}
	k_work_reschedule(&route_poll_work, K_MSEC(ROUTE_POLL_MS));
}

/** Evaluate every expectation this node was given. */
static void check_fn(struct k_work *work)
{
	struct lg_stats st;
	struct lg_power_report pw;
	unsigned int fails = 0;

	ARG_UNUSED(work);

	lg_stats_get(&st);
	lg_power_report_get(&pw);

	/* A hundred nodes each printing a multi-line report buries the trace the
	 * visualisation is parsing, so a scenario run turns them off.
	 */
	if (arg_report) {
		lg_stats_print();
		lg_power_report_print();
		if (arg_given(exp_max_gap_ms)) {
			LOG_INF("worst stretch without a route: %u ms", route_gap_worst_ms);
		}
	}

	if (arg_given(exp_route) && !lg_has_route()) {
		bs_trace_warning_line_time("FAIL 0x%04x: expected a route to a backhaul\n",
					   lg_local_addr());
		fails++;
	}
	if (arg_given(exp_links) && lg_link_count() < exp_links) {
		bs_trace_warning_line_time("FAIL 0x%04x: expected >=%u links\n", lg_local_addr(),
					   exp_links);
		fails++;
	}
	if (arg_given(exp_joins) && st.joins_ok < exp_joins) {
		bs_trace_warning_line_time("FAIL 0x%04x: expected >=%u successful joins, got %u\n",
					   lg_local_addr(), exp_joins, st.joins_ok);
		fails++;
	}
	if (arg_given(exp_rx_from) && rx_source_count < exp_rx_from) {
		bs_trace_warning_line_time(
			"FAIL 0x%04x: expected traffic from >=%u nodes, got %u (%u messages)\n",
			lg_local_addr(), exp_rx_from, rx_source_count, rx_total);
		fails++;
	}
	if (arg_given(exp_duty_max) && pw.duty_permille > exp_duty_max) {
		bs_trace_warning_line_time(
			"FAIL 0x%04x: radio on %u.%u%% exceeds the %u.%u%% ceiling\n",
			lg_local_addr(), pw.duty_permille / 10U, pw.duty_permille % 10U,
			exp_duty_max / 10U, exp_duty_max % 10U);
		fails++;
	}
	/* Guards the contended test against silently passing with the synthetic
	 * refusal mode switched off, which would make it prove nothing at all.
	 */
	if (arg_given(exp_min_blocked) && st.slots_blocked < exp_min_blocked) {
		bs_trace_warning_line_time(
			"FAIL 0x%04x: expected >=%u refused radio requests, got %u\n",
			lg_local_addr(), exp_min_blocked, st.slots_blocked);
		fails++;
	}
	/* Offset by one so that "expect leaf" is distinguishable from "not given",
	 * since bsim cannot tell an absent unsigned option from a zero one.
	 */
	if (arg_given(exp_max_gap_ms) && route_gap_worst_ms > exp_max_gap_ms) {
		bs_trace_warning_line_time(
			"FAIL 0x%04x: went %u ms without a route, ceiling %u ms\n",
			lg_local_addr(), route_gap_worst_ms, exp_max_gap_ms);
		fails++;
	}
	if (arg_given(exp_role) && (uint32_t)lg_role() != exp_role - 1U) {
		bs_trace_warning_line_time("FAIL 0x%04x: expected role %u, settled on %u\n",
					   lg_local_addr(), exp_role - 1U, (unsigned)lg_role());
		fails++;
	}
	if (arg_given(exp_no_authfail) && st.auth_failures) {
		bs_trace_warning_line_time("FAIL 0x%04x: %u authentication failures\n",
					   lg_local_addr(), st.auth_failures);
		fails++;
	}

	if (fails) {
		/*
		 * Reported as a warning and then exited explicitly, rather than with
		 * bs_trace_error_line_time(): that does terminate the process, but it
		 * leaves the exit status at zero, so the failure never reaches the
		 * runner and continuous integration reports green on a broken
		 * protocol. A test that cannot fail is worse than no test.
		 */
		bs_trace_warning_line_time("0x%04x: %u expectation(s) NOT MET\n",
					   lg_local_addr(), fails);
		nsi_exit(1);
	}

	bs_trace_info_time(1, "0x%04x: all expectations met\n", lg_local_addr());
}

/* ------------------------------------------------------------------------- */
/* Command line                                                              */
/* ------------------------------------------------------------------------- */

static void register_args(void)
{
	static bs_args_struct_t args[] = {
		{.option = "lg_addr", .name = "addr", .type = 'u', .dest = &arg_addr,
		 .descript = "Short address for this node"},
		{.option = "lg_role", .name = "role", .type = 'u', .dest = &arg_role,
		 .descript = "Capability: 0 never routes, 1 may route, 2 may also backhaul"},
		{.option = "lg_period", .name = "ms", .type = 'u', .dest = &arg_period_ms,
		 .descript = "Telemetry period, 0 to send nothing"},
		{.option = "lg_stop", .name = "ms", .type = 'u', .dest = &arg_stop_ms,
		 .descript = "Stop the node at this uptime, to model a failure"},
		{.option = "lg_delay", .name = "ms", .type = 'u', .dest = &arg_delay_ms,
		 .descript = "Delay starting the node, to model arriving at a running grid"},
		{.option = "lg_check", .name = "ms", .type = 'u', .dest = &arg_check_ms,
		 .descript = "When to evaluate expectations"},
		{.option = "lg_snap", .name = "ms", .type = 'u', .dest = &arg_snap_ms,
		 .descript = "Trace a state snapshot this often, 0 for none"},
		{.option = "lg_uplink", .name = "ms", .type = 'u', .dest = &arg_uplink_ms,
		 .descript = "Withdraw this backhaul's uplink at this uptime"},
		{.option = "lg_flap", .name = "ms", .type = 'u', .dest = &arg_flap_ms,
		 .descript = "Keep losing the uplink this often after the first time"},
		{.option = "lg_flap_down", .name = "ms", .type = 'u', .dest = &arg_flap_down_ms,
		 .descript = "How long the uplink stays down each time"},
		{.option = "lg_report", .name = "0/1", .type = 'u', .dest = &arg_report,
		 .descript = "Print the stats and power reports at check time"},
		{.option = "exp_route", .name = "n", .type = 'u', .dest = &exp_route,
		 .descript = "Expect a route to a backhaul"},
		{.option = "exp_links", .name = "n", .type = 'u', .dest = &exp_links,
		 .descript = "Expect at least n authenticated links"},
		{.option = "exp_rx_from", .name = "n", .type = 'u', .dest = &exp_rx_from,
		 .descript = "Expect traffic from at least n distinct nodes"},
		{.option = "exp_joins", .name = "n", .type = 'u', .dest = &exp_joins,
		 .descript = "Expect at least n successful handshakes"},
		{.option = "exp_duty_max", .name = "permille", .type = 'u',
		 .dest = &exp_duty_max,
		 .descript = "Fail if radio on-time exceeds this, in tenths of a percent"},
		{.option = "exp_no_authfail", .name = "n", .type = 'u', .dest = &exp_no_authfail,
		 .descript = "Fail if any frame failed to authenticate"},
		{.option = "exp_min_blocked", .name = "n", .type = 'u', .dest = &exp_min_blocked,
		 .descript = "Expect at least n refused radio requests"},
		{.option = "exp_max_gap", .name = "ms", .type = 'u', .dest = &exp_max_gap_ms,
		 .descript = "Fail if this node was ever without a route for longer"},
		{.option = "exp_role", .name = "n", .type = 'u', .dest = &exp_role,
		 .descript = "Expect to have settled on this role, plus one "
			     "(1 leaf, 2 router, 3 backhaul)"},
		ARG_TABLE_ENDMARKER,
	};

	bs_add_extra_dynargs(args);
}
NATIVE_TASK(register_args, PRE_BOOT_1, 100);

/* ------------------------------------------------------------------------- */

int main(void)
{
	int err;

	arg_addr = arg_or(arg_addr, 0);
	arg_role = arg_or(arg_role, LG_ROLE_ROUTER);
	arg_period_ms = arg_or(arg_period_ms, 0);
	arg_stop_ms = arg_or(arg_stop_ms, 0);
	arg_delay_ms = arg_or(arg_delay_ms, 0);
	arg_check_ms = arg_or(arg_check_ms, 40000);
	arg_snap_ms = arg_or(arg_snap_ms, 0);
	arg_uplink_ms = arg_or(arg_uplink_ms, 0);
	arg_flap_ms = arg_or(arg_flap_ms, 0);
	arg_flap_down_ms = arg_or(arg_flap_down_ms, 15000);
	arg_report = arg_or(arg_report, 1);

	if (arg_addr == 0) {
		arg_addr = bsim_args_get_global_device_nbr() + 1;
	}

	LOG_INF("test node 0x%04x, capability %u", (unsigned)arg_addr, arg_role);

	err = lg_provision_sim(LOCKGRID_NETWORK_ID, (lg_addr_t)arg_addr, (enum lg_role)arg_role);
	if (err) {
		bs_trace_warning_line_time("FAIL provisioning: %d\n", err);
		nsi_exit(1);
	}

	lg_callbacks_register(&callbacks);

	/* Scheduled before any delay so the check time is measured from boot, and
	 * therefore comparable across nodes that start at different moments.
	 */
	k_work_reschedule(&check_work, K_MSEC(arg_check_ms));

	if (arg_delay_ms) {
		/* Arrive at a grid that has already settled, rather than forming
		 * one. A settled router wants nothing, so this is the case that
		 * depends on the join window floor.
		 */
		LOG_INF("waiting %u ms before joining an established grid", arg_delay_ms);
		k_sleep(K_MSEC(arg_delay_ms));
	}

	/* No role is passed in: the node decides for itself whether it should be a
	 * leaf or a router. What the command line supplies is the capability the
	 * certificate grants, and whether a backhaul-capable node's uplink is up.
	 */
	if (arg_role >= LG_ROLE_BACKHAUL) {
		(void)lg_backhaul_set(true);
	}

	err = lg_start();
	if (err) {
		bs_trace_warning_line_time("FAIL lg_start: %d\n", err);
		nsi_exit(1);
	}

	if (arg_period_ms) {
		k_work_reschedule(&telemetry_work, K_MSEC(arg_period_ms));
	}
	if (arg_stop_ms) {
		k_work_reschedule(&stop_work, K_MSEC(arg_stop_ms));
	}
	if (arg_uplink_ms) {
		k_work_reschedule(&uplink_work, K_MSEC(arg_uplink_ms));
	}
	if (arg_given(exp_max_gap_ms)) {
		k_work_reschedule(&route_poll_work, K_MSEC(ROUTE_POLL_MS));
	}
	if (arg_snap_ms) {
		/* Offset by the period so the first snapshot is not taken before the
		 * node has had a chance to hear anything.
		 */
		k_work_reschedule(&snap_work, K_MSEC(arg_snap_ms));
	}

	return 0;
}
