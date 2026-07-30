/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_join.c
 * @brief Certificate-backed authentication and link key establishment.
 *
 * A joiner and a router run a mutually authenticated ephemeral Diffie-Hellman
 * exchange. Each proves possession of the private key matching a certificate
 * signed by the grid's certificate authority, and each signs a transcript that
 * binds both nonces, both ephemeral public keys, both addresses and the network
 * id. The link key is derived from the shared secret and that transcript, so it
 * is fresh per attempt and cannot be transplanted to another pair of nodes or
 * another grid.
 *
 *   slot 0   joiner -> router   nonce_j, eph_pub_j, cert_j
 *   slot 1   router -> joiner   nonce_r, eph_pub_r, cert_r, sig_r(transcript)
 *   slot 2   joiner -> router   sig_j(transcript)          encrypted
 *   slot 3   router -> joiner   link parameters             encrypted
 *
 * Slots are 40 ms apart and each is a separately scheduled radio event. The gap
 * is not politeness: an ECDSA verify followed by a sign cannot happen inside the
 * 200 us inter-frame space, so the work has to land on the work queue between
 * radio events.
 *
 * The origin both ends measure slots from is the first bit of slot 0 - the time
 * the joiner transmitted it, and the time the router received it. Deliberately
 * not the join window the beacon advertised: the window is a contention window
 * that joiners start at a random point inside, so that two nodes which heard the
 * same beacon do not transmit on top of each other on every single fragment and
 * leave neither of them able to join. Using the frame itself as the origin means
 * the randomisation costs nothing in agreement.
 *
 * Which slot it is comes from the clock, and which side transmits comes from the
 * protocol state - never from slot parity. That combination is what makes the
 * exchange survive the scheduler: a node holding links and beaconing will
 * sometimes have something more urgent than a handshake slot, and the slot gets
 * dropped. With parity, one dropped slot puts the two ends in opposite roles and
 * the handshake dies. Here, the side that still owes a message simply sends it in
 * the next slot and the other side is still listening, because neither has
 * changed state.
 *
 * The address a node may use is carried in its certificate, so an address cannot
 * be claimed without the corresponding private key. Identity and address are the
 * same fact.
 *
 * Re-attaching after a link is lost skips all of this. The long term key derived
 * during the original handshake is kept, and a single round trip re-derives a
 * fresh session key from it. That is what makes healing cheap: a node that loses
 * its parent and moves to a neighbour it has met before is back on a schedule in
 * one exchange rather than four.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "lg_internal.h"
#include "lg_trace.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/** Spacing between handshake slots. Sized for a verify plus a sign. */
#define LG_JOIN_SLOT_US 40000
/** Slack either side of an expected handshake frame. */
#define LG_JOIN_RX_MARGIN_US 1500
/** Gap between fragments of one handshake message. */
#define LG_FRAG_GAP_US 500
/** Attempts against one router before giving up on it. */
#define LG_JOIN_MAX_RETRIES 3
/** How long a router is left alone after that many failures. */
#define LG_JOIN_BACKOFF_US (10ULL * USEC_PER_SEC)

/** Fragment header: index then total. */
#define LG_FRAG_HDR_LEN 2
/** Payload capacity of one unsecured handshake fragment. */
#define LG_FRAG_CAP (LG_PDU_MAX - LG_HDR_LEN - LG_FRAG_HDR_LEN)
/** Payload capacity of one secured handshake fragment. */
#define LG_FRAG_CAP_SEC (LG_PDU_MAX - LG_SEC_OVERHEAD - LG_FRAG_HDR_LEN)

/**
 * One handshake at a time. Two nodes trying to authenticate simultaneously is
 * rare, the exchange is short, and keeping a single context saves several
 * hundred bytes per neighbour slot.
 */
static struct {
	struct lg_neigh *n;
	/** Shared origin both ends measure slot times from. */
	uint64_t t0_us;
	uint8_t slot;
	bool resuming;
	/** True on the side that started the exchange. */
	bool joiner;

	uint8_t eph_priv[LG_EC_PRIV_LEN];
	uint8_t eph_pub[LG_EC_PUB_LEN];

	/* Outbound message, fragmented on transmit. */
	uint8_t tx[LG_XFER_MAX];
	uint16_t tx_len;
	uint8_t tx_type;
	bool tx_secured;
	uint8_t frag_idx;
	uint8_t frag_total;
	/** Air start of the most recent fragment we sent, for anchoring. */
	uint64_t last_tx_air_start_us;
	/** Its length, which is what makes that air start exact. */
	uint8_t last_tx_len;

	/* Inbound reassembly. */
	struct lg_xfer rx;
	uint8_t rx_type;

	/** Set when a step completed and produced the next message. */
	bool progressed;
	/**
	 * True while this side has a message the peer has not acknowledged by
	 * replying. Decides transmit versus receive, in place of slot parity.
	 */
	bool owe_tx;
	/** Slot the exchange must complete by: a backstop, not the real budget. */
	uint8_t slot_limit;
	/**
	 * Consecutive slots that produced no forward progress.
	 *
	 * This, not the slot index, is what the handshake gives up on. The index is
	 * derived from the clock so that both ends agree on which slot is which,
	 * which means it advances whether or not this node got the radio. Failing on
	 * it alone made the budget a wall-clock deadline: two nodes that each
	 * already hold a link lose slots to their own link events, the index races
	 * ahead, and a six-message exchange died with most of its attempts unused.
	 * That was most of the failed handshakes in a dense grid, and every one of
	 * them cost the router a join window it had already paid to open.
	 */
	uint8_t idle_slots;
	/** Whether the slot just run was ours to transmit in. */
	bool was_tx;
	/**
	 * Whether the radio was refused for the slot just run.
	 *
	 * Distinguishes "we had the radio and nothing came of it", which is evidence
	 * the exchange is not working, from "we never got the radio", which is
	 * evidence of nothing at all. Only the first may count against the budget.
	 */
	bool slot_refused;
} hs;

/**
 * Slots that may produce nothing before the handshake is abandoned.
 *
 * Generous, because a slot producing nothing is usually contention rather than a
 * missing peer, and the cost of giving up is a retry from the beginning.
 */
#define LG_JOIN_IDLE_LIMIT        8
#define LG_JOIN_IDLE_LIMIT_RESUME 5

/* Staging buffer for the fragment currently being transmitted. Received frames
 * land in the buffer the scheduler hands out.
 */
static uint8_t join_frame[LG_PDU_MAX];

void lg_join_init(void)
{
	memset(&hs, 0, sizeof(hs));
}

/* ------------------------------------------------------------------------- */
/* Transcript and key derivation                                             */
/* ------------------------------------------------------------------------- */

/**
 * Hash everything both ends must agree on. Signing this proves the signer saw
 * the same exchange, which is what stops a relay from splicing two handshakes
 * together.
 */
static int transcript_build(struct lg_neigh *n, const uint8_t *nonce_j, const uint8_t *nonce_r,
			    const uint8_t *pub_j, const uint8_t *pub_r, lg_addr_t addr_j,
			    lg_addr_t addr_r, uint8_t *out)
{
	uint8_t ids[4 + 2 + 2];
	const uint8_t *parts[5];
	size_t lens[5];

	ARG_UNUSED(n);

	lg_put_le32(&ids[0], lg.network_id);
	lg_put_le16(&ids[4], addr_j);
	lg_put_le16(&ids[6], addr_r);

	parts[0] = ids;
	lens[0] = sizeof(ids);
	parts[1] = nonce_j;
	lens[1] = LG_NONCE_LEN;
	parts[2] = nonce_r;
	lens[2] = LG_NONCE_LEN;
	parts[3] = pub_j;
	lens[3] = LG_EC_PUB_LEN;
	parts[4] = pub_r;
	lens[4] = LG_EC_PUB_LEN;

	return lg_sec_hash(parts, lens, 5, out);
}

/** Derive the session key and the long term key from one shared secret. */
static int derive_keys(struct lg_neigh *n, const uint8_t *secret, const uint8_t *transcript)
{
	uint8_t link_key[LG_KEY_LEN];
	int err;

	err = lg_sec_kdf(secret, LG_EC_PRIV_LEN, transcript, LG_HASH_LEN, "LockGrid link v1",
			 link_key);
	if (err) {
		return err;
	}

	err = lg_sec_kdf(secret, LG_EC_PRIV_LEN, transcript, LG_HASH_LEN, "LockGrid ltk v1",
			 n->ltk);
	if (err) {
		return err;
	}
	n->has_ltk = true;

	err = lg_sec_install_key(n, link_key);
	memset(link_key, 0, sizeof(link_key));
	return err;
}

/** Session key for a resumption, bound to the joiner's fresh nonce. */
static int derive_resume_key(struct lg_neigh *n, const uint8_t *nonce_j)
{
	uint8_t salt[LG_NONCE_LEN + 4];
	uint8_t key[LG_KEY_LEN];
	int err;

	memcpy(salt, nonce_j, LG_NONCE_LEN);
	lg_put_le16(&salt[LG_NONCE_LEN], MIN(lg.addr, n->addr));
	lg_put_le16(&salt[LG_NONCE_LEN + 2], MAX(lg.addr, n->addr));

	err = lg_sec_kdf(n->ltk, LG_KEY_LEN, salt, sizeof(salt), "LockGrid resume v1", key);
	if (err) {
		return err;
	}

	err = lg_sec_install_key(n, key);
	memset(key, 0, sizeof(key));
	return err;
}

/* ------------------------------------------------------------------------- */
/* Link parameter construction                                                */
/* ------------------------------------------------------------------------- */

static void link_params_fill(struct lg_link_params *pp)
{
	pp->interval_us = lg.tune.conn_interval_us;
	/* First event two intervals after the frame that carries this, so both
	 * ends have time to settle before the schedule starts.
	 */
	pp->anchor_offset_us = pp->interval_us * 2U;
	pp->chan_map = lg.chan_map;
	pp->sup_timeout_cs = (uint16_t)(lg.tune.sup_timeout_us / 10000U);
	pp->subrate_max = lg.tune.subrate_max;
	pp->sca_ppm = CONFIG_LOCKGRID_CLOCK_ACCURACY_PPM;
	pp->role = (uint8_t)lg.role;
	pp->rank = lg.rank;
	lg_sec_rand(&pp->hop_seed, sizeof(pp->hop_seed));
}

/* ------------------------------------------------------------------------- */
/* Message staging                                                            */
/* ------------------------------------------------------------------------- */

static void stage_message(uint8_t type, bool secured, uint16_t len)
{
	uint16_t cap = secured ? LG_FRAG_CAP_SEC : LG_FRAG_CAP;

	hs.owe_tx = true;
	hs.tx_type = type;
	hs.tx_secured = secured;
	hs.tx_len = len;
	hs.frag_idx = 0;
	hs.frag_total = (uint8_t)DIV_ROUND_UP(len, cap);
	if (!hs.frag_total) {
		hs.frag_total = 1;
	}
}

/** Build fragment @p idx of the staged message into @p out. */
static uint8_t build_fragment(uint8_t idx, uint8_t *out)
{
	struct lg_neigh *n = hs.n;
	uint16_t cap = hs.tx_secured ? LG_FRAG_CAP_SEC : LG_FRAG_CAP;
	uint16_t off = (uint16_t)idx * cap;
	uint16_t chunk = MIN(cap, hs.tx_len > off ? hs.tx_len - off : 0);
	uint8_t fcf = hs.tx_type | (hs.tx_secured ? LG_FCF_SEC : 0);
	uint8_t hdr_len;
	uint8_t len;

	hdr_len = lg_frame_hdr_put(out, lg.netid16, fcf, n->tx_seq++, n->addr, lg.addr);
	out[hdr_len] = idx;
	out[hdr_len + 1] = hs.frag_total;
	memcpy(&out[hdr_len + LG_FRAG_HDR_LEN], &hs.tx[off], chunk);
	len = hdr_len + LG_FRAG_HDR_LEN + chunk;

	if (hs.tx_secured) {
		if (lg_sec_encrypt(n, out, LG_HDR_LEN_SEC, &len) != 0) {
			return 0;
		}
	}

	return len;
}

/* ------------------------------------------------------------------------- */
/* Reassembly                                                                 */
/* ------------------------------------------------------------------------- */

static bool reassemble(struct lg_neigh *n, uint8_t *pdu, uint8_t len)
{
	uint8_t fcf = pdu[LG_OFF_FCF];
	uint8_t type = fcf & LG_FCF_TYPE_MASK;
	uint8_t hdr_len = (fcf & LG_FCF_SEC) ? LG_HDR_LEN_SEC : LG_HDR_LEN;
	uint8_t idx, total;
	uint16_t chunk;
	uint16_t cap;

	if (fcf & LG_FCF_SEC) {
		if (lg_sec_decrypt(n, pdu, &len) != 0) {
			return false;
		}
	}

	if (len < hdr_len + LG_FRAG_HDR_LEN) {
		return false;
	}

	idx = pdu[hdr_len];
	total = pdu[hdr_len + 1];
	chunk = len - hdr_len - LG_FRAG_HDR_LEN;
	cap = (fcf & LG_FCF_SEC) ? LG_FRAG_CAP_SEC : LG_FRAG_CAP;

	if (!total || total > 8 || idx >= total) {
		return false;
	}
	if ((uint32_t)idx * cap + chunk > LG_XFER_MAX) {
		return false;
	}

	if (hs.rx_type != type || idx == 0) {
		memset(&hs.rx, 0, sizeof(hs.rx));
		hs.rx_type = type;
	}

	memcpy(&hs.rx.buf[idx * cap], &pdu[hdr_len + LG_FRAG_HDR_LEN], chunk);
	hs.rx.frags_seen |= BIT(idx);
	if (idx == total - 1) {
		hs.rx.len = idx * cap + chunk;
	}

	if (hs.rx.frags_seen == BIT_MASK(total) && hs.rx.len) {
		hs.rx.complete = true;
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* Handshake steps, all on the work queue                                     */
/* ------------------------------------------------------------------------- */

static void join_fail(struct lg_neigh *n, const char *why)
{
	LOG_WRN("handshake with 0x%04x failed: %s", n ? n->addr : 0, why);
	lg.stats.joins_failed++;
	LG_TRACE("joinfail", "peer=%04x slot=%u", n ? n->addr : 0, hs.slot);

	if (n) {
		lg_sec_forget_key(n);
		n->join_step = LG_JOIN_IDLE;
		n->join_retries++;
		if (n->join_retries >= LG_JOIN_MAX_RETRIES) {
			n->join_backoff_us = lg_ts_now_us() + LG_JOIN_BACKOFF_US;
		}
		/* Keep the neighbour so its beacons are still tracked, but back
		 * off: a router that rejects us should not be retried in a loop.
		 */
		n->state = n->has_ltk ? LG_NS_LOST : LG_NS_HEARD;
		lg_notify(LG_EVT_AUTH_FAILED, n->addr);
	}
	memset(&hs, 0, sizeof(hs));
	lg_sched_kick();
}

/** Joiner, before slot 0: generate an ephemeral key and build JOIN_REQ. */
static int build_join_req(struct lg_neigh *n)
{
	uint16_t off = 0;

	if (lg_sec_rand(n->nonce_local, LG_NONCE_LEN) != 0) {
		return -EIO;
	}
	if (lg_sec_ecdh_keypair(hs.eph_pub, hs.eph_priv) != 0) {
		return -EIO;
	}

	memcpy(&hs.tx[off], n->nonce_local, LG_NONCE_LEN);
	off += LG_NONCE_LEN;
	memcpy(&hs.tx[off], hs.eph_pub, LG_EC_PUB_LEN);
	off += LG_EC_PUB_LEN;
	memcpy(&hs.tx[off], lg.cert, lg.cert_len);
	off += lg.cert_len;

	stage_message(LG_FT_JOIN_REQ, false, off);
	return 0;
}

/** Router, after slot 0: check the joiner, derive the key, build JOIN_RSP. */
static int handle_join_req(struct lg_neigh **np, const uint8_t *msg, uint16_t len)
{
	const uint8_t *nonce_j = msg;
	const uint8_t *pub_j = msg + LG_NONCE_LEN;
	const uint8_t *cert_j = msg + LG_NONCE_LEN + LG_EC_PUB_LEN;
	uint8_t secret[LG_EC_PRIV_LEN];
	uint8_t transcript[LG_HASH_LEN];
	uint8_t peer_pub[LG_EC_PUB_LEN];
	lg_addr_t cert_addr;
	uint8_t cert_role;
	struct lg_neigh *n = *np;
	uint16_t off;
	int err;

	if (len != LG_NONCE_LEN + LG_EC_PUB_LEN + LG_CERT_LEN) {
		return -EINVAL;
	}

	err = lg_sec_cert_verify(cert_j, LG_CERT_LEN, &cert_addr, &cert_role, peer_pub);
	if (err) {
		return err;
	}

	/* The certificate names the address, so the source address in the frame
	 * has to match it. Otherwise a valid certificate could be replayed to
	 * claim someone else's place in the grid.
	 */
	if (cert_addr != n->addr) {
		LOG_WRN("certificate address 0x%04x does not match sender 0x%04x", cert_addr,
			n->addr);
		return -EPERM;
	}

	memcpy(n->nonce_peer, nonce_j, LG_NONCE_LEN);
	memcpy(n->eph_pub_peer, pub_j, LG_EC_PUB_LEN);
	memcpy(n->peer_static_pub, peer_pub, LG_EC_PUB_LEN);
	n->role = cert_role;

	if (lg_sec_rand(n->nonce_local, LG_NONCE_LEN) != 0) {
		return -EIO;
	}
	if (lg_sec_ecdh_keypair(hs.eph_pub, hs.eph_priv) != 0) {
		return -EIO;
	}

	err = transcript_build(n, n->nonce_peer, n->nonce_local, n->eph_pub_peer, hs.eph_pub,
			       n->addr, lg.addr, transcript);
	if (err) {
		return err;
	}
	memcpy(n->transcript, transcript, LG_HASH_LEN);

	err = lg_sec_ecdh(hs.eph_priv, n->eph_pub_peer, secret);
	if (err) {
		return err;
	}
	err = derive_keys(n, secret, transcript);
	memset(secret, 0, sizeof(secret));
	if (err) {
		return err;
	}

	off = 0;
	memcpy(&hs.tx[off], n->nonce_local, LG_NONCE_LEN);
	off += LG_NONCE_LEN;
	memcpy(&hs.tx[off], hs.eph_pub, LG_EC_PUB_LEN);
	off += LG_EC_PUB_LEN;
	memcpy(&hs.tx[off], lg.cert, lg.cert_len);
	off += lg.cert_len;

	err = lg_sec_sign(transcript, &hs.tx[off]);
	if (err) {
		return err;
	}
	off += LG_SIG_LEN;

	stage_message(LG_FT_JOIN_RSP, false, off);
	return 0;
}

/** Joiner, after slot 1: check the router, derive the key, build JOIN_CFM. */
static int handle_join_rsp(struct lg_neigh *n, const uint8_t *msg, uint16_t len)
{
	const uint8_t *nonce_r = msg;
	const uint8_t *pub_r = msg + LG_NONCE_LEN;
	const uint8_t *cert_r = msg + LG_NONCE_LEN + LG_EC_PUB_LEN;
	const uint8_t *sig_r = cert_r + LG_CERT_LEN;
	uint8_t secret[LG_EC_PRIV_LEN];
	uint8_t transcript[LG_HASH_LEN];
	uint8_t peer_pub[LG_EC_PUB_LEN];
	lg_addr_t cert_addr;
	uint8_t cert_role;
	int err;

	if (len != LG_NONCE_LEN + LG_EC_PUB_LEN + LG_CERT_LEN + LG_SIG_LEN) {
		return -EINVAL;
	}

	err = lg_sec_cert_verify(cert_r, LG_CERT_LEN, &cert_addr, &cert_role, peer_pub);
	if (err) {
		return err;
	}
	if (cert_addr != n->addr) {
		return -EPERM;
	}

	memcpy(n->nonce_peer, nonce_r, LG_NONCE_LEN);
	memcpy(n->eph_pub_peer, pub_r, LG_EC_PUB_LEN);
	memcpy(n->peer_static_pub, peer_pub, LG_EC_PUB_LEN);
	n->role = cert_role;

	err = transcript_build(n, n->nonce_local, n->nonce_peer, hs.eph_pub, n->eph_pub_peer,
			       lg.addr, n->addr, transcript);
	if (err) {
		return err;
	}

	/* The router's signature over the transcript is what actually
	 * authenticates it: the certificate alone only says the key is genuine.
	 */
	err = lg_sec_verify(peer_pub, transcript, sig_r);
	if (err) {
		return err;
	}

	memcpy(n->transcript, transcript, LG_HASH_LEN);

	err = lg_sec_ecdh(hs.eph_priv, n->eph_pub_peer, secret);
	if (err) {
		return err;
	}
	err = derive_keys(n, secret, transcript);
	memset(secret, 0, sizeof(secret));
	if (err) {
		return err;
	}

	err = lg_sec_sign(transcript, hs.tx);
	if (err) {
		return err;
	}

	stage_message(LG_FT_JOIN_CFM, true, LG_SIG_LEN);
	return 0;
}

/** Router, after slot 2: check the joiner's signature, build JOIN_ACK. */
static int handle_join_cfm(struct lg_neigh *n, const uint8_t *msg, uint16_t len)
{
	struct lg_link_params pp;
	int err;

	if (len != LG_SIG_LEN) {
		return -EINVAL;
	}

	err = lg_sec_verify(n->peer_static_pub, n->transcript, msg);
	if (err) {
		return err;
	}

	link_params_fill(&pp);
	memcpy(hs.tx, &pp, sizeof(pp));
	stage_message(LG_FT_JOIN_ACK, true, sizeof(pp));
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Resumption                                                                 */
/* ------------------------------------------------------------------------- */

static int build_resume_req(struct lg_neigh *n)
{
	if (lg_sec_rand(n->nonce_local, LG_NONCE_LEN) != 0) {
		return -EIO;
	}
	memcpy(hs.tx, n->nonce_local, LG_NONCE_LEN);
	stage_message(LG_FT_RESUME_REQ, false, LG_NONCE_LEN);
	return 0;
}

static int handle_resume_req(struct lg_neigh *n, const uint8_t *msg, uint16_t len)
{
	struct lg_link_params pp;
	int err;

	if (len != LG_NONCE_LEN || !n->has_ltk) {
		return -EINVAL;
	}

	memcpy(n->nonce_peer, msg, LG_NONCE_LEN);
	err = derive_resume_key(n, n->nonce_peer);
	if (err) {
		return err;
	}

	/* The response is encrypted under the resumed key. Being able to read it
	 * proves this node holds the long term key, and the joiner's own first
	 * link frame proves the same in the other direction.
	 */
	link_params_fill(&pp);
	memcpy(hs.tx, &pp, sizeof(pp));
	stage_message(LG_FT_RESUME_RSP, true, sizeof(pp));
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Driving the exchange                                                       */
/* ------------------------------------------------------------------------- */

int lg_join_start(struct lg_neigh *n)
{
	int err;

	if (hs.n && hs.n != n) {
		return -EBUSY;
	}
	if (n->state == LG_NS_LINKED) {
		return -EALREADY;
	}
	if (n->join_backoff_us) {
		if (lg_ts_now_us() < n->join_backoff_us) {
			return -ECONNREFUSED;
		}
		/* Backoff served. Forget the failures rather than writing this
		 * router off for good - the reason may well have been contention
		 * with another joiner, which is transient.
		 */
		n->join_backoff_us = 0;
		n->join_retries = 0;
	}
	if (n->join_retries >= LG_JOIN_MAX_RETRIES) {
		return -ECONNREFUSED;
	}
	if (!lg.cert || lg.cert_len != LG_CERT_LEN) {
		return -ENOKEY;
	}

	memset(&hs, 0, sizeof(hs));
	hs.n = n;
	hs.joiner = true;
	hs.t0_us = n->join_next_us + n->join_spread_us;
	hs.slot = 0;
	hs.resuming = n->has_ltk;
	/* Enough slots for every message plus a retry of each, so a slot the
	 * scheduler had to drop costs latency rather than the whole attempt.
	 */
	/* Four messages need four slots; the rest is retry budget. A dense grid
	 * loses handshake messages to collisions often enough that a tight budget
	 * turns a recoverable exchange into a failure and a ten second backoff.
	 */
	hs.slot_limit = hs.resuming ? 24 : 48;

	err = hs.resuming ? build_resume_req(n) : build_join_req(n);
	if (err) {
		join_fail(n, "could not build the first message");
		return err;
	}

	n->state = LG_NS_JOINING;
	n->join_step = hs.resuming ? LG_JOIN_SEND_RESUME : LG_JOIN_SEND_REQ;
	return 0;
}

/** Absolute time of handshake slot @p slot. */
static uint64_t slot_time(uint8_t slot)
{
	return hs.t0_us + (uint64_t)slot * LG_JOIN_SLOT_US;
}

/**
 * Next slot boundary from the clock.
 *
 * Both ends compute the same value from the same origin, so a slot that one of
 * them had to drop does not shift its idea of where the slots are.
 */
static uint8_t slot_next_from_clock(void)
{
	uint64_t now = lg_ts_now_us();
	uint64_t elapsed;

	if (now <= hs.t0_us) {
		return 0;
	}
	elapsed = now - hs.t0_us;
	return (uint8_t)MIN(elapsed / LG_JOIN_SLOT_US + 1U, 255U);
}

uint64_t lg_join_next_us(struct lg_neigh *n)
{
	if (!hs.n || hs.n != n || n->state != LG_NS_JOINING) {
		return UINT64_MAX;
	}
	if (hs.slot == 0) {
		/* Slot zero is the one the joiner picked inside the contention
		 * window; it has not happened yet.
		 */
		return slot_time(0);
	}
	return slot_time(MAX(hs.slot, slot_next_from_clock()));
}

/** True when the next slot is ours to transmit in. */
bool lg_join_slot_is_tx(void)
{
	return hs.n != NULL && hs.owe_tx;
}

uint64_t lg_join_win_next_us(void)
{
	/* A router with a pending join window and no handshake under way. */
	if (hs.n || !lg.join_win_us) {
		return UINT64_MAX;
	}
	if (lg_neigh_linked_count() >= CONFIG_LOCKGRID_MAX_LINKS) {
		return UINT64_MAX;
	}
	return lg.join_win_us;
}

/*
 * A receiver does not know how many fragments are coming until the first one
 * arrives, so its window is sized for the largest handshake message: three full
 * frames plus the gaps, which is about 14 ms, inside LG_JOIN_SLOT_LEN_US. This
 * is by far the most expensive receive LockGrid ever does, which is why it only
 * happens on a join and why re-attaching uses the two-slot resumption instead.
 */

/* --- Transmit side --- */

static void join_tx_start(struct lg_event *ev)
{
	uint8_t len;

	lg_power_activity(LG_PWR_ACT_JOIN);

	if (!hs.n) {
		lg_sched_event_done();
		return;
	}

	hs.was_tx = true;
	hs.frag_idx = 0;
	len = build_fragment(0, join_frame);
	hs.last_tx_len = len;
	if (!len || lg_phy_tx_at(ev->anchor_us, ev->chan, join_frame, len) != 0) {
		lg_sched_event_done();
	}
}

static void join_tx_done(struct lg_event *ev, uint64_t air_end_us)
{
	uint8_t len;

	/* Remember when this fragment started on air. If it turns out to be the
	 * message that completes the handshake, the link's anchor is measured
	 * from it, and the peer measures from the same frame's arrival. The
	 * length has to be the one actually sent: using the maximum PDU size
	 * here puts the two ends almost 3 ms apart, which no receive window
	 * narrow enough to be worth having would cover.
	 */
	hs.last_tx_air_start_us = air_end_us - LG_AIRTIME_US(hs.last_tx_len);

	hs.frag_idx++;
	if (hs.frag_idx < hs.frag_total) {
		len = build_fragment(hs.frag_idx, join_frame);
		hs.last_tx_len = len;
		if (len && lg_phy_tx_at(air_end_us + LG_FRAG_GAP_US, ev->chan, join_frame, len) ==
				   0) {
			return;
		}
	}
	lg_sched_event_done();
}

/* --- Receive side --- */

static void join_rx_start(struct lg_event *ev)
{
	lg_power_activity(LG_PWR_ACT_JOIN);
	hs.was_tx = false;

	if (lg_phy_rx_window(ev->anchor_us - LG_JOIN_RX_MARGIN_US, ev->close_us, ev->chan,
			     ev->rxbuf) != 0) {
		lg_sched_event_done();
	}
}

static void join_rx_done(struct lg_event *ev, const struct lg_phy_rx *rx)
{
	/* Reassembly and all the public key work happen on the work queue. */
	lg_rx_post(rx->pdu, rx->len, rx->rssi, ev->chan, rx->air_start_us);

	/* More fragments are due; keep the window open for them. */
	if (lg_ts_now_us() + LG_SYNC_WINDOW_US < ev->close_us) {
		if (lg_phy_rx_window(lg_ts_now_us() + LG_RAMPUP_US + 20, ev->close_us, ev->chan,
				     ev->rxbuf) == 0) {
			return;
		}
	}
	lg_sched_event_done();
}

static void join_rx_fail(struct lg_event *ev, enum lg_phy_rx_fail reason)
{
	if (reason != LG_PHY_RX_TIMEOUT && lg_ts_now_us() + LG_SYNC_WINDOW_US < ev->close_us) {
		if (lg_phy_rx_window(lg_ts_now_us() + LG_RAMPUP_US + 20, ev->close_us, ev->chan,
				     ev->rxbuf) == 0) {
			return;
		}
	}
	lg_sched_event_done();
}

static void join_aborted(struct lg_event *ev)
{
	ARG_UNUSED(ev);

	/*
	 * Radio time that was asked for and not granted. This is not a failure of
	 * the handshake: the slot simply produced nothing, which is the same
	 * situation as a message that was lost, and the retry logic already knows
	 * how to deal with that.
	 *
	 * Treating it as fatal - which this did - meant a node sharing the radio
	 * could never authenticate at all. One refused slot out of the dozen a
	 * handshake needs was enough to abandon the attempt, and since the node
	 * then backed off and retried, the next attempt was just as likely to lose
	 * a slot somewhere. The protocol has to assume it will not always get the
	 * radio, and this was the one place that assumed otherwise.
	 */
	hs.was_tx = false;
	hs.progressed = false;
	hs.slot_refused = true;
}

const struct lg_ev_ops lg_join_tx_ev_ops = {
	.start = join_tx_start,
	.tx_done = join_tx_done,
	.rx_done = NULL,
	.rx_fail = NULL,
	.aborted = join_aborted,
};

const struct lg_ev_ops lg_join_rx_ev_ops = {
	.start = join_rx_start,
	.tx_done = NULL,
	.rx_done = join_rx_done,
	.rx_fail = join_rx_fail,
	.aborted = join_aborted,
};

/**
 * @brief Advance to the next slot after an event finishes. Work queue only.
 */
void lg_join_event_finish(void)
{
	struct lg_neigh *n = hs.n;

	if (!n) {
		/* A join window that heard nothing. */
		lg.join_win_us = 0;
		return;
	}

	if (n->state != LG_NS_JOINING) {
		memset(&hs, 0, sizeof(hs));
		return;
	}

	/* The router finishes by transmitting the link parameters. Now that the
	 * frame is on air it can bring the link up, anchored on that frame, and
	 * the joiner anchors on the same frame's arrival.
	 *
	 * The test is on the slot that just ran having been ours to transmit in -
	 * not on owing a transmission, which is true from the moment the message
	 * is staged and would establish the link before the joiner has been told
	 * anything at all.
	 */
	if (!hs.joiner && hs.was_tx &&
	    (hs.tx_type == LG_FT_JOIN_ACK || hs.tx_type == LG_FT_RESUME_RSP)) {
		struct lg_link_params pp;

		memcpy(&pp, hs.tx, sizeof(pp));
		n->anchor_us = hs.last_tx_air_start_us + pp.anchor_offset_us;
		n->join_step = LG_JOIN_DONE;
		n->join_retries = 0;
		lg.stats.joins_ok++;
		LG_TRACE("joinok", "peer=%04x role=router resume=%u", n->addr, hs.resuming ? 1U : 0U);
		lg_link_establish(n, &pp, true);
		lg.join_win_us = 0;
		memset(&hs, 0, sizeof(hs));
		return;
	}

	/*
	 * Hand the exchange over. Having transmitted, this side is now waiting,
	 * so it listens next; if that wait produces nothing the debt comes back
	 * and the same message goes out again in a later slot. A step that
	 * consumed a message and produced the next one has already taken the debt
	 * back via stage_message().
	 */
	if (hs.was_tx) {
		hs.owe_tx = false;
	} else if (!hs.progressed) {
		/* Nothing arrived. Resend whatever we last sent, if anything. */
		hs.owe_tx = (hs.tx_len != 0);
	}

	LOG_DBG("hs fin: slot %u -> %u, was_tx %u prog %u owe %u txlen %u idle %u", hs.slot,
		MAX((uint8_t)(hs.slot + 1U), slot_next_from_clock()), hs.was_tx, hs.progressed,
		hs.owe_tx, hs.tx_len, hs.idle_slots);

	/* Only forward progress resets this. Retransmitting is not progress: a
	 * joiner talking into the join window of a router that is already busy with
	 * somebody else transmits happily for as long as it is allowed to, so
	 * counting a transmission as progress let the exchange limp to the deadline
	 * instead of giving up and retrying - 1497 of 1869 failures in a dense grid
	 * spent nearly two seconds each getting nowhere.
	 */
	if (hs.progressed) {
		hs.idle_slots = 0;
	} else if (!hs.slot_refused) {
		hs.idle_slots++;
	}
	/* A refused slot leaves the count alone: it neither proves the exchange is
	 * working nor that it is not. Counting it would abandon handshakes on a
	 * shared radio for no reason - the mistake this file has already made once,
	 * further up in join_aborted().
	 */
	hs.slot_refused = false;

	hs.slot = MAX((uint8_t)(hs.slot + 1U), slot_next_from_clock());
	hs.progressed = false;

	if (hs.idle_slots >= (hs.resuming ? LG_JOIN_IDLE_LIMIT_RESUME : LG_JOIN_IDLE_LIMIT)) {
		join_fail(n, "handshake produced nothing in too many slots");
		return;
	}
	if (hs.slot >= hs.slot_limit) {
		join_fail(n, "handshake exceeded its slot deadline");
		return;
	}

	if (hs.owe_tx && hs.tx_len == 0) {
		join_fail(n, "nothing staged for our slot");
	}
}

/* ------------------------------------------------------------------------- */
/* Frame dispatch, on the work queue                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief Feed a raw handshake PDU in. Called from the work queue dispatcher.
 *
 * @param pdu Mutable copy of the received frame; decryption happens in place.
 * @param air_start_us When the frame's first bit arrived, used for anchoring.
 */
void lg_join_rx_pdu(uint8_t *pdu, uint8_t len, int8_t rssi, uint64_t air_start_us)
{
	uint8_t type = lg_frame_type(pdu);
	lg_addr_t src = lg_get_le16(&pdu[LG_OFF_SRC]);
	struct lg_neigh *n;
	int err;

	if (type == LG_FT_JOIN_REQ || type == LG_FT_RESUME_REQ) {
		/* Router side: a joiner has arrived in our window. */
		if (hs.n && hs.n->addr != src) {
			return;
		}
		if (!hs.n) {
			n = lg_neigh_find(src);
			if (!n) {
				n = lg_neigh_alloc(src);
			}
			if (!n) {
				if (!lg_neigh_make_room(LG_ETX_ONE)) {
					return;
				}
				n = lg_neigh_alloc(src);
				if (!n) {
					return;
				}
			}
			if (lg_neigh_linked_count() >= CONFIG_LOCKGRID_MAX_LINKS) {
				return;
			}

			memset(&hs, 0, sizeof(hs));
			hs.n = n;
			hs.joiner = false;
			hs.resuming = (type == LG_FT_RESUME_REQ);
			/* Slot origin is this frame, not the advertised window, so
			 * the joiner's random start inside that window needs no
			 * further agreement.
			 */
			hs.t0_us = air_start_us;
			hs.slot = 0;
			hs.slot_limit = hs.resuming ? 24 : 48;
			n->state = LG_NS_JOINING;
		}
	}

	n = hs.n;
	if (!n || n->addr != src) {
		LOG_DBG("hs drop: type %u from 0x%04x, active peer 0x%04x", type, src,
			n ? n->addr : 0);
		return;
	}
	lg_neigh_rx_rssi(n, rssi);

	if (!reassemble(n, pdu, len)) {
		LOG_DBG("hs frag: type %u len %u seen 0x%02x", type, len, hs.rx.frags_seen);
		return;
	}
	LOG_DBG("hs msg: type %u len %u slot %u", type, hs.rx.len, hs.slot);

	switch (type) {
	case LG_FT_JOIN_REQ:
		err = handle_join_req(&n, hs.rx.buf, hs.rx.len);
		if (err) {
			join_fail(n, "join request rejected");
			return;
		}
		break;

	case LG_FT_JOIN_RSP:
		err = handle_join_rsp(n, hs.rx.buf, hs.rx.len);
		if (err) {
			join_fail(n, "join response rejected");
			return;
		}
		break;

	case LG_FT_JOIN_CFM:
		err = handle_join_cfm(n, hs.rx.buf, hs.rx.len);
		if (err) {
			join_fail(n, "confirmation signature rejected");
			return;
		}
		break;

	case LG_FT_JOIN_ACK:
	case LG_FT_RESUME_RSP: {
		struct lg_link_params pp;

		if (hs.rx.len != sizeof(pp)) {
			join_fail(n, "malformed link parameters");
			return;
		}
		memcpy(&pp, hs.rx.buf, sizeof(pp));

		/* The router owns the schedule, so this node is the responder.
		 * The anchor is measured from the frame that carried the
		 * parameters, which was timestamped on arrival.
		 */
		n->anchor_us = air_start_us + pp.anchor_offset_us;
		n->join_step = LG_JOIN_DONE;
		n->join_retries = 0;
		lg.stats.joins_ok++;
		LG_TRACE("joinok", "peer=%04x role=joiner resume=%u", n->addr, hs.resuming ? 1U : 0U);
		lg_link_establish(n, &pp, false);
		memset(&hs, 0, sizeof(hs));
		return;
	}

	case LG_FT_RESUME_REQ:
		err = handle_resume_req(n, hs.rx.buf, hs.rx.len);
		if (err) {
			/* The long term key did not work; fall back to a full
			 * handshake next time this neighbour is tried.
			 */
			n->has_ltk = false;
			join_fail(n, "resumption rejected");
			return;
		}
		break;

	default:
		return;
	}

	/* Whatever this step produced is now staged for our next slot. The
	 * router's final message brings the link up from lg_join_event_finish()
	 * once it is actually on air.
	 */
	/* The peer replied, so whatever we owed has been delivered; the debt is
	 * now ours again only because this step staged the next message.
	 */
	hs.progressed = true;
	memset(&hs.rx, 0, sizeof(hs.rx));
}
