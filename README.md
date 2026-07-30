# LockGrid

A low power, self-healing 2.4 GHz mesh for the nRF54L15, built directly on the
RADIO peripheral and validated in BabbleSim.

LockGrid is a **scheduled-rendezvous** mesh. Nodes find a small set of neighbours
by occasional scanning, authenticate each of them with a certificate-backed
handshake, and then hold BLE-like periodic connection events with them. Routing
is a rank-based tree towards one or more backhaul nodes, with the metric built
from measured delivery rather than hop count.

Everything after the handshake is encrypted and authenticated with a pairwise
link key, so **if two nodes are exchanging traffic, they have authenticated each
other** — there is no unauthenticated data path.

---

## The design constraint that shapes everything

Receive time is the dominant cost in a mesh protocol, and the primary metric this
implementation is tuned against is **radio on-time**. Transmitting a frame costs
a bounded, known amount of energy. Listening costs whatever you were willing to
wait, and a protocol that listens speculatively will lose to one that knows when
to wake up.

Every significant design decision below follows from that.

### Measured result

Three nodes (backhaul, router, leaf), 180 s of simulated time, two links each,
200 ms connection interval, subrate settled at ×16:

| Node | Role | Steady state | Including discovery |
|---|---|---|---|
| 0x0001 | backhaul | **11.2 µA** | 20.7 µA |
| 0x0002 | router | **14.6 µA** | 47.9 µA |
| 0x0003 | leaf | **9.6 µA** | 25.3 µA |

Steady state excludes scanning and joining, which are paid once at power-on;
"including discovery" amortises them over only 180 s, so it falls as the run
lengthens. Zero supervision timeouts, receive windows settled at 280–586 µs.

The energy model charges the real fixed cost of starting the radio, not just air
time — this matters more than it sounds:

| Phase | | Charge |
|---|---|---|
| Pre-processing | 13 µs @ 2.2 mA | 29 nC |
| Crystal ramp | 380 µs @ 1.0 mA | 380 nC |
| Settle | 500 µs @ 0.6 mA | 300 nC |
| Start radio | 100 µs @ 2.4 mA | 240 nC |
| TX→RX turnaround | 143 µs @ 1.2 mA | 172 nC |
| Standby between halves | 58 µs @ 2.9 mA | 168 nC |
| **Fixed per event** | **1 194 µs** | **1 289 nC** |

Against ~3.8 µC of air time for a keepalive, that fixed cost is a sixth of an
idle connection event — and it is paid whether the event carries a payload or
nothing at all. Two consequences drive the design:

- **Skipping an event saves the whole 8.2 µC**, not just the air time. A link at
  every event costs ~41 µA; at ×16 it is under 3 µA. Subrating is by a wide margin
  the largest lever in the protocol, which is why the default ceiling is 16 rather
  than something conservative.
- **Shortening the interval is disproportionately expensive**, because the fixed
  cost is paid more often while the payload does not change. Crystal ramp and
  settle alone are 680 nC, 53 % of the fixed cost;
  `CONFIG_LOCKGRID_PWR_HFXO_ALWAYS_ON` models keeping HFXO running, which is only
  worth it below roughly 100 ms intervals.

> An earlier version of this model charged only 40 µs of ramp at 2.7 mA — 108 nC
> against the real 1 289 nC, a 12× understatement per event. It never modelled the
> crystal ramp, the settle or the turnaround. Every figure above is from the
> corrected model. The correction came from Nordic's own per-phase measurements.

### Where the reduction came from

1. **Narrow, drift-sized receive windows.** Every received frame is timestamped
   from the radio's `END` event and the frame's known air time, so the responder
   re-derives its anchor exactly and the window collapses to a floor of a few
   hundred microseconds. Widening is proportional to time since the last
   successful synchronisation, not to a worst case. Measured synchronisation
   error on a healthy link is 0–1 µs.

2. **Asymmetric windows.** Only the responder pays for clock uncertainty. The
   initiator's window stays at the floor permanently, because the responder
   replies at a time it computed from a frame it had just measured.

3. **Beacons that advertise their own next occurrence.** `struct lg_beacon`
   carries `next_beacon_offset_us`. A node that has heard a router once opens a
   3 ms window at the predicted time instead of a 30 ms blind scan — and while it
   has a live prediction, blind scanning drops to the occasional rate. This is
   also what makes re-attaching to a lost router cheap.

4. **Phase-locked subrating.** When both ends are idle the initiator raises a
   subrate factor and only events whose counter is a multiple of it are used.
   Steady state runs at ×16, so fifteen out of sixteen events cost nothing at all
   — including their fixed radio-start cost, which is what makes this the single
   biggest saving available. The ceiling is clamped at runtime against the
   supervision timeout, since a link whose events are spaced further apart than
   the timeout can tolerate would die on its first collision.

5. **Beacon back-off.** In a converged grid, beaconing is the largest remaining
   consumer and nobody is listening. It stretches by
   `CONFIG_LOCKGRID_BEACON_IDLE_FACTOR` until an unlinked node is heard again.
   This cut beacon transmit time 12× in the measurement above.

6. **Join windows only when plausible.** A join window is 20 ms of receive.
   Opening one after every beacon would cost a router more than all its links put
   together, so it opens only when an unlinked node has been heard recently or the
   node is itself short of links.

7. **Fast ramp-up with software-scheduled turnarounds.** The radio's hardware
   inter-frame-spacing machine forces normal ramp-up, which costs 90 µs more on
   *every* radio event. LockGrid uses fast ramp-up (40 µs) and schedules each
   turnaround itself.

8. **Events end the moment they are done.** Nothing sits in receive waiting for
   traffic that is not coming. "More data" expedites the *next* event rather than
   extending the current one, which also keeps every radio reservation short —
   important when sharing the radio with Bluetooth.

---

### At scale

100 nodes on a jittered 10 m grid, two backhauls, 44 leaves, mean 6.5 audible
neighbours, 150 s:

| Time | Routed | Links | Mean duty |
|---|---|---|---|
| 5 s | 6 / 100 | 4 | 36.9 % |
| 55 s | 79 / 100 | 100 | 21.6 % |
| 130 s | **94 / 100** | 141 | 14.2 % |

796 forwards and 208 deliveries, so multi-hop routing is carrying real traffic;
depth reaches five hops. The duty figure falls as nodes attach, because an
unattached node deliberately spends most of its time receiving and an attached one
barely any.

Two defects only appeared at this scale, both invisible with three nodes:

- **The scheduler had no priority classes.** A handshake needs four radio events
  40 ms apart, and every slot lost to a beacon or a scan cost it two slots of retry
  budget. In a dense grid that starved handshakes out entirely — 25 nodes reached
  only 9/25 routed. With priority classes it is 25/25 by 80 s.
- **Joiners picked a uniformly random offset** inside a contention window barely
  wider than one fragment train, so two joiners almost always collided. They now
  pick discrete sub-slots that cannot overlap.

Of the six nodes still unrouted at 130 s, two have no router at all within range —
unroutable by construction rather than by protocol. Link slots were the other
binding constraint: at this density routers reach the default four links long
before everyone has a parent, which is why the scale overlay raises
`LOCKGRID_MAX_LINKS` to eight.

## Layers

```
        lg_core.c      node state, transmit queue, inbound dispatch
        lg_sched.c     the single-event scheduler
   ┌────────────────┬──────────────┬───────────────┬──────────────┐
   │ lg_link.c      │ lg_disc.c    │ lg_join.c     │ lg_route.c   │
   │ connection     │ beacons and  │ certificate   │ rank, parent │
   │ events         │ scanning     │ handshake     │ selection    │
   └────────────────┴──────────────┴───────────────┴──────────────┘
        lg_neigh.c  lg_chan.c  lg_sec.c  lg_frame.c  lg_power.c
   ┌──────────────────────────────────────────────────────────────┐
   │ lg_phy_nrf.c        RADIO programming, absolute-time TX/RX   │
   ├──────────────────────────────────────────────────────────────┤
   │ lg_ts_direct.c  │  lg_ts_mpsl.c    radio arbitration          │
   └──────────────────────────────────────────────────────────────┘
```

**Threading.** The radio runs entirely from interrupt context. Anything that
takes more than a few microseconds — public key operations, routing decisions,
application callbacks, even AES — is deferred to the LockGrid work queue. Frames
a link is about to send are built and encrypted *before* the event, so the
interrupt path only has to point the radio at a buffer.

**Scheduling.** There is exactly one radio event outstanding. Everything that
wants the radio publishes the absolute time it next needs it and the earliest
wins. There is no polling loop and no periodic tick. A node holding more links
than it can schedule loses events, and those losses are charged to the link's
metrics like lost frames — so the routing metric notices, and the topology
spreads out instead of piling onto one router.

---

## PHY

IEEE 802.15.4-2006 O-QPSK DSSS, 250 kbps, channels 11–26. 32-bit zero preamble,
one-byte SFD, 8-bit length including the two CRC bytes, LockGrid PDU directly
after it. Maximum PDU 125 bytes.

Air time is exactly `(len + 8) × 32` µs, which is what makes the timestamp
arithmetic in `lg_phy_nrf.c` exact and therefore what makes narrow windows
possible.

> The LE Coded PHY was in the original design as a long-range alternate and was
> removed on request, since the 802.15.4 PHY already delivers comparable
> sensitivity. Nothing in the MAC assumes a single PHY, so re-adding it is
> localised to `lg_phy_nrf.c` and the air-time macro.

## Channels

The map starts as all 16 channels and is driven by measurement, not a static
list. Every frame outcome is charged to the channel it happened on; a channel
whose error rate crosses `CONFIG_LOCKGRID_CHAN_BAD_PER_PERMILLE` is retired, and
retired channels are re-probed with the radio's energy detect block so the map
recovers when an interferer moves.

The hop sequence is Bluetooth's channel selection algorithm #2 over the live map,
so both ends derive it from the event counter and a shared seed without messaging.

> One non-obvious detail: CSA#2's multiply-and-add step contributes nothing to the
> low four bits (17·a ≡ a mod 16), and Bluetooth hides this by reducing modulo 37,
> a prime. With at most 16 channels the reduction is modulo a power of two, and
> without an added avalanche step *every event lands on the same channel*. See
> `mix16()` in `lg_chan.c`.

Map changes take effect at a shared event counter, and — like every parameter
change — the side that originated it will not apply it until the peer has been
seen to reply. An unconfirmed change is abandoned rather than applied.

## Security

Two key layers:

- **Grid key**, provisioned, shared by every node. Authenticates beacons and lets
  a node discard an outsider's traffic cheaply. It authenticates nothing:
  possessing it does not let anyone form a link.
- **Pairwise link key**, established per neighbour by mutually authenticated
  ephemeral Diffie-Hellman.

```
slot 0   joiner → router   nonce_j, eph_pub_j, cert_j
slot 1   router → joiner   nonce_r, eph_pub_r, cert_r, sig_r(transcript)
slot 2   joiner → router   sig_j(transcript)                    encrypted
slot 3   router → joiner   link parameters                      encrypted
```

The transcript binds both nonces, both ephemeral public keys, both addresses and
the network id. `link_key = HKDF(ECDH(eph_j, eph_r), salt=transcript)`. All
P-256 / SHA-256 / AES-CCM through the PSA Crypto API.

**The address is in the certificate**, so an address cannot be claimed without
the matching private key. Identity and address are the same fact.

Data frames carry a 4-byte CCM tag, a monotonic frame counter and a sliding
replay window. The nonce is `(source address, format byte, counter)` — deliberately
*not* derived from the link role, because roles are only assigned once the link is
established and the last two handshake messages are already encrypted by then.

**Re-attaching skips all of it.** The long term key from the original handshake is
kept, and one round trip re-derives a fresh session key from it. That is what
makes healing cheap.

Honest notes on the security model:

- Link-layer acknowledgement inside an event is based on CRC, not the tag, the
  same tradeoff Bluetooth makes. A frame that later fails its tag is counted as an
  error rather than accepted.
- Beacons are readable by anyone (a joiner has to be able to act on one) and
  MIC'd with the grid key. A joiner rate-limits handshake attempts per source, so
  a forged beacon costs it a bounded amount of work.
- `CONFIG_LOCKGRID_SIM_PROVISIONING` compiles a CA **private** key into the image
  so a node can issue itself a certificate at boot. It is a development
  convenience — everything downstream is the real code path — and it must never be
  set on a product.

## Routing

Rank is the cost of the cheapest path to a backhaul, zero at a backhaul. Link
cost is built from ETX (a moving average of delivery outcomes, the dominant term),
an RSSI penalty near the noise floor, and refused radio time. Three rules keep it
stable: a parent's rank must be strictly lower than the rank it would give us
(acyclicity); a parent is only replaced when the alternative is better by a
margin; and more than one parent is kept, so failover costs nothing.

Rank also decides **who initiates a handshake** — the node with the worse rank
does. That is not cosmetic: there is one handshake context per node, so if both
ends decide to be the joiner simultaneously, neither can act as the router and
both attempts die.

Route advertisements carried on existing links let a settled node learn the
topology without spending any receive time, and turn "find a better parent" from
open-ended scanning into a short scan that stops as soon as the wanted address is
heard.

## Choosing a role

No node is told whether it is a leaf or a router. The right answer depends on where
the node turns out to be, which is not knowable when it is provisioned, so the node
decides at runtime and can change its mind. `lg_start()` takes no role argument.

The certificate does not carry one either. A certificate admits a node to the
network — that is all it can honestly say, because what the node should then do
depends on its neighbours, not on its identity.

Backhaul is the exception, and not really an exception at all: terminating traffic
means having somewhere to terminate it, which is a property of the hardware. The
application declares it with `lg_backhaul_set(true)` and withdraws it when the
uplink drops, at which point the node keeps routing but stops claiming to be a sink.

The leaf-versus-router decision is a cost question. A router pays for beacons, join
windows, more links and other nodes' traffic; a leaf pays for one uplink. So leaf is
the default, and a node only takes the role on when the grid gets something for it:

- it hears fewer than `LOCKGRID_ROUTER_DENSITY_TARGET` routers, meaning the area is
  thin and its neighbours may have nowhere to attach, or
- it can hear a node with no route at all, which is a direct request for a parent.

It gives the role back when neither holds and nothing is routing through it. The
thresholds differ in each direction and a 15 s dwell applies, so a node sitting on
the boundary cannot oscillate. Promotion also requires having a route already: a
router with nowhere to forward to is worse than useless, because neighbours will
attach to it and believe they have a path.

The consequence is that the grid grows outward from the backhauls without anything
coordinating it. Nodes beside a backhaul attach, find they are the only router in a
thin area, promote, and become the way the next ring attaches. An unrouted node
beacons even as a leaf, which is what makes it visible to be adopted.

## Runtime tuning

The parameters that decide steady-state current are changeable while the node is
running, through `lg_tuning_set()`: connection interval for new links, subrate
ceiling, supervision timeout, the three scan rates, scan window, beacon interval
and its idle multiplier, and the join-window floor. Kconfig supplies the defaults.

Values are clamped to what the protocol can honour — most importantly the subrate
against the supervision timeout — and the result can be read back with
`lg_tuning_get()`. The main trade the API exposes is latency for battery: raising
the supervision timeout allows a higher subrate, at the cost of taking longer to
notice a neighbour has gone.

```c
struct lg_tuning t;

lg_tuning_get(&t);
t.conn_interval_us = 500000;   /* 500 ms */
t.sup_timeout_us   = 30000000; /* 30 s, which permits a higher subrate */
t.subrate_max      = 16;
lg_tuning_set(&t);             /* clamped, then applied from the next event */
```

## Radio arbitration

Two backends behind `lg_ts.h`:

- **`lg_ts_direct.c`** — LockGrid owns RADIO and two GRTC compare channels. Every
  request is granted at exactly the requested microsecond. **This is what all the
  results above were measured with.**
- **`lg_ts_mpsl.c`** — each radio event becomes an MPSL timeslot, so LockGrid can
  share the chip with a Bluetooth LE stack. Refused slots are reported upward and
  folded into the link's ETX exactly like lost frames, which is the right
  behaviour: a neighbour whose events keep losing to Bluetooth really is a worse
  route.

**Status of the MPSL backend:** it builds and links for `nrf54l15bsim`, the
timeslot session opens and `mpsl_timeslot_request()` returns success, but no grant
ever arrives. That is not a defect in this code — **MPSL and the SoftDevice
Controller cannot run under BabbleSim**, because they need access to internal
resources. Nordic is raising a feature request for it. The backend therefore needs
real hardware to validate.

What *can* be validated without MPSL is the property that actually matters: that
nothing above `lg_ts.h` assumes it will get the radio time it asked for.
`CONFIG_LOCKGRID_TS_REFUSE_PERMILLE` makes the direct arbiter refuse a configurable
fraction of requests, at the moment they were due, reported exactly as a cancelled
timeslot would be. The `contended` test runs the grid with a tenth of all requests
refused and requires every node to still authenticate and route.

That test found a real defect, and precisely the one worth finding: a single
refused slot used to abort the entire handshake. A node sharing the radio could
therefore never authenticate at all — one lost slot out of the dozen a handshake
needs was enough to abandon the attempt, and the retry was just as likely to lose
another. A refusal is now treated as exactly what it is, a slot that produced
nothing, which the retry logic already handled.

---

## Building and running

```sh
source /opt/ncs/sdks/ncs-main/activate-nrf.sh
```

Sample node:

```sh
west build -b nrf54l15bsim/nrf54l15/cpuapp -d build lockgrid/samples/lockgrid_node
cp build/lockgrid_node/zephyr/zephyr.exe $BSIM_OUT_PATH/bin/bs_lockgrid_node
```

Three nodes for 60 s:

```sh
cd $BSIM_OUT_PATH/bin
./bs_lockgrid_node -s=demo -d=0 -lg_addr=1 -lg_role=2 -lg_report=60000 &
./bs_lockgrid_node -s=demo -d=1 -lg_addr=2 -lg_role=1 -lg_period=10000 -lg_report=60000 &
./bs_lockgrid_node -s=demo -d=2 -lg_addr=3 -lg_role=0 -lg_period=10000 -lg_report=60000 &
./bs_2G4_phy_v1 -s=demo -D=3 -sim_length=61e6
```

Node arguments: `-lg_addr`, `-lg_role`, `-lg_period`, `-lg_report`, `-lg_stop`,
`-lg_snap`.

`-lg_role` is a *capability*, not an assignment: 0 never routes, 1 may route,
2 may also terminate traffic. No node is told to be a router. Every node starts
as a leaf and decides for itself, at runtime, whether to take the role on — see
[Choosing a role](#choosing-a-role). Passing 2 makes the node call
`lg_backhaul_set(true)`, which is the application declaring that its uplink is up.

Test suite:

```sh
export BOARD=nrf54l15bsim/nrf54l15/cpuapp
west build -b $BOARD -d build lockgrid/tests/bsim/lockgrid
cp build/lockgrid/zephyr/zephyr.exe \
   $BSIM_OUT_PATH/bin/bs_nrf54l15bsim_nrf54l15_cpuapp_lockgrid_tests_bsim_lockgrid_prj_conf

for t in mesh_form heal late_join interference chanmap contended \
         backhaul_loss backhaul_flap; do
  ./lockgrid/tests/bsim/lockgrid/tests_scripts/$t.sh || echo "$t FAILED"
done
```

Each node evaluates its expectations at the end of the run, reports any that were
not met, and exits non-zero so the runner propagates the failure — a test that
cannot fail is worse than no test.

> Worth knowing if you write bsim tests: `bs_trace_error_line_time()` prints an
> ERROR and terminates the process, but leaves the **exit status at zero**. A suite
> built on it reports green while failing. These tests report the failure as a
> warning and then call `nsi_exit(1)`.

## Simulating at scale, and watching it

`tools/gen_scenario.py` lays out N nodes and writes the path-loss matrix the phy
needs. This part is not optional: BabbleSim's default channel gives every device
the same attenuation to every other, so 100 nodes become a single-hop star and
none of the mesh behaviour is exercised.

```sh
# Build the traced binary. overlay-trace.conf turns the trace on and raises the
# neighbour and route tables, which a dense grid needs.
west build -b nrf54l15bsim/nrf54l15/cpuapp -d build-trace lockgrid/tests/bsim/lockgrid \
  -- -DEXTRA_CONF_FILE=overlay-trace.conf
cp build-trace/lockgrid/zephyr/zephyr.exe $BSIM_OUT_PATH/bin/bs_lockgrid_trace

# 100 nodes in 12 clusters, 3 with a backhaul uplink on the left, 240 s, with the
# radio environment deliberately disturbed while it runs.
lockgrid/tools/gen_scenario.py --layout clustered --nodes 100 --clusters 12 \
  --cluster-cols 4 --uplinks 3 --out /tmp/lgc --sim-length 240 \
  --outages 3 --fades 16 --interferers 2
/tmp/lgc/run.sh

# Turn the traces into a browser view.
lockgrid/tools/trace_to_json.py --scenario /tmp/lgc --out /tmp/lgc/trace.json
lockgrid/tools/make_viz.py --trace /tmp/lgc/trace.json --out /tmp/lgc/lockgrid.html
```

Two layouts. `grid` is a jittered lattice of uniform density. `clustered` puts
groups of nodes on a lattice with open ground between them, so the grid can only be
joined up through the few marginal links between adjacent clusters — which is where
the interesting failures happen. A two-dimensional arrangement of clusters is
deliberate: a chain would mean severing one boundary strands everything beyond it,
so every outage would look the same and only ever test total loss.

Three ways to disturb it, all built from the channel model's timed attenuation
files, which is more useful than adding noise alone because a path can be faded
right out and brought back:

| Flag | What it does |
|---|---|
| `--outages N` | Sever every path across a cluster boundary for `--outage-len` seconds |
| `--fades N` | Fade individual links, taking a parent away without taking the route away |
| `--interferers N` | WLAN-shaped interferers that come and go, so the noise floor varies |

The windows are written into `positions.json`, so the visualisation marks them on
the timeline and a burst of reconnections can be read against its cause instead of
guessed at.

`gen_scenario.py` reports the audibility its layout produced, and warns if a node
hears nobody or if a cluster has no bridge at all — both of which otherwise look
exactly like protocol bugs. Spacing sets the link budget, and it is the one thing
worth checking before blaming the protocol: beyond about 31 m a link falls under the
level at which a handshake is attempted, so cluster spacing and range have to stay
inside it or the grid simply never forms.

The HTML is self-contained with the trace embedded: topology graph, packets
animating along the links, a timeline you can scrub, and a per-node inspector
showing where each node's radio time went. Role is drawn as shape and link quality
as line weight, so colour is free to carry a measurement. The role drawn is the one
the node had chosen for itself at that point on the timeline, so shapes change as
the run progresses.

Current is reported over a sliding window rather than averaged from boot. In a run
that takes a couple of minutes to converge the two differ enormously: a node that
scanned hard for two minutes before finding a route carries that cost in its boot
average for the rest of the run, however cheap it becomes afterwards. The window
answers the question actually being asked — what is this node costing now — and it
makes the energy cost of a disturbance visible, because the figure climbs while the
grid repairs itself and falls back when it is done. It is computed by differencing
the firmware's own cumulative charge, so it introduces no second energy model to
disagree with the first.

`CONFIG_LOCKGRID_TRACE` is what produces the data — one `LGT` line per event with
its own microsecond timestamp, emitted with `printk` so a line is atomic even from
the radio interrupt. `CONFIG_LOCKGRID_TRACE_FRAMES` adds a line per frame, which is
the bulk of it.

## Configuration

The knobs that matter most for radio on-time:

| Option | Default | Effect |
|---|---|---|
| `LOCKGRID_CONN_INTERVAL_MS` | 200 | Events per second per link |
| `LOCKGRID_SUBRATE_MAX` | 16 | How much an idle link backs off — the biggest lever |
| `LOCKGRID_RX_WINDOW_MIN_US` | 200 | The window floor; paid on every event of every link |
| `LOCKGRID_CLOCK_ACCURACY_PPM` | 250 | How fast windows widen between syncs |
| `LOCKGRID_MAX_LINKS` | 4 | Each link is a periodic receive window |
| `LOCKGRID_BEACON_IDLE_FACTOR` | 16 | Beacon back-off once nobody needs us |
| `LOCKGRID_SUPERVISION_TIMEOUT_MS` | 20000 | Bounds both healing latency and the subrate ceiling |
| `LOCKGRID_SCAN_INTERVAL_*_MS` | 50 / 3000 / 60000 | The three scanning tiers |
| `LOCKGRID_ROUTER_DENSITY_TARGET` | 2 | Routers a node wants to hear before it stops volunteering to be one |
| `LOCKGRID_MIN_LINKS_TARGET` | 2 | Links a node holds before it stops looking; also gates join windows |
| `LOCKGRID_MAX_RANK` | 512 | Depth ceiling; bounds a routing loop but does not collapse it |

PSA crypto resolves to Mbed TLS on the simulated board, because nrf_security's
accelerated backend is Cortex-M only. On real nRF54L hardware the same
`PSA_WANT_*` set is served by CRACEN through nrf_security with no change to
LockGrid's source.

## Known limitations

- `CONFIG_LOCKGRID_EARLY_ABORT` uses the radio bit counter to reject a foreign
  frame mid-reception, cutting a 4.3 ms receive to under 400 µs. It relies on
  EasyDMA filling the buffer as bytes arrive, which hardware does but the
  BabbleSim RADIO model does not — the model writes the payload at packet end, so
  the header is not in memory when the bit counter fires. **Defaulted off on the
  simulated board**, which makes the simulated figures for overheard traffic
  pessimistic relative to hardware. Verifying this needs real silicon.
- **A grid that loses every backhaul at once does not learn it promptly.** Rank is a
  distance-vector metric and the acyclicity rule compares only against the immediate
  neighbour's advertised rank, which cannot see a loop more than one hop long. When
  the last sink disappears the remaining nodes route through each other: each one's
  neighbour still advertises a finite rank, so each takes a finite rank, and the
  whole grid believes in a path that no longer exists. Traffic is forwarded around
  the loop until its hop count runs out, and `LG_EVT_ROUTE_LOST` never fires.
  Measured in `backhaul_flap.sh`: two backhauls, both uplinks down for 30 s, and not
  one node reported losing its route.

  Mitigated but not fixed. Rank changes are now pushed on the next link event rather
  than waiting for the 30 s advertisement timer, so genuine news travels one
  connection interval per hop instead of thirty seconds per hop; and
  `LOCKGRID_MAX_RANK` stops the inflation running to infinity. Neither makes the
  loop collapse quickly — the ranks climbed about 48 per 18 s, so any useful ceiling
  is minutes away. The fix is for an advertisement to name the backhaul it
  terminates at and carry a sequence number only that backhaul increments, so a node
  can distinguish a live root from a remembered one. That changes the advertisement
  layout and has not been done.
- One handshake at a time per node. Two nodes joining the same router serialise,
  and in a dense cluster where a node can hear fifteen others this is the binding
  constraint on how fast a grid forms: 100 nodes take ~130 s, and a joiner talking
  into the window of a router already busy with somebody else gets nothing and
  retries. It is also why a router's cost has a long tail — every failed handshake
  is a join window the router had already paid to open.
- A node with no route receives almost continuously, by design: nothing else it
  could do with that energy is worth anything. It costs close to 1 mA while it
  lasts, so a node that cannot be routed at all — one exists in the 100-node
  clustered layout, with no router in range — never becomes cheap. Read the median
  rather than the mean when judging a grid's consumption.
- A single event slot means colliding link events are skipped rather than
  overlapped. Deliberate, but it caps how many links one node can usefully hold.
- Multi-hop forwarding is exercised to five hops in the 100-node scenario.
- The path-loss model in `gen_scenario.py` cuts paths off at a hard radius. Real
  propagation has no edge; the cutoff exists so a topology is reproducible and
  tunable rather than a matter of luck.
- The energy figures are a model driven by per-phase current and duration
  constants, not measurements of a running chip. The phase values are Nordic's
  nRF54L15 figures, and the ratios between receive, transmit and the fixed
  start-up cost are what the design decisions rest on — but nothing here has been
  confirmed against a power analyser.
- Channel retirement needs minutes of evidence at production settings, because a
  subrated link on a 200 ms interval touches any given channel only a few times a
  minute. That is correct behaviour, but it means the feature is only observable in
  a reasonable test run under `overlay-fastchan.conf`.
- Nothing has run on real hardware.
