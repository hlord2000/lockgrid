#!/usr/bin/env bash
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# Four nodes form a grid, then the router that others have been using goes dark
# half way through. The remaining nodes must notice through supervision timeout,
# recompute their rank and re-attach - and the backhaul must still be hearing
# from the leaf at the end.
#
# The re-attach is expected to use the cached long term key, so it costs one
# round trip rather than a full certificate handshake.

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="lockgrid_heal"
verbosity_level=2
EXECUTE_TIMEOUT=400
sim_length_us=90e6
kill_ms=35000
check_ms=85000

cd ${BSIM_OUT_PATH}/bin

# Backhaul. Must still be receiving from the surviving nodes at the end.
Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=0 \
  -lg_addr=1 -lg_role=2 -lg_check=${check_ms} \
  -exp_links=1 -exp_rx_from=2 -exp_no_authfail=1

# The router that will be taken away. No expectations: it is switched off.
Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=1 \
  -lg_addr=2 -lg_role=1 -lg_period=5000 -lg_stop=${kill_ms} -lg_check=${check_ms}

# Two nodes that must survive the loss and still have a route at the end.
Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=2 \
  -lg_addr=3 -lg_role=1 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=3 \
  -lg_addr=4 -lg_role=0 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} \
  -D=4 -sim_length=${sim_length_us} $@

wait_for_background_jobs
