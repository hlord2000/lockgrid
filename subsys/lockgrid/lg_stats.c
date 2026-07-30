/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "lg_internal.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

void lg_stats_get(struct lg_stats *out)
{
	*out = lg.stats;
}

static bool print_neigh(const struct lg_neighbor_info *info, void *user)
{
	ARG_UNUSED(user);

	LOG_INF("  0x%04x %-8s%s rank %5u cost %5u rssi %4d etx %u.%02u per %u.%u%% "
		"int %u ms x%u win %u us",
		info->addr,
		info->linked ? "linked" : "heard",
		info->parent ? " parent" : "",
		info->peer_rank, info->link_cost, info->rssi, info->etx_q4 / 16U,
		(info->etx_q4 % 16U) * 100U / 16U, info->per_permille / 10U,
		info->per_permille % 10U, info->interval_us / 1000U, info->subrate,
		info->rx_window_us);
	return true;
}

void lg_stats_print(void)
{
	struct lg_stats s;
	char map[LG_CHAN_COUNT + 1];

	lg_stats_get(&s);

	for (int i = 0; i < LG_CHAN_COUNT; i++) {
		map[i] = (lg.chan_map & BIT(i)) ? '+' : '.';
	}
	map[LG_CHAN_COUNT] = '\0';

	LOG_INF("node 0x%04x role %u rank %u parent 0x%04x links %u", lg.addr, lg.role, lg.rank,
		lg_parent(), lg_neigh_linked_count());
	LOG_INF("  channels 11..26 [%s] map 0x%04x, %u updates, %u retired", map, lg.chan_map,
		s.chan_map_updates, s.channels_blacklisted);
	LOG_INF("  discovery: beacons tx %u rx %u, blind scans %u, tracked %u", s.beacons_tx,
		s.beacons_rx, s.scans, s.tracked_beacons);
	LOG_INF("  auth: joins ok %u failed %u, auth failures %u, replays %u", s.joins_ok,
		s.joins_failed, s.auth_failures, s.replays_dropped);
	LOG_INF("  link: events %u skipped %u, tx %u rx %u retx %u", s.link_events,
		s.link_events_skipped, s.frames_tx, s.frames_rx, s.frames_retx);
	LOG_INF("  errors: crc %u, rx timeouts %u, supervision %u, slots blocked %u",
		s.crc_errors, s.rx_timeouts, s.supervision_timeouts, s.slots_blocked);
	LOG_INF("  routing: forwarded %u, dropped %u, parent switches %u", s.msgs_forwarded,
		s.msgs_dropped_no_route, s.parent_switches);

	LOG_INF("  neighbours:");
	lg_neighbor_foreach(print_neigh, NULL);
}
