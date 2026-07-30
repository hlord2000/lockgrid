/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_phy_nrf.c
 * @brief RADIO programming for the LockGrid 802.15.4 PHY. See lg_phy.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <string.h>
#include <hal/nrf_radio.h>

#include "lg_internal.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* Time between the START task and the first bit leaving the antenna. */
#define LG_TX_CHAIN_US 1
/* Time between the last bit arriving and the END event firing. */
#define LG_RX_CHAIN_US 22

#define LG_CRC_POLY_154 0x011021UL
#define LG_CRC_LEN_154  2

/* Standard start-of-frame delimiter, kept so a sniffer and the energy detect
 * block behave normally.
 */
#define LG_SFD_154 0xA7

/* Offset of the PDU inside the radio frame buffer: the length byte comes first. */
#define LG_PDU_OFF 1

/* Bytes the early filter needs to see: netid, fcf, seq, dst. */
#define LG_FILTER_BYTES 6

/* ED_RSSIOFFS from the RADIO chapter: PRF[dBm] = -93 + EDSAMPLE. */
#define LG_ED_RSSI_OFFSET (-93)

/* One energy detect loop covers 128 us of receive. */
#define LG_ED_LOOP_US 128

/*
 * TXPOWER on nRF54L is an encoded register value, not a number of dBm: writing 0
 * selects the minimum output power, not 0 dBm. This maps the configured dBm onto
 * the encoding, picking the closest supported level at or below the request so
 * a regulatory limit is never exceeded by rounding.
 */
static const struct {
	int8_t dbm;
	uint16_t reg;
} tx_power_table[] = {
	{8, RADIO_TXPOWER_TXPOWER_Pos8dBm},   {7, RADIO_TXPOWER_TXPOWER_Pos7dBm},
	{6, RADIO_TXPOWER_TXPOWER_Pos6dBm},   {5, RADIO_TXPOWER_TXPOWER_Pos5dBm},
	{4, RADIO_TXPOWER_TXPOWER_Pos4dBm},   {3, RADIO_TXPOWER_TXPOWER_Pos3dBm},
	{2, RADIO_TXPOWER_TXPOWER_Pos2dBm},   {1, RADIO_TXPOWER_TXPOWER_Pos1dBm},
	{0, RADIO_TXPOWER_TXPOWER_0dBm},      {-1, RADIO_TXPOWER_TXPOWER_Neg1dBm},
	{-2, RADIO_TXPOWER_TXPOWER_Neg2dBm},  {-3, RADIO_TXPOWER_TXPOWER_Neg3dBm},
	{-4, RADIO_TXPOWER_TXPOWER_Neg4dBm},  {-5, RADIO_TXPOWER_TXPOWER_Neg5dBm},
	{-6, RADIO_TXPOWER_TXPOWER_Neg6dBm},  {-7, RADIO_TXPOWER_TXPOWER_Neg7dBm},
	{-8, RADIO_TXPOWER_TXPOWER_Neg8dBm},  {-9, RADIO_TXPOWER_TXPOWER_Neg9dBm},
	{-10, RADIO_TXPOWER_TXPOWER_Neg10dBm},{-12, RADIO_TXPOWER_TXPOWER_Neg12dBm},
	{-14, RADIO_TXPOWER_TXPOWER_Neg14dBm},{-16, RADIO_TXPOWER_TXPOWER_Neg16dBm},
	{-18, RADIO_TXPOWER_TXPOWER_Neg18dBm},{-20, RADIO_TXPOWER_TXPOWER_Neg20dBm},
	{-28, RADIO_TXPOWER_TXPOWER_Neg28dBm},{-40, RADIO_TXPOWER_TXPOWER_Neg40dBm},
	{-46, RADIO_TXPOWER_TXPOWER_Neg46dBm},
};

static uint16_t tx_power_reg(int dbm)
{
	for (size_t i = 0; i < ARRAY_SIZE(tx_power_table); i++) {
		if (tx_power_table[i].dbm <= dbm) {
			return tx_power_table[i].reg;
		}
	}
	return tx_power_table[ARRAY_SIZE(tx_power_table) - 1].reg;
}

enum lg_phy_op {
	LG_OP_IDLE,
	LG_OP_TX_ARMED,
	LG_OP_TX,
	LG_OP_RX_ARMED,
	LG_OP_RX,
};

static struct {
	const struct lg_phy_cb *cb;
	uint16_t netid16;
	lg_addr_t local;
	bool promiscuous;

	enum lg_phy_op op;
	uint8_t chan;

	/* Radio scratch buffer: length byte, PDU, then room for the CRC the
	 * radio appends.
	 */
	uint8_t frame[LG_PDU_OFF + LG_PDU_MAX + LG_CRC_LEN_154] __aligned(4);

	uint8_t *rx_dst;
	uint64_t rx_close_us;

	uint64_t tx_air_end_us;
	uint8_t tx_len;

	/* When the receiver became active, so its on-time can be charged. */
	uint64_t rx_on_us;
	bool charged;
	/* Whether the radio has already been brought up during this event. */
	bool in_event;
} p;

/* ------------------------------------------------------------------------- */
/* Energy accounting                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Charges are computed from the schedule rather than sampled around interrupts.
 * Ramp-up is a fixed cost per radio start, a transmission costs exactly its air
 * time, and a receive costs from the moment the receiver came up to the moment
 * it was switched off. Sampling the clock at interrupt boundaries would fold
 * ramp-up and receive time together, which is precisely the distinction this
 * accounting exists to make.
 */

/**
 * Charge the fixed cost of this radio activation.
 *
 * The first activation in an event pays for bringing the radio up; every one
 * after it is a turnaround, which is cheaper but not free. lg_phy_stop() ends
 * the event, so the next activation starts a fresh one.
 */
static void charge_fixed(void)
{
	if (p.in_event) {
		lg_power_turnaround();
	} else {
		p.in_event = true;
		lg_power_event_start();
	}
}

static void charge_tx(void)
{
	lg_power_charge(LG_PHY_STATE_TX, LG_AIRTIME_US(p.tx_len));
}

static void charge_rx_until(uint64_t end_us)
{
	if (p.charged) {
		return;
	}
	p.charged = true;
	if (end_us > p.rx_on_us) {
		lg_power_charge(LG_PHY_STATE_RX, (uint32_t)(end_us - p.rx_on_us));
	}
}

/* ------------------------------------------------------------------------- */
/* Register programming                                                       */
/* ------------------------------------------------------------------------- */

static void radio_configure(uint8_t chan)
{
	static const nrf_radio_packet_conf_t pc = {
		.lflen = 8,
		.s0len = 0,
		.s1len = 0,
		.s1incl = false,
		.cilen = 0,
		.plen = NRF_RADIO_PREAMBLE_LENGTH_32BIT_ZERO,
		.crcinc = true,
		.termlen = 0,
		.maxlen = LG_PDU_MAX + LG_CRC_LEN_154,
		.statlen = 0,
		.balen = 0,
		.big_endian = false,
		.whiteen = false,
	};

	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_IEEE802154_250KBIT);
	nrf_radio_packet_configure(NRF_RADIO, &pc);
	nrf_radio_sfd_set(NRF_RADIO, LG_SFD_154);
	nrf_radio_crc_configure(NRF_RADIO, LG_CRC_LEN_154, NRF_RADIO_CRC_ADDR_IEEE802154,
				LG_CRC_POLY_154);
	nrf_radio_crcinit_set(NRF_RADIO, 0);

	nrf_radio_frequency_set(NRF_RADIO, lg_phy_chan_freq(chan));
	nrf_radio_txpower_set(NRF_RADIO,
			      (nrf_radio_txpower_t)tx_power_reg(CONFIG_LOCKGRID_TX_POWER_DBM));
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);

	p.chan = chan;
}

static void radio_disable(void)
{
	nrf_radio_shorts_set(NRF_RADIO, 0);
	nrf_radio_int_disable(NRF_RADIO, ~0U);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
}

/* ------------------------------------------------------------------------- */
/* Transmit                                                                   */
/* ------------------------------------------------------------------------- */

static void tx_fire(void)
{
	p.op = LG_OP_TX;
	charge_fixed();

	nrf_radio_packetptr_set(NRF_RADIO, p.frame);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK);
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
}

int lg_phy_tx_at(uint64_t air_start_us, uint8_t chan, const uint8_t *pdu, uint8_t len)
{
	uint64_t fire_us;

	if (len == 0 || len > LG_PDU_MAX) {
		return -EMSGSIZE;
	}

	radio_configure(chan);

	/* The length field counts the CRC because CRCINC is set. */
	p.frame[0] = len + LG_CRC_LEN_154;
	memcpy(&p.frame[LG_PDU_OFF], pdu, len);

	p.tx_len = len;
	p.tx_air_end_us = air_start_us + LG_AIRTIME_US(len);

	fire_us = air_start_us - LG_RAMPUP_US - LG_TX_CHAIN_US;
	if (fire_us <= lg_ts_now_us()) {
		/* Too late to hit the requested slot. Refusing is better than
		 * transmitting at the wrong time and desynchronising the peer.
		 */
		return -ETIME;
	}

	p.op = LG_OP_TX_ARMED;
	lg_ts_timer_at(fire_us, tx_fire);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Receive                                                                    */
/* ------------------------------------------------------------------------- */

static void rx_close(void);

static void rx_fire(void)
{
	p.op = LG_OP_RX;
	charge_fixed();
	p.rx_on_us = lg_ts_now_us() + LG_RAMPUP_US;
	p.charged = false;

	nrf_radio_packetptr_set(NRF_RADIO, p.frame);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH);

	/* RSSISTART on address match gives a reading taken while the frame is
	 * actually on air, which is what the link metrics want.
	 */
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK |
						NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);

	if (IS_ENABLED(CONFIG_LOCKGRID_EARLY_ABORT)) {
		/* Interrupt once the header bytes that decide whether this frame
		 * is for us have arrived.
		 */
		nrf_radio_bcc_set(NRF_RADIO, (LG_PDU_OFF + LG_FILTER_BYTES) * 8);
		nrf_radio_shorts_enable(NRF_RADIO, NRF_RADIO_SHORT_ADDRESS_BCSTART_MASK);
		nrf_radio_int_enable(NRF_RADIO,
				     NRF_RADIO_INT_END_MASK | NRF_RADIO_INT_BCMATCH_MASK);
	} else {
		nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK);
	}

	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);

	/* Give up if nothing starts arriving before the window closes. */
	lg_ts_timer_at(p.rx_close_us, rx_close);
}

static void rx_close(void)
{
	if (p.op != LG_OP_RX) {
		return;
	}

	/* A frame that has already started is allowed to finish: aborting it
	 * would throw away the receive energy already spent on it. The length
	 * byte arrives immediately after the SFD, so the extension can be the
	 * frame's actual air time rather than the worst case - which for a short
	 * frame is the difference between 400 us and 4.3 ms of receiver on-time.
	 */
	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
		uint8_t psdu = p.frame[0];
		uint32_t remaining = LG_AIRTIME_US(psdu <= LG_PDU_MAX + LG_CRC_LEN_154
							   ? psdu
							   : LG_PDU_MAX);

		lg_ts_timer_at(p.rx_close_us + remaining, rx_close);
		return;
	}

	charge_rx_until(p.rx_close_us);
	radio_disable();
	p.op = LG_OP_IDLE;
	if (p.cb->rx_failed) {
		p.cb->rx_failed(LG_PHY_RX_TIMEOUT);
	}
}

int lg_phy_rx_window(uint64_t open_us, uint64_t close_us, uint8_t chan, uint8_t *buf)
{
	uint64_t fire_us;

	if (close_us <= open_us) {
		return -EINVAL;
	}

	radio_configure(chan);

	p.rx_dst = buf;
	p.rx_close_us = close_us;

	fire_us = open_us - LG_RAMPUP_US;
	if (fire_us <= lg_ts_now_us()) {
		return -ETIME;
	}

	p.op = LG_OP_RX_ARMED;
	lg_ts_timer_at(fire_us, rx_fire);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Interrupt handling                                                         */
/* ------------------------------------------------------------------------- */

/**
 * Decide, from the header bytes received so far, whether the rest of the frame
 * is worth the receive current.
 */
static bool early_filter_pass(void)
{
	const uint8_t *pdu = &p.frame[LG_PDU_OFF];
	lg_addr_t dst;

	if (lg_get_le16(&pdu[LG_OFF_NETID]) != p.netid16) {
		return false;
	}
	if (p.promiscuous) {
		return true;
	}

	dst = lg_get_le16(&pdu[LG_OFF_DST]);
	return dst == p.local || dst == LG_ADDR_BROADCAST;
}

static void handle_bcmatch(void)
{
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH);

	if (early_filter_pass()) {
		return;
	}

	/* Stop the receiver mid-frame. This is where most of the receive cost of
	 * overheard traffic disappears: a full 125 byte frame is 4.3 ms of
	 * receive, and bailing out after the header costs under 400 us.
	 */
	charge_rx_until(lg_ts_now_us());
	radio_disable();
	p.op = LG_OP_IDLE;
	lg_ts_timer_cancel();
	if (p.cb->rx_failed) {
		p.cb->rx_failed(LG_PHY_RX_FILTERED);
	}
}

static void handle_tx_end(void)
{
	uint64_t air_end = p.tx_air_end_us;

	charge_tx();
	radio_disable();
	p.op = LG_OP_IDLE;
	if (p.cb->tx_done) {
		p.cb->tx_done(air_end);
	}
}

static void handle_rx_end(void)
{
	struct lg_phy_rx rx;
	uint64_t now = lg_ts_now_us();
	bool crc_ok = nrf_radio_crc_status_check(NRF_RADIO);
	uint8_t rssi_raw = nrf_radio_rssi_sample_get(NRF_RADIO);
	uint8_t psdu_len = p.frame[0];
	uint8_t len = (psdu_len >= LG_CRC_LEN_154) ? psdu_len - LG_CRC_LEN_154 : 0;

	lg_ts_timer_cancel();
	charge_rx_until(now);
	radio_disable();
	p.op = LG_OP_IDLE;

	if (!crc_ok || len == 0 || len > LG_PDU_MAX) {
		if (p.cb->rx_failed) {
			p.cb->rx_failed(LG_PHY_RX_CRC);
		}
		return;
	}

	memcpy(p.rx_dst, &p.frame[LG_PDU_OFF], len);

	/* The END event fires a fixed chain delay after the last bit, and the
	 * air time is exact for a given length, so both edges can be
	 * reconstructed. Links use air_start_us to re-anchor, which is what
	 * keeps the receive windows narrow.
	 */
	rx.pdu = p.rx_dst;
	rx.len = len;
	rx.rssi = -(int8_t)rssi_raw;
	rx.air_end_us = now - LG_RX_CHAIN_US;
	rx.air_start_us = rx.air_end_us - LG_AIRTIME_US(len);

	if (p.cb->rx_done) {
		p.cb->rx_done(&rx);
	}
}

void lg_phy_isr(void)
{
	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH)) {
		handle_bcmatch();
		if (p.op == LG_OP_IDLE) {
			return;
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

		if (p.op == LG_OP_TX) {
			handle_tx_end();
		} else if (p.op == LG_OP_RX) {
			handle_rx_end();
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Public entry points                                                        */
/* ------------------------------------------------------------------------- */

void lg_phy_stop(void)
{
	if (p.op == LG_OP_RX) {
		charge_rx_until(lg_ts_now_us());
	}
	lg_ts_timer_cancel();
	radio_disable();
	p.op = LG_OP_IDLE;
	p.in_event = false;
}

bool lg_phy_busy(void)
{
	return p.op != LG_OP_IDLE;
}

void lg_phy_network_set(uint32_t network_id)
{
	p.netid16 = (uint16_t)network_id;
}

void lg_phy_filter_set(lg_addr_t local, bool promiscuous)
{
	p.local = local;
	p.promiscuous = promiscuous;
}

int8_t lg_phy_noise_floor(uint8_t chan, uint32_t period_us)
{
	uint32_t loops = MAX(period_us / LG_ED_LOOP_US, 1U);
	uint8_t sample;
	int spins = 0;

	radio_configure(chan);
	charge_fixed();

	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_EDSTART_MASK);
	nrf_radio_ed_loop_count_set(NRF_RADIO, loops);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_EDEND);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);

	lg_power_charge(LG_PHY_STATE_RX, loops * LG_ED_LOOP_US);

	/* Bounded spin. The measurement is short and only runs from the channel
	 * evaluation event, which has the radio to itself.
	 */
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_EDEND)) {
		if (++spins > (int)(loops * LG_ED_LOOP_US) + 1000) {
			radio_disable();
			return INT8_MIN;
		}
		k_busy_wait(1);
	}

	sample = nrf_radio_ed_sample_get(NRF_RADIO);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_EDEND);
	radio_disable();

	return (int8_t)(LG_ED_RSSI_OFFSET + (int)sample);
}

int lg_phy_init(const struct lg_phy_cb *cb)
{
	p.cb = cb;
	p.op = LG_OP_IDLE;
	p.local = LG_ADDR_INVALID;

	/* The interrupt is wired up by the radio access backend, which is the
	 * only part that knows whether this node owns the RADIO vector.
	 */
	return 0;
}
