/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_neigh.c
 * @brief Neighbour table and link quality metrics.
 *
 * The link cost that drives routing is built from three measurements:
 *
 *  - ETX, the expected number of transmissions per delivered frame, tracked as
 *    a moving average of acknowledgement outcomes. This is the dominant term
 *    because a link that needs three attempts costs three times the energy.
 *  - RSSI, used only as a penalty near the noise floor. A link can have a good
 *    ETX today and a small margin, which makes it fragile; the penalty biases
 *    away from those without overriding measured delivery.
 *  - Refused radio time, which the MPSL backend reports. A neighbour whose
 *    events keep losing to Bluetooth is genuinely worse even though the air is
 *    fine, and folding refusals into ETX makes the routing metric notice.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "lg_internal.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* Anything worse than this is not worth routing through. */
#define LG_ETX_MAX (LG_ETX_ONE * 8U)

/* Below this RSSI a link is close enough to the noise floor to be fragile. */
#define LG_RSSI_WEAK_DBM (-88)
/* Cost added per dB below LG_RSSI_WEAK_DBM. */
#define LG_RSSI_PENALTY_PER_DB 2

struct lg_neigh *lg_neigh_find(lg_addr_t addr)
{
	if (addr == LG_ADDR_INVALID) {
		return NULL;
	}

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		if (lg.neigh[i].state != LG_NS_FREE && lg.neigh[i].addr == addr) {
			return &lg.neigh[i];
		}
	}
	return NULL;
}

struct lg_neigh *lg_neigh_alloc(lg_addr_t addr)
{
	struct lg_neigh *n = lg_neigh_find(addr);

	if (n) {
		return n;
	}

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		if (lg.neigh[i].state == LG_NS_FREE) {
			n = &lg.neigh[i];
			memset(n, 0, sizeof(*n));
			n->addr = addr;
			n->state = LG_NS_HEARD;
			n->rssi = INT8_MIN;
			n->etx_q4 = LG_ETX_ONE;
			n->peer_rank = LG_RANK_INFINITE;
			n->link_cost = LG_RANK_INFINITE;
			n->subrate = 1;
			n->subrate_max = 1;
			return n;
		}
	}
	return NULL;
}

void lg_neigh_free(struct lg_neigh *n)
{
	lg_sec_forget_key(n);
	memset(n, 0, sizeof(*n));
	n->state = LG_NS_FREE;
}

uint8_t lg_neigh_linked_count(void)
{
	uint8_t count = 0;

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		if (lg.neigh[i].state == LG_NS_LINKED) {
			count++;
		}
	}
	return count;
}

void lg_neigh_tx_result(struct lg_neigh *n, bool acked)
{
	uint16_t sample;

	n->tx_attempts++;
	if (acked) {
		n->tx_acked++;
		n->consec_misses = 0;
	} else {
		n->consec_misses++;
	}

	/* Instantaneous ETX for this attempt: 16 for a success, and a large
	 * value for a loss so a run of losses moves the average quickly.
	 */
	sample = acked ? LG_ETX_ONE : LG_ETX_ONE * 4U;
	n->etx_q4 = (uint16_t)CLAMP(lg_ewma(n->etx_q4, sample, true), LG_ETX_ONE, LG_ETX_MAX);

	if (n->tx_attempts) {
		uint32_t lost = n->tx_attempts - n->tx_acked;

		n->per_permille = (uint16_t)((lost * 1000U) / n->tx_attempts);
	}

	/* Halve the counters periodically so the reported error rate reflects
	 * the recent past rather than the whole life of the link.
	 */
	if (n->tx_attempts >= 128U) {
		n->tx_attempts /= 2U;
		n->tx_acked /= 2U;
	}

	lg_neigh_update_cost(n);
}

void lg_neigh_rx_rssi(struct lg_neigh *n, int8_t rssi)
{
	n->rssi = (int8_t)lg_ewma(n->rssi, rssi, n->rssi != INT8_MIN);
}

void lg_neigh_update_cost(struct lg_neigh *n)
{
	uint32_t cost = n->etx_q4;

	if (n->rssi != INT8_MIN && n->rssi < LG_RSSI_WEAK_DBM) {
		cost += (uint32_t)(LG_RSSI_WEAK_DBM - n->rssi) * LG_RSSI_PENALTY_PER_DB;
	}

	n->link_cost = (uint16_t)MIN(cost, LG_RANK_INFINITE - 1U);
}

struct lg_neigh *lg_neigh_make_room(uint16_t cand_cost)
{
	struct lg_neigh *worst = NULL;

	/* Prefer a slot that is not carrying a link at all. */
	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		struct lg_neigh *n = &lg.neigh[i];

		if (n->state == LG_NS_FREE) {
			return n;
		}
		if (n->state == LG_NS_HEARD || n->state == LG_NS_LOST) {
			if (!worst || n->link_cost > worst->link_cost) {
				worst = n;
			}
		}
	}

	if (worst) {
		lg_neigh_free(worst);
		return worst;
	}

	/* Everything is linked. Only displace one if the candidate is clearly
	 * better, and never displace a parent: losing the uplink to chase a
	 * marginally better neighbour is how a mesh starts flapping.
	 */
	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		struct lg_neigh *n = &lg.neigh[i];

		if (n->is_parent) {
			continue;
		}
		if (n->link_cost > cand_cost + LG_ETX_ONE) {
			if (!worst || n->link_cost > worst->link_cost) {
				worst = n;
			}
		}
	}

	if (worst) {
		lg_link_down(worst, LG_TD_TOO_MANY_LINKS);
		lg_neigh_free(worst);
		return worst;
	}

	return NULL;
}

uint8_t lg_link_count(void)
{
	return lg_neigh_linked_count();
}

void lg_neighbor_foreach(bool (*cb)(const struct lg_neighbor_info *info, void *user), void *user)
{
	if (!cb) {
		return;
	}

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		struct lg_neigh *n = &lg.neigh[i];
		struct lg_neighbor_info info;

		if (n->state == LG_NS_FREE) {
			continue;
		}

		info.addr = n->addr;
		info.linked = (n->state == LG_NS_LINKED);
		info.parent = n->is_parent;
		info.role = (enum lg_role)n->role;
		info.peer_rank = n->peer_rank;
		info.link_cost = n->link_cost;
		info.rssi = n->rssi;
		info.etx_q4 = n->etx_q4;
		info.per_permille = n->per_permille;
		info.interval_us = n->interval_us;
		info.subrate = n->subrate;
		info.rx_window_us = n->rx_window_us;

		if (!cb(&info, user)) {
			return;
		}
	}
}
