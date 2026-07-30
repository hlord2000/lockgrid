/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include "lg_internal.h"

uint8_t lg_frame_hdr_put(uint8_t *pdu, uint16_t netid, uint8_t fcf, uint8_t seq, lg_addr_t dst,
			 lg_addr_t src)
{
	lg_put_le16(&pdu[LG_OFF_NETID], netid);
	pdu[LG_OFF_FCF] = fcf;
	pdu[LG_OFF_SEQ] = seq;
	lg_put_le16(&pdu[LG_OFF_DST], dst);
	lg_put_le16(&pdu[LG_OFF_SRC], src);

	return (fcf & LG_FCF_SEC) ? LG_HDR_LEN_SEC : LG_HDR_LEN;
}

bool lg_frame_hdr_get(const uint8_t *pdu, uint8_t len, uint16_t netid, uint8_t *fcf, uint8_t *seq,
		      lg_addr_t *dst, lg_addr_t *src)
{
	uint8_t hdr_len;

	if (len < LG_HDR_LEN) {
		return false;
	}
	if (lg_get_le16(&pdu[LG_OFF_NETID]) != netid) {
		return false;
	}

	*fcf = pdu[LG_OFF_FCF];
	*seq = pdu[LG_OFF_SEQ];
	*dst = lg_get_le16(&pdu[LG_OFF_DST]);
	*src = lg_get_le16(&pdu[LG_OFF_SRC]);

	hdr_len = (*fcf & LG_FCF_SEC) ? LG_SEC_OVERHEAD : LG_HDR_LEN;
	return len >= hdr_len;
}

uint32_t lg_frame_event_budget_us(uint32_t rx_window_us)
{
	/* A minimal event is one frame each way with an inter-frame space in
	 * between, plus the ramp-up paid at the start and the receive window the
	 * peer's clock drift forces on us. Frames are sized at the largest PDU
	 * so a reservation never has to grow mid-event.
	 */
	uint32_t frame = LG_AIRTIME_US(LG_PDU_MAX);
	uint32_t budget = LG_RAMPUP_US + frame + LG_IFS_US + rx_window_us + frame;

	return MIN(budget, lg_ts_max_grant_us());
}
