/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_chan.c
 * @brief Channel quality tracking, blacklisting and hop sequence generation.
 *
 * LockGrid hops over the 802.15.4 channel set, 11 to 26, 5 MHz apart. Two
 * mechanisms keep it out of trouble:
 *
 *  - Every frame outcome is charged to the channel it happened on. A channel
 *    whose error rate crosses CONFIG_LOCKGRID_CHAN_BAD_PER_PERMILLE is retired
 *    from the map, and periodically probed again so the map recovers when the
 *    interferer moves. That is what makes "known clean channels" true over time
 *    rather than at provisioning.
 *
 *  - Between connection events the radio's energy detect block samples the
 *    noise floor of one channel at a time. Because it piggybacks on an event
 *    that already has the radio warm, this costs almost nothing.
 *
 * The hop sequence is the Bluetooth channel selection algorithm #2 over the
 * live map. Both ends derive it from the event counter and a seed agreed when
 * the link was set up, so neither has to be told where the other will be. Map
 * changes are applied at a shared event counter, so a lost update cannot
 * desynchronise the two ends silently.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "lg_internal.h"
#include "lg_trace.h"

LOG_MODULE_DECLARE(lockgrid, CONFIG_LOCKGRID_LOG_LEVEL);

/* Frames on a channel before its error rate is trusted. */
#define LG_CHAN_MIN_SAMPLES 12
/* Sample counts are halved once they reach this, so old evidence decays. */
#define LG_CHAN_DECAY_AT 64

/* Channels 15, 20, 25 and 26 sit in the gaps between the three non-overlapping
 * Wi-Fi channels, so they are the least likely to be occupied. LockGrid starts
 * from them and lets the measurements take over.
 */
#define LG_CHAN_PREFERRED_MASK                                                                     \
	(BIT(15 - LG_CHAN_FIRST) | BIT(20 - LG_CHAN_FIRST) | BIT(25 - LG_CHAN_FIRST) |              \
	 BIT(26 - LG_CHAN_FIRST))

void lg_chan_init(void)
{
	for (int i = 0; i < LG_CHAN_COUNT; i++) {
		lg.chan[i].noise_dbm = INT8_MIN;
		lg.chan[i].per_permille = 0;
		lg.chan[i].blacklisted = false;
		lg.chan[i].frames_ok = 0;
		lg.chan[i].frames_bad = 0;
	}

	/* Start with everything available. Nodes converge on the quiet channels
	 * from measurements, which is more robust than trusting a static list
	 * that may be wrong for this particular site.
	 */
	lg.chan_map = BIT_MASK(LG_CHAN_COUNT);
	lg.next_chan_eval_us = lg_ts_now_us() + CONFIG_LOCKGRID_CHAN_EVAL_PERIOD_MS * 1000ULL;
}

uint8_t lg_chan_map_count(uint16_t map)
{
	return POPCOUNT(map & BIT_MASK(LG_CHAN_COUNT));
}

uint8_t lg_chan_remap(uint16_t map, uint16_t index)
{
	uint8_t n = lg_chan_map_count(map);
	uint8_t seen = 0;

	if (n == 0) {
		return LG_CHAN_FIRST;
	}
	index %= n;

	for (int i = 0; i < LG_CHAN_COUNT; i++) {
		if (map & BIT(i)) {
			if (seen == index) {
				return LG_CHAN_FIRST + i;
			}
			seen++;
		}
	}
	return LG_CHAN_FIRST;
}

/* Bluetooth channel selection algorithm #2 helpers. */
static uint16_t csa_permute(uint16_t v)
{
	v = ((v & 0xAAAA) >> 1) | ((v & 0x5555) << 1);
	v = ((v & 0xCCCC) >> 2) | ((v & 0x3333) << 2);
	v = ((v & 0xF0F0) >> 4) | ((v & 0x0F0F) << 4);
	v = (v >> 8) | (v << 8);
	return v;
}

static uint16_t csa_mam(uint16_t a, uint16_t b)
{
	return (uint16_t)(17U * (uint32_t)a + b);
}

/**
 * Avalanche the pseudo-random value before it is reduced.
 *
 * Bluetooth reduces its channel index modulo 37, a prime, which mixes the whole
 * word into the result. LockGrid has at most 16 channels, so the reduction is
 * modulo a power of two and only the low bits survive - and those bits are
 * degenerate in the algorithm above, because 17*a is congruent to a modulo 16,
 * so the multiply-and-add step contributes nothing to them. Without this step
 * every event lands on the same channel.
 */
static uint16_t mix16(uint16_t x)
{
	x ^= x >> 8;
	x *= 0x2B9DU;
	x ^= x >> 7;
	x *= 0x1B2DU;
	x ^= x >> 9;
	return x;
}

uint8_t lg_chan_hop(uint16_t map, uint16_t seed, uint16_t counter)
{
	uint16_t prn = counter ^ seed;

	for (int i = 0; i < 3; i++) {
		prn = csa_permute(prn);
		prn = csa_mam(prn, seed);
	}
	prn ^= seed;

	return lg_chan_remap(map, mix16(prn));
}

void lg_chan_result(uint8_t chan, bool ok)
{
	struct lg_chan_stat *cs;
	uint32_t total;

	if (chan < LG_CHAN_FIRST || chan > LG_CHAN_LAST) {
		return;
	}
	cs = &lg.chan[chan - LG_CHAN_FIRST];

	if (ok) {
		cs->frames_ok++;
	} else {
		cs->frames_bad++;
	}

	if (cs->frames_ok + cs->frames_bad >= LG_CHAN_DECAY_AT) {
		cs->frames_ok /= 2;
		cs->frames_bad /= 2;
	}

	total = cs->frames_ok + cs->frames_bad;
	if (total) {
		cs->per_permille = (uint16_t)((cs->frames_bad * 1000U) / total);
	}
}

void lg_chan_noise(uint8_t chan, int8_t dbm)
{
	struct lg_chan_stat *cs;

	if (chan < LG_CHAN_FIRST || chan > LG_CHAN_LAST || dbm == INT8_MIN) {
		return;
	}
	cs = &lg.chan[chan - LG_CHAN_FIRST];

	cs->noise_dbm = cs->noise_dbm == INT8_MIN
				? dbm
				: (int8_t)lg_ewma(cs->noise_dbm, dbm, true);
	cs->last_eval_us = lg_ts_now_us();
}

uint8_t lg_chan_next_eval(void)
{
	/* Round-robin over every channel, including retired ones: a retired
	 * channel needs measuring most, because that is the only way it comes
	 * back.
	 */
	static uint8_t next;
	uint8_t chan = LG_CHAN_FIRST + (next % LG_CHAN_COUNT);

	next++;
	return chan;
}

/** Enough usable channels left that retiring one more is safe. */
static bool can_retire(void)
{
	return lg_chan_map_count(lg.chan_map) > CONFIG_LOCKGRID_CHAN_MIN_GOOD;
}

bool lg_chan_reevaluate(void)
{
	uint16_t before = lg.chan_map;

	for (int i = 0; i < LG_CHAN_COUNT; i++) {
		struct lg_chan_stat *cs = &lg.chan[i];
		uint32_t samples = cs->frames_ok + cs->frames_bad;
		bool in_map = (lg.chan_map & BIT(i)) != 0;
		bool noisy = cs->noise_dbm != INT8_MIN &&
			     cs->noise_dbm > CONFIG_LOCKGRID_CHAN_NOISE_FLOOR_DBM;

		if (in_map) {
			bool errored = samples >= LG_CHAN_MIN_SAMPLES &&
				       cs->per_permille >= CONFIG_LOCKGRID_CHAN_BAD_PER_PERMILLE;

			if ((errored || noisy) && can_retire()) {
				lg.chan_map &= ~BIT(i);
				cs->blacklisted = true;
				cs->probing = false;
				lg.stats.channels_blacklisted++;
				LOG_DBG("channel %u out: per %u/1000, noise %d dBm",
					LG_CHAN_FIRST + i, cs->per_permille, cs->noise_dbm);
			}
			continue;
		}

		/* Retired. Bring it back when the noise floor says the
		 * interferer has gone, or when a probe went well.
		 */
		if (!noisy && (cs->noise_dbm != INT8_MIN || cs->probing)) {
			bool clean = samples < LG_CHAN_MIN_SAMPLES ||
				     cs->per_permille <= CONFIG_LOCKGRID_CHAN_GOOD_PER_PERMILLE;

			if (clean) {
				lg.chan_map |= BIT(i);
				cs->blacklisted = false;
				cs->probing = false;
				cs->frames_ok = 0;
				cs->frames_bad = 0;
				cs->per_permille = 0;
				LOG_DBG("channel %u back in: noise %d dBm", LG_CHAN_FIRST + i,
					cs->noise_dbm);
			} else {
				/* Give it another chance next round with fresh
				 * counters rather than holding a grudge.
				 */
				cs->probing = true;
				cs->frames_ok = 0;
				cs->frames_bad = 0;
			}
		}
	}

	if (lg_chan_map_count(lg.chan_map) < CONFIG_LOCKGRID_CHAN_MIN_GOOD) {
		/* Everything looks bad, which usually means the problem is not
		 * the channels. Fall back to the four that avoid Wi-Fi rather
		 * than hopping over a map that cannot hold a link.
		 */
		lg.chan_map = LG_CHAN_PREFERRED_MASK;
		LOG_WRN("channel map exhausted, falling back to the Wi-Fi gaps");
	}

	if (lg.chan_map != before) {
		lg.stats.chan_map_updates++;
		LG_TRACE("chanmap", "map=%04x was=%04x", lg.chan_map, before);
		return true;
	}
	return false;
}

uint16_t lg_channel_map(void)
{
	return lg.chan_map;
}
