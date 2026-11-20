// SPDX-License-Identifier: GPL-3.0-or-later
// The live link in the page (0.4): STATE and CTRL against the firmware's own test (tests/basic.txt, decoded by
// host/tests too), the clock under USB's and Bluetooth's jitter, the plan across a switch, the conductor's patterns,
// and Bluetooth's packets there and back.
//   node web/tests/live_test.js
"use strict";
const fs = require("fs"), path = require("path"), assert = require("assert");
const MT = require("../setlist.js"), Live = require("../live.js"), Cond = require("../conductor.js"), Link = require("../link.js");
const ROOT = path.join(__dirname, "..", "..");
let n = 0;
function ok(c, what) { n++; assert.ok(c, what); }
function framesIn(kind, type) {
  return fs.readFileSync(path.join(ROOT, "tests", "basic.txt"), "utf8").split("\n").map(l => l.trim().split(/\s+/))
    .filter(w => w[0] === kind && w.slice(1, 5).join(" ") === "F0 7D 4D 54").map(w => w.slice(1).map(x => parseInt(x, 16)))
    .filter(f => f[5] === type);
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// ---- STATE and CTRL: the firmware's frames
const st = framesIn("expect_sysex", MT.T.STATE).map(f => MT.decodeState(MT.parse(f).payload));
ok(st.length === 3, "three STATEs in basic.txt");
ok(!st[0].running && st[0].active === 4 && st[0].songNo === 4 && st[0].song.mode === 1 && st[0].song.bpm === 150, "stopped: song 5");
ok(same(st[0].song.mix, [7, 4, 0, 2]) && st[0].song.accent_every === 3, "song 5's CLASSIC mix");
ok(st[1].running && st[1].seq === 1 && st[1].q8 === Math.floor(44100 * 60 * 256 / 150 / 12), "started: q8 at 150 BPM");
ok(st[1].tick === 0 && st[1].start === st[1].at, "anchored at tick 0");
ok(st[2].start === st[1].start && st[2].q8 === st[1].q8, "the periodic STATE keeps the anchor");
const sent = framesIn("sysex", MT.T.CTRL).map(f => f.join(" "));
for (const f of [MT.ctrl(MT.C.START), MT.ctrl(MT.C.STOP), MT.ctrl(MT.C.PICK, 0), MT.ctrl(MT.C.BPM, 100),
  MT.ctrl(MT.C.MUTE, 1), MT.ctrl(MT.C.MUTE, 0)]) ok(sent.includes(f.join(" ")), `CTRL ${f.join(" ")} in basic.txt`);
ok(MT.q8Mul(12, st[1].q8) === Math.round(12 * st[1].q8 / 256), "q8Mul: a beat");

// ---- the clock: STATEs over a link with jitter, the page's estimate of when each tick sounds
function rng(seed) { let s = seed; return () => (s = (s * 1103515245 + 12345) % 2147483648) / 2147483648; }
function stateAt(devNow, over) {   // a STATE as the FM-1 would send it at sample devNow
  return Object.assign({ seq: 0, flags: 1, active: 0, songNo: 0, song: MT.song({ used: true, bpm: 120 }), tick: 0,
    at: 1000000, q8: Math.floor(44100 * 60 * 256 / 120 / 12), now: devNow >>> 0, running: true, muted: false, next: false }, over || {});
}
function simulate(name, lagFn, base, ppm) {   // base: the link's least lag, a fixed latency (the calibration's)
  const clk = Live.clock(), P = Live.plan(clk), r = rng(7), trueOffset = 5000;   // page ms = device ms * (1+ppm) + 5000
  let worst = 0;
  for (let k = 0; k < 120; k++) {   // two minutes, a STATE a second
    const dev = 1000000 + k * 44100, devMs = dev / 44.1;
    const pageSent = devMs * (1 + ppm * 1e-6) + trueOffset;
    P.take(stateAt(dev), pageSent + lagFn(r, k));
    if (k < 10) continue;   // (it settles over the first few)
    const tick = k * 24 + 6, devT = P.cur.timeOf(tick), truePage = devT / 44.1 * (1 + ppm * 1e-6) + trueOffset;
    worst = Math.max(worst, Math.abs(clk.toPage(devT) - base - truePage));
  }
  ok(worst < 1.0, `${name}: the beats within 1 ms past its fixed latency (worst ${worst.toFixed(3)} ms)`);
}
simulate("USB, 2 ms and jitter to 3 ms more", (r) => 2 + r() * 3, 2, 0);
simulate("Bluetooth by timestamp, 0.2-0.7 ms", (r) => 0.2 + r() * 0.5, 0.2, 0);
simulate("Bluetooth bunched (one in four late by 30 ms)", (r, k) => 0.3 + (k % 4 ? r() * 0.5 : 30), 0.3, 0);
simulate("100 ppm drift", (r) => 1 + r(), 1, 100);

// u32 samples wrap after 27 h: the clock carries on
{
  const clk = Live.clock(), P = Live.plan(clk), near = 4294967296 - 44100;
  P.take(stateAt(near, { at: near - 1000 }), 1000);
  const before = clk.offset;
  P.take(stateAt((near + 88200) >>> 0, { at: near - 1000 }), 3000);
  ok(Math.abs(clk.offset - before) < 0.01, "the wrap of the FM-1's samples");
}

// ---- the plan: ticks between two times, a switch at the next bar
{
  const clk = Live.clock(), P = Live.plan(clk), q8 = Math.floor(44100 * 60 * 256 / 120 / 12);
  ok(P.take(stateAt(990000), 0) === true, "a start changes the plan");
  ok(P.take(stateAt(990000 + 44100), 1000) === false, "the same STATE again does not");
  const t = P.ticks(1000000, 1000000 + 22050);   // half a second at 120: a beat, 12 ticks
  ok(t.length === 12 && t[0].tick === 0 && t[0].dev === 1000000, "a beat's ticks from tick 0");
  const barAt = P.cur.timeOf(48);   // the next bar of 4/4: song 2 at 90 BPM, its own bar from 0 there
  const q8b = Math.floor(44100 * 60 * 256 / 90 / 12), song2 = MT.song({ used: true, bpm: 90, beats: 3 });
  ok(P.take(stateAt(1030000, { flags: 5, next: true, songNo: 1, song: song2, tick: 0, at: barAt, q8: q8b }), 1100) === true, "a switch waiting changes it");
  const across = P.ticks(barAt - 2 * MT.q8Mul(1, q8), barAt + 3 * MT.q8Mul(1, q8b));
  ok(across.length === 5 && across[1].seg === P.cur && across[2].seg === P.next && across[2].tick === 0 && across[2].dev === barAt, "the ticks run across the switch");
  ok(P.where(barAt + 1).seg.songNo === 1 && P.where(barAt - 1).seg.songNo === 0, "where: the song at a time");
  // the switch lands: the FM-1's STATE of what plays now is the waiting one, anchored at tick 0
  ok(P.take(stateAt(barAt + 1000, { songNo: 1, song: song2, tick: 0, at: barAt, q8: q8b }), 2000) === false, "the landed switch is no change");
  ok(P.next === null && P.cur.songNo === 1, "and the waiting one is now what plays");
  ok(P.take(stateAt(barAt + 2000, { flags: 0, running: false }), 2100) === true && !P.running, "a stop");
}

// ---- the conductor: a pattern for every meter
const pat = s => Cond.patternFor(MT.song(Object.assign({ used: true }, s)));
const preset = name => { const g = MT.song({ used: true }); MT.presetApply(g, MT.PRESETS.findIndex(p => p[0] === name)); return Cond.patternFor(g); };
ok(same(pat({ beats: 4 }).beats, [12, 12, 12, 12]), "4/4 in 4");
ok(same(pat({ beats: 3, div: 2 }).beats, [12, 12, 12]), "3/4 in eighths: in 3");
ok(same(preset("6/8").beats, [12, 12]), "6/8 in 2 (dotted quarters)");
ok(same(preset("12/8").beats, [12, 12, 12, 12]), "12/8 in 4");
ok(same(preset("6/8 IN 6").beats, [12, 12, 12, 12, 12, 12]), "6/8 IN 6 in 6");
ok(same(preset("5/8 3+2").beats, [36, 24]), "5/8 3+2 in 2, long and short");
ok(same(preset("7/8 2+2+3").beats, [24, 24, 36]), "7/8 2+2+3 in 3");
ok(same(pat({ mode: 1, accent_every: 3 }).beats, [12, 12, 12]) && same(pat({ mode: 1, accent_every: 0 }).beats, [12]), "CLASSIC: by its accent");
for (let k = 1; k <= 9; k++) {
  const p = { beats: Array(k).fill(12) }, pts = Cond.points(k);
  ok(pts.length === k && pts[0].x === 0 && pts[0].y > 0.85, `in ${k}: the first beat down`);
  let worst = 0;
  for (let i = 0; i < k; i++) {   // the tip is on each beat's point at its tick, and moves smoothly between
    const q = Cond.pos(p, i * 12);
    worst = Math.max(worst, Math.hypot(q.x - pts[i].x, q.y - pts[i].y));
  }
  ok(worst < 1e-9, `in ${k}: the tip on each beat's point`);
  let jump = 0;
  for (let t = 0; t < k * 12; t += 0.25) { const a = Cond.pos(p, t), b = Cond.pos(p, t + 0.25); jump = Math.max(jump, Math.hypot(a.x - b.x, a.y - b.y)); }
  ok(jump < 0.15, `in ${k}: no jumps`);
}
ok(Cond.points(4)[1].x < 0 && Cond.points(4)[2].x > 0 && Cond.points(3)[1].x > 0, "4: left then right; 3: right");

// ---- Bluetooth: a frame out as packets and back, in pieces, with the timestamps
{
  const f = [0xF0, 0x7D, 0x4D, 0x54, 0x01, 0x46, ...Array.from({ length: 55 }, (_, i) => i), 0xF7];
  const pk = Link.blePackets(f, 12345);
  ok(pk.every(p => p.length <= Link.BLE_PKT) && pk.length === Math.ceil((f.length + 3) / 19), "packets of 20 bytes at most");
  const parser = Link.bleParser();
  const got = pk.flatMap((p, i) => parser.feed(p, 50000 + i));
  ok(got.length === 1 && same(Array.from(got[0].data), f), "the frame back whole");
  const notes = parser.feed([0x80 | ((60000 >> 7) & 0x3F), 0x80 | (60000 & 0x7F), 0x90, 0x00, 0x7F, 0x80 | ((60005 & 0x7F)), 0x00, 0x00], 60200);
  ok(notes.length === 2 && notes[1].timeStamp - notes[0].timeStamp === 5, "running status, each by its own timestamp");
}

console.log(`ok: ${n} checks`);
