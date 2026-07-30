#!/usr/bin/env bash
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# Exercises the channel map itself. A strong WLAN interferer sits on top of
# 802.15.4 channels 15 to 18 while two nodes hold a link at a short interval with
# subrating disabled, so per-channel evidence accumulates in seconds rather than
# minutes. The nodes must retire the affected channels from the map and keep the
# link up while doing it - a map update that the two ends applied at different
# times would break the hop sequence and show up as a supervision timeout.

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="lockgrid_chanmap"
verbosity_level=2
EXECUTE_TIMEOUT=600
sim_length_us=45e6
check_ms=40000

cd ${BSIM_OUT_PATH}/bin

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf_overlay-fastchan_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=0 \
  -lg_addr=1 -lg_role=2 -lg_check=${check_ms} \
  -exp_links=1 -exp_rx_from=1 -exp_no_authfail=1

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf_overlay-fastchan_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=1 \
  -lg_addr=2 -lg_role=1 -lg_period=2000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1

Execute ./bs_device_2G4_WLAN_actmod -v=${verbosity_level} -s=${simulation_id} -d=2 \
  -channel=6 -power=10 -ConfigSet=100

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} \
  -D=3 -sim_length=${sim_length_us} $@

wait_for_background_jobs
