#!/usr/bin/env bash
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# Backhauls that keep falling off, roughly once a minute each.
#
# Losing a backhaul once is the easy case: the grid re-parents onto the other one
# and settles. This is the case that does not settle. Both backhauls drop their
# uplink about once a minute, offset from each other, so over a five minute run the
# set of sinks changes ten times and is sometimes empty. Backhaul is the one role a
# node does not choose - it is a fact about the hardware, declared through
# lg_backhaul_set() - so this exercises the only externally driven role transition
# the protocol has, repeatedly.
#
# What is asserted is that no node is stranded for longer than it takes to notice
# and re-parent, counting only gaps after a node has had a route at least once -
# otherwise the figure measures how long the grid took to form, not how long a
# disturbance stranded anybody.
#
# Worth knowing what this test does NOT prove. With both uplinks down there is no
# route to have, and every node should say so. They do not: rank is distance-vector
# and the acyclicity rule only inspects the immediate neighbour, so the remaining
# nodes route through each other and the whole grid holds a finite rank to a sink
# that is gone. That is why the measured worst gap is zero even across a 30 s
# total-sink outage, and why this test passes without demonstrating recovery from
# one. See the routing-loop entry under Known limitations in the README.
#
# Frame authentication must never fail. Re-parenting onto a neighbour that is
# already linked requires no handshake at all - that is the point of keeping the
# long term key - so a sink disappearing must not invalidate a single frame. Note
# this is a weaker claim than "no handshake ever fails": nodes do still try to form
# redundant links while all this is going on, and some of those attempts lose to
# contention for the one handshake context each node has. What is asserted is that
# losing a backhaul costs no re-authentication of traffic already flowing.

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

simulation_id="lockgrid_backhaul_flap"
verbosity_level=2
EXECUTE_TIMEOUT=900
sim_length_us=300e6
check_ms=295000

# Offset so the two uplinks are not down together for the whole of every cycle,
# but do overlap sometimes - a grid with no sink at all has to be survivable too.
flap_period=60000
flap_down=40000

cd ${BSIM_OUT_PATH}/bin

EXE=./bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=0 \
  -lg_addr=1 -lg_role=2 -lg_check=${check_ms} \
  -lg_uplink=40000 -lg_flap=${flap_period} -lg_flap_down=${flap_down} \
  -exp_links=1 -exp_no_authfail=1 -exp_max_gap=200000

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=1 \
  -lg_addr=2 -lg_role=2 -lg_check=${check_ms} \
  -lg_uplink=50000 -lg_flap=${flap_period} -lg_flap_down=${flap_down} \
  -exp_links=1 -exp_no_authfail=1 -exp_max_gap=200000

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=2 \
  -lg_addr=3 -lg_role=1 -lg_period=10000 -lg_check=${check_ms} \
  -exp_links=1 -exp_no_authfail=1 -exp_max_gap=200000

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=3 \
  -lg_addr=4 -lg_role=1 -lg_period=10000 -lg_check=${check_ms} \
  -exp_links=1 -exp_no_authfail=1 -exp_max_gap=200000

Execute $EXE -v=${verbosity_level} -s=${simulation_id} -d=4 \
  -lg_addr=5 -lg_role=1 -lg_period=10000 -lg_check=${check_ms} \
  -exp_links=1 -exp_no_authfail=1 -exp_max_gap=200000

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id} \
  -D=5 -sim_length=${sim_length_us} $@

wait_for_background_jobs
