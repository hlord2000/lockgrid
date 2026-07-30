/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_provision_sim.h
 * @brief Development-only provisioning from a compiled-in certificate authority.
 *
 * A real deployment provisions each node with a certificate signed offline by a
 * CA whose private key the node never sees. That is not convenient for
 * simulation or bring-up, so this helper compiles the CA private key into the
 * image and signs a certificate for the node at boot.
 *
 * The point is that everything downstream is the real thing: the certificate has
 * the production layout, it is verified against the CA public key by the same
 * code path a field node uses, and the handshake performs genuine ECDSA and
 * ECDHE. Only the moment of issuance is faked.
 *
 * Enabled by CONFIG_LOCKGRID_SIM_PROVISIONING, which defaults on for the
 * simulated board and must never be set on a product.
 */

#ifndef LOCKGRID_INCLUDE_LG_PROVISION_SIM_H_
#define LOCKGRID_INCLUDE_LG_PROVISION_SIM_H_

#include <lockgrid/lockgrid.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate a key pair, issue a certificate for it and provision it.
 *
 * Calls lg_provision() on success, so the node is ready for lg_start().
 *
 * @param network_id Grid to join. The grid key is derived from it, so every node
 *                   given the same value ends up with the same key.
 * @param addr       Short address to put in the certificate. Must be unique
 *                   within the grid.
 * @param max_role   Highest role the certificate will permit.
 *
 * @retval 0 Provisioned.
 * @retval -EIO A cryptographic operation failed.
 */
int lg_provision_sim(uint32_t network_id, lg_addr_t addr, enum lg_role max_role);

#ifdef __cplusplus
}
#endif

#endif /* LOCKGRID_INCLUDE_LG_PROVISION_SIM_H_ */
