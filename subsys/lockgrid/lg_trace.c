/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_trace.c
 * @brief Event trace emission. See lg_trace.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdarg.h>

#include "lg_internal.h"
#include "lg_trace.h"

#if defined(CONFIG_LOCKGRID_TRACE)

/*
 * Built into one buffer and emitted with a single printk, so a line cannot be
 * interleaved with another thread's output. Trace calls come from both the work
 * queue and the radio interrupt, and a torn line is worse than no line: the
 * parser would silently attribute events to the wrong node.
 */
#define LG_TRACE_LINE_MAX 224

void lg_trace_emit(const char *event, const char *fmt, ...)
{
	char line[LG_TRACE_LINE_MAX];
	va_list ap;
	int n, body;

	n = snprintk(line, sizeof(line), "LGT %llu %04x %s ", lg_ts_now_us(), lg.addr, event);
	if (n < 0 || n >= (int)sizeof(line)) {
		return;
	}

	va_start(ap, fmt);
	body = vsnprintk(&line[n], sizeof(line) - n, fmt, ap);
	va_end(ap);

	/*
	 * A truncated line is worse than a missing one, because the parser reads the
	 * fields that survived and silently drops the rest: adding one field to a
	 * line that was already near the limit cost the last two fields of every
	 * power sample, and nothing said so. Say so.
	 */
	if (body < 0 || body >= (int)(sizeof(line) - n)) {
		printk("LGT %llu %04x truncated event=%s need=%d\n", lg_ts_now_us(), lg.addr,
		       event, n + (body < 0 ? 0 : body));
		return;
	}

	printk("%s\n", line);
}

/** One line per neighbour, so the visualisation can draw the graph and its costs. */
static bool trace_neigh(const struct lg_neighbor_info *info, void *user)
{
	ARG_UNUSED(user);

	lg_trace_emit("neigh", "peer=%04x linked=%u parent=%u rank=%u cost=%u rssi=%d etx=%u "
			       "per=%u int=%u sub=%u win=%u",
		      info->addr, info->linked ? 1U : 0U, info->parent ? 1U : 0U, info->peer_rank,
		      info->link_cost, info->rssi, info->etx_q4, info->per_permille,
		      info->interval_us, info->subrate, info->rx_window_us);
	return true;
}

void lg_trace_snapshot(void)
{
	struct lg_power_report pw;
	struct lg_stats st;

	lg_power_report_get_steady(&pw);
	lg_stats_get(&st);

	lg_trace_emit("state", "rank=%u parent=%04x links=%u map=%04x role=%u", lg.rank,
		      lg_parent(), lg_link_count(), lg.chan_map, (unsigned)lg.role);
	/* total_nc is cumulative, which is what makes a windowed figure possible:
	 * differencing it between two snapshots gives the current drawn over that
	 * stretch alone. The averages beside it are measured from boot, so during a
	 * long convergence they say more about how the grid formed than about what a
	 * settled node costs.
	 */
	lg_trace_emit("pwr", "on=%llu elapsed=%llu duty=%u ua=%llu steady=%llu nc=%llu "
			     "rxlink=%llu rxscan=%llu rxjoin=%llu txlink=%llu txbeacon=%llu "
			     "fixed=%llu starts=%u",
		      pw.radio_on_us, pw.elapsed_us, pw.duty_permille, pw.avg_na / 1000U,
		      pw.steady_na / 1000U, pw.total_nc, pw.rx_us_link, pw.rx_us_scan,
		      pw.rx_us_join, pw.tx_us_link, pw.tx_us_beacon, pw.fixed_nc,
		      pw.radio_starts);
	lg_neighbor_foreach(trace_neigh, NULL);
	lg_trace_emit("counters", "btx=%u brx=%u scans=%u tracked=%u joinok=%u joinfail=%u "
				  "ev=%u skip=%u crc=%u to=%u sup=%u fwd=%u roles=%u",
		      st.beacons_tx, st.beacons_rx, st.scans, st.tracked_beacons, st.joins_ok,
		      st.joins_failed, st.link_events, st.link_events_skipped, st.crc_errors,
		      st.rx_timeouts, st.supervision_timeouts, st.msgs_forwarded,
		      st.role_changes);
}

#else /* !CONFIG_LOCKGRID_TRACE */

void lg_trace_snapshot(void)
{
}

#endif /* CONFIG_LOCKGRID_TRACE */
