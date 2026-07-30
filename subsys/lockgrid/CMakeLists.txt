# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(lockgrid)

zephyr_library_sources(
  lg_core.c
  lg_phy_nrf.c
  lg_sched.c
  lg_frame.c
  lg_chan.c
  lg_neigh.c
  lg_link.c
  lg_disc.c
  lg_route.c
  lg_role.c
  lg_sec.c
  lg_join.c
  lg_power.c
  lg_stats.c
  # Always compiled: the body is gated on CONFIG_LOCKGRID_TRACE, but the public
  # lg_trace_snapshot() stub has to exist either way so applications can call it
  # unconditionally.
  lg_trace.c
)

zephyr_library_sources_ifdef(CONFIG_LOCKGRID_RADIO_DIRECT lg_ts_direct.c)
zephyr_library_sources_ifdef(CONFIG_LOCKGRID_RADIO_MPSL   lg_ts_mpsl.c)
zephyr_library_sources_ifdef(CONFIG_LOCKGRID_SIM_PROVISIONING lg_provision_sim.c)

zephyr_library_include_directories(.)
