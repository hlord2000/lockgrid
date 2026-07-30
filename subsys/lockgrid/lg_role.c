/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_role.c
 * @brief Runtime role selection.
 *
 * A node is not told whether it is a leaf or a router. It decides, and it can
 * change its mind, because the right answer depends on where it turns out to be
 * rather than on anything knowable at provisioning time.
 *
 * Backhaul is the exception. Terminating traffic means having somewhere to
 * terminate it - a wired uplink, a cellular modem - which is a property of the
 * hardware, not something a node can elect itself into. The application declares
 * it through lg_backhaul_set().
 *
 * None of this comes from the certificate. The certificate admits a node to the
 * network; it says nothing about what the node should then do, because that is not
 * knowable when it is issued.
 *
 * The leaf-versus-router decision is a cost question. A router pays for beacons,
 * for join windows, for more links and for forwarding other nodes' traffic; a
 * leaf pays for one uplink and nothing else. So the default is leaf, and a node
 * only takes on the cost when the grid gets something for it:
 *
 *  - it hears too few routers, meaning the area is thin and its neighbours may
 *    have nowhere to attach, or
 *  - it can hear a node that has no route at all, which is a direct signal that
 *    somebody nearby needs a parent.
 *
 * It gives the role back when neither is true any more and nothing is routing
 * through it. The thresholds differ in each direction, so a node sitting exactly
 * at the boundary does not oscillate.
 *
 * The grid therefore grows outward from the backhauls: nodes next to a backhaul
 * attach, find they are the only router in a thin area, promote, and become the
 * way the next ring attaches. Nothing coordinates this.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "lg_internal.h"
#include "lg_trace.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/** How recently an unrouted neighbour must have been heard to still count. */
#define LG_UNROUTED_LIFETIME_US (20ULL * USEC_PER_SEC)

/** Shortest time between role changes, so a marginal node cannot oscillate. */
#define LG_ROLE_DWELL_US (15ULL * USEC_PER_SEC)

/** Routers this node can hear that already have a route of their own. */
static uint8_t routers_heard(void)
{
	uint8_t count = 0;

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		const struct lg_neigh *n = &lg.neigh[i];

		if (n->state == LG_NS_FREE) {
			continue;
		}
		if (n->role == LG_ROLE_LEAF || n->peer_rank == LG_RANK_INFINITE) {
			continue;
		}
		count++;
	}
	return count;
}

/** Whether anything is currently reaching the grid through us. */
static bool has_children(void)
{
	uint64_t now = lg_ts_now_us();

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_ROUTES; i++) {
		if (lg.routes[i].dst != LG_ADDR_INVALID && lg.routes[i].expiry_us > now) {
			return true;
		}
	}
	return false;
}

static bool unrouted_nearby(void)
{
	uint64_t now = lg_ts_now_us();

	return lg.unrouted_seen_us &&
	       now - lg.unrouted_seen_us < LG_UNROUTED_LIFETIME_US;
}

static void apply(enum lg_role want, const char *why)
{
	enum lg_role was = lg.role;

	if (want == was) {
		return;
	}

	lg.role = want;
	lg.role_changed_us = lg_ts_now_us();
	lg.stats.role_changes++;

	/*
	 * Rank is a property of the role at the top of the tree, so a node that
	 * becomes or stops being a backhaul has to recompute rather than keep the
	 * value it had. Recomputing is all that is needed: lg_route_recompute()
	 * already pins a backhaul at zero and derives everything else from the
	 * parent set.
	 *
	 * Setting the rank here first, as this did, was worse than redundant. On
	 * demotion it wrote LG_RANK_INFINITE before the recompute, so the recompute
	 * saw a rank that was already infinite and concluded nothing had changed -
	 * which meant a node that stopped being a backhaul and had nowhere else to
	 * forward to never told the application it had lost its route.
	 */
	(void)lg_route_recompute();

	LOG_INF("role %u -> %u: %s (rank %u, routers heard %u)", was, want, why, lg.rank,
		routers_heard());
	LG_TRACE("role", "old=%u new=%u rank=%u routers=%u", (unsigned)was, (unsigned)want,
		 lg.rank, routers_heard());

	/* Beaconing and join windows both follow the role, and a promotion has to
	 * start advertising before anyone can attach to it.
	 */
	lg_disc_retune();
	lg_sched_kick();
	lg_notify(LG_EVT_ROLE_CHANGED, lg.addr);
}

void lg_role_init(void)
{
	/*
	 * Start as the cheapest thing the node is allowed to be. A node that is
	 * backhaul capable and whose uplink is already up is the exception: it is
	 * the seed the rest of the grid grows from, so it takes the role
	 * immediately rather than waiting to be asked.
	 */
	lg.unrouted_seen_us = 0;
	lg.role_changed_us = 0;

	if (lg.backhaul_active) {
		lg.role = LG_ROLE_BACKHAUL;
		lg.rank = 0;
	} else {
		lg.role = LG_ROLE_LEAF;
		lg.rank = LG_RANK_INFINITE;
	}
}

void lg_role_evaluate(void)
{
	uint64_t now = lg_ts_now_us();
	uint8_t routers;

	if (!lg.started) {
		return;
	}

	/* Backhaul is not a decision, it is a fact about the uplink. */
	if (lg.backhaul_active) {
		apply(LG_ROLE_BACKHAUL, "uplink is up");
		return;
	}
	if (lg.role == LG_ROLE_BACKHAUL) {
		/* The uplink went away. Keep routing - the node is still useful as a
		 * relay - but stop claiming to be a sink.
		 */
		apply(LG_ROLE_ROUTER, "uplink went down");
		return;
	}

	/* Give a change time to take effect before reconsidering it. */
	if (lg.role_changed_us && now - lg.role_changed_us < LG_ROLE_DWELL_US) {
		return;
	}

	routers = routers_heard();

	if (lg.role == LG_ROLE_LEAF) {
		/* Promotion needs a route of our own: a router with nowhere to
		 * forward to is worse than useless, because neighbours will attach
		 * to it and believe they have a path.
		 */
		if (!lg_has_route()) {
			return;
		}
		if (unrouted_nearby()) {
			apply(LG_ROLE_ROUTER, "a neighbour has no route");
		} else if (routers < CONFIG_LOCKGRID_ROUTER_DENSITY_TARGET) {
			apply(LG_ROLE_ROUTER, "too few routers in range");
		}
		return;
	}

	/* Demotion. Never while something is routing through us, and only once
	 * there is clearly enough redundancy without us - one more than the
	 * promotion threshold, so the two decisions cannot chase each other.
	 */
	if (has_children() || unrouted_nearby()) {
		return;
	}
	if (routers > CONFIG_LOCKGRID_ROUTER_DENSITY_TARGET) {
		apply(LG_ROLE_LEAF, "enough routers in range without us");
	}
}

void lg_role_note_unrouted(void)
{
	lg.unrouted_seen_us = lg_ts_now_us();
}

int lg_backhaul_set(bool uplink_up)
{
	if (lg.backhaul_active == uplink_up) {
		return 0;
	}

	lg.backhaul_active = uplink_up;
	if (lg.started) {
		/* Bypasses the dwell time: losing an uplink should not leave the
		 * node advertising rank zero for another fifteen seconds.
		 */
		lg.role_changed_us = 0;
		lg_role_evaluate();
	}
	return 0;
}

enum lg_role lg_role(void)
{
	return lg.role;
}
