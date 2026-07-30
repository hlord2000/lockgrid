#!/usr/bin/env bash
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# A leaf arrives at a grid that has already settled.
#
# This is the case that has no natural trigger. A router that has reached its link
# target and has a route wants nothing, and a leaf never beacons, so nothing tells
# the router that anybody is trying to join. Without CONFIG_LOCKGRID_JOIN_WINDOW_
# FLOOR_MS the leaf hears the beacons, works out exactly when the join window
# should be, transmits into it and is never heard.
#
# Three routers plus a backhaul run for 45 s and reach their link targets, then the
# leaf powers on and must still get in.

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="lockgrid_late_join"
verbosity_level=2
EXECUTE_TIMEOUT=600
sim_length_us=130e6
leaf_delay_ms=45000
check_ms=125000

cd ${BSIM_OUT_PATH}/bin

# A settled grid: a backhaul and two routers, all of which reach the link target
# well before the leaf appears.
Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=0 \
  -lg_addr=1 -lg_role=2 -lg_check=${check_ms} \
  -exp_links=2 -exp_no_authfail=1

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=1 \
  -lg_addr=2 -lg_role=1 -lg_period=10000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=2

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=2 \
  -lg_addr=3 -lg_role=1 -lg_period=10000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=2

# The late leaf. It must authenticate and find a route despite nothing in the grid
# having any reason to expect it.
Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=3 \
  -lg_addr=4 -lg_role=0 -lg_delay=${leaf_delay_ms} -lg_period=10000 \
  -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1 -exp_joins=1

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} \
  -D=4 -sim_length=${sim_length_us} $@

wait_for_background_jobs
