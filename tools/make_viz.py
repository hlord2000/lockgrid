#!/usr/bin/env python3
# Copyright (c) 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

"""Build a self-contained HTML view of a LockGrid run.

Takes the JSON from trace_to_json.py and writes one HTML file with the trace
embedded, so it opens from the filesystem with no server and no network access.

Usage:
  ./make_viz.py --trace /tmp/lg100/trace.json --out /tmp/lg100/lockgrid.html
"""

import argparse
import json
import os

HTML = r"""<title>LockGrid — __TITLE__</title>
<style>
/* ---------------------------------------------------------------------------
   Tokens. Every colour and size is defined here and referenced through a
   variable, so both themes are a matter of redefining the tokens rather than
   restyling components.
   --------------------------------------------------------------------------- */
:root {
  /* Graphite with a blue bias rather than a neutral grey: the page is an
     instrument panel for a radio, and a cold ground makes the warm transmit
     colour read as heat. */
  --bg:        #eef1f6;
  --panel:     #ffffff;
  --panel-2:   #f7f9fc;
  --line:      #d3dae6;
  --line-soft: #e4e9f2;
  --ink:       #1b2130;
  --ink-2:     #4a5468;
  --ink-3:     #7b8699;

  /* Chrome accent. Kept clear of the data colours so a highlighted control is
     never confused with a measurement. */
  --accent:    #4553c7;
  --accent-2:  #6b77dd;
  --accent-bg: #e6e9fb;

  /* The two data colours that matter: the protocol is a story about receive
     time versus transmit time, so those get the strongest pair on the page. */
  --rx:        #0d7f96;
  --tx:        #b76512;

  /* Semantic, independent of the accent. */
  --good:      #1c8a5f;
  --warn:      #b07d16;
  --crit:      #bb3a56;

  --mono: ui-monospace, "SF Mono", "Cascadia Mono", "JetBrains Mono", Menlo, Consolas, monospace;
  --sans: system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", sans-serif;

  --r: 5px;
  --gap: 12px;
}

@media (prefers-color-scheme: dark) {
  :root {
    --bg:        #0f131b;
    --panel:     #161b26;
    --panel-2:   #1b2130;
    --line:      #2b3345;
    --line-soft: #222937;
    --ink:       #e6ebf4;
    --ink-2:     #9fabc0;
    --ink-3:     #6a7689;
    --accent:    #7b87ee;
    --accent-2:  #9aa4f5;
    --accent-bg: #232a46;
    --rx:        #3ec4dd;
    --tx:        #e39445;
    --good:      #3fc98d;
    --warn:      #e0b344;
    --crit:      #f0728c;
  }
}

/* The viewer's own toggle has to win over the OS preference in both
   directions, so the tokens are restated for each explicit theme. */
:root[data-theme="light"] {
  --bg:#eef1f6; --panel:#ffffff; --panel-2:#f7f9fc; --line:#d3dae6;
  --line-soft:#e4e9f2; --ink:#1b2130; --ink-2:#4a5468; --ink-3:#7b8699;
  --accent:#4553c7; --accent-2:#6b77dd; --accent-bg:#e6e9fb;
  --rx:#0d7f96; --tx:#b76512; --good:#1c8a5f; --warn:#b07d16; --crit:#bb3a56;
}
:root[data-theme="dark"] {
  --bg:#0f131b; --panel:#161b26; --panel-2:#1b2130; --line:#2b3345;
  --line-soft:#222937; --ink:#e6ebf4; --ink-2:#9fabc0; --ink-3:#6a7689;
  --accent:#7b87ee; --accent-2:#9aa4f5; --accent-bg:#232a46;
  --rx:#3ec4dd; --tx:#e39445; --good:#3fc98d; --warn:#e0b344; --crit:#f0728c;
}

* { box-sizing: border-box; }

body {
  margin: 0;
  background: var(--bg);
  color: var(--ink);
  /* Mono-dominant on purpose: this is a telemetry console, and nearly
     everything on it is a number that should line up with the number above. */
  font: 400 13px/1.5 var(--mono);
  font-variant-numeric: tabular-nums;
  -webkit-font-smoothing: antialiased;
}

.prose { font-family: var(--sans); }

/* --------------------------------------------------------------------------- */
/* Shell                                                                       */
/* --------------------------------------------------------------------------- */

.app {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 310px;
  grid-template-rows: auto minmax(0, 1fr) auto;
  grid-template-areas: "head head" "stage rail" "foot foot";
  gap: var(--gap);
  padding: var(--gap);
  min-height: 100vh;
  max-height: 100vh;
}

header { grid-area: head; }
.stage  { grid-area: stage; min-width: 0; min-height: 0; }
.rail   { grid-area: rail;  min-height: 0; overflow-y: auto; }
footer  { grid-area: foot; }

.card {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--r);
}

/* --------------------------------------------------------------------------- */
/* Header: the summary, before any detail                                      */
/* --------------------------------------------------------------------------- */

header {
  display: flex;
  align-items: stretch;
  gap: var(--gap);
  flex-wrap: wrap;
}

.brand {
  display: flex;
  flex-direction: column;
  justify-content: center;
  padding: 8px 14px;
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--r);
  min-width: 190px;
}
.brand b { font-size: 15px; letter-spacing: .04em; }
.brand span { color: var(--ink-3); font-size: 11px; letter-spacing: .06em; text-transform: uppercase; }

.stats {
  display: flex;
  gap: 1px;
  flex: 1 1 480px;
  background: var(--line-soft);
  border: 1px solid var(--line);
  border-radius: var(--r);
  overflow: hidden;
}
.stat {
  flex: 1 1 0;
  min-width: 84px;
  padding: 8px 12px;
  background: var(--panel);
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.stat dt {
  font-size: 10px;
  letter-spacing: .08em;
  text-transform: uppercase;
  color: var(--ink-3);
}
.stat dd { margin: 0; font-size: 19px; font-weight: 500; line-height: 1.15; }
.stat dd small { font-size: 11px; color: var(--ink-3); font-weight: 400; }

/* --------------------------------------------------------------------------- */
/* Stage                                                                       */
/* --------------------------------------------------------------------------- */

.stage { position: relative; }
.stage canvas { display: block; width: 100%; height: 100%; border-radius: var(--r); }

.legend {
  position: absolute;
  left: 12px; bottom: 12px;
  display: flex;
  flex-direction: column;
  gap: 5px;
  padding: 9px 11px;
  background: color-mix(in srgb, var(--panel) 88%, transparent);
  border: 1px solid var(--line);
  border-radius: var(--r);
  font-size: 11px;
  color: var(--ink-2);
  backdrop-filter: blur(3px);
}
.legend div { display: flex; align-items: center; gap: 7px; }
.legend i { width: 20px; height: 0; border-top-width: 2px; border-top-style: solid; }
.swatch { width: 9px; height: 9px; border-radius: 50%; }
.swatch.sq { border-radius: 1px; }
.swatch.hollow { background: transparent; border: 1.5px solid var(--ink-2); }

.viewtools {
  position: absolute;
  right: 12px; top: 12px;
  display: flex;
  gap: 6px;
}

/* --------------------------------------------------------------------------- */
/* Controls                                                                    */
/* --------------------------------------------------------------------------- */

button, select {
  font: inherit;
  color: var(--ink);
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--r);
  padding: 5px 10px;
  cursor: pointer;
}
button:hover, select:hover { border-color: var(--accent-2); }
button[aria-pressed="true"] {
  background: var(--accent-bg);
  border-color: var(--accent);
  color: var(--accent);
}
:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }

/* --------------------------------------------------------------------------- */
/* Timeline                                                                    */
/* --------------------------------------------------------------------------- */

footer { display: flex; flex-direction: column; gap: 8px; padding: 10px 12px; }

.transport { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.transport .clock {
  font-size: 17px;
  min-width: 106px;
  font-weight: 500;
}
.transport .clock small { color: var(--ink-3); font-weight: 400; font-size: 12px; }

/* Radio energy panel. Two numbers per role and one rule they are measured
   against, because "is it under budget" is the only question being asked. */
.pw-row {
  display: grid;
  grid-template-columns: 74px 1fr 62px;
  align-items: center;
  gap: 8px;
  padding: 3px 0;
  font-size: 11px;
}
.pw-label { color: var(--ink-2); display: flex; justify-content: space-between; padding-right: 6px; }
.pw-label small { color: var(--ink-3); font-variant-numeric: tabular-nums; }
.pw-bar {
  position: relative;
  height: 11px;
  background: var(--line-soft);
  border-radius: 2px;
  overflow: visible;
}
.pw-bar i {
  position: absolute; inset: 0 auto 0 0;
  background: var(--accent);
  border-radius: 2px;
  transition: width .18s ease;
}
.pw-bar i.over { background: var(--crit); }
.pw-head {
  display: flex; justify-content: space-between;
  font-size: 10px; color: var(--ink-3);
  padding-bottom: 3px;
}
.pw-bar u {
  position: absolute; top: -2px; width: 2px; height: 15px;
  margin-left: -1px;
  background: var(--good);
}
.pw-bar u.bad { background: var(--crit); }
.pw-bar b {
  position: absolute; top: -4px; bottom: -4px; width: 0;
  border-left: 1px dashed var(--ink-3);
}
.pw-val {
  text-align: right;
  font-family: var(--mono);
  font-variant-numeric: tabular-nums;
  color: var(--ink);
}
.pw-val small { color: var(--ink-3); }

/* Key for the bands under the activity track: what the scenario did, as opposed
   to what the protocol did about it. */
.tl-key { display: flex; gap: 11px; margin-left: auto; font-size: 10px; color: var(--ink-3); }
.tl-key span { display: flex; align-items: center; gap: 4px; }
.tl-key i { width: 12px; height: 2.5px; border-radius: 1px; }

.track { position: relative; height: 52px; }
.track canvas { display: block; width: 100%; height: 100%; }
input[type="range"] {
  position: absolute;
  inset: auto 0 -3px 0;
  width: 100%;
  margin: 0;
  accent-color: var(--accent);
}

/* --------------------------------------------------------------------------- */
/* Inspector                                                                   */
/* --------------------------------------------------------------------------- */

.rail { display: flex; flex-direction: column; gap: var(--gap); }
.panel { padding: 11px 13px; }
.panel h2 {
  margin: 0 0 9px;
  font-size: 10px;
  letter-spacing: .1em;
  text-transform: uppercase;
  color: var(--ink-3);
  font-weight: 500;
}

/* Panel subtitle: carries the unit, so it should not inherit the heading's
   uppercase treatment. */
.panel h2 small {
  text-transform: none;
  letter-spacing: 0;
  font-family: var(--mono);
  color: var(--ink-3);
  opacity: .8;
}

.kv { display: grid; grid-template-columns: auto 1fr; gap: 3px 10px; font-size: 12px; }
.kv dt { color: var(--ink-3); }
.kv dd { margin: 0; text-align: right; }

/* A bar is easier to compare across nodes than a number, and the split shows
   where the radio time actually went — which is the whole point. */
.bars { display: flex; flex-direction: column; gap: 7px; margin-top: 4px; }
.bar-row { display: grid; grid-template-columns: 58px 1fr auto; gap: 8px; align-items: center; font-size: 11px; }
.bar-row span:first-child { color: var(--ink-3); }
.bar { height: 7px; background: var(--line-soft); border-radius: 99px; overflow: hidden; }
.bar i { display: block; height: 100%; border-radius: 99px; }

.pill {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 1px 7px;
  border-radius: 99px;
  font-size: 11px;
  border: 1px solid;
}
.pill.ok   { color: var(--good); border-color: color-mix(in srgb, var(--good) 40%, transparent); background: color-mix(in srgb, var(--good) 10%, transparent); }
.pill.bad  { color: var(--crit); border-color: color-mix(in srgb, var(--crit) 40%, transparent); background: color-mix(in srgb, var(--crit) 10%, transparent); }

.log { display: flex; flex-direction: column; gap: 1px; font-size: 11px; max-height: 210px; overflow-y: auto; }
.log div { display: grid; grid-template-columns: 46px 1fr; gap: 8px; padding: 2px 0; border-bottom: 1px solid var(--line-soft); }
.log time { color: var(--ink-3); }
.log b { font-weight: 500; }

.hint { color: var(--ink-3); font-size: 11px; }

@media (max-width: 900px) {
  .app {
    grid-template-columns: minmax(0, 1fr);
    grid-template-areas: "head" "stage" "rail" "foot";
    max-height: none;
  }
  .stage { height: 60vh; }
}

@media (prefers-reduced-motion: reduce) {
  * { animation: none !important; transition: none !important; }
}
</style>

<div class="app">
  <header>
    <div class="brand">
      <b>LockGrid</b>
      <span id="scale">— nodes</span>
    </div>
    <dl class="stats" id="stats"></dl>
  </header>

  <div class="stage card">
    <canvas id="graph"></canvas>
    <div class="viewtools">
      <button id="btn-heard" aria-pressed="false" title="Also draw neighbours that are known but not linked">Heard</button>
      <button id="btn-packets" aria-pressed="true" title="Animate frames along the links">Packets</button>
      <select id="colour" title="What the node fill encodes">
        <option value="rank">Fill: hops to backhaul</option>
        <option value="duty">Fill: radio on-time</option>
        <option value="ua">Fill: current draw</option>
      </select>
    </div>
    <div class="legend">
      <div><span class="swatch sq" style="background:var(--ink)"></span> backhaul <span class="hint">(has an uplink)</span></div>
      <div><span class="swatch" style="background:var(--ink)"></span> router <span class="hint">(chose to route)</span></div>
      <div><span class="swatch hollow"></span> leaf</div>
      <div><span class="swatch" style="background:var(--crit)"></span> no route</div>
      <div><i style="border-color:var(--accent)"></i> uplink</div>
      <div><i style="border-color:var(--ink-3)"></i> link (thicker = cheaper)</div>
      <div><span class="swatch" style="background:var(--tx)"></span> transmit &nbsp;<span class="swatch" style="background:var(--rx)"></span> receive</div>
    </div>
  </div>

  <div class="rail">
    <div class="card panel">
      <h2>Radio energy <small>µA, mean / worst</small></h2>
      <div id="power"></div>
    </div>
    <div class="card panel">
      <h2>Inspector</h2>
      <div id="inspect"><p class="hint">Click a node for its schedule and energy. Hover a link for its cost.</p></div>
    </div>
    <div class="card panel">
      <h2>Events</h2>
      <div class="log" id="log"></div>
    </div>
  </div>

  <footer class="card">
    <div class="transport">
      <button id="play">Play</button>
      <button id="restart" title="Back to the start">Restart</button>
      <div class="clock"><span id="t">0.0</span><small> / <span id="tend">0</span> s</small></div>
      <select id="speed">
        <option value="1">1×</option>
        <option value="4" selected>4×</option>
        <option value="10">10×</option>
        <option value="30">30×</option>
      </select>
      <span class="hint" id="tip">Frames in the last 400 ms are drawn as moving dots.</span>
      <span class="tl-key">
        <span><i style="background:var(--crit)"></i>cluster cut</span>
        <span><i style="background:var(--warn)"></i>link fade</span>
        <span><i style="background:var(--ink-3)"></i>interferer</span>
        <span><i style="background:var(--accent)"></i>uplink down</span>
      </span>
    </div>
    <div class="track">
      <canvas id="activity"></canvas>
      <input type="range" id="scrub" min="0" max="1000" value="0" aria-label="Simulated time" />
    </div>
  </footer>
</div>

<script id="trace" type="application/json">__DATA__</script>
<script>
"use strict";

const DATA = JSON.parse(document.getElementById("trace").textContent);
const NODES = DATA.meta.nodes;
const END = DATA.meta.end_us || 1;
const FRAMES = DATA.frames;      // [t_us, src, peer, chan, type, is_rx, rssi]
const TOPO = DATA.topology;
const DISCRETE = DATA.discrete;
const TYPES = DATA.meta.frame_types || {};

const byAddr = new Map(NODES.map(n => [n.addr, n]));
const SCENARIO = DATA.meta.events || [];

/* The role a node had chosen by this point in the run. Falls back to the uplink
   flag only before the node's first snapshot: a node with an uplink is a backhaul
   from the moment it starts, and everything else starts as a leaf. */
function roleAt(n, st) {
  if (st && st.role != null) return st.role;
  return n.uplink ? 2 : 0;
}

/* Frame classes worth telling apart on screen. Everything else is link
   maintenance, which is the majority and should stay quiet. */
function frameClass(type) {
  if (type === 0) return "beacon";
  if (type >= 1 && type <= 6) return "join";
  if (type === 9) return "data";
  return "link";
}

/* --------------------------------------------------------------------------- */
/* Layout: scenario metres to canvas pixels                                    */
/* --------------------------------------------------------------------------- */

let view = { s: 1, ox: 0, oy: 0 };

function fitView(w, h) {
  const pad = 34;
  const xs = NODES.map(n => n.x), ys = NODES.map(n => n.y);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const spanX = Math.max(1, maxX - minX), spanY = Math.max(1, maxY - minY);
  const s = Math.min((w - pad * 2) / spanX, (h - pad * 2) / spanY);
  view.s = s;
  view.ox = pad + (w - pad * 2 - spanX * s) / 2 - minX * s;
  view.oy = pad + (h - pad * 2 - spanY * s) / 2 - minY * s;
}
const px = n => n.x * view.s + view.ox;
const py = n => n.y * view.s + view.oy;

/* --------------------------------------------------------------------------- */
/* Topology snapshot lookup                                                    */
/* --------------------------------------------------------------------------- */

function snapAt(t_us) {
  let lo = 0, hi = TOPO.length - 1, best = TOPO[0];
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (TOPO[mid].t <= t_us) { best = TOPO[mid]; lo = mid + 1; } else { hi = mid - 1; }
  }
  return best;
}

let maxRank = 1;
for (const s of TOPO) {
  for (const k in s.nodes) {
    const r = s.nodes[k].rank;
    if (r != null && r !== 65535 && r > maxRank) maxRank = r;
  }
}

/* --------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* --------------------------------------------------------------------------- */

const graph = document.getElementById("graph");
const gctx = graph.getContext("2d");
const activity = document.getElementById("activity");
const actx = activity.getContext("2d");

let dpr = 1;
function resize() {
  dpr = Math.min(2, window.devicePixelRatio || 1);
  for (const [cv, cx] of [[graph, gctx], [activity, actx]]) {
    const r = cv.getBoundingClientRect();
    cv.width = Math.max(1, Math.round(r.width * dpr));
    cv.height = Math.max(1, Math.round(r.height * dpr));
    cx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  const r = graph.getBoundingClientRect();
  fitView(r.width, r.height);
  drawActivity();
}

const css = v => getComputedStyle(document.documentElement).getPropertyValue(v).trim();

/* Sequential ramp for the node fill. Built from the accent hue so the graph
   stays in the page's palette instead of introducing a fourth colour family. */
function rampColour(f) {
  f = Math.max(0, Math.min(1, f));
  const dark = document.documentElement.getAttribute("data-theme") === "dark" ||
    (!document.documentElement.getAttribute("data-theme") &&
      window.matchMedia("(prefers-color-scheme: dark)").matches);
  const l = dark ? 72 - f * 34 : 40 + f * 38;
  const c = dark ? 0.11 - f * 0.045 : 0.13 - f * 0.05;
  const h = 268 - f * 34;
  return `oklch(${l}% ${c.toFixed(3)} ${h})`;
}

let showHeard = false, showPackets = true, fillMode = "rank";
let hoverEdge = null, selected = null;
let nodeHit = [];

function drawGraph(t_us) {
  const r = graph.getBoundingClientRect();
  const snap = snapAt(t_us);
  gctx.clearRect(0, 0, r.width, r.height);

  gctx.fillStyle = css("--panel-2");
  gctx.fillRect(0, 0, r.width, r.height);

  const ns = snap.nodes || {};

  /* Edges first, so nodes sit on top of them. */
  const edges = snap.edges || [];
  for (const e of edges) {
    const a = byAddr.get(e.a), b = byAddr.get(e.b);
    if (!a || !b) continue;
    const linked = !!e.linked;
    if (!linked && !showHeard) continue;

    const isHover = hoverEdge && hoverEdge.a === e.a && hoverEdge.b === e.b;
    /* Cost is an ETX in sixteenths: 16 is a link that never retransmits.
       Cheaper links draw heavier, so the spine of the tree is visible. */
    const cost = e.cost == null ? 64 : e.cost;
    const quality = Math.max(0.15, Math.min(1, 16 / Math.max(16, cost)));

    gctx.beginPath();
    gctx.moveTo(px(a), py(a));
    gctx.lineTo(px(b), py(b));
    if (!linked) {
      gctx.setLineDash([2, 4]);
      gctx.lineWidth = 1;
      gctx.strokeStyle = css("--line");
    } else {
      gctx.setLineDash([]);
      gctx.lineWidth = (e.parent ? 2.4 : 1.2) * (0.6 + quality) * (isHover ? 1.8 : 1);
      gctx.strokeStyle = e.parent ? css("--accent") : css("--ink-3");
      gctx.globalAlpha = e.parent ? 0.95 : 0.42 + quality * 0.35;
    }
    gctx.stroke();
    gctx.globalAlpha = 1;
    gctx.setLineDash([]);
  }

  if (showPackets) drawPackets(t_us, edges);

  /* Nodes. Role is shape, so colour is free to carry a measurement.

     The role drawn is the one the node had chosen for itself at this point in the
     run, which is why it changes as the timeline advances. The scenario only says
     which nodes have a backhaul uplink; leaf-versus-router is the protocol's
     decision, so before a node's first snapshot there is nothing better to assume
     than the cheapest role it could be. */
  nodeHit = [];
  for (const n of NODES) {
    const st = ns[String(n.addr)] || {};
    const routed = st.rank != null && st.rank !== 65535;
    const role = roleAt(n, st);
    const x = px(n), y = py(n);
    const rad = role === 2 ? 7.5 : 5.5;

    let fill;
    if (!routed) {
      fill = css("--crit");
    } else if (fillMode === "duty") {
      fill = rampColour(Math.min(1, (st.duty || 0) / 300));
    } else if (fillMode === "ua") {
      /* Scaled to the budget, so a node at or past it saturates the ramp. */
      fill = rampColour(Math.min(1, (nodeUa(st) ?? 0) / BUDGET_UA));
    } else {
      fill = rampColour(maxRank ? (st.rank || 0) / maxRank : 0);
    }

    gctx.beginPath();
    if (role === 2) {
      gctx.rect(x - rad, y - rad, rad * 2, rad * 2);   // backhaul
    } else {
      gctx.arc(x, y, rad, 0, Math.PI * 2);             // router / leaf
    }

    if (role === 0) {
      gctx.lineWidth = 1.8;
      gctx.strokeStyle = fill;
      gctx.stroke();
    } else {
      gctx.fillStyle = fill;
      gctx.fill();
    }

    if (selected === n.addr) {
      gctx.beginPath();
      gctx.arc(x, y, rad + 5, 0, Math.PI * 2);
      gctx.lineWidth = 2;
      gctx.strokeStyle = css("--accent");
      gctx.stroke();
    }
    nodeHit.push({ addr: n.addr, x, y, r: rad + 6 });
  }
}

/* Packets are the moving part: each frame in the trailing window is a dot
   travelling from its sender towards its peer, so the direction of traffic and
   the shape of the tree are visible at the same time. */
const WINDOW_US = 400000;
let cursor = 0;

function drawPackets(t_us, edges) {
  /* FRAMES is time-sorted; keep a cursor so scrubbing forward is cheap. */
  if (cursor > 0 && FRAMES[cursor - 1] && FRAMES[cursor - 1][0] > t_us) cursor = 0;
  while (cursor < FRAMES.length && FRAMES[cursor][0] < t_us - WINDOW_US) cursor++;

  const colours = {
    data: css("--accent"), join: css("--tx"),
    beacon: css("--ink-3"), link: css("--rx"),
  };

  for (let i = cursor; i < FRAMES.length; i++) {
    const f = FRAMES[i];
    if (f[0] > t_us) break;
    const a = byAddr.get(f[1]);
    if (!a) continue;
    const age = (t_us - f[0]) / WINDOW_US;
    const alpha = 1 - age;
    const cls = frameClass(f[4]);

    if (!f[2]) {
      /* A broadcast, so there is no far end: draw an expanding ring instead of
         a dot, which reads as "sent to nobody in particular". */
      gctx.beginPath();
      gctx.arc(px(a), py(a), 6 + age * 16, 0, Math.PI * 2);
      gctx.strokeStyle = colours.beacon;
      gctx.globalAlpha = alpha * 0.5;
      gctx.lineWidth = 1.2;
      gctx.stroke();
      gctx.globalAlpha = 1;
      continue;
    }

    const b = byAddr.get(f[2]);
    if (!b) continue;
    /* Receptions travel the other way, so both halves of an exchange are
       visible rather than overlapping. */
    const [from, to] = f[5] ? [b, a] : [a, b];
    const p = 1 - age;
    const x = px(from) + (px(to) - px(from)) * p;
    const y = py(from) + (py(to) - py(from)) * p;

    gctx.beginPath();
    gctx.arc(x, y, cls === "data" ? 3.2 : 2.2, 0, Math.PI * 2);
    gctx.fillStyle = f[5] ? css("--rx") : (cls === "data" ? colours.data : css("--tx"));
    gctx.globalAlpha = 0.35 + alpha * 0.65;
    gctx.fill();
    gctx.globalAlpha = 1;
  }
}

/* --------------------------------------------------------------------------- */
/* Activity histogram under the scrubber                                       */
/* --------------------------------------------------------------------------- */

let bins = null;
function computeBins(n) {
  const b = { tx: new Float64Array(n), rx: new Float64Array(n), routed: new Float64Array(n),
              ua: new Float64Array(n) };
  for (const f of FRAMES) {
    const i = Math.min(n - 1, Math.floor(f[0] / END * n));
    if (f[5]) b.rx[i]++; else b.tx[i]++;
  }
  for (const s of TOPO) {
    const i = Math.min(n - 1, Math.floor(s.t / END * n));
    let routed = 0;
    for (const k in s.nodes) {
      const r = s.nodes[k].rank;
      if (r != null && r !== 65535) routed++;
    }
    b.routed[i] = Math.max(b.routed[i], routed);

    /* Mean current across the grid, so the energy cost of each disturbance shows
       up on the timeline next to its cause. */
    const us = [];
    for (const k in s.nodes) {
      const ua = nodeUa(s.nodes[k]);
      if (ua != null) us.push(ua);
    }
    if (us.length) {
      us.sort((a, b2) => a - b2);
      b.ua[i] = Math.max(b.ua[i], us[us.length >> 1]);
    }
  }
  /* Carry forward so the lines are continuous between samples. */
  let last = 0, lastUa = 0;
  for (let i = 0; i < n; i++) {
    if (b.routed[i] === 0) b.routed[i] = last; else last = b.routed[i];
    if (b.ua[i] === 0) b.ua[i] = lastUa; else lastUa = b.ua[i];
  }
  return b;
}

function drawActivity() {
  const r = activity.getBoundingClientRect();
  const n = Math.max(40, Math.floor(r.width));
  if (!bins || bins.tx.length !== n) bins = computeBins(n);
  actx.clearRect(0, 0, r.width, r.height);

  const peak = Math.max(1, ...bins.tx, ...bins.rx);
  const h = r.height - 12;

  /* What the scenario did to the radio environment, drawn under the activity so a
     burst of reconnections can be read against its cause rather than guessed at.
     Outages are the strongest: every path across a cluster boundary is faded out,
     so everything on the far side has to find another way round. */
  const bandColour = { outage: css("--crit"), fade: css("--warn"),
                       interferer: css("--ink-3"), uplink: css("--accent") };
  const bandRow = { outage: 0, fade: 1, interferer: 2, uplink: 3 };
  for (const ev of SCENARIO) {
    const x0 = ev.start_us / END * r.width;
    const x1 = Math.max(x0 + 1, ev.end_us / END * r.width);
    if (ev.kind === "outage" || ev.kind === "uplink") {
      actx.fillStyle = bandColour[ev.kind];
      actx.globalAlpha = ev.kind === "outage" ? 0.13 : 0.08;
      actx.fillRect(x0, 0, x1 - x0, h);
    }
    actx.globalAlpha = ev.kind === "outage" || ev.kind === "uplink" ? 0.85 : 0.5;
    actx.fillStyle = bandColour[ev.kind] || css("--ink-3");
    actx.fillRect(x0, h + (bandRow[ev.kind] ?? 0) * 3.5, x1 - x0, 2.5);
  }
  actx.globalAlpha = 1;

  for (let i = 0; i < n; i++) {
    const x = i / n * r.width;
    const w = Math.max(1, r.width / n);
    const ht = bins.tx[i] / peak * h * 0.5;
    const hr = bins.rx[i] / peak * h * 0.5;
    actx.fillStyle = css("--tx");
    actx.globalAlpha = 0.75;
    actx.fillRect(x, h / 2 - ht, w, ht);
    actx.fillStyle = css("--rx");
    actx.fillRect(x, h / 2, w, hr);
  }
  actx.globalAlpha = 1;

  /* Convergence: how many nodes had a route. The point of the whole run. */
  actx.beginPath();
  for (let i = 0; i < n; i++) {
    const x = i / n * r.width;
    const y = h - (bins.routed[i] / NODES.length) * h;
    i ? actx.lineTo(x, y) : actx.moveTo(x, y);
  }
  actx.strokeStyle = css("--good");
  actx.lineWidth = 1.6;
  actx.stroke();

  /* Mean current, on its own scale topped at twice the target so the target line
     sits mid-height and crossing it is unmistakable. */
  const uaFull = BUDGET_UA * 2;
  actx.beginPath();
  for (let i = 0; i < n; i++) {
    const x = i / n * r.width;
    const y = h - Math.min(1, bins.ua[i] / uaFull) * h;
    i ? actx.lineTo(x, y) : actx.moveTo(x, y);
  }
  actx.strokeStyle = css("--warn");
  actx.lineWidth = 1.6;
  actx.stroke();

  actx.setLineDash([3, 3]);
  actx.beginPath();
  actx.moveTo(0, h / 2);
  actx.lineTo(r.width, h / 2);
  actx.strokeStyle = css("--ink-3");
  actx.lineWidth = 1;
  actx.globalAlpha = 0.55;
  actx.stroke();
  actx.setLineDash([]);
  actx.globalAlpha = 1;

  actx.fillStyle = css("--ink-3");
  actx.font = "10px " + css("--mono");
  actx.fillText("transmit / receive per bin", 4, 10);
  actx.fillStyle = css("--warn");
  actx.fillText(`median µA (${BUDGET_UA} target at mid-height)`, 4, h - 3);
  actx.fillStyle = css("--good");
  actx.fillText("nodes routed", r.width - 78, 10);
}

/* --------------------------------------------------------------------------- */
/* Inspector                                                                   */
/* --------------------------------------------------------------------------- */

const fmtUs = u => u == null ? "—" : (u >= 1e6 ? (u / 1e6).toFixed(2) + " s" :
  u >= 1e3 ? (u / 1e3).toFixed(1) + " ms" : u + " µs");
const ROLE = ["leaf", "router", "backhaul"];

function renderInspector(t_us) {
  const el = document.getElementById("inspect");
  const snap = snapAt(t_us);

  if (hoverEdge) {
    const e = hoverEdge;
    el.innerHTML = `
      <dl class="kv">
        <dt>link</dt><dd>0x${e.a.toString(16).padStart(4, "0")} ↔ 0x${e.b.toString(16).padStart(4, "0")}</dd>
        <dt>state</dt><dd>${e.linked ? '<span class="pill ok">linked</span>' : '<span class="pill bad">heard only</span>'}</dd>
        <dt>uplink</dt><dd>${e.parent ? "yes" : "no"}</dd>
        <dt>cost</dt><dd>${e.cost ?? "—"}</dd>
        <dt>ETX</dt><dd>${e.etx == null ? "—" : (e.etx / 16).toFixed(2)}</dd>
        <dt>error rate</dt><dd>${e.per == null ? "—" : (e.per / 10).toFixed(1) + " %"}</dd>
        <dt>RSSI</dt><dd>${e.rssi ?? "—"} dBm</dd>
        <dt>interval</dt><dd>${e.int ? (e.int / 1000) + " ms" : "—"}</dd>
        <dt>subrate</dt><dd>${e.sub ? "×" + e.sub : "—"}</dd>
        <dt>rx window</dt><dd>${e.win == null ? "—" : e.win + " µs"}</dd>
      </dl>
      <p class="hint" style="margin:9px 0 0">A window near its floor means the two ends are
      synchronised; a wide one means drift has been accumulating.</p>`;
    return;
  }

  if (selected == null) {
    el.innerHTML = '<p class="hint">Click a node for its schedule and energy. Hover a link for its cost.</p>';
    return;
  }

  const meta = byAddr.get(selected) || {};
  const st = (snap.nodes || {})[String(selected)] || {};
  const routed = st.rank != null && st.rank !== 65535;
  const role = roleAt(meta, st);
  /* How many times this node has changed its mind so far. */
  let changes = 0, lastChange = null;
  for (const d of DISCRETE) {
    if (d.t > t_us) break;
    if (d.ev === "role" && d.n === selected) { changes++; lastChange = d; }
  }
  const on = st.on || 0;
  const parts = [
    ["rx link", st.rxlink || 0, "--rx"],
    ["rx scan", st.rxscan || 0, "--rx"],
    ["rx join", st.rxjoin || 0, "--rx"],
    ["tx link", st.txlink || 0, "--tx"],
    ["tx beacon", st.txbeacon || 0, "--tx"],
  ];
  const peak = Math.max(1, ...parts.map(p => p[1]));

  el.innerHTML = `
    <dl class="kv">
      <dt>address</dt><dd>0x${selected.toString(16).padStart(4, "0")}</dd>
      <dt>role</dt><dd>${ROLE[role] || "?"}${role === 2 ? "" :
        `<small class="hint"> chosen at runtime${changes ? `, ${changes} change${changes > 1 ? "s" : ""}` : ""}</small>`}</dd>
      <dt>position</dt><dd>${meta.x} , ${meta.y} m${meta.cluster != null ?
        `<small class="hint"> cluster ${meta.cluster}</small>` : ""}</dd>
      <dt>rank</dt><dd>${routed ? st.rank : '<span class="pill bad">no route</span>'}</dd>
      <dt>uplink</dt><dd>${st.parent ? "0x" + st.parent.toString(16).padStart(4, "0") : "—"}</dd>
      <dt>links</dt><dd>${st.links ?? "—"}</dd>
      <dt>radio on</dt><dd>${fmtUs(on)}</dd>
      <dt>duty</dt><dd>${st.duty == null ? "—" : (st.duty / 10).toFixed(1) + " %"}</dd>
      <dt>current</dt><dd>${nodeUa(st) == null ? "—" :
        `<span class="pill ${nodeUa(st) > BUDGET_UA ? "bad" : "ok"}">${nodeUa(st).toFixed(1)} µA</span>`}
        ${st.ua == null ? "" : `<small class="hint"> ${st.ua} since boot</small>`}</dd>
      <dt>radio starts</dt><dd>${st.starts == null ? "—" : st.starts.toLocaleString()}</dd>
      <dt>channels</dt><dd>${st.map == null ? "—" : popcount(st.map) + " of 16"}</dd>
    </dl>
    <div class="bars">
      ${parts.map(([k, v, c]) => `
        <div class="bar-row">
          <span>${k}</span>
          <span class="bar"><i style="width:${(v / peak * 100).toFixed(1)}%;background:var(${c})"></i></span>
          <span>${fmtUs(v)}</span>
        </div>`).join("")}
    </div>
    <p class="hint" style="margin:9px 0 0">Receive dominates by design; the bars show which
    activity is buying it.${lastChange ?
      ` Became a ${ROLE[lastChange.new]} at ${(lastChange.t / 1e6).toFixed(1)}s, hearing
        ${lastChange.routers} other router${lastChange.routers === 1 ? "" : "s"}.` : ""}</p>`;
}

const popcount = v => { let c = 0; while (v) { c += v & 1; v >>= 1; } return c; };

function renderLog(t_us) {
  const el = document.getElementById("log");
  const interesting = new Set(["linkup", "linkdown", "parent", "joinok", "joinfail",
    "supervision", "chanmap", "deliver", "role"]);
  const rows = [];
  for (let i = DISCRETE.length - 1; i >= 0 && rows.length < 40; i--) {
    const d = DISCRETE[i];
    if (d.t > t_us || !interesting.has(d.ev)) continue;
    if (selected != null && d.n !== selected &&
        d.peer !== selected && d.new !== selected && d.origin !== selected) continue;
    rows.push(d);
  }
  el.innerHTML = rows.map(d => {
    const n = "0x" + d.n.toString(16).padStart(4, "0");
    const p = d.peer != null ? "0x" + d.peer.toString(16).padStart(4, "0") : "";
    let txt;
    switch (d.ev) {
      case "linkup":   txt = `<b>${n}</b> linked ${p}`; break;
      case "linkdown": txt = `<b>${n}</b> lost ${p}`; break;
      case "parent":   txt = `<b>${n}</b> uplink → 0x${(d.new || 0).toString(16).padStart(4, "0")}, rank ${d.rank}`; break;
      case "joinok":   txt = `<b>${n}</b> authenticated ${p}${d.resume ? " (resumed)" : ""}`; break;
      case "joinfail": txt = `<b>${n}</b> handshake failed ${p}`; break;
      case "supervision": txt = `<b>${n}</b> supervision timeout ${p}`; break;
      case "chanmap":  txt = `<b>${n}</b> channel map → ${popcount(d.map)} of 16`; break;
      case "deliver":  txt = `<b>${n}</b> received from 0x${(d.origin || 0).toString(16).padStart(4, "0")}, ${d.hops} hops`; break;
      case "role":     txt = `<b>${n}</b> became a ${ROLE[d.new] || "?"} <span class="hint">(was ${ROLE[d.old] || "?"}, hears ${d.routers} routers)</span>`; break;
      default: txt = d.ev;
    }
    return `<div><time>${(d.t / 1e6).toFixed(1)}s</time><span>${txt}</span></div>`;
  }).join("") || '<p class="hint">Nothing yet at this point on the timeline.</p>';
}

/* The current each node's radio on-time implies, at the point on the timeline
   being viewed.

   Measured over a sliding window from the firmware's own cumulative charge, not
   averaged from boot. In a run that takes a couple of minutes to converge, a
   boot average is dominated by the cost of getting there: a node that scanned
   hard for two minutes before finding a route carries that in its average for the
   rest of the run, however cheap it becomes afterwards. A window answers the
   question actually being asked - what is this node costing now - and it also
   makes the cost of an outage visible, because the figure rises while the grid is
   repairing itself and falls back when it is done. */
const BUDGET_UA = 20;
const PWR = DATA.meta.power || { roles: {}, window_s: 20 };

/* Windowed where available, falling back to the boot average only for a node
   that has not yet produced two samples. */
function nodeUa(v) {
  if (v.uaw != null) return v.uaw;
  return v.steady != null ? v.steady : v.ua;
}

function powerAt(snap) {
  const ns = snap.nodes || {};
  const out = { n: 0, mean: 0, median: 0, worst: 0, worstAddr: null,
                byRole: [[], [], []], starts: 0, fixed: 0, over: 0 };
  const all = [];
  let sum = 0;
  for (const k in ns) {
    const v = ns[k];
    const ua = nodeUa(v);
    if (ua == null) continue;
    out.n++;
    sum += ua;
    all.push(ua);
    if (ua > out.worst) { out.worst = ua; out.worstAddr = Number(k); }
    if (ua > BUDGET_UA) out.over++;
    const r = roleAt(byAddr.get(Number(k)) || {}, v);
    if (r >= 0 && r <= 2) out.byRole[r].push(ua);
    out.starts += v.starts || 0;
    out.fixed += v.fixed || 0;
  }
  out.mean = out.n ? sum / out.n : 0;
  all.sort((a, b) => a - b);
  out.median = all.length ? all[all.length >> 1] : 0;
  return out;
}

function renderPower(t_us) {
  const el = document.getElementById("power");
  const pw = powerAt(snapAt(t_us));
  if (!pw.n) {
    el.innerHTML = '<p class="hint">No energy samples yet at this point.</p>';
    return;
  }

  /* Scaled against the budget rather than against the worst node, so a bar that
     crosses the line is the thing that draws the eye.

     The bar is the median, not the mean. A node that has lost its route receives
     almost continuously by design and can sit two orders of magnitude above a
     settled one, which drags a mean far away from anything a typical node is
     doing. The median says what most nodes cost; the tick says how bad the worst
     one is. Both are needed and neither substitutes for the other. */
  const scale = Math.max(BUDGET_UA * 1.25, 3);
  const row = (label, vals) => {
    if (!vals.length) return "";
    const sorted = [...vals].sort((a, b) => a - b);
    const median = sorted[sorted.length >> 1];
    const worst = sorted[sorted.length - 1];
    const clip = v => Math.min(100, v / scale * 100);
    return `
      <div class="pw-row">
        <span class="pw-label">${label}<small>${vals.length}</small></span>
        <span class="pw-bar">
          <i class="${median > BUDGET_UA ? "over" : ""}" style="width:${clip(median).toFixed(1)}%"></i>
          <u class="${worst > BUDGET_UA ? "bad" : "ok"}" style="left:${clip(worst).toFixed(1)}%"></u>
          <b style="left:${clip(BUDGET_UA).toFixed(1)}%"></b>
        </span>
        <span class="pw-val">${median.toFixed(1)}<small> / ${worst.toFixed(0)}</small></span>
      </div>`;
  };

  const settled = PWR.converged_us != null && t_us >= PWR.converged_us;

  el.innerHTML =
    `<div class="pw-head"><span></span><span>median&nbsp;/&nbsp;worst&nbsp;µA</span></div>` +
    row("backhaul", pw.byRole[2]) + row("router", pw.byRole[1]) + row("leaf", pw.byRole[0]) +
    `<dl class="kv" style="margin-top:10px">
       <dt>over budget</dt><dd>${pw.over === 0 ?
         `<span class="pill ok">none of ${pw.n}</span>` :
         `<span class="pill bad">${pw.over} of ${pw.n}</span>`}</dd>
       <dt>grid state</dt><dd>${settled ?
         '<span class="pill ok">converged</span>' :
         '<span class="pill bad">still forming</span>'}
         ${PWR.converged_us != null ?
           `<small class="hint"> all routed at ${(PWR.converged_us / 1e6).toFixed(0)}s</small>` : ""}</dd>
       <dt>radio starts</dt><dd>${pw.starts.toLocaleString()} <small class="hint">across the grid</small></dd>
     </dl>
     <p class="hint" style="margin:9px 0 0">Measured over the preceding
     ${PWR.window_s}&thinsp;s, so it tracks what the grid is costing now rather than
     averaging in how it formed. Bar is the median for the role, the tick its worst
     node, the dashed rule the ${BUDGET_UA}&thinsp;µA target. A node with no route
     receives almost continuously by design and can sit two orders of magnitude above
     a settled one — which is why the worst node climbs during formation and after each
     cluster cut, and why the median is the figure to read for a typical node.</p>`;
}

function renderStats(t_us) {
  const snap = snapAt(t_us);
  const ns = snap.nodes || {};
  let routed = 0, links = 0, duty = 0, dutyN = 0, on = 0;
  const roles = [0, 0, 0];
  for (const k in ns) {
    const v = ns[k];
    if (v.rank != null && v.rank !== 65535) routed++;
    if (v.duty != null) { duty += v.duty; dutyN++; }
    if (v.on != null) on = Math.max(on, v.on);
    const r = roleAt(byAddr.get(Number(k)) || {}, v);
    if (r >= 0 && r <= 2) roles[r]++;
  }
  const pw = powerAt(snap);
  for (const e of snap.edges || []) if (e.linked) links++;

  let delivered = 0, forwarded = 0, joins = 0, roleChanges = 0, healed = 0;
  for (const d of DISCRETE) {
    if (d.t > t_us) break;
    if (d.ev === "deliver") delivered++;
    else if (d.ev === "forward") forwarded++;
    else if (d.ev === "joinok") joins++;
    else if (d.ev === "role") roleChanges++;
    /* A parent change after the grid has formed is the protocol healing: the node
       had a route, lost the link carrying it, and found another. */
    else if (d.ev === "parent" && d.t > 30000000) healed++;
  }

  const stat = (label, value, sub) => `
    <div class="stat"><dt>${label}</dt><dd>${value}${sub ? `<small> ${sub}</small>` : ""}</dd></div>`;

  document.getElementById("stats").innerHTML =
    stat("routed", routed, "/ " + NODES.length) +
    stat("roles", `${roles[1]}<small>R</small> ${roles[0]}<small>L</small>`,
         `${roles[2]} backhaul`) +
    stat("role changes", roleChanges) +
    stat("re-parented", healed, "after 30s") +
    stat("links", links) +
    stat("delivered", delivered) +
    /* Radio on-time is the metric the protocol was built around, so the current it
       implies is a headline figure rather than something to go hunting for. */
    stat("median current", pw.n ? pw.median.toFixed(1) : "—",
         `µA / ${PWR.window_s}s`) +
    stat("worst node", pw.n ? pw.worst.toFixed(0) : "—",
         `µA / ${BUDGET_UA} target`);
}

/* --------------------------------------------------------------------------- */
/* Interaction                                                                 */
/* --------------------------------------------------------------------------- */

let t = 0, playing = false, speed = 4, lastFrame = 0;

function setT(v) {
  t = Math.max(0, Math.min(END, v));
  document.getElementById("scrub").value = String(Math.round(t / END * 1000));
  document.getElementById("t").textContent = (t / 1e6).toFixed(1);
  render();
}

function render() {
  drawGraph(t);
  renderStats(t);
  renderPower(t);
  renderInspector(t);
  renderLog(t);
}

function loop(now) {
  if (playing) {
    const dt = lastFrame ? (now - lastFrame) : 16;
    setT(t + dt * 1000 * speed);
    if (t >= END) { playing = false; document.getElementById("play").textContent = "Play"; }
  }
  lastFrame = now;
  requestAnimationFrame(loop);
}

document.getElementById("play").addEventListener("click", e => {
  playing = !playing;
  if (playing && t >= END) setT(0);
  e.currentTarget.textContent = playing ? "Pause" : "Play";
});
document.getElementById("restart").addEventListener("click", () => setT(0));
document.getElementById("scrub").addEventListener("input", e => {
  playing = false;
  document.getElementById("play").textContent = "Play";
  setT(Number(e.target.value) / 1000 * END);
});
document.getElementById("speed").addEventListener("change", e => { speed = Number(e.target.value); });
document.getElementById("btn-heard").addEventListener("click", e => {
  showHeard = !showHeard;
  e.currentTarget.setAttribute("aria-pressed", String(showHeard));
  render();
});
document.getElementById("btn-packets").addEventListener("click", e => {
  showPackets = !showPackets;
  e.currentTarget.setAttribute("aria-pressed", String(showPackets));
  render();
});
document.getElementById("colour").addEventListener("change", e => {
  fillMode = e.target.value;
  render();
});

/* Hit testing: nodes take priority over edges, since they are the smaller target. */
function pick(mx, my) {
  for (const h of nodeHit) {
    if ((mx - h.x) ** 2 + (my - h.y) ** 2 < h.r * h.r) return { node: h.addr };
  }
  const snap = snapAt(t);
  let best = null, bestD = 7;
  for (const e of snap.edges || []) {
    if (!e.linked && !showHeard) continue;
    const a = byAddr.get(e.a), b = byAddr.get(e.b);
    if (!a || !b) continue;
    const d = segDist(mx, my, px(a), py(a), px(b), py(b));
    if (d < bestD) { bestD = d; best = e; }
  }
  return best ? { edge: best } : null;
}

function segDist(x, y, x1, y1, x2, y2) {
  const dx = x2 - x1, dy = y2 - y1;
  const len = dx * dx + dy * dy || 1;
  let s = ((x - x1) * dx + (y - y1) * dy) / len;
  s = Math.max(0, Math.min(1, s));
  return Math.hypot(x - (x1 + s * dx), y - (y1 + s * dy));
}

graph.addEventListener("mousemove", ev => {
  const r = graph.getBoundingClientRect();
  const hit = pick(ev.clientX - r.left, ev.clientY - r.top);
  const next = hit && hit.edge ? hit.edge : null;
  graph.style.cursor = hit ? "pointer" : "default";
  if (next !== hoverEdge) { hoverEdge = next; render(); }
});
graph.addEventListener("mouseleave", () => { if (hoverEdge) { hoverEdge = null; render(); } });
graph.addEventListener("click", ev => {
  const r = graph.getBoundingClientRect();
  const hit = pick(ev.clientX - r.left, ev.clientY - r.top);
  selected = hit && hit.node != null ? (selected === hit.node ? null : hit.node) : selected;
  if (hit && hit.node == null && !hit.edge) selected = null;
  render();
});

window.addEventListener("resize", () => { resize(); render(); });
window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => { drawActivity(); render(); });
new MutationObserver(() => { drawActivity(); render(); })
  .observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });

document.getElementById("scale").textContent =
  `${NODES.length} nodes · ${DATA.meta.spacing_m} m spacing · ${DATA.meta.range_m} m range`;
document.getElementById("tend").textContent = (END / 1e6).toFixed(0);
document.getElementById("tip").textContent =
  `${DATA.meta.frames_kept.toLocaleString()} frames traced · dots show the last 400 ms`;

resize();
setT(0);
requestAnimationFrame(loop);
</script>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--trace", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--title", default=None)
    args = ap.parse_args()

    with open(args.trace) as f:
        raw = f.read()
    doc = json.loads(raw)

    title = args.title or f"{len(doc['meta']['nodes'])} node mesh"

    # </script> inside the JSON would close the tag early.
    safe = raw.replace("</", "<\\/")

    # Token replacement rather than %-formatting: the template is mostly CSS and
    # JavaScript, both of which are full of literal percent signs.
    html = HTML.replace("__TITLE__", title).replace("__DATA__", safe)
    with open(args.out, "w") as f:
        f.write(html)

    print(f"{args.out} ({os.path.getsize(args.out)/1024:.0f} kB)")


if __name__ == "__main__":
    main()
