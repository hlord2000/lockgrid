#!/usr/bin/env bash
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# A backhaul, a router and a leaf are switched on at the same moment with no
# prior knowledge of each other. Within the check window every node must have
# authenticated at least one neighbour and found a route, and the backhaul must
# be receiving the telemetry both of the others are sending.
#
# This is the end-to-end test: it only passes if discovery, the certificate
# handshake, the connection-event schedule, channel hopping, the routing metric
# and forwarding all work at once.

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="lockgrid_mesh_form"
verbosity_level=2
EXECUTE_TIMEOUT=300
sim_length_us=45e6
check_ms=40000

cd ${BSIM_OUT_PATH}/bin

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=0 \
  -lg_addr=1 -lg_role=2 -lg_check=${check_ms} \
  -exp_links=2 -exp_rx_from=2 -exp_no_authfail=1 -exp_duty_max=40

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=1 \
  -lg_addr=2 -lg_role=1 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1 -exp_joins=1 -exp_duty_max=60

Execute ./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf \
  -v=${verbosity_level} -s=${simulation_id} -d=2 \
  -lg_addr=3 -lg_role=0 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1 -exp_joins=1 -exp_duty_max=60

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} \
  -D=3 -sim_length=${sim_length_us} $@

wait_for_background_jobs
