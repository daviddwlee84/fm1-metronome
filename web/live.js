// SPDX-License-Identifier: GPL-3.0-or-later
// The live link's timing (METRONOME 0.4), platform-free: the FM-1's STATEs (setlist.js decodeState) into "tick t of
// this song sounds at page time x". The FM-1 sends a STATE when something changes and once a second while playing,
// never a message a beat; this page plays along on its own clock:
//   - Clock: the FM-1's sample count against the page's ms. Each STATE says when it was sent (its "now"); it came
//     then or later, so the smallest lag over the last few seconds is the offset (USB's and Bluetooth's jitter and
//     bunching fall out; the clocks' drift is followed as old lags age out). Always by the message's timeStamp, never
//     by when the page got round to reading it.
//   - Plan: what plays (the segment) and the switch waiting (the next one), each a song, its anchor and q8, as the
//     FM-1's render plays them; ticks(from, to) lists the ticks to sound between two FM-1 times.
// Loads in a page (window.MTLive) and in node (web/tests/live_test.js).
"use strict";
(function (root) {
  const MT = typeof module === "object" && module.exports ? require("./setlist.js") : root.MT;
  const SR = MT.SR, WRAP = 4294967296;

  // the FM-1's u32 samples as an ever-growing number (they wrap after 27 h)
  function unwrapper() {
    let base = 0, last = null;
    return s => {
      if (last !== null) {
        const d = (s - last) >>> 0;
        if (d < WRAP / 2) { if (s < last) base += WRAP; } else if (s > last) base -= WRAP;   // forward / a little back
      }
      last = s;
      return base + s;
    };
  }

  function clock(windowMs) {
    const win = windowMs || 8000, seen = [];   // [arrived ms, lag ms]
    const ext = unwrapper();
    const C = {
      offset: null,                            // page ms = device ms + offset
      ext,
      observe(devNow, arrivedMs) {
        const devMs = ext(devNow) / SR * 1000, lag = arrivedMs - devMs;
        seen.push([arrivedMs, lag]);
        while (seen.length > 1 && arrivedMs - seen[0][0] > win) seen.shift();
        C.offset = Math.min(...seen.map(x => x[1]));
        return C.offset;
      },
      reset() { seen.length = 0; C.offset = null; },
      toPage(devSamples) { return devSamples / SR * 1000 + C.offset; },   // an extended sample -> page ms
      toDevice(pageMs) { return (pageMs - C.offset) / 1000 * SR; },
    };
    return C;
  }

  // a segment: tick t sounds at start + q8Mul(t, q8) (extended samples); from its anchor tick on
  function segment(st, ext) {
    const at = ext(st.at), start = at - MT.q8Mul(st.tick, st.q8);
    return { song: st.song, songNo: st.songNo, q8: st.q8, tick: st.tick, at, start,
      timeOf(t) { return start + MT.q8Mul(t, st.q8); },
      firstAtOrAfter(dev) {   // the first tick (from the anchor on) at or after FM-1 time dev
        if (dev <= at) return st.tick;
        let t = Math.max(st.tick, Math.floor((dev - start) * 256 / st.q8));
        while (start + MT.q8Mul(t, st.q8) < dev) t++;
        while (t > st.tick && start + MT.q8Mul(t - 1, st.q8) >= dev) t--;
        return t;
      },
      // the same timing and song (a switch that landed comes back anchored at tick 0: the same start)
      same(o) { return !!o && o.start === start && o.q8 === st.q8 && JSON.stringify(o.song) === JSON.stringify(st.song); },
    };
  }

  function plan(clk) {
    const P = {
      running: false, muted: false, active: 0, cur: null, next: null, seq: -1,
      // a STATE in: true when what sounds changed (the page re-plans what it had scheduled), false for the same again
      take(st, arrivedMs) {
        clk.observe(st.now, arrivedMs);
        P.seq = st.seq; P.active = st.active; P.muted = st.muted;
        if (!st.running) {
          const was = P.running;
          P.running = false; P.cur = P.next = null; P.stopped = st.song; P.stoppedNo = st.songNo;
          return was;
        }
        const seg = segment(st, clk.ext);
        if (st.next) {
          const changed = !seg.same(P.next);
          P.next = seg;
          return changed;
        }
        const landed = !!P.next && seg.same(P.next);   // the switch has landed: what the page planned already
        const changed = !landed && (!P.running || !seg.same(P.cur));
        if (landed || changed) P.next = null;   // (a switch still waiting is sent again after a change)
        P.running = true; P.cur = seg;
        return changed;   // (a switch still waiting comes in the STATE after this one)
      },
      reset() { P.running = false; P.cur = P.next = null; P.seq = -1; clk.reset(); },   // the link gone
      // the segment playing at FM-1 time dev (the next one from its anchor on)
      at(dev) { return P.next && dev >= P.next.at ? P.next : P.cur; },
      // the ticks to sound in [from, to) FM-1 time: {tick, dev, seg}
      ticks(from, to) {
        const out = [];
        if (!P.running || !P.cur) return out;
        const segs = P.next ? [[P.cur, P.next.at], [P.next, Infinity]] : [[P.cur, Infinity]];
        for (const [seg, end] of segs) {
          const lo = Math.max(from, seg === P.cur ? -Infinity : seg.at), hi = Math.min(to, end);
          if (lo >= hi) continue;
          for (let t = seg.firstAtOrAfter(lo); ; t++) {
            const dev = seg.timeOf(t);
            if (dev >= hi) break;
            out.push({ tick: t, dev, seg });
          }
        }
        return out;
      },
      // where the bar is at FM-1 time dev: {seg, tick (fractional), beat, inBar (ticks)}, null stopped or before
      where(dev) {
        const seg = P.running && P.at(dev);
        if (!seg || dev < seg.start) return null;
        const tick = (dev - seg.start) * 256 / seg.q8, bar = MT.barTicks(seg.song);
        return { seg, tick, inBar: tick % bar, beat: Math.floor((tick % bar) / MT.TPB) };
      },
    };
    return P;
  }

  const MTLive = { clock, plan, segment, unwrapper };
  if (typeof module === "object" && module.exports) module.exports = MTLive;
  else root.MTLive = MTLive;
})(typeof window !== "undefined" ? window : globalThis);
