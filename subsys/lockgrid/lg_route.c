/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_route.c
 * @brief Rank, parent selection and forwarding.
 *
 * Each node holds a rank: the cost of its cheapest path to a backhaul, zero at
 * a backhaul itself. Rank is the sum of the parent's rank and the measured cost
 * of the link to it, so a path that needs retransmissions is expensive in the
 * same units as a path that is simply long. That is what makes the topology
 * settle on links that actually deliver rather than on hop count.
 *
 * Three rules keep it stable:
 *
 *  - A node may only choose a parent whose rank is strictly lower than its own
 *    would become. This is the acyclicity rule; without it two nodes can point
 *    at each other and both believe they have a route.
 *  - A parent is only replaced when the alternative is better by a margin, so a
 *    link whose cost is oscillating around a rival's does not cause churn.
 *  - More than one parent is kept when possible. Failing over to an already
 *    authenticated backup costs nothing, whereas finding a new parent costs a
 *    scan and a handshake.
 *
 * Traffic towards a specific node that is not a neighbour goes up to a common
 * ancestor and back down using routes that children register with their parents.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "lg_internal.h"
#include "lg_trace.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* A rival must beat the current parent by this much before we switch. In ETX
 * sixteenths, so this is a quarter of a retransmission.
 */
#define LG_RANK_HYSTERESIS 4

/* Downward routes are dropped if a child stops refreshing them. */
#define LG_ROUTE_LIFETIME_US (120ULL * USEC_PER_SEC)

/* Neighbours advertised per route advertisement. */
#define LG_ADV_MAX_ENTRIES 6

static struct {
	lg_addr_t origin;
	uint16_t msg_id;
} dup_cache[LG_DUP_CACHE_SIZE];
static uint8_t dup_next;

void lg_route_init(void)
{
	memset(lg.routes, 0, sizeof(lg.routes));
	memset(dup_cache, 0, sizeof(dup_cache));
	memset(lg.gossip, 0, sizeof(lg.gossip));
	dup_next = 0;
	lg.want_addr = LG_ADDR_INVALID;
	/* lg_role_init() has already chosen the role, so the rank follows from it. */
	lg.rank = (lg.role == LG_ROLE_BACKHAUL) ? 0 : LG_RANK_INFINITE;
}

/* ------------------------------------------------------------------------- */
/* Rank and parent selection                                                  */
/* ------------------------------------------------------------------------- */

/**
 * Cost this node would have if it routed through @p n.
 *
 * Anything at or beyond LOCKGRID_MAX_RANK is unreachable, and that ceiling is what
 * makes the metric terminate rather than merely converge slowly.
 *
 * The acyclicity rule below only compares against the immediate neighbour's rank,
 * which cannot see a loop more than one hop long. When every backhaul loses its
 * uplink at once, the remaining nodes route through each other: each one's
 * neighbour still advertises a finite rank, so each takes a finite rank of its own,
 * and the whole grid believes it has a path to a sink that no longer exists. The
 * ranks do climb - the loop adds its own cost every round - but from 32 towards
 * 65535 in steps of a link cost that is thousands of rounds, so in practice it
 * never arrives. Traffic is forwarded round the loop until its hop count runs out
 * and no node ever learns it is disconnected.
 *
 * A ceiling bounds that: the loop cannot inflate for ever, and a path deeper than
 * the deployment can legitimately have is rejected outright. It is not a fix. The
 * loop inflates by its own cost per advertisement round, which measured about 48
 * rank per 18 s in a five-node grid, so reaching even a modest ceiling takes
 * minutes - far longer than a backhaul outage lasts. The grid still spends a short
 * total-sink outage believing in a sink that is gone.
 *
 * The real fix is for an advertisement to name the backhaul the path terminates at
 * and carry a sequence number that only that backhaul increments, so a node can
 * tell a live root from a remembered one and a loop collapses in a single round.
 * That changes the advertisement layout, so it is called out in the README's
 * limitations rather than smuggled in here.
 */
static uint32_t rank_through(const struct lg_neigh *n)
{
	uint32_t cost;

	if (n->state != LG_NS_LINKED || n->peer_rank == LG_RANK_INFINITE) {
		return LG_RANK_INFINITE;
	}
	cost = (uint32_t)n->peer_rank + n->link_cost;
	if (cost >= CONFIG_LOCKGRID_MAX_RANK) {
		return LG_RANK_INFINITE;
	}
	return cost;
}

bool lg_route_recompute(void)
{
	struct lg_neigh *best[CONFIG_LOCKGRID_MAX_PARENTS] = {0};
	uint32_t best_cost[CONFIG_LOCKGRID_MAX_PARENTS];
	lg_addr_t old_parent = lg_parent();
	uint16_t old_rank = lg.rank;
	bool changed;

	if (lg.role == LG_ROLE_BACKHAUL) {
		lg.rank = 0;
		for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
			lg.neigh[i].is_parent = false;
		}
		return false;
	}

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_PARENTS; i++) {
		best_cost[i] = LG_RANK_INFINITE;
	}

	/* Insertion sort of the candidates into the parent set. */
	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		struct lg_neigh *n = &lg.neigh[i];
		uint32_t cost = rank_through(n);

		if (cost >= LG_RANK_INFINITE) {
			continue;
		}

		/* Acyclicity: the parent's own rank must be below the rank we
		 * would take on. A neighbour routing through us fails this and
		 * is skipped, which is what stops two nodes adopting each other.
		 */
		if (n->peer_rank >= cost) {
			continue;
		}

		for (int s = 0; s < CONFIG_LOCKGRID_MAX_PARENTS; s++) {
			if (cost < best_cost[s]) {
				for (int t = CONFIG_LOCKGRID_MAX_PARENTS - 1; t > s; t--) {
					best_cost[t] = best_cost[t - 1];
					best[t] = best[t - 1];
				}
				best_cost[s] = cost;
				best[s] = n;
				break;
			}
		}
	}

	/* Hysteresis: keep the existing primary unless the new best is clearly
	 * better. Comparing against the neighbour we currently use, not against
	 * the stale rank, avoids ratcheting.
	 */
	if (old_parent != LG_ADDR_INVALID && best[0] && best[0]->addr != old_parent) {
		struct lg_neigh *cur = lg_neigh_find(old_parent);
		uint32_t cur_cost = cur ? rank_through(cur) : LG_RANK_INFINITE;

		if (cur_cost < LG_RANK_INFINITE && cur_cost <= best_cost[0] + LG_RANK_HYSTERESIS) {
			/* Put the incumbent back at the head of the set. */
			for (int s = 1; s < CONFIG_LOCKGRID_MAX_PARENTS; s++) {
				if (best[s] == cur) {
					best[s] = best[0];
					best_cost[s] = best_cost[0];
					best[0] = cur;
					best_cost[0] = cur_cost;
					break;
				}
			}
		}
	}

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		lg.neigh[i].is_parent = false;
	}
	for (int s = 0; s < CONFIG_LOCKGRID_MAX_PARENTS; s++) {
		if (best[s]) {
			best[s]->is_parent = true;
		}
	}

	lg.rank = best[0] ? (uint16_t)MIN(best_cost[0], LG_RANK_INFINITE - 1U)
			  : LG_RANK_INFINITE;

	changed = (lg_parent() != old_parent);
	if (changed) {
		lg.stats.parent_switches++;
		LOG_INF("parent 0x%04x -> 0x%04x, rank %u -> %u", old_parent, lg_parent(), old_rank,
			lg.rank);
		LG_TRACE("parent", "old=%04x new=%04x rank=%u", old_parent, lg_parent(), lg.rank);
	}

	if (lg.rank != old_rank) {
		/*
		 * Push the new rank to every linked neighbour on its next link event
		 * rather than waiting for the 30 s advertisement timer. One connection
		 * interval per hop instead of thirty seconds per hop, which is the
		 * difference between a backhaul's disappearance being noticed and the
		 * grid forwarding into a hole until the timer happens to fire.
		 */
		lg.adv_urgent = true;
		for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
			lg.neigh[i].adv_urgent_done = false;
		}
	}

	if (old_rank != LG_RANK_INFINITE && lg.rank == LG_RANK_INFINITE) {
		/* Routes learned through the old parent are worthless now. */
		lg_notify(LG_EVT_ROUTE_LOST, LG_ADDR_INVALID);
	}

	return changed;
}

lg_addr_t lg_parent(void)
{
	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		if (lg.neigh[i].is_parent && lg.neigh[i].state == LG_NS_LINKED) {
			return lg.neigh[i].addr;
		}
	}
	return LG_ADDR_INVALID;
}

uint16_t lg_rank(void)
{
	return lg.rank;
}

bool lg_has_route(void)
{
	return lg.role == LG_ROLE_BACKHAUL || lg.rank != LG_RANK_INFINITE;
}

/* ------------------------------------------------------------------------- */
/* Downward routes                                                            */
/* ------------------------------------------------------------------------- */

static struct lg_route *route_find(lg_addr_t dst)
{
	for (int i = 0; i < CONFIG_LOCKGRID_MAX_ROUTES; i++) {
		if (lg.routes[i].dst == dst) {
			return &lg.routes[i];
		}
	}
	return NULL;
}

void lg_route_add(lg_addr_t dst, lg_addr_t via, uint8_t hops)
{
	struct lg_route *r;
	uint64_t now = lg_ts_now_us();

	if (dst == LG_ADDR_INVALID || dst == lg.addr) {
		return;
	}

	r = route_find(dst);
	if (!r) {
		/* Take a free slot, else the one that expires soonest. */
		for (int i = 0; i < CONFIG_LOCKGRID_MAX_ROUTES; i++) {
			if (lg.routes[i].dst == LG_ADDR_INVALID ||
			    lg.routes[i].expiry_us <= now) {
				r = &lg.routes[i];
				break;
			}
			if (!r || lg.routes[i].expiry_us < r->expiry_us) {
				r = &lg.routes[i];
			}
		}
	}
	if (!r) {
		return;
	}

	/* Prefer the shorter path when two children claim the same destination. */
	if (r->dst == dst && r->expiry_us > now && r->hops < hops) {
		r->expiry_us = now + LG_ROUTE_LIFETIME_US;
		return;
	}

	r->dst = dst;
	r->next_hop = via;
	r->hops = hops;
	r->expiry_us = now + LG_ROUTE_LIFETIME_US;
}

void lg_route_flush_via(lg_addr_t via)
{
	for (int i = 0; i < CONFIG_LOCKGRID_MAX_ROUTES; i++) {
		if (lg.routes[i].next_hop == via) {
			memset(&lg.routes[i], 0, sizeof(lg.routes[i]));
		}
	}
}

lg_addr_t lg_route_next_hop(lg_addr_t final, uint8_t flags)
{
	struct lg_neigh *n;
	struct lg_route *r;

	if (flags & LG_DATA_F_UPWARD) {
		return lg_parent();
	}

	/* A neighbour we already have a link with is always the cheapest hop. */
	n = lg_neigh_find(final);
	if (n && n->state == LG_NS_LINKED) {
		return final;
	}

	r = route_find(final);
	if (r && r->expiry_us > lg_ts_now_us()) {
		struct lg_neigh *via = lg_neigh_find(r->next_hop);

		if (via && via->state == LG_NS_LINKED) {
			return r->next_hop;
		}
		/* The child that registered it is gone. */
		memset(r, 0, sizeof(*r));
	}

	/* Not known locally: push it up and let an ancestor route it down. */
	return lg_parent();
}

/* ------------------------------------------------------------------------- */
/* Advertisements                                                             */
/* ------------------------------------------------------------------------- */

uint8_t lg_route_build_adv(uint8_t *buf, size_t cap)
{
	uint8_t n_entries = 0;
	uint8_t off;

	if (cap < 5) {
		return 0;
	}

	buf[0] = LG_CTRL_ROUTE_ADV;
	lg_put_le16(&buf[1], lg.rank);
	buf[3] = (uint8_t)lg.role;
	off = 5; /* entry count goes at buf[4] */

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS && n_entries < LG_ADV_MAX_ENTRIES;
	     i++) {
		struct lg_neigh *n = &lg.neigh[i];

		if (n->state != LG_NS_LINKED) {
			continue;
		}
		if (off + 4U > cap) {
			break;
		}
		lg_put_le16(&buf[off], n->addr);
		lg_put_le16(&buf[off + 2], n->link_cost);
		off += 4;
		n_entries++;
	}

	buf[4] = n_entries;
	return off;
}

/** Remember a node we have only heard about, for a later targeted scan. */
static void gossip_record(lg_addr_t addr, uint16_t rank)
{
	struct lg_gossip *slot = NULL;
	uint64_t now = lg_ts_now_us();

	if (addr == lg.addr || addr == LG_ADDR_INVALID || lg_neigh_find(addr)) {
		return;
	}

	for (int i = 0; i < LG_GOSSIP_SIZE; i++) {
		if (lg.gossip[i].addr == addr) {
			slot = &lg.gossip[i];
			break;
		}
		if (lg.gossip[i].addr == LG_ADDR_INVALID || lg.gossip[i].addr == 0) {
			slot = &lg.gossip[i];
		} else if (!slot || lg.gossip[i].heard_us < slot->heard_us) {
			slot = &lg.gossip[i];
		}
	}
	if (!slot) {
		return;
	}

	slot->addr = addr;
	slot->rank = rank;
	slot->heard_us = now;
}

/**
 * Decide whether any node we have only heard about is worth going to look for.
 *
 * Gossip cannot establish a link on its own, because the candidate's schedule is
 * unknown. What it can do is turn open-ended scanning into a short scan with a
 * specific target that ends the moment that target is heard.
 */
static void gossip_consider_seeking(void)
{
	uint64_t now = lg_ts_now_us();
	struct lg_gossip *best = NULL;

	/* Already looking for somebody. */
	if (lg.want_addr != LG_ADDR_INVALID && now < lg.want_until_us) {
		return;
	}

	for (int i = 0; i < LG_GOSSIP_SIZE; i++) {
		struct lg_gossip *g = &lg.gossip[i];

		if (g->addr == LG_ADDR_INVALID || g->addr == 0) {
			continue;
		}
		if (g->rank == LG_RANK_INFINITE || lg_neigh_find(g->addr)) {
			continue;
		}
		/* Only worth the receive time if it would actually improve us. */
		if (lg.rank != LG_RANK_INFINITE && g->rank + LG_GOSSIP_SEEK_MARGIN >= lg.rank) {
			continue;
		}
		if (!best || g->rank < best->rank) {
			best = g;
		}
	}

	if (best) {
		lg.want_addr = best->addr;
		lg.want_until_us = now + LG_WANT_TIMEOUT_US;
		LOG_DBG("seeking 0x%04x on hearsay: rank %u against our %u", best->addr,
			best->rank, lg.rank);
		lg_disc_retune();
	}
}

void lg_route_handle_adv(struct lg_neigh *n, const uint8_t *buf, uint8_t len)
{
	uint8_t n_entries;
	uint8_t off = 5;
	uint16_t peer_rank;

	if (len < 5) {
		return;
	}

	peer_rank = lg_get_le16(&buf[1]);
	n->role = buf[3];
	n_entries = buf[4];

	if (n->peer_rank != peer_rank) {
		n->peer_rank = peer_rank;
	}
	n->route_adv_us = lg_ts_now_us();

	for (uint8_t i = 0; i < n_entries && off + 4U <= len; i++, off += 4) {
		lg_addr_t addr = lg_get_le16(&buf[off]);
		uint16_t cost = lg_get_le16(&buf[off + 2]);

		/* A node the neighbour can reach: as a candidate uplink its rank
		 * would be the neighbour's rank plus the neighbour's cost to it.
		 */
		if (peer_rank != LG_RANK_INFINITE) {
			gossip_record(addr, (uint16_t)MIN((uint32_t)peer_rank + cost,
							  LG_RANK_INFINITE - 1U));
		}

		/* If the neighbour is below us in the tree, everything it can
		 * reach is reachable downward through it.
		 */
		if (peer_rank > lg.rank && lg.rank != LG_RANK_INFINITE) {
			lg_route_add(addr, n->addr, 2);
		}
	}

	if (lg_route_recompute()) {
		lg_notify(LG_EVT_PARENT_CHANGED, lg_parent());
	}

	gossip_consider_seeking();
}

void lg_route_handle_reg(struct lg_neigh *n, const uint8_t *buf, uint8_t len)
{
	uint8_t count;
	uint8_t off = 2;

	if (len < 2) {
		return;
	}
	count = buf[1];

	for (uint8_t i = 0; i < count && off + 2U <= len; i++, off += 2) {
		lg_route_add(lg_get_le16(&buf[off]), n->addr, 1);
	}
}

/* ------------------------------------------------------------------------- */
/* Duplicate suppression                                                      */
/* ------------------------------------------------------------------------- */

bool lg_route_is_duplicate(lg_addr_t origin, uint16_t msg_id)
{
	for (int i = 0; i < LG_DUP_CACHE_SIZE; i++) {
		if (dup_cache[i].origin == origin && dup_cache[i].msg_id == msg_id) {
			return true;
		}
	}

	dup_cache[dup_next].origin = origin;
	dup_cache[dup_next].msg_id = msg_id;
	dup_next = (dup_next + 1) % LG_DUP_CACHE_SIZE;
	return false;
}
