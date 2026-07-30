/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_sec.c
 * @brief Frame protection, key derivation and certificate verification.
 *
 * Two key layers:
 *
 *  - The grid key is provisioned and shared by every node. It protects beacons
 *    and lets a node discard an outsider's traffic cheaply. It authenticates
 *    nothing: holding it does not let anyone form a link.
 *
 *  - A pairwise link key is established per neighbour by an ECDHE exchange in
 *    which both ends prove possession of a CA-signed certificate. Every data
 *    frame is AES-CCM encrypted and authenticated under that key, with a
 *    monotonic frame counter and a sliding replay window. A frame that does not
 *    authenticate is dropped before the MAC sees it, so two nodes that are
 *    exchanging traffic have necessarily authenticated each other.
 *
 * Everything here runs on the LockGrid work queue, never in the radio interrupt:
 * frames a link is about to send are encrypted when they are staged, and
 * received frames are decrypted after the event. The link-layer acknowledgement
 * inside an event is therefore based on CRC alone, the same tradeoff Bluetooth
 * makes, and a frame that later fails its tag is counted as an error rather than
 * silently accepted.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <psa/crypto.h>

#include "lg_internal.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* AES-CCM with a 13 byte nonce, the largest CCM allows, leaving room for the
 * source address, a direction byte and the frame counter.
 */
#define LG_CCM_NONCE_LEN 13

#define LG_CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, CONFIG_LOCKGRID_MIC_LEN)

/** Nonce format version, so the layout can change without ambiguity. */
#define LG_NONCE_FORMAT_V1 1

/* Uncompressed point encoding: 0x04 followed by X and Y. */
#define LG_EC_POINT_LEN (1 + LG_EC_PUB_LEN)

static psa_key_id_t net_key_id;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static int import_aes_key(const uint8_t *key, psa_key_usage_t usage, psa_key_id_t *out)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t st;

	psa_set_key_usage_flags(&attr, usage);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, LG_CCM_ALG);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, LG_KEY_LEN * 8);

	st = psa_import_key(&attr, key, LG_KEY_LEN, out);
	psa_reset_key_attributes(&attr);

	return st == PSA_SUCCESS ? 0 : -EIO;
}

/**
 * Nonce layout: source address, a format byte, the frame counter, zero padding.
 *
 * The source address is what separates the two directions, so each end has its
 * own nonce space and its own counter. Deliberately not derived from the link
 * role: roles are only assigned when the link is established, and the last two
 * handshake messages are already encrypted by then, so a role-dependent nonce
 * would have the two ends computing different values for exactly those frames.
 */
static void nonce_build(uint8_t *nonce, lg_addr_t src, uint32_t ctr)
{
	memset(nonce, 0, LG_CCM_NONCE_LEN);
	lg_put_le16(&nonce[0], src);
	nonce[2] = LG_NONCE_FORMAT_V1;
	lg_put_le32(&nonce[3], ctr);
}

/* ------------------------------------------------------------------------- */
/* Init and randomness                                                        */
/* ------------------------------------------------------------------------- */

int lg_sec_init(void)
{
	psa_status_t st = psa_crypto_init();

	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed: %d", (int)st);
		return -EIO;
	}

	if (net_key_id) {
		psa_destroy_key(net_key_id);
		net_key_id = 0;
	}

	return import_aes_key(lg.net_key, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT,
			      &net_key_id);
}

int lg_sec_rand(void *out, size_t len)
{
	return psa_generate_random(out, len) == PSA_SUCCESS ? 0 : -EIO;
}

/* ------------------------------------------------------------------------- */
/* Frame protection                                                           */
/* ------------------------------------------------------------------------- */

int lg_sec_encrypt(struct lg_neigh *n, uint8_t *pdu, uint8_t hdr_len, uint8_t *len)
{
	uint8_t nonce[LG_CCM_NONCE_LEN];
	uint8_t scratch[LG_PDU_MAX];
	size_t out_len;
	psa_status_t st;
	uint8_t body_len;

	if (!n->has_key) {
		return -ENOKEY;
	}
	if (*len < hdr_len) {
		return -EINVAL;
	}
	body_len = *len - hdr_len;

	/* Counter goes in the clear so the peer can reconstruct the nonce, and
	 * it is authenticated as part of the header.
	 */
	n->tx_ctr++;
	lg_put_le32(&pdu[LG_OFF_FCTR], n->tx_ctr);
	nonce_build(nonce, lg.addr, n->tx_ctr);

	if ((size_t)hdr_len + body_len + CONFIG_LOCKGRID_MIC_LEN > LG_PDU_MAX) {
		return -EMSGSIZE;
	}

	st = psa_aead_encrypt(n->key_id, LG_CCM_ALG, nonce, sizeof(nonce), pdu, hdr_len,
			      &pdu[hdr_len], body_len, scratch, sizeof(scratch), &out_len);
	if (st != PSA_SUCCESS) {
		LOG_ERR("aead encrypt failed: %d", (int)st);
		return -EIO;
	}

	memcpy(&pdu[hdr_len], scratch, out_len);
	*len = hdr_len + (uint8_t)out_len;
	return 0;
}

/**
 * Sliding-window replay filter. Accepts a counter ahead of the highest seen, or
 * one inside the window that has not been seen before.
 */
static bool replay_check(struct lg_neigh *n, uint32_t ctr)
{
	if (ctr == 0) {
		return false;
	}

	if (ctr > n->rx_ctr) {
		uint32_t shift = ctr - n->rx_ctr;

		if (shift >= 32) {
			n->replay_mask = 0;
		} else {
			n->replay_mask <<= shift;
		}
		n->replay_mask |= 1U;
		n->rx_ctr = ctr;
		return true;
	}

	uint32_t behind = n->rx_ctr - ctr;

	if (behind >= MIN(CONFIG_LOCKGRID_REPLAY_WINDOW, 32)) {
		return false;
	}
	if (n->replay_mask & BIT(behind)) {
		return false;
	}
	n->replay_mask |= BIT(behind);
	return true;
}

int lg_sec_decrypt(struct lg_neigh *n, uint8_t *pdu, uint8_t *len)
{
	uint8_t nonce[LG_CCM_NONCE_LEN];
	uint8_t scratch[LG_PDU_MAX];
	size_t out_len;
	psa_status_t st;
	uint32_t ctr;
	uint8_t body_len;

	if (!n->has_key) {
		return -ENOKEY;
	}
	if (*len < LG_SEC_OVERHEAD) {
		return -EINVAL;
	}

	ctr = lg_get_le32(&pdu[LG_OFF_FCTR]);
	body_len = *len - LG_HDR_LEN_SEC;

	nonce_build(nonce, lg_get_le16(&pdu[LG_OFF_SRC]), ctr);

	st = psa_aead_decrypt(n->key_id, LG_CCM_ALG, nonce, sizeof(nonce), pdu, LG_HDR_LEN_SEC,
			      &pdu[LG_HDR_LEN_SEC], body_len, scratch, sizeof(scratch), &out_len);
	if (st != PSA_SUCCESS) {
		lg.stats.auth_failures++;
		return -EBADMSG;
	}

	/* Replay is only checked after the tag verifies, so a forged counter
	 * cannot be used to poison the window.
	 */
	if (!replay_check(n, ctr)) {
		lg.stats.replays_dropped++;
		return -EALREADY;
	}

	memcpy(&pdu[LG_HDR_LEN_SEC], scratch, out_len);
	*len = LG_HDR_LEN_SEC + (uint8_t)out_len;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Beacon protection                                                          */
/* ------------------------------------------------------------------------- */

/*
 * A beacon is authenticated with the grid key but not encrypted: a joiner has to
 * be able to read one to know a grid is there. The MIC is computed over the
 * whole beacon with a zero-length plaintext, which turns AES-CCM into a MAC and
 * avoids pulling in a second primitive.
 *
 * A joiner cannot check this MIC, so it will act on an unverified beacon. That
 * is bounded: the worst an attacker achieves is making a joiner spend one
 * handshake attempt, and lg_join.c rate-limits attempts per source. Nodes that
 * already hold the grid key do verify it, so beacon spoofing does not work
 * against an established grid.
 */

static int beacon_mac(const uint8_t *data, size_t len, uint8_t *mac_out)
{
	uint8_t nonce[LG_CCM_NONCE_LEN] = {0};
	uint8_t out[16];
	size_t out_len;
	psa_status_t st;

	/* Nonce is fixed; the beacon body carries its own epoch and the MAC is
	 * only a filter, not a replay defence.
	 */
	nonce[0] = 'B';

	st = psa_aead_encrypt(net_key_id,
			      PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, LG_BEACON_MIC_LEN),
			      nonce, sizeof(nonce), data, len, NULL, 0, out, sizeof(out),
			      &out_len);
	if (st != PSA_SUCCESS || out_len < LG_BEACON_MIC_LEN) {
		return -EIO;
	}

	memcpy(mac_out, out, LG_BEACON_MIC_LEN);
	return 0;
}

int lg_sec_beacon_seal(uint8_t *pdu, uint8_t *len)
{
	int err;

	if ((size_t)*len + LG_BEACON_MIC_LEN > LG_PDU_MAX) {
		return -EMSGSIZE;
	}

	err = beacon_mac(pdu, *len, &pdu[*len]);
	if (err) {
		return err;
	}

	*len += LG_BEACON_MIC_LEN;
	return 0;
}

int lg_sec_beacon_open(uint8_t *pdu, uint8_t *len)
{
	uint8_t expect[LG_BEACON_MIC_LEN];
	uint8_t body_len;
	int err;

	if (*len <= LG_BEACON_MIC_LEN) {
		return -EINVAL;
	}
	body_len = *len - LG_BEACON_MIC_LEN;

	err = beacon_mac(pdu, body_len, expect);
	if (err) {
		return err;
	}

	if (memcmp(expect, &pdu[body_len], LG_BEACON_MIC_LEN) != 0) {
		return -EBADMSG;
	}

	*len = body_len;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Hashing and key derivation                                                 */
/* ------------------------------------------------------------------------- */

int lg_sec_hash(const uint8_t *const *parts, const size_t *lens, size_t n, uint8_t *out)
{
	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
	size_t out_len;
	psa_status_t st;

	st = psa_hash_setup(&op, PSA_ALG_SHA_256);
	if (st != PSA_SUCCESS) {
		return -EIO;
	}

	for (size_t i = 0; i < n; i++) {
		st = psa_hash_update(&op, parts[i], lens[i]);
		if (st != PSA_SUCCESS) {
			psa_hash_abort(&op);
			return -EIO;
		}
	}

	st = psa_hash_finish(&op, out, LG_HASH_LEN, &out_len);
	return st == PSA_SUCCESS ? 0 : -EIO;
}

int lg_sec_kdf(const uint8_t *secret, size_t secret_len, const uint8_t *salt, size_t salt_len,
	       const char *info, uint8_t *key_out)
{
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	psa_status_t st;

	st = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
	if (st != PSA_SUCCESS) {
		return -EIO;
	}

	if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len) !=
		    PSA_SUCCESS ||
	    psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, secret,
					   secret_len) != PSA_SUCCESS ||
	    psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO,
					   (const uint8_t *)info, strlen(info)) != PSA_SUCCESS ||
	    psa_key_derivation_output_bytes(&op, key_out, LG_KEY_LEN) != PSA_SUCCESS) {
		psa_key_derivation_abort(&op);
		return -EIO;
	}

	psa_key_derivation_abort(&op);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Elliptic curve operations                                                  */
/* ------------------------------------------------------------------------- */

int lg_sec_ecdh_keypair(uint8_t *pub, uint8_t *priv)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	uint8_t point[LG_EC_POINT_LEN];
	size_t olen;
	psa_status_t st;
	int ret = -EIO;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	st = psa_generate_key(&attr, &id);
	if (st != PSA_SUCCESS) {
		goto out;
	}

	st = psa_export_public_key(id, point, sizeof(point), &olen);
	if (st != PSA_SUCCESS || olen != LG_EC_POINT_LEN) {
		goto out;
	}
	/* Strip the 0x04 uncompressed-point tag; LockGrid carries X and Y only. */
	memcpy(pub, &point[1], LG_EC_PUB_LEN);

	st = psa_export_key(id, priv, LG_EC_PRIV_LEN, &olen);
	if (st != PSA_SUCCESS || olen != LG_EC_PRIV_LEN) {
		goto out;
	}

	ret = 0;
out:
	psa_reset_key_attributes(&attr);
	if (id) {
		psa_destroy_key(id);
	}
	return ret;
}

int lg_sec_ecdh(const uint8_t *priv, const uint8_t *peer_pub, uint8_t *secret)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	uint8_t point[LG_EC_POINT_LEN];
	size_t olen;
	psa_status_t st;
	int ret = -EIO;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	st = psa_import_key(&attr, priv, LG_EC_PRIV_LEN, &id);
	if (st != PSA_SUCCESS) {
		goto out;
	}

	point[0] = 0x04;
	memcpy(&point[1], peer_pub, LG_EC_PUB_LEN);

	st = psa_raw_key_agreement(PSA_ALG_ECDH, id, point, sizeof(point), secret,
				   LG_EC_PRIV_LEN, &olen);
	if (st != PSA_SUCCESS || olen != LG_EC_PRIV_LEN) {
		goto out;
	}

	ret = 0;
out:
	psa_reset_key_attributes(&attr);
	if (id) {
		psa_destroy_key(id);
	}
	return ret;
}

/** Import a raw P-256 private key for signing. */
static int import_sign_key(const uint8_t *priv, size_t len, psa_key_id_t *out)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t st;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	st = psa_import_key(&attr, priv, len, out);
	psa_reset_key_attributes(&attr);

	return st == PSA_SUCCESS ? 0 : -EIO;
}

int lg_sec_sign(const uint8_t *hash, uint8_t *sig)
{
	psa_key_id_t id = 0;
	size_t olen;
	int err;

	if (!lg.priv_key || lg.priv_key_len != LG_EC_PRIV_LEN) {
		return -ENOKEY;
	}

	err = import_sign_key(lg.priv_key, lg.priv_key_len, &id);
	if (err) {
		return err;
	}

	err = psa_sign_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, LG_HASH_LEN, sig,
			    LG_SIG_LEN, &olen) == PSA_SUCCESS
		      ? 0
		      : -EIO;

	psa_destroy_key(id);
	return (err == 0 && olen == LG_SIG_LEN) ? 0 : -EIO;
}

int lg_sec_verify(const uint8_t *pub, const uint8_t *hash, const uint8_t *sig)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	uint8_t point[LG_EC_POINT_LEN];
	psa_status_t st;
	int ret = -EBADMSG;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	point[0] = 0x04;
	memcpy(&point[1], pub, LG_EC_PUB_LEN);

	st = psa_import_key(&attr, point, sizeof(point), &id);
	if (st != PSA_SUCCESS) {
		ret = -EIO;
		goto out;
	}

	st = psa_verify_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, LG_HASH_LEN, sig,
			     LG_SIG_LEN);
	if (st == PSA_SUCCESS) {
		ret = 0;
	}

out:
	psa_reset_key_attributes(&attr);
	if (id) {
		psa_destroy_key(id);
	}
	return ret;
}

/* ------------------------------------------------------------------------- */
/* Certificates                                                               */
/* ------------------------------------------------------------------------- */

int lg_sec_cert_verify(const uint8_t *cert, size_t len, lg_addr_t *addr, uint8_t *role,
		       uint8_t *pub_out)
{
	uint8_t hash[LG_HASH_LEN];
	const uint8_t *parts[1] = {cert};
	const size_t lens[1] = {LG_CERT_BODY_LEN};
	int err;

	if (len != LG_CERT_LEN) {
		return -EINVAL;
	}
	if (!lg.ca_pub || lg.ca_pub_len != LG_EC_PUB_LEN) {
		return -ENOKEY;
	}

	err = lg_sec_hash(parts, lens, 1, hash);
	if (err) {
		return err;
	}

	err = lg_sec_verify(lg.ca_pub, hash, &cert[LG_CERT_OFF_SIG]);
	if (err) {
		LOG_WRN("certificate signature does not verify");
		return -EBADMSG;
	}

	*addr = lg_get_le16(&cert[LG_CERT_OFF_ADDR]);
	*role = cert[LG_CERT_OFF_ROLE];
	memcpy(pub_out, &cert[LG_CERT_OFF_PUB], LG_EC_PUB_LEN);

	if (*addr == LG_ADDR_INVALID || *addr == LG_ADDR_BROADCAST || *addr == LG_ADDR_BACKHAUL) {
		return -EINVAL;
	}

	return 0;
}

/* ------------------------------------------------------------------------- */
/* Link key installation                                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief Import a neighbour's freshly derived link key into PSA.
 *
 * Declared in lg_internal.h via lg_sec_install_key(); kept here so the key
 * material only ever passes through this file.
 */
int lg_sec_install_key(struct lg_neigh *n, const uint8_t *key)
{
	int err;

	if (n->key_id) {
		psa_destroy_key((psa_key_id_t)n->key_id);
		n->key_id = 0;
	}

	memcpy(n->link_key, key, LG_KEY_LEN);
	err = import_aes_key(key, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT,
			     (psa_key_id_t *)&n->key_id);
	if (err) {
		n->has_key = false;
		return err;
	}

	n->has_key = true;
	n->tx_ctr = 0;
	n->rx_ctr = 0;
	n->replay_mask = 0;
	return 0;
}

void lg_sec_forget_key(struct lg_neigh *n)
{
	if (n->key_id) {
		psa_destroy_key((psa_key_id_t)n->key_id);
		n->key_id = 0;
	}
	n->has_key = false;
	memset(n->link_key, 0, sizeof(n->link_key));
}
