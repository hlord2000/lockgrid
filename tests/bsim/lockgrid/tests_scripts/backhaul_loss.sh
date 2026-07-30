#!/usr/bin/env bash
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# A backhaul loses its uplink while the grid is running.
#
# Backhaul is the one role a node does not choose. It is a fact about the
# hardware - there is either something to terminate traffic on or there is not -
# so the application declares it, and can withdraw it. This is the withdrawal.
#
# Two nodes start with an uplink. At 50 s the first one loses it. What must
# happen is not that the node goes away: it still has links, still has radio, and
# is still perfectly useful as a relay. What must change is that it stops
# claiming to be a sink, so:
#
#   - 0x0001 must have settled on router, not backhaul, by the end
#   - everything must still have a route, now via 0x0002
#   - nothing may fail to authenticate, because no re-handshaking is required to
#     re-parent onto an existing link
#
# The transition deliberately bypasses the role dwell time. Fifteen seconds of a
# node advertising rank zero when it has nowhere to forward to would be fifteen
# seconds of its children believing they had a path.

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="lockgrid_backhaul_loss"
verbosity_level=2
EXECUTE_TIMEOUT=400
sim_length_us=95e6
check_ms=90000
uplink_ms=50000

cd ${BSIM_OUT_PATH}/bin

EXE=./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf

# Loses its uplink half way through. Must demote itself to router and keep going.
Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=0 \
  -lg_addr=1 -lg_role=2 -lg_uplink=${uplink_ms} -lg_check=${check_ms} \
  -exp_role=2 -exp_route=1 -exp_links=1 -exp_no_authfail=1

# Keeps its uplink, and has to take over as the grid's sink.
Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=1 \
  -lg_addr=2 -lg_role=2 -lg_check=${check_ms} \
  -exp_role=3 -exp_links=1 -exp_no_authfail=1

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=2 \
  -lg_addr=3 -lg_role=1 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1 -exp_no_authfail=1

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=3 \
  -lg_addr=4 -lg_role=1 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1 -exp_no_authfail=1

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} \
  -D=4 -sim_length=${sim_length_us} $@

wait_for_background_jobs
