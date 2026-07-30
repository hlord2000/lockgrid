/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief A LockGrid node.
 *
 * Role and address come from the command line so one build can populate a whole
 * simulated grid:
 *
 *   -lg_addr=<n>     short address, must be unique
 *   -lg_role=<0|1|2> capability: 0 never routes, 1 may route, 2 may also backhaul
 *   -lg_period=<ms>  telemetry period, 0 to send nothing
 *   -lg_report=<ms>  how often to print statistics and the energy split
 *   -lg_stop=<ms>    stop the radio at this point, to model a node dying
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <lockgrid/lockgrid.h>
#include <lockgrid/lg_provision_sim.h>

#if defined(CONFIG_SOC_SERIES_BSIM_NRFXX)
#include <posix_native_task.h>
#include "bs_cmd_line.h"
#include "bs_dynargs.h"
#include "bsim_args_runner.h"
#define LOCKGRID_BSIM 1
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define LOCKGRID_NETWORK_ID 0x10CC6217UL
#define TELEMETRY_PORT      7

static uint32_t arg_addr;
static uint32_t arg_role = LG_ROLE_ROUTER;
static uint32_t arg_period_ms = 5000;
static uint32_t arg_report_ms = 30000;
static uint32_t arg_stop_ms;
static uint32_t arg_snap_ms;


/*
 * BabbleSim presets every unsigned command line option to UINT_MAX before
 * parsing, so "not given" is not zero: anything with a meaningful default has to
 * be normalised before use.
 */
#define LG_ARG_UNSET UINT32_MAX

static uint32_t arg_or(uint32_t v, uint32_t fallback)
{
	return (v == LG_ARG_UNSET) ? fallback : v;
}

static uint32_t rx_count;

static void on_rx(const struct lg_rx_info *info, const uint8_t *data, size_t len)
{
	uint32_t seq = (len >= 4) ? sys_get_le32(data) : 0;

	rx_count++;
	LOG_INF("rx port %u from 0x%04x via 0x%04x, %u hops, %d dBm, seq %u, %u bytes",
		info->port, info->src, info->prev_hop, info->hops, info->rssi, seq,
		(unsigned)len);
}

static void on_evt(enum lg_evt evt, lg_addr_t peer)
{
	static const char *const names[] = {
		"link up", "link down", "parent changed", "route lost", "auth failed",
		"role changed",
	};

	LOG_INF("event: %s, peer 0x%04x (role %u, rank %u)",
		evt < ARRAY_SIZE(names) ? names[evt] : "?", peer, lg_role(), lg_rank());
}

static const struct lg_callbacks callbacks = {
	.rx = on_rx,
	.evt = on_evt,
};

/* ------------------------------------------------------------------------- */
/* Periodic work                                                              */
/* ------------------------------------------------------------------------- */

static void telemetry_fn(struct k_work *work);
static void report_fn(struct k_work *work);
static void stop_fn(struct k_work *work);
static void snap_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(snap_work, snap_fn);
static K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_fn);
static K_WORK_DELAYABLE_DEFINE(report_work, report_fn);
static K_WORK_DELAYABLE_DEFINE(stop_work, stop_fn);

static void telemetry_fn(struct k_work *work)
{
	static uint32_t seq;
	uint8_t payload[16];
	int err;

	ARG_UNUSED(work);

	sys_put_le32(++seq, payload);
	sys_put_le32(lg_rank(), &payload[4]);
	sys_put_le32(k_uptime_get_32(), &payload[8]);
	sys_put_le32(lg_local_addr(), &payload[12]);

	err = lg_send(LG_ADDR_BACKHAUL, TELEMETRY_PORT, payload, sizeof(payload),
		      LG_TX_RELIABLE);
	if (err == -ENOTCONN) {
		LOG_DBG("no route to a backhaul yet, holding seq %u", seq);
		seq--;
	} else if (err) {
		LOG_WRN("send failed: %d", err);
		seq--;
	}

	k_work_reschedule(&telemetry_work, K_MSEC(arg_period_ms));
}

static void report_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	lg_stats_print();
	lg_power_report_print();
	LOG_INF("  application received %u messages", rx_count);

	k_work_reschedule(&report_work, K_MSEC(arg_report_ms));
}

/* Periodic trace snapshot: the visualisation needs a node's rank, parent,
 * neighbour costs and energy so far at points along the timeline, none of which
 * can be reconstructed from events alone.
 */
static void snap_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	lg_trace_snapshot();
	k_work_reschedule(&snap_work, K_MSEC(arg_snap_ms));
}

static void stop_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("going dark on purpose, to exercise the healing path");
	k_work_cancel_delayable(&telemetry_work);
	lg_stop();
}

/* ------------------------------------------------------------------------- */
/* Command line                                                               */
/* ------------------------------------------------------------------------- */

#if defined(LOCKGRID_BSIM)
static void register_args(void)
{
	static bs_args_struct_t args[] = {
		{
			.option = "lg_addr",
			.name = "addr",
			.type = 'u',
			.dest = &arg_addr,
			.descript = "LockGrid short address for this node",
		},
		{
			.option = "lg_role",
			.name = "role",
			.type = 'u',
			.dest = &arg_role,
			.descript = "Capability: 0 never routes, 1 may route, 2 may also backhaul",
		},
		{
			.option = "lg_period",
			.name = "ms",
			.type = 'u',
			.dest = &arg_period_ms,
			.descript = "Telemetry period in ms, 0 to disable",
		},
		{
			.option = "lg_report",
			.name = "ms",
			.type = 'u',
			.dest = &arg_report_ms,
			.descript = "Statistics report period in ms",
		},
		{
			.option = "lg_snap",
			.name = "ms",
			.type = 'u',
			.dest = &arg_snap_ms,
			.descript = "Trace snapshot period in ms, 0 to disable",
		},
		{
			.option = "lg_stop",
			.name = "ms",
			.type = 'u',
			.dest = &arg_stop_ms,
			.descript = "Stop the node at this uptime, to model a failure",
		},
		ARG_TABLE_ENDMARKER,
	};

	bs_add_extra_dynargs(args);
}

/* Arguments have to be registered before the simulator parses the command line,
 * which happens well before the kernel starts.
 */
NATIVE_TASK(register_args, PRE_BOOT_1, 100);
#endif /* LOCKGRID_BSIM */

/* ------------------------------------------------------------------------- */

int main(void)
{
	int err;
	lg_addr_t addr;

#if defined(LOCKGRID_BSIM)
	arg_addr = arg_or(arg_addr, 0);
	arg_role = arg_or(arg_role, LG_ROLE_ROUTER);
	arg_period_ms = arg_or(arg_period_ms, 5000);
	arg_report_ms = arg_or(arg_report_ms, 30000);
	arg_stop_ms = arg_or(arg_stop_ms, 0);
	arg_snap_ms = arg_or(arg_snap_ms, 0);

	if (arg_addr == 0) {
		/* Fall back to the simulated device number so a node always has
		 * a usable address even without -lg_addr.
		 */
		arg_addr = bsim_args_get_global_device_nbr() + 1;
	}
#endif
	if (arg_addr == 0) {
		arg_addr = 1;
	}
	addr = (lg_addr_t)arg_addr;

	LOG_INF("LockGrid node starting: addr 0x%04x, capability %u", addr, arg_role);

	/* A certificate permitting backhaul also permits router and leaf, so the
	 * issued certificate allows anything at or below the requested role.
	 */
	err = lg_provision_sim(LOCKGRID_NETWORK_ID, addr, (enum lg_role)arg_role);
	if (err) {
		LOG_ERR("provisioning failed: %d", err);
		return err;
	}

	lg_callbacks_register(&callbacks);

	/* No role is passed in: the node decides for itself whether it should be a
	 * leaf or a router. What the command line supplies is the capability the
	 * certificate grants, and whether a backhaul-capable node's uplink is up.
	 */
	if (arg_role >= LG_ROLE_BACKHAUL) {
		(void)lg_backhaul_set(true);
	}

	err = lg_start();
	if (err) {
		LOG_ERR("lg_start failed: %d", err);
		return err;
	}

	if (arg_period_ms && lg_role() != LG_ROLE_BACKHAUL) {
		k_work_reschedule(&telemetry_work, K_MSEC(arg_period_ms));
	}
	if (arg_report_ms) {
		k_work_reschedule(&report_work, K_MSEC(arg_report_ms));
	}
	if (arg_stop_ms) {
		k_work_reschedule(&stop_work, K_MSEC(arg_stop_ms));
	}
	if (arg_snap_ms) {
		k_work_reschedule(&snap_work, K_MSEC(arg_snap_ms));
	}

	return 0;
}
