/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_ts.h
 * @brief Radio access arbitration and timing.
 *
 * Everything above this header schedules in absolute microseconds and never
 * touches a timer or an arbiter itself. Two backends implement it:
 *
 *  - lg_ts_direct.c owns RADIO and two GRTC compare channels. Every request is
 *    granted at exactly the requested time.
 *  - lg_ts_mpsl.c turns each request into an MPSL timeslot, so LockGrid shares
 *    the radio with Bluetooth LE. Requests can be refused, and the protocol
 *    above treats a refusal like a lost frame.
 *
 * The time base is the GRTC SYSCOUNTER, which ticks at 1 MHz, so LockGrid time
 * is microseconds since boot and never wraps in practice.
 */

#ifndef LOCKGRID_LG_TS_H_
#define LOCKGRID_LG_TS_H_

#include <stdint.h>
#include <stdbool.h>

/** Why a request did not result in radio time. */
enum lg_ts_fail {
	/** The arbiter gave the slot to a higher priority user. */
	LG_TS_FAIL_BLOCKED,
	/** The slot was withdrawn after it had been granted. */
	LG_TS_FAIL_CANCELLED,
};

/** Callbacks the MAC installs once at init. Both run in ISR context. */
struct lg_ts_cb {
	/**
	 * @brief The radio is now ours until the reserved length elapses.
	 *
	 * @param at_us   Time the grant started.
	 * @param user    Value passed to lg_ts_request().
	 */
	void (*granted)(uint64_t at_us, void *user);
	/** @brief The request did not produce radio time. */
	void (*failed)(enum lg_ts_fail reason, void *user);
};

/** @brief Bring the backend up. */
int lg_ts_init(const struct lg_ts_cb *cb);

/** @brief Tear the backend down and drop any pending request. */
void lg_ts_uninit(void);

/** @brief Current time in microseconds. Safe from any context. */
uint64_t lg_ts_now_us(void);

/**
 * @brief Ask for the radio.
 *
 * Replaces any request that has not been granted yet. There is at most one
 * outstanding request, which is what keeps the scheduler above honest: it must
 * always know which event is next.
 *
 * @param at_us  Absolute time the grant should start.
 * @param len_us How long the radio is needed for.
 * @param urgent Ask the arbiter to preempt lower priority users.
 * @param user   Opaque value handed back in the callbacks.
 *
 * @retval 0 Request accepted.
 * @retval -EINVAL @p at_us is already in the past by more than the backend can absorb.
 */
int lg_ts_request(uint64_t at_us, uint32_t len_us, bool urgent, void *user);

/** @brief Give the radio back before the reservation ends. */
void lg_ts_release(void);

/**
 * @brief Ask for more time in the current grant.
 *
 * @retval 0       Granted, the reservation now runs @p extra_us longer.
 * @retval -EBUSY  The arbiter refused; the event has to finish early.
 */
int lg_ts_extend(uint32_t extra_us);

/**
 * @brief Run @p cb at absolute time @p at_us, inside the current grant.
 *
 * There is one such timer, and arming it replaces any previous arming. The
 * callback runs in ISR context.
 */
void lg_ts_timer_at(uint64_t at_us, void (*cb)(void));

/** @brief Disarm the fine timer. */
void lg_ts_timer_cancel(void);

/** @brief True while LockGrid holds the radio. */
bool lg_ts_granted(void);

/** @brief Time at which the current grant expires, 0 if not granted. */
uint64_t lg_ts_grant_end_us(void);

/**
 * @brief Longest a grant may last on this backend.
 *
 * MPSL caps a timeslot at 100 ms; the direct backend has no limit but reports
 * the same value so the scheduler behaves identically either way.
 */
uint32_t lg_ts_max_grant_us(void);

#endif /* LOCKGRID_LG_TS_H_ */
