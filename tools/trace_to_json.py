#!/usr/bin/env python3
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

"""Turn LockGrid trace logs into the JSON the visualisation reads.

Each node writes its own log; every protocol event is one `LGT` line carrying its
own microsecond timestamp. This collects them all, merges in the node positions
from the scenario generator, and emits a single compact document.

The output separates three things, because the visualisation needs them
differently:

  frames     every transmission and reception, for the packet animation
  topology   periodic snapshots of who is linked to whom and at what cost, since
             an edge's state at an arbitrary point on the timeline cannot be
             reconstructed from events alone
  discrete   the events worth calling out - links coming and going, parent
             changes, handshake outcomes, channel map changes, deliveries

Usage:
  ./trace_to_json.py --scenario /tmp/lg100 --out /tmp/lg100/trace.json
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict

LGT_RE = re.compile(r"LGT (\d+) ([0-9a-f]{4}) (\w+) ?(.*)$")

# Frame types, matching enum lg_frame_type.
FRAME_TYPE_NAMES = {
    0: "beacon", 1: "join_req", 2: "join_rsp", 3: "join_cfm", 4: "join_ack",
    5: "resume_req", 6: "resume_rsp", 7: "link_req", 8: "link_rsp",
    9: "data", 10: "ack", 11: "ctrl",
}

# Events kept in the discrete list, with the fields worth carrying through.
DISCRETE = {
    "boot": ("role",),
    "linkup": ("peer", "init", "int", "seed", "rank"),
    "linkdown": ("peer", "reason"),
    "parent": ("old", "new", "rank"),
    "joinok": ("peer", "role", "resume"),
    "joinfail": ("peer", "slot"),
    "supervision": ("peer",),
    "chanmap": ("map", "was"),
    "deliver": ("origin", "via", "port", "hops", "len"),
    "forward": ("origin", "final", "from", "hops"),
    # beaconrx is deliberately absent. It is by far the most numerous event - more
    # than every other kind put together - and nothing reads it: the per-node
    # snapshot already carries a received-beacon count, and a list of every beacon
    # heard is not something anybody scrubs through. Carrying it made the embedded
    # document several times larger than the page that reads it.
    # Roles are chosen at runtime, so a change of role is a protocol event in its
    # own right rather than a property of the scenario.
    "role": ("old", "new", "rank", "routers"),
}

# Discrete events whose named fields are plain integers even though the generic
# rule would read them as hex. "old"/"new" are addresses for a parent change but
# role numbers for a role change.
DECIMAL_FIELDS = {"role": ("old", "new")}


def parse_kv(rest: str) -> dict:
    out = {}
    for tok in rest.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        out[k] = v
    return out


def as_int(v, base=10):
    try:
        return int(v, base)
    except (TypeError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", required=True,
                    help="directory holding positions.json and logs/")
    ap.add_argument("--out", required=True)
    ap.add_argument("--window-s", type=float, default=20.0,
                    help="window over which to compute current from the charge "
                         "deltas; the averages the firmware reports are measured "
                         "from boot, so during a long convergence they describe how "
                         "the grid formed rather than what a settled node costs")
    ap.add_argument("--max-frames", type=int, default=120000,
                    help="frames kept for the animation; beyond this they are "
                         "uniformly thinned rather than truncated, so the whole "
                         "run stays represented")
    args = ap.parse_args()

    pos_path = os.path.join(args.scenario, "positions.json")
    with open(pos_path) as f:
        scenario = json.load(f)

    by_addr = {n["addr"]: n for n in scenario["nodes"]}

    truncated = defaultdict(int)
    frames = []          # [t_us, src_addr, peer_addr, chan, type, is_rx, rssi]
    discrete = []        # {t, node, ev, ...}
    # snapshots[(addr, t)] = {"state": {...}, "pwr": {...}, "neigh": [...]}
    snaps = defaultdict(lambda: {"state": {}, "pwr": {}, "counters": {}, "neigh": []})

    log_dir = os.path.join(args.scenario, "logs")
    logs = sorted(f for f in os.listdir(log_dir)
                  if f.endswith(".log") and not f.startswith("phy"))
    if not logs:
        sys.exit(f"no node logs in {log_dir}")

    for name in logs:
        with open(os.path.join(log_dir, name), errors="replace") as f:
            for line in f:
                m = LGT_RE.search(line)
                if not m:
                    continue
                t_us = int(m.group(1))
                addr = int(m.group(2), 16)
                ev = m.group(3)
                kv = parse_kv(m.group(4))

                if ev == "truncated":
                    # The emitter says so rather than letting a short line be
                    # parsed as if it were complete.
                    truncated[kv.get("event", "?")] += 1
                    continue

                if ev in ("tx", "rx"):
                    peer = as_int(kv.get("peer"), 16)
                    frames.append([
                        t_us, addr, peer if peer is not None else 0,
                        as_int(kv.get("ch")) or 0,
                        as_int(kv.get("type")) or 0,
                        1 if ev == "rx" else 0,
                        as_int(kv.get("rssi")) or 0,
                    ])
                elif ev == "beacontx":
                    frames.append([t_us, addr, 0, as_int(kv.get("ch")) or 0, 0, 0, 0])
                elif ev == "state":
                    s = snaps[(addr, t_us)]["state"]
                    s["rank"] = as_int(kv.get("rank"))
                    s["parent"] = as_int(kv.get("parent"), 16)
                    s["links"] = as_int(kv.get("links"))
                    s["map"] = as_int(kv.get("map"), 16)
                    s["role"] = as_int(kv.get("role"))
                elif ev == "pwr":
                    p = snaps[(addr, t_us)]["pwr"]
                    # "ua" is the average since boot, which includes the one-off
                    # cost of scanning and joining; "steady" has that subtracted and
                    # is the figure a deployed node would actually draw.
                    for k in ("on", "elapsed", "duty", "rxlink", "rxscan", "rxjoin",
                              "txlink", "txbeacon", "ua", "steady", "fixed",
                              "starts", "nc"):
                        p[k] = as_int(kv.get(k))
                elif ev == "counters":
                    snaps[(addr, t_us)]["counters"] = {
                        k: as_int(v) for k, v in kv.items()
                    }
                elif ev == "neigh":
                    snaps[(addr, t_us)]["neigh"].append({
                        "peer": as_int(kv.get("peer"), 16),
                        "linked": as_int(kv.get("linked")),
                        "parent": as_int(kv.get("parent")),
                        "rank": as_int(kv.get("rank")),
                        "cost": as_int(kv.get("cost")),
                        "rssi": as_int(kv.get("rssi")),
                        "etx": as_int(kv.get("etx")),
                        "per": as_int(kv.get("per")),
                        "sub": as_int(kv.get("sub")),
                        "win": as_int(kv.get("win")),
                    })
                elif ev in DISCRETE:
                    rec = {"t": t_us, "n": addr, "ev": ev}
                    for k in DISCRETE[ev]:
                        v = kv.get(k)
                        if v is None:
                            continue
                        # Addresses and bitmaps are hex in the trace.
                        if k in DECIMAL_FIELDS.get(ev, ()):
                            rec[k] = as_int(v)
                        elif k in ("peer", "old", "new", "origin", "via", "from",
                                 "final", "map", "was", "seed"):
                            rec[k] = as_int(v, 16)
                        else:
                            rec[k] = as_int(v)
                    discrete.append(rec)

    frames.sort(key=lambda r: r[0])
    discrete.sort(key=lambda r: r["t"])

    kept = len(frames)
    if kept > args.max_frames:
        # Uniform thinning keeps the whole timeline represented; truncation would
        # make the animation stop part way through the run.
        step = kept / args.max_frames
        frames = [frames[int(i * step)] for i in range(args.max_frames)]

    # Current over a window, from the cumulative charge.
    #
    # nC per us is mA, so scaling by 1000 gives uA. This is the same energy model
    # the firmware uses - it is the firmware's own running total, differenced -
    # so it introduces no second set of constants to disagree with.
    window_us = args.window_s * 1e6
    series = defaultdict(list)
    for (addr, t_us), data in snaps.items():
        nc = data["pwr"].get("nc")
        if nc is not None:
            series[addr].append((t_us, nc))
    windowed = {}
    for addr, pts in series.items():
        pts.sort()
        for i, (t2, nc2) in enumerate(pts):
            # Walk back to the first sample at least a window away, so the figure
            # covers a full window wherever one exists.
            j = i
            while j > 0 and t2 - pts[j][0] < window_us:
                j -= 1
            t1, nc1 = pts[j]
            if t2 > t1 and nc2 >= nc1:
                windowed[(addr, t2)] = round((nc2 - nc1) / (t2 - t1) * 1000.0, 1)

    # Collapse the per-node snapshots onto a shared timeline. Nodes sample on their
    # own schedule, so bucket to the nearest second and carry the latest sample
    # forward - which is what "state at time T" means for a node that has not
    # sampled recently.
    bucket = defaultdict(dict)
    for (addr, t_us), data in snaps.items():
        if not data["state"]:
            continue
        # Kept so a carried-forward snapshot can still be matched to the windowed
        # figure computed for the moment it was actually taken.
        data["_t"] = t_us
        bucket[round(t_us / 1e6)][addr] = data

    topology = []
    carried = {}
    for sec in sorted(bucket):
        carried.update(bucket[sec])
        entry = {"t": sec * 1000000, "nodes": {}, "edges": []}
        seen_edges = set()
        for addr, data in carried.items():
            st, pw = data["state"], data["pwr"]
            entry["nodes"][str(addr)] = {
                "rank": st.get("rank"), "parent": st.get("parent"),
                "links": st.get("links"), "map": st.get("map"),
                # The role the node has chosen for itself at this point in the
                # run, which is not knowable from the scenario.
                "role": st.get("role"),
                "on": pw.get("on"), "duty": pw.get("duty"),
                "rxlink": pw.get("rxlink"), "rxscan": pw.get("rxscan"),
                "rxjoin": pw.get("rxjoin"), "txlink": pw.get("txlink"),
                "txbeacon": pw.get("txbeacon"),
                "ua": pw.get("ua"), "steady": pw.get("steady"),
                "fixed": pw.get("fixed"), "starts": pw.get("starts"),
                # Current over the preceding window rather than since boot.
                "uaw": windowed.get((addr, data.get("_t", 0))),
            }
            for nb in data["neigh"]:
                peer = nb["peer"]
                if peer is None:
                    continue
                # One entry per unordered pair, from whichever end reported it.
                key = (min(addr, peer), max(addr, peer))
                if key in seen_edges:
                    continue
                seen_edges.add(key)
                entry["edges"].append({
                    "a": addr, "b": peer, "linked": nb["linked"],
                    "parent": nb["parent"], "cost": nb["cost"], "rssi": nb["rssi"],
                    "etx": nb["etx"], "per": nb["per"], "sub": nb["sub"],
                    "win": nb["win"],
                })
        topology.append(entry)

    end_us = max(
        [f[0] for f in frames] + [d["t"] for d in discrete] +
        [t["t"] for t in topology] + [0]
    )

    # Per-role steady figures over the stretch after the grid had converged, which
    # is the only part of the run where "what does a node cost" has an answer.
    converged_us = None
    for entry in topology:
        routed = sum(1 for v in entry["nodes"].values()
                     if v.get("rank") not in (None, 65535))
        if routed >= len(scenario["nodes"]):
            converged_us = entry["t"]
            break

    summary = {"converged_us": converged_us, "window_s": args.window_s, "roles": {}}
    if topology:
        tail_from = converged_us if converged_us is not None else topology[-1]["t"]
        per_role = defaultdict(list)
        for entry in topology:
            if entry["t"] < tail_from:
                continue
            for nd in entry["nodes"].values():
                if nd.get("uaw") is not None:
                    per_role[nd.get("role")].append(nd["uaw"])
        for r, vals in per_role.items():
            v = sorted(vals)
            summary["roles"][str(r)] = {
                "mean": round(sum(v) / len(v), 1),
                "p90": v[int(len(v) * 0.9)],
                "worst": v[-1],
                "n": len(v),
            }

    doc = {
        "meta": {
            "power": summary,
            "nodes": scenario["nodes"],
            "spacing_m": scenario.get("spacing_m"),
            "range_m": scenario.get("range_m"),
            "sim_length_s": scenario.get("sim_length_s"),
            "end_us": end_us,
            "frame_types": FRAME_TYPE_NAMES,
            "frames_total": kept,
            "frames_kept": len(frames),
            "clusters": scenario.get("clusters", 1),
            "layout": scenario.get("layout"),
            # What the scenario deliberately did to the radio environment, so the
            # timeline can be read against it rather than guessed at.
            "events": scenario.get("events", []),
        },
        "frames": frames,
        "topology": topology,
        "discrete": discrete,
    }

    with open(args.out, "w") as f:
        json.dump(doc, f, separators=(",", ":"))

    if truncated:
        print("WARNING: the emitter reported truncated trace lines, so some fields "
              "are missing:")
        for ev, cnt in sorted(truncated.items(), key=lambda kv: -kv[1]):
            print(f"    {ev}: {cnt}")

    size_kb = os.path.getsize(args.out) / 1024
    print(f"{len(logs)} logs -> {args.out} ({size_kb:.0f} kB)")
    print(f"  frames    {kept} recorded, {len(frames)} kept")
    print(f"  topology  {len(topology)} snapshots")
    print(f"  discrete  {len(discrete)} events")
    kinds = defaultdict(int)
    for d in discrete:
        kinds[d["ev"]] += 1
    for k in sorted(kinds, key=lambda x: -kinds[x]):
        print(f"    {k:<12} {kinds[k]}")

    roles = defaultdict(int)
    for entry in topology[-1:]:
        for nd in entry["nodes"].values():
            roles[nd.get("role")] += 1
    if roles:
        names = {0: "leaf", 1: "router", 2: "backhaul"}
        final = ", ".join(f"{roles[r]} {names.get(r, r)}"
                          for r in sorted(roles, key=lambda x: (x is None, x)))
        print(f"  roles at the end of the run: {final}")

    # The primary metric. Reported per role, because a router and a leaf are not
    # meant to draw the same and averaging them together hides both.
    if topology:
        names = {0: "leaf", 1: "router", 2: "backhaul"}
        per_role = defaultdict(list)
        for nd in topology[-1]["nodes"].values():
            if nd.get("steady") is not None:
                per_role[nd.get("role")].append(nd["steady"])
        for r in sorted(per_role, key=lambda x: (x is None, x)):
            v = sorted(per_role[r])
            print(f"  since boot, {names.get(r, r)}: mean "
                  f"{sum(v)/len(v):.1f} uA, worst {v[-1]} uA, n={len(v)}")

    if converged_us is not None:
        print(f"  grid fully routed at {converged_us/1e6:.0f}s")
    else:
        print("  grid never fully routed")
    print(f"  current over a {args.window_s:.0f}s window, after convergence:")
    for r in sorted(summary["roles"], key=lambda x: (x == "None", x)):
        st = summary["roles"][r]
        print(f"    {names.get(int(r) if r != 'None' else None, r):<9} "
              f"mean {st['mean']:5.1f} uA   p90 {st['p90']:5.1f}   "
              f"worst {st['worst']:5.1f}   n={st['n']}")


if __name__ == "__main__":
    main()
