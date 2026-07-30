#!/usr/bin/env bash
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# LockGrid on a radio it does not own.
#
# MPSL cannot run under BabbleSim, so the MPSL backend cannot be exercised here.
# The property that matters can be, though: a fifth of all radio requests are
# refused at the moment they were due, which is what a cancelled timeslot costs.
# What is asserted is tolerance, not richness: every node must still authenticate a
# neighbour and find a route. Topology richness does degrade at this rate - the
# backhaul typically ends up holding one link where it would otherwise hold two -
# and that is the honest cost of losing a fifth of all radio opportunities.
#
# Each node also has to report refused requests, so the test cannot pass by having
# the synthetic contention silently switched off.

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="lockgrid_contended"
verbosity_level=2
EXECUTE_TIMEOUT=600
sim_length_us=90e6
check_ms=85000

cd ${BSIM_OUT_PATH}/bin

EXE=./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf_overlay-contended_conf

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=0 \
  -lg_addr=1 -lg_role=2 -lg_check=${check_ms} \
  -exp_links=1 -exp_rx_from=1 -exp_no_authfail=1 -exp_min_blocked=10

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=1 \
  -lg_addr=2 -lg_role=1 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1 -exp_min_blocked=10

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=2 \
  -lg_addr=3 -lg_role=0 -lg_period=5000 -lg_check=${check_ms} \
  -exp_route=1 -exp_links=1 -exp_min_blocked=10

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} \
  -D=3 -sim_length=${sim_length_us} $@

wait_for_background_jobs
