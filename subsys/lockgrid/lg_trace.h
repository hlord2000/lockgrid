/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lg_trace.h
 * @brief Machine-readable event trace, for offline analysis and visualisation.
 *
 * Every line is `LGT <us> <addr> <event> <key>=<value>...`, printed with printk
 * rather than through the logging subsystem so that a line is atomic, carries its
 * own microsecond timestamp, and costs nothing to parse. BabbleSim prefixes each
 * line with the device number, which the parser ignores; it uses the embedded
 * timestamp instead, because that is the event's real time rather than the time
 * the log backend got round to it.
 *
 * Frame-level tracing is separately gated: with a hundred nodes it is by far the
 * bulk of the output, and topology work rarely needs it.
 */

#ifndef LOCKGRID_LG_TRACE_H_
#define LOCKGRID_LG_TRACE_H_

#include <zephyr/kernel.h>

#if defined(CONFIG_LOCKGRID_TRACE)

/** @brief Emit one trace line. Adds the timestamp, address and a newline. */
void lg_trace_emit(const char *event, const char *fmt, ...);

#define LG_TRACE(_event, _fmt, ...) lg_trace_emit(_event, _fmt, ##__VA_ARGS__)

#if defined(CONFIG_LOCKGRID_TRACE_FRAMES)
#define LG_TRACE_FRAME(_event, _fmt, ...) lg_trace_emit(_event, _fmt, ##__VA_ARGS__)
#else
#define LG_TRACE_FRAME(_event, _fmt, ...)
#endif

#else /* !CONFIG_LOCKGRID_TRACE */

#define LG_TRACE(_event, _fmt, ...)
#define LG_TRACE_FRAME(_event, _fmt, ...)

#endif /* CONFIG_LOCKGRID_TRACE */

#endif /* LOCKGRID_LG_TRACE_H_ */
