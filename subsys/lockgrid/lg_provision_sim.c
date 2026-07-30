/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_provision_sim.c
 * @brief Development-only certificate issuance. See lg_provision_sim.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <psa/crypto.h>

#include <lockgrid/lg_provision_sim.h>
#include "lg_internal.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/*
 * Test certificate authority private scalar. A fixed value well below the P-256
 * group order, so it is a valid private key. Compiled in on purpose: this file
 * exists to remove the provisioning step during development, and it is gated on
 * CONFIG_LOCKGRID_SIM_PROVISIONING for exactly that reason.
 */
static const uint8_t ca_priv[LG_EC_PRIV_LEN] = {
	0x4C, 0x6F, 0x63, 0x6B, 0x47, 0x72, 0x69, 0x64, 0x54, 0x65, 0x73, 0x74,
	0x43, 0x41, 0x4B, 0x65, 0x79, 0x44, 0x6F, 0x4E, 0x6F, 0x74, 0x53, 0x68,
	0x69, 0x70, 0x54, 0x68, 0x69, 0x73, 0x30, 0x31,
};

/* Storage the provisioned identity points at, so it outlives this call. */
static uint8_t node_cert[LG_CERT_LEN];
static uint8_t node_priv[LG_EC_PRIV_LEN];
static uint8_t ca_pub[LG_EC_PUB_LEN];
static uint8_t net_key[LG_KEY_LEN];

#define LG_EC_POINT_LEN (1 + LG_EC_PUB_LEN)

/** Import a raw private scalar and read back the matching public point. */
static int derive_ca_public(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	uint8_t point[LG_EC_POINT_LEN];
	size_t olen;
	int ret = -EIO;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH |
					       PSA_KEY_USAGE_EXPORT);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	if (psa_import_key(&attr, ca_priv, sizeof(ca_priv), &id) != PSA_SUCCESS) {
		goto out;
	}
	if (psa_export_public_key(id, point, sizeof(point), &olen) != PSA_SUCCESS ||
	    olen != LG_EC_POINT_LEN) {
		goto out;
	}

	memcpy(ca_pub, &point[1], LG_EC_PUB_LEN);
	ret = 0;
out:
	psa_reset_key_attributes(&attr);
	if (id) {
		psa_destroy_key(id);
	}
	return ret;
}

/** Generate this node's key pair, returning the public point and the scalar. */
static int generate_node_key(uint8_t *pub)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	uint8_t point[LG_EC_POINT_LEN];
	size_t olen;
	int ret = -EIO;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	if (psa_generate_key(&attr, &id) != PSA_SUCCESS) {
		goto out;
	}
	if (psa_export_public_key(id, point, sizeof(point), &olen) != PSA_SUCCESS ||
	    olen != LG_EC_POINT_LEN) {
		goto out;
	}
	if (psa_export_key(id, node_priv, sizeof(node_priv), &olen) != PSA_SUCCESS ||
	    olen != LG_EC_PRIV_LEN) {
		goto out;
	}

	memcpy(pub, &point[1], LG_EC_PUB_LEN);
	ret = 0;
out:
	psa_reset_key_attributes(&attr);
	if (id) {
		psa_destroy_key(id);
	}
	return ret;
}

/** Hash the certificate body and sign it with the CA key. */
static int sign_cert(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	psa_hash_operation_t hop = PSA_HASH_OPERATION_INIT;
	uint8_t hash[LG_HASH_LEN];
	size_t olen;
	int ret = -EIO;

	if (psa_hash_setup(&hop, PSA_ALG_SHA_256) != PSA_SUCCESS ||
	    psa_hash_update(&hop, node_cert, LG_CERT_BODY_LEN) != PSA_SUCCESS ||
	    psa_hash_finish(&hop, hash, sizeof(hash), &olen) != PSA_SUCCESS) {
		psa_hash_abort(&hop);
		return -EIO;
	}

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	if (psa_import_key(&attr, ca_priv, sizeof(ca_priv), &id) != PSA_SUCCESS) {
		goto out;
	}
	if (psa_sign_hash(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash),
			  &node_cert[LG_CERT_OFF_SIG], LG_SIG_LEN, &olen) != PSA_SUCCESS ||
	    olen != LG_SIG_LEN) {
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

/**
 * Derive the grid key from the network id.
 *
 * A product provisions this as a secret. Deriving it means every simulated node
 * given the same network id agrees without a distribution step, which is what
 * makes a multi-node simulation possible from a single build.
 */
static int derive_net_key(uint32_t network_id)
{
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	static const uint8_t salt[] = "LockGrid simulated grid key";
	uint8_t secret[4];
	int ret = -EIO;

	secret[0] = (uint8_t)network_id;
	secret[1] = (uint8_t)(network_id >> 8);
	secret[2] = (uint8_t)(network_id >> 16);
	secret[3] = (uint8_t)(network_id >> 24);

	if (psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) != PSA_SUCCESS) {
		return -EIO;
	}
	if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt,
					   sizeof(salt) - 1) != PSA_SUCCESS ||
	    psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, secret,
					   sizeof(secret)) != PSA_SUCCESS ||
	    psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO,
					   (const uint8_t *)"netkey", 6) != PSA_SUCCESS ||
	    psa_key_derivation_output_bytes(&op, net_key, sizeof(net_key)) != PSA_SUCCESS) {
		goto out;
	}
	ret = 0;
out:
	psa_key_derivation_abort(&op);
	return ret;
}

int lg_provision_sim(uint32_t network_id, lg_addr_t addr, enum lg_role max_role)
{
	struct lg_identity id;
	uint8_t node_pub[LG_EC_PUB_LEN];
	int err;

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	err = derive_ca_public();
	if (err) {
		LOG_ERR("could not derive the test CA public key");
		return err;
	}

	err = generate_node_key(node_pub);
	if (err) {
		LOG_ERR("could not generate a node key pair");
		return err;
	}

	err = derive_net_key(network_id);
	if (err) {
		LOG_ERR("could not derive the grid key");
		return err;
	}

	memset(node_cert, 0, sizeof(node_cert));
	lg_put_le16(&node_cert[LG_CERT_OFF_ADDR], addr);
	node_cert[LG_CERT_OFF_ROLE] = (uint8_t)max_role;
	node_cert[LG_CERT_OFF_FLAGS] = 0;
	lg_put_le32(&node_cert[LG_CERT_OFF_NOTBEF], 0);
	lg_put_le32(&node_cert[LG_CERT_OFF_NOTAFT], UINT32_MAX);
	memcpy(&node_cert[LG_CERT_OFF_PUB], node_pub, LG_EC_PUB_LEN);

	err = sign_cert();
	if (err) {
		LOG_ERR("could not sign the node certificate");
		return err;
	}

	id.network_id = network_id;
	id.cert = node_cert;
	id.cert_len = sizeof(node_cert);
	id.priv_key = node_priv;
	id.priv_key_len = sizeof(node_priv);
	id.ca_pub_key = ca_pub;
	id.ca_pub_key_len = sizeof(ca_pub);
	id.net_key = net_key;
	id.net_key_len = sizeof(net_key);

	return lg_provision(&id);
}
