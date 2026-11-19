// SPDX-License-Identifier: GPL-3.0-or-later
// web/setlist.js against the firmware's own test frames (tests/basic.txt) and the host's file (host/tests).
//   node web/tests/setlist_test.js
"use strict";
const fs = require("fs"), path = require("path"), assert = require("assert");
const MT = require("../setlist.js");
const ROOT = path.join(__dirname, "..", "..");
let n = 0;
function ok(c, what) { n++; assert.ok(c, what); }

function framesIn(kind, type) {
  return fs.readFileSync(path.join(ROOT, "tests", "basic.txt"), "utf8").split("\n").map(l => l.trim().split(/\s+/))
    .filter(w => w[0] === kind && w.slice(1, 5).join(" ") === "F0 7D 4D 54").map(w => w.slice(1).map(x => parseInt(x, 16)))
    .filter(f => f[5] === type);
}
function assemble(fr) { const a = MT.assembler(); let s = null; for (const f of fr) s = a.feed(MT.parse(f).payload); return s; }
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

ok(MT.crc16([..."123456789"].map(c => c.charCodeAt(0))) === 0x29B1, "crc16 check value");
const all = Array.from({ length: 256 }, (_, i) => i);
ok(same(MT.unpack7(MT.pack7(all)), all) && MT.pack7(all).every(b => b < 0x80), "pack7");
ok(MT.RECORD_N === 454 && MT.FRAMES === 10, "record size");

// the firmware's backup of its boot state is the page's default setlist
const back = framesIn("expect_sysex", MT.T.SET);
ok(back.length === MT.FRAMES, "ten SET frames in basic.txt");
ok(same(assemble(back), MT.setlist()), "the backup is the default setlist");
ok(same(MT.frames(MT.setlist()), back.map(f => [...f.slice(0, 5), MT.T.SET_LOAD, ...f.slice(6)])), "frames match");

// the setlist basic.txt loads is the host's three_songs.json, and the page writes that file the same way
const file = fs.readFileSync(path.join(ROOT, "host", "tests", "three_songs.json"), "utf8");
const three = MT.fromJSON(file);
ok(MT.toJSON(three) + "\n" === file, "the page writes the host's file byte for byte");
const load = framesIn("sysex", MT.T.SET_LOAD);
ok(same(MT.frames(three), load.slice(0, MT.FRAMES)), "the page sends what the firmware's test loads");
assert.throws(() => assemble(load.slice(2 * MT.FRAMES, 3 * MT.FRAMES)), /CRC/); n++;

// markings and presets as metro.c has them (its unit test checks every edge)
ok(MT.MARKS[MT.marking(120)][0] === "ALLEGRO" && MT.marking(119) === 5 && MT.marking(30) === 0 && MT.marking(300) === 8, "markings");
ok(MT.markingJump(120, 1) === 132 && MT.markingJump(140, -1) === 132 && MT.markingJump(35, -1) === 35, "jumps");
for (let i = 0; i < MT.PRESETS.length; i++) {
  const g = MT.song({ used: true }); MT.presetApply(g, i);
  ok(MT.presetMatch(g) === i && g.steps[0] === 3, `preset ${MT.PRESETS[i][0]}`);
}
const g78 = MT.song(); MT.presetApply(g78, 9);
ok(MT.meterText(g78) === "7/8 2+2+3" && MT.meterText(MT.song({ beats: 2, div: 3, steps: MT.gridDefault(2, 3) })) === "6/8", "meter text");

// what sounds: 12/8 is one accent, three normal, eight ghosts a bar
const g128 = MT.song(); MT.presetApply(g128, 7);
const count = v => { let c = 0; for (let t = 0; t < 48; t++) c += MT.hits(g128, t).filter(h => h[0] === v).length; return c; };
ok(count(MT.V.ACCENT) === 1 && count(MT.V.NORMAL) === 3 && count(MT.V.GHOST) === 8, "12/8 sounds");

// files by hand, and refusals
const hand = MT.fromJSON(JSON.stringify({ app: "METRONOME", kind: "setlist", format: 1, active: 2,
  songs: [{ key: 2, bpm: 132, grid: { beats: 2, div: 3 } }] }));
ok(hand.active === 1 && same(hand.songs[1].steps.slice(0, 6), [3, 1, 1, 2, 1, 1]), "a file by hand");
for (const bad of [{ songs: [] }, { songs: [{ key: 1, bpm: 20 }] }, { songs: [{ key: 1, bpm: 120, grid: { beats: 5, div: 4 } }] },
  { songs: [{ key: 1, bpm: 120, grid: { steps: "AXN" } }] }, { songs: [{ key: 17, bpm: 120 }] }]) {
  assert.throws(() => MT.fromJSON(JSON.stringify({ app: "METRONOME", kind: "setlist", format: 1, ...bad }))); n++;
}
console.log(`ok: ${n} checks`);
