#!/usr/bin/env bash
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# A grid runs with a WLAN interferer parked on top of part of the band. The nodes
# must keep their links up: the channel map should retire the affected channels
# and the hop sequence should stop using them, without either end of a link
# losing track of where the other is.

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="lockgrid_interference"
verbosity_level=2
EXECUTE_TIMEOUT=600
sim_length_us=45e6
check_ms=40000

cd ${BSIM_OUT_PATH}/bin

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=0 \
  -lg_addr=1 -lg_role=2 -lg_check=${check_ms} \
  -exp_links=1 -exp_rx_from=1 -exp_no_authfail=1

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=1 \
  -lg_addr=2 -lg_role=1 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1

# A WLAN-shaped interferer on WLAN channel 6 (2437 MHz), which covers 802.15.4
# channels 15 to 18, driving traffic at 25% of the WLAN capacity.
Execute ./bs_device_2G4_WLAN_actmod -v=${verbosity_level} -s=${simulation_id} -d=2 \
  -channel=6 -power=-10 -ConfigSet=25

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} \
  -D=3 -sim_length=${sim_length_us} $@

wait_for_background_jobs
