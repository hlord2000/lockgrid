#!/usr/bin/env python3
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

"""Generate a multi-node LockGrid scenario for BabbleSim.

BabbleSim's default channel gives every device the same attenuation to every
other device, which turns any number of nodes into a single-hop star: everybody
hears everybody, nothing has to be routed, and none of the mesh behaviour is
exercised. To get a real topology the phy needs a path-loss matrix, which is what
this script produces alongside the runner.

Two layouts:

  grid        jittered lattice, uniform density
  clustered   groups of nodes with open ground between them, so the grid can only
              be joined up through a few marginal links between adjacent clusters.
              Those bridges are where the interesting failures happen.

Nodes are never told what role to play. The generator only decides which of them
have a backhaul uplink; everything else works out at runtime whether it should be
a leaf or a router.

Time-varying conditions are produced with the channel model's timed attenuation
files, which is more useful than adding noise alone: a path can be faded right out
and brought back, so links genuinely drop and nodes have to find another parent.

  --outages N     sever every path across a cluster boundary for a while, which
                  costs everything beyond it its route
  --fades N       fade individual links, which forces a parent change without
                  taking the route away
  --interferers N WLAN-shaped interferers that come and go

Outputs, all into the target directory:
  positions.json     node id, uplink flag and coordinates, for the visualisation
  att_matrix.txt     the NxN path-loss matrix for the multiatt channel
  timed/*.txt        per-path attenuation over time, for fades and outages
  run.sh             spawns the phy, every node and any interferers

Example:
  ./gen_scenario.py --layout clustered --nodes 100 --out /tmp/lg100 \\
      --sim-length 240 --outages 2 --fades 12 --interferers 2
  /tmp/lg100/run.sh
"""

import argparse
import json
import math
import os
import random
import stat

# Log-distance path loss. n=3.0 is a reasonable indoor/cluttered figure: 2.0 is
# free space, 4.0 is heavily obstructed.
REF_DIST_M = 1.0
REF_LOSS_DB = 40.0
PATH_LOSS_EXPONENT = 3.0

# Attenuation given to a path beyond the configured range.
#
# Paths are cut off at a hard radius rather than left to fade out. That is a
# simplification - real propagation has no edge - but it is the one that makes a
# topology reproducible and tunable, which is the point of the exercise.
OUT_OF_RANGE_DB = 100.0

# Beyond about 31 m the received signal falls under the level at which LockGrid
# will start a handshake (CONFIG_LOCKGRID_CHAN_NOISE_FLOOR_DBM minus 10, so
# -85 dBm), so ranges above that produce a grid that never forms and looks for all
# the world like a protocol bug. Keep spacing and range inside it.
USABLE_RANGE_M = 30.0

# The phy rejects attenuations outside this range.
ATT_MIN_DB, ATT_MAX_DB = -100.0, 100.0


def path_loss_db(distance_m: float, range_m: float) -> float:
    """Log-distance path loss inside @range_m, out of range beyond it."""
    if distance_m > range_m:
        return OUT_OF_RANGE_DB
    if distance_m < REF_DIST_M:
        distance_m = REF_DIST_M
    loss = REF_LOSS_DB + 10.0 * PATH_LOSS_EXPONENT * math.log10(distance_m / REF_DIST_M)
    return min(max(loss, ATT_MIN_DB), ATT_MAX_DB)


def layout_grid(n, spacing_m, jitter, rng):
    cols = max(1, int(math.ceil(math.sqrt(n))))
    nodes = []
    for i in range(n):
        col, row = i % cols, i // cols
        nodes.append({
            "index": i, "addr": i + 1, "cluster": 0,
            "x": round(col * spacing_m + rng.uniform(-jitter, jitter) * spacing_m, 2),
            "y": round(row * spacing_m + rng.uniform(-jitter, jitter) * spacing_m, 2),
        })
    return nodes


def layout_clustered(n, clusters, radius_m, spacing_m, cols, rng):
    """Clusters on a lattice, with open ground between them.

    A two-dimensional arrangement rather than a chain, deliberately. A chain gives
    depth but no alternatives: severing one boundary strands everything beyond it,
    so every outage looks the same and only tests total loss. On a lattice most
    clusters have more than one neighbour, so an outage forces nodes to find another
    way round - which is the behaviour actually worth watching.

    Centre jitter is kept small. Adjacent centres must stay within range plus two
    cluster radii or there are no bridges at all, and a cluster with no bridges is
    simply unreachable.
    """
    cols = max(1, min(cols, clusters))
    centres = []
    for c in range(clusters):
        cx = (c % cols) * spacing_m + rng.uniform(-0.12, 0.12) * spacing_m
        cy = (c // cols) * spacing_m + rng.uniform(-0.12, 0.12) * spacing_m
        centres.append((cx, cy))

    nodes = []
    per = [n // clusters] * clusters
    for i in range(n - sum(per)):
        per[i % clusters] += 1

    idx = 0
    for c, count in enumerate(per):
        cx, cy = centres[c]
        for _ in range(count):
            # Uniform over the disc, so the middle is not artificially crowded.
            r = radius_m * math.sqrt(rng.random())
            a = rng.uniform(0, 2 * math.pi)
            nodes.append({
                "index": idx, "addr": idx + 1, "cluster": c,
                "x": round(cx + r * math.cos(a), 2),
                "y": round(cy + r * math.sin(a), 2),
            })
            idx += 1
    return nodes


def assign_uplinks(nodes, count, rng):
    """Give a few nodes on the left a backhaul uplink.

    This is the only thing the generator decides about a node's part in the grid.
    Roles are not assigned: a node works out at runtime whether it should be a leaf
    or a router, so the shape of the tree is the protocol's doing, not the
    scenario's.
    """
    for node in nodes:
        node["uplink"] = False

    leftmost = sorted(nodes, key=lambda nd: nd["x"])
    # Spread them within the left-hand group rather than picking the N closest
    # together, so the grid does not have a single point of failure by accident.
    pool = leftmost[:max(count, len(leftmost) // 8)]
    for node in rng.sample(pool, min(count, len(pool))):
        node["uplink"] = True
    return nodes


def audible_pairs(nodes, range_m):
    """Every ordered pair within range, with its distance."""
    out = []
    for a in nodes:
        for b in nodes:
            if a is b:
                continue
            d = math.hypot(a["x"] - b["x"], a["y"] - b["y"])
            if d <= range_m:
                out.append((a["index"], b["index"], d))
    return out


def write_timed(path, baseline_db, windows, sim_us):
    """A path's attenuation over time.

    Each window is (start_us, end_us): the path is out of range across it, with a
    short ramp either side so the channel is not asked to step discontinuously in
    the middle of a packet.
    """
    ramp = 200000
    points = [(0, baseline_db)]
    for start, end in windows:
        points += [
            (max(1, start - ramp), baseline_db),
            (start, OUT_OF_RANGE_DB),
            (end, OUT_OF_RANGE_DB),
            (min(sim_us, end + ramp), baseline_db),
        ]
    points.append((sim_us, baseline_db))
    points.sort(key=lambda p: p[0])

    with open(path, "w") as f:
        f.write("# generated by gen_scenario.py\n")
        last_t = -1
        for t, att in points:
            if t <= last_t:
                continue
            f.write(f"{int(t)} {att:.1f}\n")
            last_t = t


def build_events(nodes, pairs, args, rng, out_dir, sim_us):
    """Decide which paths vary over time, and write their files.

    Returns {(tx, rx): filename} plus the schedule itself, so the visualisation can
    mark the windows on the timeline instead of leaving the viewer to infer from a
    burst of reconnections that something was done to the radio environment.
    """
    timed_dir = os.path.join(out_dir, "timed")
    os.makedirs(timed_dir, exist_ok=True)
    for stale in os.listdir(timed_dir):
        os.remove(os.path.join(timed_dir, stale))

    by_pair = {(a, b): d for a, b, d in pairs}
    windows = {}   # (tx, rx) -> [(start, end)]
    described = []

    n_clusters = max(nd["cluster"] for nd in nodes) + 1
    cluster_of = {nd["index"]: nd["cluster"] for nd in nodes}

    # Boundary outages. Everything beyond the severed boundary loses its route and
    # has to get it back, which is the strongest test of healing available.
    if args.outages and n_clusters > 1:
        # Only boundaries that actually carry links are worth severing; an outage
        # on a pair with no bridges would be invisible.
        carried = {}
        for (a, b) in by_pair:
            ca, cb = cluster_of[a], cluster_of[b]
            if ca != cb:
                carried[(min(ca, cb), max(ca, cb))] = carried.get(
                    (min(ca, cb), max(ca, cb)), 0) + 1
        boundaries = [pair for pair, cnt in carried.items() if cnt >= 4]
        rng.shuffle(boundaries)
        for k, boundary in enumerate(boundaries[:args.outages]):
            # Spread the outages over the middle of the run, leaving time either
            # side to converge and to recover.
            span = sim_us * 0.6
            start = int(sim_us * 0.25 + k * span / max(1, args.outages))
            end = int(start + args.outage_len * 1e6)
            crossed = 0
            for (a, b) in by_pair:
                ca, cb = cluster_of[a], cluster_of[b]
                if {ca, cb} == set(boundary):
                    windows.setdefault((a, b), []).append((start, end))
                    crossed += 1
            described.append({
                "kind": "outage", "start_us": start, "end_us": end,
                "label": f"cluster {boundary[0]}\u2013{boundary[1]} cut",
                "detail": f"every one of the {crossed // 2} links between clusters "
                          f"{boundary[0]} and {boundary[1]} faded out",
            })

    # Individual link fades. These take a parent away without taking the route
    # away, so a node should switch to another neighbour rather than drop out.
    if args.fades:
        candidates = [(a, b) for (a, b) in by_pair if a < b]
        rng.shuffle(candidates)
        for k, (a, b) in enumerate(candidates[:args.fades]):
            start = int(rng.uniform(sim_us * 0.2, sim_us * 0.85))
            end = int(min(sim_us, start + args.fade_len * 1e6))
            windows.setdefault((a, b), []).append((start, end))
            windows.setdefault((b, a), []).append((start, end))
            described.append({
                "kind": "fade", "start_us": start, "end_us": end,
                "label": f"link {a}\u2013{b} fades",
                "detail": f"a single link faded out for {args.fade_len:.0f}s, which "
                          f"takes a parent away without taking the route away",
            })

    files = {}
    for (a, b), wins in windows.items():
        d = by_pair[(a, b)]
        name = f"p{a:03d}_{b:03d}.txt"
        write_timed(os.path.join(timed_dir, name), path_loss_db(d, args.range_m),
                    sorted(wins), sim_us)
        files[(a, b)] = os.path.join(timed_dir, name)

    return files, described


def write_matrix(path, nodes, n_interferers, range_m, timed_files, sim_us,
                 interferer_duty):
    """One line per ordered pair. Omitted pairs fall back to the command line.

    Also returns when each interferer was audible, for the timeline.
    """
    described = []
    n_nodes = len(nodes)
    total = n_nodes + n_interferers
    timed_dir = os.path.join(os.path.dirname(path), "timed")

    # Interferers fade in and out, so the noise floor varies over the run instead
    # of being a constant the channel map can settle against once and forget.
    interferer_files = []
    for k in range(n_interferers):
        wins = []
        period = sim_us / max(1, interferer_duty)
        for j in range(interferer_duty):
            start = int((j + 0.5) * period + k * period / 3)
            end = int(min(sim_us, start + period * 0.45))
            if start < sim_us:
                wins.append((start, end))
        name = f"intf{k}.txt"
        # Baseline here is "audible"; the windows are when it goes quiet, so the
        # interferer is present for roughly half the run.
        write_timed(os.path.join(timed_dir, name), 55.0, wins, sim_us)
        interferer_files.append(os.path.join(timed_dir, name))

        # An interferer is audible between the quiet windows, which is the inverse
        # of what was just written.
        edges = [0] + [t for w in wins for t in w] + [sim_us]
        for j in range(0, len(edges) - 1, 2):
            if edges[j + 1] > edges[j]:
                described.append({
                    "kind": "interferer", "start_us": edges[j], "end_us": edges[j + 1],
                    "label": f"interferer {k} audible",
                    "detail": "a WLAN-shaped interferer raising the noise floor",
                })

    with open(path, "w") as f:
        f.write("# LockGrid path loss matrix, log-distance model\n")
        f.write(f"# {n_nodes} nodes + {n_interferers} interferer(s), reference "
                f"{REF_LOSS_DB} dB at {REF_DIST_M} m, exponent {PATH_LOSS_EXPONENT}, "
                f"range {range_m} m\n")
        for tx in range(total):
            for rx in range(total):
                if tx == rx:
                    continue
                if tx >= n_nodes or rx >= n_nodes:
                    k = (tx - n_nodes) if tx >= n_nodes else (rx - n_nodes)
                    f.write(f'{tx} {rx} : "{interferer_files[k]}"\n')
                    continue
                key = (tx, rx)
                if key in timed_files:
                    f.write(f'{tx} {rx} : "{timed_files[key]}"\n')
                else:
                    a, b = nodes[tx], nodes[rx]
                    d = math.hypot(a["x"] - b["x"], a["y"] - b["y"])
                    f.write(f"{tx} {rx} : {path_loss_db(d, range_m):.1f}\n")

    return described


def write_runner(path, out_dir, nodes, args):
    lines = [
        "#!/usr/bin/env bash",
        "# Generated by gen_scenario.py -- do not edit.",
        "set -u",
        "",
        'BIN="${BSIM_OUT_PATH:?set BSIM_OUT_PATH}/bin"',
        f'OUT="{out_dir}"',
        f'SIM="{args.sim_id}"',
        f'EXE="$BIN/{args.exe}"',
        "",
        'if [ ! -x "$EXE" ]; then',
        '  echo "missing $EXE -- build the trace overlay and copy it there" >&2',
        "  exit 1",
        "fi",
        "",
        'mkdir -p "$OUT/logs"',
        'rm -f "$OUT/logs/"*.log',
        "",
        "# The phy loads its channel and modem plugins by a path relative to its own",
        "# directory, so everything has to be started from there.",
        'cd "$BIN" || exit 1',
        "",
        "# Nodes. Only -lg_role=2 marks a node as having a backhaul uplink; every",
        "# node decides for itself whether to be a leaf or a router.",
    ]

    flap_n = 0
    for node in nodes:
        i = node["index"]
        cap = 2 if node["uplink"] else 1
        extra = ""
        if node["uplink"] and args.uplink_flap:
            # Staggered, so the backhauls do not all vanish together every cycle -
            # though they must be allowed to overlap sometimes, because a grid with
            # no sink at all has to be survivable too.
            # Close enough together that the down windows overlap, so the grid
            # spends some of the run with no sink at all. That case has to be
            # survivable too: the right behaviour is for nodes to lose their route
            # and get it back, not to claim one that does not exist.
            first = args.uplink_flap * (1.0 + flap_n / 6.0)
            extra = (f' -lg_uplink={round(first * 1000)}'
                     f' -lg_flap={round(args.uplink_flap * 1000)}'
                     f' -lg_flap_down={round(args.uplink_down * 1000)}')
            flap_n += 1
        lines.append(
            f'"$EXE" -s="$SIM" -d={i} -lg_addr={node["addr"]} -lg_role={cap}{extra} '
            f'-lg_period={args.telemetry_ms} -lg_report=0 -lg_snap={args.snap_ms} '
            f'> "$OUT/logs/n{i:03d}.log" 2>&1 &'
        )

    for k in range(args.interferers):
        dev = len(nodes) + k
        chan = [6, 1, 11][k % 3]
        lines.append("")
        lines.append(f"# WLAN interferer {k} on WLAN channel {chan}, fading in and out.")
        lines.append(
            f'"$BIN/bs_device_2G4_WLAN_actmod" -s="$SIM" -d={dev} '
            f'-channel={chan} -power=0 -ConfigSet=50 '
            f'> "$OUT/logs/intf{k}.log" 2>&1 &'
        )

    lines += [
        "",
        "# The phy owns the run length; when it exits the nodes are torn down.",
        f'"$BIN/bs_2G4_phy_v1" -s="$SIM" -D={len(nodes) + args.interferers} '
        f'-sim_length={int(args.sim_length * 1e6)} '
        f'-channel=multiatt -argschannel -at={OUT_OF_RANGE_DB:.0f} '
        f'-file="$OUT/att_matrix.txt" -argsmain '
        f'> "$OUT/logs/phy.log" 2>&1',
        "PHY_RC=$?",
        "",
        "wait",
        'echo "phy exited $PHY_RC; logs in $OUT/logs"',
        "exit $PHY_RC",
    ]

    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--layout", choices=("grid", "clustered"), default="clustered")
    ap.add_argument("--nodes", type=int, default=100)
    ap.add_argument("--uplinks", type=int, default=2,
                    help="nodes on the left given a backhaul uplink")
    ap.add_argument("--clusters", type=int, default=12)
    ap.add_argument("--cluster-cols", type=int, default=4,
                    help="clusters per row; more than one row gives alternative "
                         "routes, so an outage forces a reroute rather than a "
                         "blackout")
    ap.add_argument("--cluster-radius", type=float, default=8.0)
    ap.add_argument("--cluster-spacing", type=float, default=32.0,
                    help="between cluster centres; wide enough that only the "
                         "closest nodes of adjacent clusters can hear each other")
    ap.add_argument("--spacing", type=float, default=10.0,
                    help="grid layout only: lattice pitch")
    ap.add_argument("--jitter", type=float, default=0.3)
    ap.add_argument("--range", type=float, default=28.0, dest="range_m",
                    help="radio range in metres; above ~30 m a link falls under "
                         "the level at which a handshake will be attempted")
    ap.add_argument("--outages", type=int, default=0,
                    help="cluster boundaries to sever, one at a time")
    ap.add_argument("--outage-len", type=float, default=30.0, help="seconds")
    ap.add_argument("--fades", type=int, default=0,
                    help="individual links to fade out and back")
    ap.add_argument("--fade-len", type=float, default=25.0, help="seconds")
    ap.add_argument("--uplink-flap", type=float, default=0.0,
                    help="seconds between uplink losses at the backhauls; a "
                         "backhaul on a cellular link is not a stable fact, and "
                         "this is the one role transition a node does not choose")
    ap.add_argument("--uplink-down", type=float, default=15.0,
                    help="seconds an uplink stays down each time")
    ap.add_argument("--interferers", type=int, default=0)
    ap.add_argument("--interferer-bursts", type=int, default=4,
                    help="how many times each interferer goes quiet and returns")
    ap.add_argument("--sim-length", type=float, default=150.0)
    ap.add_argument("--telemetry-ms", type=int, default=15000)
    ap.add_argument("--snap-ms", type=int, default=5000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--sim-id", default="lockgrid_scale")
    ap.add_argument("--exe", default="bs_lockgrid_trace")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    if args.range_m > USABLE_RANGE_M:
        print(f"note: range {args.range_m} m puts the weakest link under the "
              f"handshake threshold; clamping to {USABLE_RANGE_M} m")
        args.range_m = USABLE_RANGE_M

    rng = random.Random(args.seed)
    os.makedirs(args.out, exist_ok=True)
    sim_us = int(args.sim_length * 1e6)

    if args.layout == "clustered":
        nodes = layout_clustered(args.nodes, args.clusters, args.cluster_radius,
                                 args.cluster_spacing, args.cluster_cols, rng)
    else:
        nodes = layout_grid(args.nodes, args.spacing, args.jitter, rng)

    nodes = assign_uplinks(nodes, args.uplinks, rng)
    pairs = audible_pairs(nodes, args.range_m)

    timed_files, described = build_events(nodes, pairs, args, rng, args.out, sim_us)
    described += write_matrix(os.path.join(args.out, "att_matrix.txt"), nodes,
                              args.interferers, args.range_m, timed_files, sim_us,
                              args.interferer_bursts)
    if args.uplink_flap:
        n_up = sum(1 for nd in nodes if nd["uplink"])
        for k in range(n_up):
            t = args.uplink_flap * (1.0 + k / 6.0)
            while t < args.sim_length:
                described.append({
                    "kind": "uplink", "start_us": int(t * 1e6),
                    "end_us": int(min(args.sim_length, t + args.uplink_down) * 1e6),
                    "label": f"backhaul {k} uplink down",
                    "detail": "a backhaul lost its uplink and demoted itself to "
                              "router; everything reaching the grid through it has "
                              "to find another sink",
                })
                t += args.uplink_flap
    described.sort(key=lambda e: e["start_us"])

    with open(os.path.join(args.out, "positions.json"), "w") as f:
        json.dump({
            "nodes": nodes,
            "layout": args.layout,
            "spacing_m": args.cluster_spacing if args.layout == "clustered" else args.spacing,
            "range_m": args.range_m,
            "sim_length_s": args.sim_length,
            "clusters": args.clusters if args.layout == "clustered" else 1,
            "events": described,
        }, f, indent=1)

    write_runner(os.path.join(args.out, "run.sh"), args.out, nodes, args)

    # Report the connectivity the layout produced. If this is far off a handful of
    # neighbours each, the grid will be either a star or disconnected, and no amount
    # of protocol will fix it.
    deg = {nd["index"]: 0 for nd in nodes}
    worst = 0.0
    for a, b, d in pairs:
        deg[a] += 1
        worst = min(worst, -path_loss_db(d, args.range_m))
    degrees = sorted(deg.values())

    # Bridges are the pairs that hold the chain together, and they are what the
    # outages take away.
    cluster_of = {nd["index"]: nd["cluster"] for nd in nodes}
    bridges = sum(1 for a, b, _ in pairs
                  if cluster_of[a] != cluster_of[b] and a < b)

    print(f"{len(nodes)} nodes, {args.layout} layout, "
          f"{args.uplinks} with a backhaul uplink (left side)")
    if args.layout == "clustered":
        print(f"{args.clusters} clusters, radius {args.cluster_radius} m, "
              f"centres {args.cluster_spacing} m apart, range {args.range_m} m")
        print(f"inter-cluster bridge links: {bridges}")
    print(f"neighbours per node: min {degrees[0]}, "
          f"median {degrees[len(degrees)//2]}, max {degrees[-1]}")
    print(f"weakest in-range link: {worst:.0f} dBm at 0 dBm transmit")
    by_kind = {}
    for d in described:
        by_kind.setdefault(d["kind"], []).append(d)
    for kind, items in by_kind.items():
        span = ", ".join(f"{i['start_us']/1e6:.0f}-{i['end_us']/1e6:.0f}s"
                         for i in items[:4])
        more = f" and {len(items) - 4} more" if len(items) > 4 else ""
        print(f"  {kind}: {len(items)} window(s) at {span}{more}")
    if degrees[0] == 0:
        print("WARNING: a node hears nobody; raise --range or shrink --cluster-spacing")
    if bridges == 0 and args.layout == "clustered":
        print("WARNING: no inter-cluster links at all; the clusters cannot be joined")
    print(f"wrote {args.out}/{{positions.json,att_matrix.txt,timed/,run.sh}}")


if __name__ == "__main__":
    main()
