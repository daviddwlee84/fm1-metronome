// SPDX-License-Identifier: GPL-3.0-or-later
// A conductor beating the song's meter (the live page). patternFor(song) picks the beat pattern from the song as
// the FM-1 plays it; pos(pattern, tickInBar) is where the baton's tip is; draw() paints the figure on a canvas.
//   GRID, a beat in quarters / 8ths / 16ths: its beats (1 to 9). GRID in triplets (6/8, 9/8, 12/8): the dotted
//   quarters, so 6/8 is beaten in 2. The presets in eighths with groups (5/8 3+2, 7/8 2+2+3): a beat a group,
//   long and short (5/8 in 2, 7/8 in 3); 6/8 IN 6 in 6. CLASSIC: its accent (BEAT 0: in 1).
// The shapes are the usual ones: the first beat down, the last up, the ones between to the left, then the right
// (3: down, right, up; 4: down, left, right, up; 6 German: down, left, left, right, right, up). Between two beats the
// tip rebounds and falls to the next (its speed greatest at the beat: the ictus).
// Loads in a page (window.MTConductor) and in node (web/tests/live_test.js).
"use strict";
(function (root) {
  const MT = typeof module === "object" && module.exports ? require("./setlist.js") : root.MT;
  const TPB = 12;

  function groupsOf(steps) {   // "AggNg" -> [3, 2]: a group starts at an accent or a normal step
    const g = [];
    for (const lv of steps) { if (lv >= 2 || !g.length) g.push(1); else g[g.length - 1]++; }
    return g;
  }

  // {beats: [ticks of each beat], name}
  function patternFor(song) {
    if (song.mode === 1) {
      const n = song.accent_every || 1;
      return { beats: Array(n).fill(TPB), name: song.accent_every ? `in ${n}` : "in 1" };
    }
    const m = MT.presetMatch(song), steps = song.steps.slice(0, song.beats * song.div);
    if (m >= 0 && MT.PRESETS[m][0].includes("+")) {   // 5/8 3+2, 7/8 2+2+3: a beat a group of eighths
      const g = groupsOf(steps);
      return { beats: g.map(k => k * TPB), name: `${MT.PRESETS[m][0]} in ${g.length}` };
    }
    const n = song.beats;
    return { beats: Array(n).fill(TPB), name: `in ${n}` };
  }

  // the beats' points (the ictus), in a box x -1..1 (left to right), y 0 (top) .. 1 (bottom)
  function points(n) {
    if (n === 1) return [{ x: 0, y: 0.9 }];
    if (n === 2) return [{ x: 0, y: 0.92 }, { x: 0.32, y: 0.78 }];
    if (n === 3) return [{ x: 0, y: 0.92 }, { x: 0.62, y: 0.8 }, { x: 0.12, y: 0.42 }];
    const mid = n - 2, left = Math.ceil(mid / 2), right = mid - left, pts = [{ x: 0, y: 0.92 }];
    for (let i = 0; i < left; i++) pts.push({ x: -0.42 - 0.22 * i, y: 0.82 - 0.04 * i });
    for (let i = 0; i < right; i++) pts.push({ x: 0.42 + 0.22 * i, y: 0.82 - 0.04 * i });
    pts.push({ x: 0.12, y: 0.42 });
    return pts;
  }

  // the tip at tickInBar: {x, y, beat (0-based), frac (0..1 into it)}
  function pos(p, tickInBar) {
    const n = p.beats.length, pts = points(n), bar = p.beats.reduce((a, b) => a + b, 0);
    let t = ((tickInBar % bar) + bar) % bar, i = 0;
    while (i < n - 1 && t >= p.beats[i]) t -= p.beats[i++];
    const u = Math.min(1, t / p.beats[i]), a = pts[i], b = pts[(i + 1) % n];
    const toOne = (i + 1) % n === 0;                       // back to the downbeat: the big preparation
    const h = n === 1 ? 0.7 : toOne ? 0.38 + 0.3 * (a.y - 0.42) : 0.26;
    const e = u * u * (3 - 2 * u);                         // across: eased
    return { x: a.x + (b.x - a.x) * e, y: a.y + (b.y - a.y) * u * u - h * Math.sin(Math.PI * u), beat: i, frac: u };
  }

  // the conductor, seen from the players: head, shoulders, the left hand still, the right with the baton on the tip
  function draw(ctx, w, h, p, where, theme) {
    const c = theme || {}, fg = c.fg || "#ddd", dim = c.dim || "#666", accent = c.accent || "#f0c85a";
    ctx.clearRect(0, 0, w, h);
    const s = Math.min(w, h * 1.1), cx = w / 2, top = h * 0.1;
    const X = x => cx + x * s * 0.42, Y = y => top + y * s * 0.5;   // the field above, the figure below it
    const n = p.beats.length, pts = points(n);
    // the path of the whole bar, faint
    ctx.strokeStyle = dim; ctx.lineWidth = Math.max(1, s / 260); ctx.globalAlpha = 0.5;
    ctx.beginPath();
    const bar = p.beats.reduce((a, b) => a + b, 0);
    for (let k = 0; k <= 96; k++) { const q = pos(p, bar * k / 96); k ? ctx.lineTo(X(q.x), Y(q.y)) : ctx.moveTo(X(q.x), Y(q.y)); }
    ctx.stroke(); ctx.globalAlpha = 1;
    // the beats' points, numbered; the one just struck lit
    ctx.font = `${Math.round(s / 22)}px system-ui, sans-serif`; ctx.textAlign = "center"; ctx.textBaseline = "middle";
    pts.forEach((q, i) => {
      const lit = where && where.beat === i && where.frac < 0.25;
      ctx.fillStyle = lit ? accent : dim;
      ctx.beginPath(); ctx.arc(X(q.x), Y(q.y), (lit ? s / 45 : s / 80), 0, 7); ctx.fill();
      ctx.fillStyle = lit ? accent : dim;
      ctx.fillText(String(i + 1), X(q.x), Y(q.y) + s / 18);
    });
    // the figure: below the field, facing us
    const tip = where ? pos(p, where.inBar) : { x: 0.1, y: 0.55 };
    const bodyY = h * 0.84, sh = s * 0.11, headR = s * 0.042;
    const shL = { x: cx - sh, y: bodyY }, shR = { x: cx + sh, y: bodyY };
    ctx.fillStyle = c.figure || "#2b2b33"; ctx.strokeStyle = fg; ctx.lineWidth = Math.max(2, s / 90); ctx.lineCap = "round";
    ctx.beginPath();   // the body (a coat), the head
    ctx.moveTo(shL.x - s * 0.03, bodyY); ctx.lineTo(shR.x + s * 0.03, bodyY); ctx.lineTo(cx + sh * 1.15, h); ctx.lineTo(cx - sh * 1.15, h);
    ctx.closePath(); ctx.fill();
    ctx.beginPath(); ctx.arc(cx, bodyY - headR * 1.5, headR, 0, 7); ctx.fill(); ctx.stroke();
    // the left arm: a quiet hand, held in front
    ctx.beginPath(); ctx.moveTo(shL.x, shL.y); ctx.lineTo(shL.x - s * 0.06, shL.y + s * 0.07); ctx.lineTo(shL.x + s * 0.01, shL.y + s * 0.09); ctx.stroke();
    // the right arm to the hand, the baton from the hand to the tip
    const tx = X(tip.x), ty = Y(tip.y), hx = shR.x + (tx - shR.x) * 0.72, hy = shR.y + (ty - shR.y) * 0.72;
    const ex = (shR.x + hx) / 2 + s * 0.06, ey = (shR.y + hy) / 2 + s * 0.04;   // the elbow, out and down
    ctx.beginPath(); ctx.moveTo(shR.x, shR.y); ctx.lineTo(ex, ey); ctx.lineTo(hx, hy); ctx.stroke();
    ctx.strokeStyle = accent; ctx.lineWidth = Math.max(1.5, s / 160);
    ctx.beginPath(); ctx.moveTo(hx, hy); ctx.lineTo(tx, ty); ctx.stroke();
    ctx.fillStyle = accent; ctx.beginPath(); ctx.arc(tx, ty, s / 70, 0, 7); ctx.fill();
  }

  const MTConductor = { patternFor, points, pos, draw, groupsOf };
  if (typeof module === "object" && module.exports) module.exports = MTConductor;
  else root.MTConductor = MTConductor;
})(typeof window !== "undefined" ? window : globalThis);
