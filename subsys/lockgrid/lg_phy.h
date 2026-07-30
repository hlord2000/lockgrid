/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_phy.h
 * @brief RADIO programming for the LockGrid PHY.
 *
 * LockGrid uses the IEEE 802.15.4-2006 O-QPSK DSSS PHY: 250 kbps, channels 11
 * to 26 at 5 MHz spacing, 32-bit zero preamble, one byte start-of-frame
 * delimiter, and an 8-bit length that includes the two CRC bytes. The LockGrid
 * PDU sits directly after the length byte.
 *
 * The MAC asks for transmissions and receive windows at absolute times. This
 * layer works out ramp-up and chain delays so that a transmission puts its
 * first bit on the antenna at the requested microsecond, and a receive window
 * has the antenna listening across exactly the requested span. That arithmetic
 * is what lets receive windows be a few hundred microseconds wide instead of
 * milliseconds, which is where the power goes.
 *
 * Fast ramp-up is used and every turnaround is scheduled explicitly rather than
 * handed to the radio's hardware inter-frame-spacing machine. That machine
 * forces normal ramp-up, which costs 90 us more on every radio event - at
 * ~3 mA, the difference between a protocol that can hold a 50 ms interval and
 * one that cannot.
 */

#ifndef LOCKGRID_LG_PHY_H_
#define LOCKGRID_LG_PHY_H_

#include <stdint.h>
#include <stdbool.h>
#include <lockgrid/lockgrid.h>
#include "lg_frame.h"

/** Lowest and highest 802.15.4 channel number LockGrid uses. */
#define LG_CHAN_FIRST 11
#define LG_CHAN_LAST  26
#define LG_CHAN_COUNT (LG_CHAN_LAST - LG_CHAN_FIRST + 1)

/**
 * Gap between the last bit of one frame and the first bit of the reply.
 *
 * 802.15.4 asks for at least aTurnaroundTime, 12 symbols or 192 us. LockGrid
 * rounds that up slightly to leave the interrupt path room to stage the reply.
 */
#define LG_IFS_US 200

/** Ramp-up before the radio can transmit or receive, with fast ramp-up on. */
#define LG_RAMPUP_US 40

/** Air time of a @p len byte PDU, including preamble, SFD, length and CRC. */
#define LG_AIRTIME_US(len) (((uint32_t)(len) + 8U) * 32U)

/**
 * Extra listening a receiver must do to catch a frame it cannot time exactly:
 * the preamble and SFD have to go by before the radio reports a frame start, so
 * a window narrower than this can straddle a perfectly timed frame.
 */
#define LG_SYNC_WINDOW_US 160

/** Why a receive window produced nothing usable. */
enum lg_phy_rx_fail {
	/** The window closed without the radio matching a frame start. */
	LG_PHY_RX_TIMEOUT,
	/** A frame arrived but its CRC was wrong. */
	LG_PHY_RX_CRC,
	/** The early filter rejected the frame part way through. */
	LG_PHY_RX_FILTERED,
	/** The radio was taken away mid-window. */
	LG_PHY_RX_ABORTED,
};

/** A successfully received frame. */
struct lg_phy_rx {
	/** Points at the buffer the MAC handed to lg_phy_rx_window(). */
	const uint8_t *pdu;
	uint8_t len;
	/** In dBm. */
	int8_t rssi;
	/**
	 * When the frame's first bit hit the antenna, in LockGrid time.
	 * Reconstructed from the END event, the fixed receive chain delay and
	 * the frame's air time, so it is exact for a given length. This is what
	 * re-anchors a link and therefore what keeps windows narrow.
	 */
	uint64_t air_start_us;
	/** When the last bit hit the antenna. */
	uint64_t air_end_us;
};

/** Events the PHY reports. All run in ISR context. */
struct lg_phy_cb {
	/**
	 * @brief A transmission finished.
	 * @param air_end_us When the last bit left the antenna.
	 */
	void (*tx_done)(uint64_t air_end_us);
	/** @brief A frame was received and passed CRC and the early filter. */
	void (*rx_done)(const struct lg_phy_rx *rx);
	/** @brief The receive window produced nothing. */
	void (*rx_failed)(enum lg_phy_rx_fail reason);
};

/** @brief One-time init. Does not touch the radio or install an interrupt. */
int lg_phy_init(const struct lg_phy_cb *cb);

/**
 * @brief Service the RADIO peripheral.
 *
 * The radio access backend decides how this is reached. With direct ownership it
 * is wired straight to the RADIO interrupt; under MPSL the arbiter owns that
 * vector and hands the event over as a timeslot signal instead.
 */
void lg_phy_isr(void);

/**
 * @brief Set the network the radio filters for.
 *
 * The 802.15.4 SFD is a single standard byte, so there is no hardware address
 * matcher to lean on. Rejecting foreign grids is instead done by the bit-counter
 * early abort, which is why CONFIG_LOCKGRID_EARLY_ABORT matters more here than
 * it would on a PHY with a 4 byte access address.
 */
void lg_phy_network_set(uint32_t network_id);

/**
 * @brief Tell the PHY which addresses are worth receiving.
 *
 * Used by the early abort filter: a frame whose destination is neither
 * @p local nor a broadcast is dropped part way through, before the radio has
 * spent energy on the rest of it.
 */
void lg_phy_filter_set(lg_addr_t local, bool promiscuous);

/** @brief Transmit @p pdu so its first bit leaves the antenna at @p air_start_us. */
int lg_phy_tx_at(uint64_t air_start_us, uint8_t chan, const uint8_t *pdu, uint8_t len);

/**
 * @brief Listen from @p open_us until @p close_us.
 *
 * If a frame starts inside the window it is received to completion even if that
 * runs past @p close_us. @p buf must stay valid until a callback arrives.
 */
int lg_phy_rx_window(uint64_t open_us, uint64_t close_us, uint8_t chan, uint8_t *buf);

/** @brief Stop the radio now. No callback follows. */
void lg_phy_stop(void);

/** @brief True if the radio is transmitting or receiving. */
bool lg_phy_busy(void);

/**
 * @brief Measure the noise floor on @p chan with the energy detect block.
 *
 * @return Measured power in dBm, or INT8_MIN on failure.
 */
int8_t lg_phy_noise_floor(uint8_t chan, uint32_t period_us);

/** @brief Centre frequency of @p chan in MHz. */
static inline uint16_t lg_phy_chan_freq(uint8_t chan)
{
	return 2405 + 5 * (chan - LG_CHAN_FIRST);
}

/** @brief Radio state, for the energy accounting. */
enum lg_phy_state {
	LG_PHY_STATE_OFF,
	LG_PHY_STATE_RAMP,
	LG_PHY_STATE_TX,
	LG_PHY_STATE_RX,
};

#endif /* LOCKGRID_LG_PHY_H_ */
