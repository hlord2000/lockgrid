# LockGrid — working notes

A scheduled-rendezvous mesh protocol for the nRF54L15, built directly on the
`NRF_RADIO` peripheral using the IEEE 802.15.4 O-QPSK PHY at 250 kbps. ~10k lines
in `subsys/lockgrid`, wired in as a Zephyr module. Validated in BabbleSim; nothing
has run on real silicon.

`README.md` is the design document — why the protocol is shaped the way it is. This
file is the operational one: how to build it, how to run it, what breaks, and which
invariants you must not violate. Read both before changing anything in
`subsys/lockgrid`.

## The one metric

**Radio on-time, with a target under 20 µA per node.** Both stated by the user, and
essentially every design decision follows from them. If a change adds receive time,
it needs a justification measured in microamps.

Starting the radio costs ~1289 nC before a single bit moves — pre-processing 29,
crystal ramp 380, settle 300, radio start 240, TX/RX turnaround 172, standby 168 —
against ~3800 nC of air time for a keepalive. So **skipping an event saves far more
than shortening one**, which is why subrating is the biggest lever and why anything
that opens a receive window is scrutinised hardest.

## Build and run

The tree's root `AGENTS.md` says to source `activate-nrf.sh`. In practice
`zephyr/zephyr-env.sh` has been sufficient for these builds; if `west` is not on
PATH, source `activate-nrf.sh` first.

Everything below assumes `BSIM_OUT_PATH` is set and you are in the ncs-main root.

### Test suite (8 tests)

```sh
source zephyr/zephyr-env.sh
export BOARD=nrf54l15bsim/nrf54l15/cpuapp BOARD_TS=nrf54l15bsim_nrf54l15_cpuapp

# Four variants; each test script picks the one it needs by name.
for pair in "build:" "build-contended:overlay-contended.conf" \
            "build-fastchan:overlay-fastchan.conf" "build-nofloor:overlay-nofloor.conf"; do
  d="lockgrid/tests/bsim/lockgrid/${pair%%:*}"; cfg="${pair#*:}"
  [ -n "$cfg" ] && extra="-DEXTRA_CONF_FILE=$cfg" || extra=""
  west build -b $BOARD -d $d lockgrid/tests/bsim/lockgrid -- $extra
  if [ -z "$cfg" ]; then sfx=""; else
    sfx="_overlay-$(echo $cfg | sed 's/overlay-//;s/\.conf//')_conf"; fi
  cp $d/lockgrid/zephyr/zephyr.exe \
     "$BSIM_OUT_PATH/bin/bs_${BOARD_TS}_lockgrid_tests_bsim_lockgrid_prj_conf${sfx}"
done

for t in mesh_form heal late_join interference chanmap contended \
         backhaul_loss backhaul_flap; do
  lockgrid/tests/bsim/lockgrid/tests_scripts/$t.sh || echo "$t FAILED"
done
```

**The binary name matters.** The scripts build a path from `BOARD_TS` and the conf
file name, and the overlay part keeps its hyphen (`overlay-contended_conf`) while
`west build` would produce an underscore. Get it wrong and the script dies with
"command not found", not a useful error.

What each test is for:

| Test | Proves |
|---|---|
| `mesh_form` | Discovery, handshake, scheduling, hopping, routing and forwarding all work at once |
| `heal` | A node going dark is detected and routed around |
| `late_join` | A node arriving at a settled grid can still get in |
| `interference` | Survives a WLAN interferer |
| `chanmap` | Channel evaluation drops bad channels |
| `contended` | Tolerates a fifth of radio requests being refused — the only way to test radio sharing without MPSL |
| `backhaul_loss` | A backhaul that loses its uplink demotes to router and the grid re-routes |
| `backhaul_flap` | Survives backhauls dropping every 60 s indefinitely |

### Scale run and visualisation

```sh
west build -b $BOARD -d lockgrid/tests/bsim/lockgrid/build-trace \
  lockgrid/tests/bsim/lockgrid -- -DEXTRA_CONF_FILE=overlay-trace.conf
cp lockgrid/tests/bsim/lockgrid/build-trace/lockgrid/zephyr/zephyr.exe \
   "$BSIM_OUT_PATH/bin/bs_lockgrid_trace"

lockgrid/tools/gen_scenario.py --layout clustered --nodes 100 --clusters 12 \
  --cluster-cols 4 --uplinks 3 --out /tmp/lgc --sim-length 240 \
  --outages 3 --fades 16 --interferers 2 --uplink-flap 60
/tmp/lgc/run.sh                                    # ~4.5 min wall clock

lockgrid/tools/trace_to_json.py --scenario /tmp/lgc --out /tmp/lgc/trace.json \
  --max-frames 26000
lockgrid/tools/make_viz.py --trace /tmp/lgc/trace.json --out /tmp/lgc/lockgrid.html
```

`gen_scenario.py` is not optional at scale: BabbleSim's default channel gives every
device the same attenuation to every other, so 100 nodes become a single-hop star and
none of the mesh behaviour is exercised. It writes an NxN path-loss matrix, plus
per-path timed attenuation files for outages, fades and interferers.

Two constraints it will warn you about, both of which otherwise look exactly like
protocol bugs: a node that hears nobody, and a cluster with no bridge link. Beyond
about 31 m a link falls under the level at which a handshake is attempted, so cluster
spacing and range must stay inside that.

## Structure

```
include/lockgrid/lockgrid.h   Public API: lg_start, lg_send, lg_tuning_*, callbacks
subsys/lockgrid/
  lg_core.c        Init, send path, runtime tuning
  lg_sched.c       The single event slot, priority classes, contention
  lg_disc.c        Beaconing, duty-cycled scanning, join windows
  lg_join.c        Certificate handshake state machine (largest file)
  lg_link.c        Connection events, subrating, supervision, control traffic
  lg_route.c       Rank, parent selection, gossip, downward routes
  lg_role.c        Runtime leaf/router decision; backhaul is declared, not chosen
  lg_sec.c         AES-CCM, ECDHE/ECDSA P-256 via PSA Crypto
  lg_chan.c        Channel quality evaluation and map maintenance
  lg_neigh.c       Neighbour table
  lg_phy_nrf.c     Direct NRF_RADIO programming
  lg_ts_direct.c   Radio owned outright (plus synthetic refusals for testing)
  lg_ts_mpsl.c     Radio via MPSL timeslots — cannot be exercised in BabbleSim
  lg_power.c       Per-activity energy accounting
  lg_trace.c       Machine-readable LGT trace lines
tools/             gen_scenario.py, trace_to_json.py, make_viz.py
tests/bsim/        Self-asserting test node + 8 scenario scripts
```

Layering: `lg_core` → `lg_sched` → (`lg_disc`, `lg_join`, `lg_link`) → `lg_phy_nrf`
→ `lg_ts_*`. `lg_route`, `lg_role`, `lg_chan`, `lg_neigh` are consulted by the middle
layer; `lg_sec` and `lg_power` are leaf services.

## Invariants — each of these was learned by breaking it

**Timing and scheduling**

- The event counter must be a **function of time** (`(anchor - epoch + interval/2) /
  interval`), never incremented locally. One skipped event otherwise desynchronises
  the hop sequence permanently.
- Subrating must select events by **counter modulo factor**, not by stepping, or the
  two ends drift onto disjoint event sets.
- Freeze the RX window width at plan time. Recomputing it at event start can open a
  window that has already passed.
- A parameter change the initiator originates must not take effect until the peer has
  been seen to reply. Apply-on-schedule-regardless splits the link.

**Handshake**

- Rank decides who initiates. There is one handshake context per node, so if both
  ends decide to be the joiner, neither can be the router and both attempts die.
- Direction comes from protocol state (`hs.owe_tx`), not slot parity, so a dropped
  slot is retried rather than fatal.
- **A refused radio slot is a slot that produced nothing, never a failure.** This
  was fatal once and meant a node sharing the radio could never authenticate.
- The slot budget counts **slots with no forward progress**, not elapsed slots. The
  slot index is clock-derived so both ends agree which slot is which, which means it
  advances whether or not this node got the radio — failing on it turned the budget
  into a wall-clock deadline. A retransmission is not progress.
- Joiners pick **discrete** contention sub-slots. A uniform random offset in a window
  barely wider than one fragment train guarantees collisions.
- Handshake events need **scheduler priority** over beacons and scans, or a dense grid
  starves them (25 nodes went from 9/25 routed to 25/25 by adding priority classes).

**Discovery and power**

- **Open a join window only on evidence a joiner exists, never because this node
  wants more links.** Gating it on "short of links" kept a window open after every
  beacon for ever — 122 µA of a router's 125 µA. Wanting links is satisfied by
  scanning and initiating; the worse-ranked end initiates, so listening gains a
  link-hungry node nothing.
- The `joiner_hint` needs a rank test **and** a `link_count < MIN_LINKS_TARGET` test,
  or every router in earshot refreshes every other's hint just by beaconing. Include
  equal rank, or peer routers never form the second link redundancy depends on.
- **Beacon rate and join-window rate are different questions.** Transmitting is an
  announcement costing ~1 ms; a join window is listening costing tens. Gating both on
  one predicate meant an unrouted node stretched its beacons by the idle factor — but
  an unrouted node's beacon is exactly how it asks to be adopted, and a 100-node grid
  formed 12 links.
- A beacon must not advertise a join window the router will not open. Decide at
  staging time and advertise `join_window_us = 0` when not offering.

## Platform traps

- **nRF54L `TXPOWER` is an encoded register value, not dBm.** Writing 0 selects
  minimum power (~−53 dBm at the antenna, giving −113 dBm RSSI). Needs the lookup
  table in `lg_phy_nrf.c`.
- **Bluetooth CSA#2 degenerates modulo 16.** 17·a ≡ a mod 16, so with 16 channels
  every event lands on the same one. Bluetooth hides this by reducing mod 37; we add
  an avalanche step (`mix16()`).
- **BabbleSim presets every unsigned option to `UINT_MAX`** before parsing, so "not
  given" is not zero. Use `arg_or()` / `arg_given()`.
- **The BabbleSim RADIO model writes the RX payload at packet end**, so BCMATCH-based
  early frame filtering reads a stale buffer. Valid on hardware, unverifiable in
  simulation — `LOCKGRID_EARLY_ABORT` defaults off on the bsim board.
- **PSA crypto resolves to Mbed TLS on bsim**, because nrf_security's accelerated
  backend is Cortex-M only. Needs `CONFIG_PSA_CRYPTO=y` plus the `PSA_WANT_*` set;
  `CONFIG_NRF_SECURITY` is not user-selectable.
- **MPSL/SoftDevice Controller cannot be simulated in BabbleSim at all** — it needs
  internal resources. Not a code defect; a feature request is open. Use
  `LOCKGRID_TS_REFUSE_PERMILLE` in the direct backend to synthesise refusals instead.

## Test-harness traps

- **`bs_trace_error_line_time()` terminates the process but leaves the exit status at
  ZERO.** A suite built on it reports green while failing. Report the failure as a
  warning and call `nsi_exit(1)`. Verify by deliberately failing an expectation and
  checking the exit code.
- **Fixed-size trace lines truncate silently.** `lg_trace_emit()` had a 160-byte
  buffer and ignored `vsnprintk`'s return, so adding one field to the `pwr` line cost
  the last two fields of every power sample and the parser reported plausible
  nonsense. The buffer is 224 now and truncation emits an explicit `truncated
  event=... need=...` line that `trace_to_json.py` counts and warns about.
- **A failing expectation may describe the old topology, not a regression.** After
  roles became runtime-chosen, `mesh_form` failed on `exp_links=2` for the backhaul,
  because a third node now promotes itself and absorbs that link — correct behaviour.
  Check which before debugging.

## Measuring power correctly

Two ways the numbers lie, both of which made the protocol look several times better
than it was:

- **`steady_na` subtracts *all* scan and join charge**, not just the initial burst.
  Written for a settled node, where scanning really is one-off. Where nodes keep
  losing their route it removes most of the real consumption.
- **Boot averages hide convergence.** A node that scanned hard for two minutes before
  finding a route carries that in its average for the rest of the run.

Use windowed deltas instead: the `pwr` trace line carries cumulative `total_nc`, and
current over any window is `(nc2 - nc1) / (t2 - t1) * 1000` µA — nC per µs is mA.
This reuses the firmware's own energy model rather than introducing a second set of
constants. `trace_to_json.py --window-s` does it per node.

**Read the median, not the mean.** A node with no route receives almost continuously
by design and sits near 1 mA, so it drags the mean far from anything typical.

Last measured, quiet window of a 240 s 100-node clustered run: leaf 12.4 µA, router
22.0, backhaul 15.6, median 18.5 — with flapping backhauls, median 24.7.

## Known gaps

- **A grid that loses every backhaul at once does not learn it.** Rank is
  distance-vector and the acyclicity rule compares only against the immediate
  neighbour, so it cannot see a loop longer than one hop. The remaining nodes route
  through each other, each holds a finite rank, and `LG_EVT_ROUTE_LOST` never fires.
  Measured in `backhaul_flap.sh`: both uplinks down 30 s, not one node reported
  losing its route. Mitigated by triggered advertisements and `LOCKGRID_MAX_RANK`;
  the real fix is for an advertisement to name the backhaul it terminates at plus a
  sequence number only that backhaul increments. Changes the advert layout, not done.
- **One handshake at a time per node.** In a dense cluster this is the binding
  constraint on formation speed, and every failed handshake is a join window the
  router already paid for.
- **`lg_ts_now_us()` is internal**, so an application cannot schedule against the
  network timebase. Blocks execute-at-timestamp use cases (synchronised lighting).
- **Nothing has run on hardware.** Energy figures are the user's per-phase constants
  applied to simulated activity, not power-analyser measurements. MPSL needs silicon.

## If you change the protocol

Run all eight tests, then a 100-node scale run, then compare windowed current per
role against the figures above. A change that improves one and silently regresses the
other is the normal failure mode — the join-window fix in particular went through
three wrong versions that each passed some tests and broke grid formation.
