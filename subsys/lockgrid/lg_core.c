/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_core.c
 * @brief Node state, lifecycle, the transmit queue and inbound frame dispatch.
 *
 * The interrupt path never decrypts, never allocates and never calls into the
 * application. It copies received frames into a small ring and posts a work
 * item; everything else happens here, on the LockGrid work queue.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "lg_internal.h"
#include "lg_trace.h"

LOG_MODULE_REGISTER(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

struct lg_ctx lg;

/* Received frames waiting for the work queue. Sized so a burst of handshake
 * fragments cannot overflow it.
 */
#define LG_RX_RING_SIZE 8

struct lg_rx_rec {
	uint8_t pdu[LG_PDU_MAX];
	uint8_t len;
	int8_t rssi;
	uint8_t chan;
	uint64_t air_start_us;
};

static struct lg_rx_rec rx_ring[LG_RX_RING_SIZE];
static atomic_t rx_head;
static atomic_t rx_tail;

static struct k_work rx_work;
static struct k_work_q lg_wq;
static K_THREAD_STACK_DEFINE(lg_wq_stack, CONFIG_LOCKGRID_WORKQ_STACK_SIZE);

/* Deferred application notifications, so callbacks never run in an interrupt. */
#define LG_NOTIFY_RING_SIZE 8

struct lg_notify_rec {
	enum lg_evt evt;
	lg_addr_t peer;
};

static struct lg_notify_rec notify_ring[LG_NOTIFY_RING_SIZE];
static atomic_t notify_head;
static atomic_t notify_tail;
static struct k_work notify_work;

struct k_work_q *lg_workq(void)
{
	return &lg_wq;
}

/* ------------------------------------------------------------------------- */
/* Notifications                                                              */
/* ------------------------------------------------------------------------- */

static void notify_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	while (atomic_get(&notify_tail) != atomic_get(&notify_head)) {
		uint32_t idx = (uint32_t)atomic_get(&notify_tail) % LG_NOTIFY_RING_SIZE;
		struct lg_notify_rec rec = notify_ring[idx];

		atomic_inc(&notify_tail);
		if (lg.cb.evt) {
			lg.cb.evt(rec.evt, rec.peer);
		}
	}
}

void lg_notify(enum lg_evt evt, lg_addr_t peer)
{
	uint32_t head = (uint32_t)atomic_get(&notify_head);
	uint32_t tail = (uint32_t)atomic_get(&notify_tail);

	if (head - tail >= LG_NOTIFY_RING_SIZE) {
		return;
	}

	notify_ring[head % LG_NOTIFY_RING_SIZE].evt = evt;
	notify_ring[head % LG_NOTIFY_RING_SIZE].peer = peer;
	atomic_inc(&notify_head);
	k_work_submit_to_queue(&lg_wq, &notify_work);
}

/* ------------------------------------------------------------------------- */
/* Inbound frames                                                             */
/* ------------------------------------------------------------------------- */

void lg_rx_post(const uint8_t *pdu, uint8_t len, int8_t rssi, uint8_t chan,
		uint64_t air_start_us)
{
	uint32_t head = (uint32_t)atomic_get(&rx_head);
	uint32_t tail = (uint32_t)atomic_get(&rx_tail);
	struct lg_rx_rec *rec;

	if (len > LG_PDU_MAX || head - tail >= LG_RX_RING_SIZE) {
		return;
	}

	rec = &rx_ring[head % LG_RX_RING_SIZE];
	memcpy(rec->pdu, pdu, len);
	rec->len = len;
	rec->rssi = rssi;
	rec->chan = chan;
	/* The PHY reconstructed this from the radio's END event and the frame's
	 * air time. Handshake and link anchoring both depend on it, so it must
	 * be carried through rather than re-sampled here, where it would be the
	 * work queue's dispatch time instead.
	 */
	rec->air_start_us = air_start_us;
	atomic_inc(&rx_head);

	k_work_submit_to_queue(&lg_wq, &rx_work);
}

static void dispatch(struct lg_rx_rec *rec)
{
	uint8_t fcf;
	uint8_t seq;
	lg_addr_t dst;
	lg_addr_t src;
	struct lg_neigh *n;
	uint8_t len = rec->len;
	uint8_t type;

	if (!lg_frame_hdr_get(rec->pdu, len, lg.netid16, &fcf, &seq, &dst, &src)) {
		return;
	}
	if (src == lg.addr) {
		return;
	}
	if (dst != lg.addr && dst != LG_ADDR_BROADCAST) {
		return;
	}

	type = fcf & LG_FCF_TYPE_MASK;

	switch (type) {
	case LG_FT_BEACON:
		lg_disc_beacon_rx(rec->pdu, len, rec->rssi, rec->air_start_us);
		return;

	case LG_FT_JOIN_REQ:
	case LG_FT_JOIN_RSP:
	case LG_FT_JOIN_CFM:
	case LG_FT_JOIN_ACK:
	case LG_FT_RESUME_REQ:
	case LG_FT_RESUME_RSP:
		lg_join_rx_pdu(rec->pdu, len, rec->rssi, rec->air_start_us);
		return;

	default:
		break;
	}

	n = lg_neigh_find(src);
	if (!n || n->state != LG_NS_LINKED) {
		return;
	}

	/* Everything past the handshake must be secured. An unsecured data or
	 * control frame is not a degraded frame, it is someone else's.
	 */
	if (!(fcf & LG_FCF_SEC)) {
		lg.stats.auth_failures++;
		return;
	}

	if (lg_sec_decrypt(n, rec->pdu, &len) != 0) {
		return;
	}

	lg_link_rx_frame(n, fcf, seq, &rec->pdu[LG_HDR_LEN_SEC], len - LG_HDR_LEN_SEC,
			 rec->rssi);
}

static void rx_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	while (atomic_get(&rx_tail) != atomic_get(&rx_head)) {
		uint32_t idx = (uint32_t)atomic_get(&rx_tail) % LG_RX_RING_SIZE;

		dispatch(&rx_ring[idx]);
		atomic_inc(&rx_tail);
	}
}

/* ------------------------------------------------------------------------- */
/* Transmit queue                                                             */
/* ------------------------------------------------------------------------- */

/** Next hop for a queued message, recomputed each time so it follows the tree. */
static lg_addr_t msg_next_hop(const struct lg_txmsg *m)
{
	if (m->flags & LG_DATA_F_BROADCAST) {
		return m->final;
	}
	return lg_route_next_hop(m->final, m->flags);
}

struct lg_txmsg *lg_txq_peek(lg_addr_t peer)
{
	struct lg_txmsg *oldest = NULL;

	for (int i = 0; i < CONFIG_LOCKGRID_TX_QUEUE_SIZE; i++) {
		struct lg_txmsg *m = &lg.txq[i];

		if (!m->used || msg_next_hop(m) != peer) {
			continue;
		}
		if (!oldest || m->enqueued_us < oldest->enqueued_us) {
			oldest = m;
		}
	}
	return oldest;
}

bool lg_txq_pending(lg_addr_t peer)
{
	return lg_txq_peek(peer) != NULL;
}

void lg_txq_free(struct lg_txmsg *m)
{
	memset(m, 0, sizeof(*m));
}

static struct lg_txmsg *txq_alloc(void)
{
	for (int i = 0; i < CONFIG_LOCKGRID_TX_QUEUE_SIZE; i++) {
		if (!lg.txq[i].used) {
			return &lg.txq[i];
		}
	}
	return NULL;
}

int lg_send_forward(lg_addr_t origin, lg_addr_t final, uint8_t port, const void *buf, size_t len,
		    uint8_t flags, uint8_t hops, uint16_t msg_id)
{
	struct lg_txmsg *m;
	lg_addr_t nh;

	if (len > LG_PAYLOAD_MAX) {
		return -EMSGSIZE;
	}
	if (!lg.started) {
		return -EAGAIN;
	}

	if (flags & LG_DATA_F_BROADCAST) {
		int sent = 0;

		/* One copy per neighbour. Receivers drop duplicates by origin and
		 * message id, so a node reachable two ways still sees it once.
		 */
		for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
			struct lg_neigh *n = &lg.neigh[i];

			if (n->state != LG_NS_LINKED) {
				continue;
			}
			m = txq_alloc();
			if (!m) {
				break;
			}
			m->used = true;
			m->origin = origin;
			m->final = n->addr;
			m->flags = flags;
			m->port = port;
			m->hops = hops;
			m->msg_id = msg_id;
			m->len = (uint8_t)len;
			m->enqueued_us = lg_ts_now_us();
			memcpy(m->payload, buf, len);
			sent++;
			lg_sched_expedite(n);
		}
		return sent ? 0 : -ENOTCONN;
	}

	nh = lg_route_next_hop(final, flags);
	if (nh == LG_ADDR_INVALID) {
		return -ENOTCONN;
	}

	m = txq_alloc();
	if (!m) {
		return -ENOMEM;
	}

	m->used = true;
	m->origin = origin;
	m->final = final;
	m->flags = flags;
	m->port = port;
	m->hops = hops;
	m->msg_id = msg_id;
	m->len = (uint8_t)len;
	m->enqueued_us = lg_ts_now_us();
	memcpy(m->payload, buf, len);

	return 0;
}

int lg_send(lg_addr_t dst, uint8_t port, const void *buf, size_t len, uint8_t flags)
{
	uint8_t dflags = 0;
	struct lg_neigh *n;
	int err;

	if (dst == LG_ADDR_BACKHAUL) {
		dflags |= LG_DATA_F_UPWARD;
	} else if (dst == LG_ADDR_BROADCAST) {
		dflags |= LG_DATA_F_BROADCAST;
	}

	err = lg_send_forward(lg.addr, dst, port, buf, len, dflags, 0, lg.msg_id_next++);
	if (err) {
		return err;
	}

	if (flags & LG_TX_URGENT) {
		lg_addr_t nh = (dflags & LG_DATA_F_UPWARD) ? lg_parent() : dst;

		n = lg_neigh_find(nh);
		if (n) {
			lg_sched_expedite(n);
		}
	}

	lg_sched_kick();
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Runtime tuning                                                             */
/* ------------------------------------------------------------------------- */

uint8_t lg_subrate_ceiling(uint32_t interval_us, uint32_t sup_timeout_us)
{
	uint32_t max;

	if (!interval_us || !sup_timeout_us) {
		return 1;
	}

	/* Allow room for two missed events inside the supervision timeout, so a
	 * single collision does not tear a healthy link down. Without this a large
	 * subrate silently makes the timeout unreachable: events end up further
	 * apart than the timeout itself, and the link dies on the first miss.
	 */
	max = sup_timeout_us / (interval_us * 3U);
	if (max < 1U) {
		max = 1U;
	}
	return (uint8_t)MIN(max, 32U);
}

void lg_tuning_init(void)
{
	lg.tune.conn_interval_us = CONFIG_LOCKGRID_CONN_INTERVAL_MS * 1000U;
	lg.tune.sup_timeout_us = CONFIG_LOCKGRID_SUPERVISION_TIMEOUT_MS * 1000U;
	lg.tune.subrate_max = CONFIG_LOCKGRID_SUBRATE_MAX;
	lg.tune.scan_unattached_us = CONFIG_LOCKGRID_SCAN_INTERVAL_UNATTACHED_MS * 1000U;
	lg.tune.scan_seeking_us = CONFIG_LOCKGRID_SCAN_INTERVAL_SEEKING_MS * 1000U;
	lg.tune.scan_attached_us = CONFIG_LOCKGRID_SCAN_INTERVAL_ATTACHED_MS * 1000U;
	lg.tune.scan_window_us = CONFIG_LOCKGRID_SCAN_WINDOW_US;
	lg.tune.beacon_interval_us = CONFIG_LOCKGRID_BEACON_INTERVAL_MS * 1000U;
	lg.tune.beacon_idle_factor = CONFIG_LOCKGRID_BEACON_IDLE_FACTOR;
	lg.tune.join_window_floor_us = CONFIG_LOCKGRID_JOIN_WINDOW_FLOOR_MS * 1000U;

	lg.tune.subrate_max = MIN(lg.tune.subrate_max,
				  lg_subrate_ceiling(lg.tune.conn_interval_us,
						     lg.tune.sup_timeout_us));
}

void lg_tuning_get(struct lg_tuning *out)
{
	if (out) {
		*out = lg.tune;
	}
}

int lg_tuning_set(const struct lg_tuning *t)
{
	struct lg_tuning n;

	if (!t) {
		return -EINVAL;
	}
	n = *t;

	n.conn_interval_us = CLAMP(n.conn_interval_us,
				   CONFIG_LOCKGRID_CONN_INTERVAL_MIN_MS * 1000U,
				   CONFIG_LOCKGRID_CONN_INTERVAL_MAX_MS * 1000U);
	n.sup_timeout_us = MAX(n.sup_timeout_us, n.conn_interval_us * 4U);
	n.subrate_max = MAX(1, MIN(n.subrate_max,
				   lg_subrate_ceiling(n.conn_interval_us, n.sup_timeout_us)));
	n.scan_window_us = MAX(n.scan_window_us, 2000U);
	n.scan_unattached_us = MAX(n.scan_unattached_us, n.scan_window_us);
	n.scan_seeking_us = MAX(n.scan_seeking_us, n.scan_window_us);
	n.scan_attached_us = MAX(n.scan_attached_us, n.scan_window_us);
	n.beacon_interval_us = MAX(n.beacon_interval_us, 100000U);
	n.beacon_idle_factor = MAX(1, n.beacon_idle_factor);

	lg.tune = n;

	LOG_INF("tuning: interval %u ms, subrate up to x%u, supervision %u ms, "
		"beacon %u ms (x%u idle)",
		n.conn_interval_us / 1000U, n.subrate_max, n.sup_timeout_us / 1000U,
		n.beacon_interval_us / 1000U, n.beacon_idle_factor);

	/* Discovery rates are re-derived on the next pass; links keep the interval
	 * they negotiated, so nothing has to be renegotiated here.
	 */
	lg_disc_retune();
	lg_sched_kick();
	return 0;
}

int lg_provision(const struct lg_identity *id)
{
	lg_addr_t addr;
	uint8_t role;
	uint8_t pub[LG_EC_PUB_LEN];

	if (!id || !id->cert || id->cert_len != LG_CERT_LEN || !id->priv_key ||
	    id->priv_key_len != LG_EC_PRIV_LEN || !id->ca_pub_key ||
	    id->ca_pub_key_len != LG_EC_PUB_LEN || !id->net_key ||
	    id->net_key_len != LG_KEY_LEN) {
		return -EINVAL;
	}
	if (lg.started) {
		return -EBUSY;
	}

	lg.network_id = id->network_id;
	lg.netid16 = (uint16_t)id->network_id;
	lg.cert = id->cert;
	lg.cert_len = id->cert_len;
	lg.priv_key = id->priv_key;
	lg.priv_key_len = id->priv_key_len;
	lg.ca_pub = id->ca_pub_key;
	lg.ca_pub_len = id->ca_pub_key_len;
	memcpy(lg.net_key, id->net_key, LG_KEY_LEN);

	/* Bring the crypto layer up now so the certificate can be checked here
	 * rather than failing later during a handshake.
	 */
	if (lg_sec_init() != 0) {
		return -EIO;
	}

	if (lg_sec_cert_verify(lg.cert, lg.cert_len, &addr, &role, pub) != 0) {
		LOG_ERR("our own certificate does not verify against the grid CA");
		return -EACCES;
	}

	/* The address comes from the certificate, so identity and address are
	 * the same fact and neither can be claimed without the other.
	 */
	lg.addr = addr;

	/* The role byte in the certificate is reserved and deliberately unused. A
	 * certificate admits a node to the network; what the node then does for the
	 * network it works out for itself at runtime.
	 */
	ARG_UNUSED(role);

	LOG_INF("provisioned as 0x%04x, network 0x%08x", addr, lg.network_id);
	return 0;
}

void lg_callbacks_register(const struct lg_callbacks *cb)
{
	if (cb) {
		lg.cb = *cb;
	} else {
		memset(&lg.cb, 0, sizeof(lg.cb));
	}
}

int lg_start(void)
{
	uint8_t cert_role;
	lg_addr_t cert_addr;
	uint8_t pub[LG_EC_PUB_LEN];
	int err;

	if (lg.started) {
		return -EALREADY;
	}
	if (lg.addr == LG_ADDR_INVALID) {
		return -EACCES;
	}

	err = lg_sec_cert_verify(lg.cert, lg.cert_len, &cert_addr, &cert_role, pub);
	if (err) {
		return err;
	}

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		lg.neigh[i].state = LG_NS_FREE;
	}
	memset(lg.txq, 0, sizeof(lg.txq));
	memset(&lg.stats, 0, sizeof(lg.stats));
	lg.msg_id_next = 1;
	lg.link_count = 0;

	lg_tuning_init();
	lg_power_init();
	lg_chan_init();
	lg_role_init();
	lg_route_init();
	lg_join_init();

	lg_phy_network_set(lg.network_id);
	/* Promiscuous, because the role can change at any time and a node that has
	 * just promoted must already be hearing broadcasts. The saving from the
	 * narrower filter is not worth having to reconfigure the radio mid-flight.
	 */
	lg_phy_filter_set(lg.addr, true);

	err = lg_sched_init();
	if (err) {
		return err;
	}

	lg.started = true;
	lg_disc_init();
	lg_role_evaluate();
	lg_disc_beacon_finish();
	lg_sched_kick();

	LOG_INF("started as 0x%04x, role %u (chosen at runtime), rank %u", lg.addr, lg.role,
		lg.rank);
	LG_TRACE("boot", "role=%u uplink=%u", (unsigned)lg.role, lg.backhaul_active ? 1U : 0U);
	return 0;
}

int lg_stop(void)
{
	if (!lg.started) {
		return -EALREADY;
	}

	for (int i = 0; i < CONFIG_LOCKGRID_MAX_NEIGHBORS; i++) {
		if (lg.neigh[i].state == LG_NS_LINKED) {
			lg_link_down(&lg.neigh[i], LG_TD_LOCAL_CLOSE);
		}
	}

	lg.started = false;
	lg_sched_stop();
	return 0;
}

lg_addr_t lg_local_addr(void)
{
	return lg.addr;
}

static int lg_core_init(void)
{
	struct k_work_queue_config cfg = {
		.name = "lockgrid",
		.no_yield = false,
	};

	k_work_queue_start(&lg_wq, lg_wq_stack, K_THREAD_STACK_SIZEOF(lg_wq_stack),
			   CONFIG_LOCKGRID_WORKQ_PRIO, &cfg);

	k_work_init(&rx_work, rx_handler);
	k_work_init(&notify_work, notify_handler);

	return 0;
}

SYS_INIT(lg_core_init, APPLICATION, 50);
