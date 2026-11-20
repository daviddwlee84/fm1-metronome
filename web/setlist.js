// SPDX-License-Identifier: GPL-3.0-or-later
// METRONOME's setlist in the browser: the wire (SysEx "MT"), the record, the JSON file, the tempo markings and the
// meter presets. The same as host/fm1_metronome (Python) and the firmware (app.c, metro.c); web/tests check it
// against the frames of the firmware's own test (tests/basic.txt). Loads in a page (window.MT) and in node.
"use strict";
(function (root) {
  const HEAD = [0x7D, 0x4D, 0x54], VER = 1;
  const T = { HELLO_ACK: 0x01, SET_REQ: 0x04, SET_LOAD: 0x05, CTRL: 0x06,
    HELLO: 0x41, HEARTBEAT: 0x42, SET: 0x44, SET_DONE: 0x45, STATE: 0x46, PONG: 0x47 };
  // 0.4, the live link: CTRL's ops, STATE's flags (app.c), and the firmware minor they start at
  const C = { START: 1, STOP: 2, PICK: 3, BPM: 4, MUTE: 5, STATE: 6, PING: 7 };
  const SF = { RUN: 1, MUTE: 2, NEXT: 4, NOW: 8 }, LIVE_MINOR = 4, PING_MINOR = 5, SR = 44100, TPB = 12, STATE_N = 48;
  const DONE = { LOADED: 0, DECLINED: 1, REFUSED: 2 };
  const FORMAT = 1, NSLOT = 16, NSTEPS = 16, SONG_N = 28, RECORD_N = 1 + 3 + NSLOT * SONG_N + 2, CHUNK = 49;
  const FRAMES = Math.ceil(RECORD_N / CHUNK);
  const BPM_MIN = 30, BPM_MAX = 300;
  const MODES = ["grid", "classic"], SOUNDS = ["click", "wood", "beep"];
  const LAYERS = ["quarter", "eighth", "triplet", "sixteenth"], LEVELS = ".gNA";
  const SWITCH = ["next bar", "now"], PAGES = ["set", "pattern"];

  // metro.c: the tempo markings (Maelzel's scale, no overlaps) and where K2 jumps
  const MARKS = [["LARGHISSIMO", BPM_MIN, 35], ["LARGO", 40, 50], ["LARGHETTO", 60, 63], ["ADAGIO", 66, 70],
    ["ANDANTE", 76, 92], ["MODERATO", 108, 112], ["ALLEGRO", 120, 132], ["PRESTO", 168, 184], ["PRESTISSIMO", 200, 208]];
  function marking(bpm) { let i = MARKS.length - 1; while (i > 0 && bpm < MARKS[i][1]) i--; return i; }
  function markingJump(bpm, dir) {
    const i = marking(bpm), typ = MARKS[i][2];
    if (dir > 0) return bpm < typ ? typ : i + 1 < MARKS.length ? MARKS[i + 1][2] : bpm;
    return bpm > typ ? typ : i > 0 ? MARKS[i - 1][2] : bpm;
  }

  // metro.c: GRID's meter presets (K3). steps null: the default pattern
  const PRESETS = [["2/4", 2, 1, null], ["3/4", 3, 1, null], ["4/4", 4, 1, null], ["5/4", 5, 1, null],
    ["6/8", 2, 3, null], ["6/8 IN 6", 6, 1, "AggNgg"], ["9/8", 3, 3, null], ["12/8", 4, 3, null],
    ["5/8 3+2", 5, 1, "AggNg"], ["7/8 2+2+3", 7, 1, "AgNgNgg"]];
  function gridDefault(beats, div) {
    const n = beats * div, s = [];
    for (let i = 0; i < NSTEPS; i++) s.push(i >= n ? 0 : i === 0 ? 3 : i % div === 0 ? 2 : 1);
    return s;
  }
  function stepsOf(text) { return [...text].map(c => LEVELS.toLowerCase().indexOf(c.toLowerCase())); }
  function presetApply(song, i) {
    const [, beats, div, steps] = PRESETS[i];
    song.beats = beats; song.div = div; song.steps = gridDefault(beats, div);
    if (steps) stepsOf(steps).forEach((v, k) => { song.steps[k] = v; });
  }
  function presetMatch(song) {
    for (let i = 0; i < PRESETS.length; i++) {
      const t = { steps: [] };
      presetApply(t, i);
      if (t.beats === song.beats && t.div === song.div && t.steps.every((v, k) => v === song.steps[k])) return i;
    }
    return -1;
  }
  const DIV_NAME = ["", "QUARTERS", "8THS", "TRIPLETS", "16THS"];
  function meterText(s) {   // as the screen says it (app.c meter_text)
    if (s.mode === 0) {
      const m = presetMatch(s);
      if (m >= 0 && PRESETS[m][3]) return PRESETS[m][0];
      return s.div === 3 ? `${s.beats * 3}/8` : `${s.beats}/4  ${DIV_NAME[s.div]}`;
    }
    return s.accent_every ? `BEAT ${s.accent_every}  CLASSIC` : "NO ACCENT  CLASSIC";
  }

  function song(over) {
    return Object.assign({ used: false, mode: 0, bpm: 120, beats: 4, div: 1, accent_every: 4, sound: 0, mix: [7, 0, 0, 0],
      steps: gridDefault(4, 1) }, over || {});
  }
  function setlist() {
    const songs = [];
    for (let i = 0; i < NSLOT; i++) songs.push(song({ used: i === 0 }));
    return { songs, active: 0, switch_now: false, page: 0 };
  }
  function songProblem(g) {
    const ok = (g.mode === 0 || g.mode === 1) && g.bpm >= BPM_MIN && g.bpm <= BPM_MAX && Number.isInteger(g.bpm)
      && g.beats >= 1 && g.beats <= 9 && g.div >= 1 && g.div <= 4 && g.beats * g.div <= NSTEPS
      && g.accent_every >= 0 && g.accent_every <= 9 && g.sound >= 0 && g.sound < SOUNDS.length
      && g.mix.length === 4 && g.mix.every(v => v >= 0 && v <= 7) && g.steps.length === NSTEPS
      && g.steps.every(v => v >= 0 && v <= 3);
    return ok ? null : "out of range";
  }
  function check(s) {
    if (s.songs.length !== NSLOT) throw new Error(`${s.songs.length} songs, not ${NSLOT}`);
    s.songs.forEach((g, i) => { const p = songProblem(g); if (p) throw new Error(`song ${i + 1}: ${p}`); });
    if (!(s.active >= 0 && s.active < NSLOT) || !s.songs[s.active].used) throw new Error(`the active song (${s.active + 1}) is not stored`);
  }

  // ---------------------------------------------------------------- the wire ---
  function crc16(b) {
    let c = 0xFFFF;
    for (const x of b) { c ^= x << 8; for (let k = 0; k < 8; k++) c = ((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1) & 0xFFFF; }
    return c;
  }
  function pack7(b) {
    const out = [];
    for (let i = 0; i < b.length; i += 7) {
      const ch = b.slice(i, i + 7);
      out.push(ch.reduce((h, x, j) => h | ((x >> 7) & 1) << j, 0));
      ch.forEach(x => out.push(x & 0x7F));
    }
    return out;
  }
  function unpack7(b) {
    const out = [];
    let i = 0;
    while (i < b.length) {
      const hi = b[i++];
      for (let j = 0; j < 7 && i < b.length; j++) out.push(b[i++] | ((hi >> j) & 1) << 7);
    }
    return out;
  }
  function frame(type, payload) { return [0xF0, ...HEAD, VER, type, ...(payload || []), 0xF7]; }
  function parse(d) {   // one of ours: {type, payload}; null for anyone else's
    if (d.length < 7 || d[0] !== 0xF0 || d[d.length - 1] !== 0xF7 || d[1] !== HEAD[0] || d[2] !== HEAD[1] || d[3] !== HEAD[2]) return null;
    if (d[4] !== VER) throw new Error(`protocol version ${d[4]}, this page knows ${VER}`);
    return { type: d[5], payload: Array.from(d.slice(6, d.length - 1)) };
  }
  function toRecord(s) {
    check(s);
    const b = [FORMAT, s.active, s.switch_now ? 1 : 0, s.page];
    for (const g of s.songs) b.push(g.used ? 1 : 0, g.mode, g.bpm & 0xFF, g.bpm >> 8, g.beats, g.div, g.accent_every, g.sound, ...g.mix, ...g.steps);
    const c = crc16(b);
    b.push(c & 0xFF, c >> 8);
    return b;
  }
  function songFrom(d) {   // a song's 28 bytes (the record's, STATE's)
    return song({ used: d[0] === 1, mode: d[1], bpm: d[2] | d[3] << 8, beats: d[4], div: d[5], accent_every: d[6],
      sound: d[7], mix: Array.from(d.slice(8, 12)), steps: Array.from(d.slice(12, 28)) });
  }

  // ------------------------------------------------------- the live link ---
  // CTRL: the page's hand on the transport. BPM goes as two 7-bit bytes
  function ctrl(op, arg) {
    if (op === C.BPM) return frame(T.CTRL, [op, arg & 0x7F, arg >> 7]);
    return frame(T.CTRL, arg === undefined ? [op] : [op, arg]);
  }
  // 0.5: PING carries 28 bits of token as four 7-bit bytes; PONG gives them back with the FM-1's sample then
  function ping(token) { return frame(T.CTRL, [C.PING, token & 0x7F, token >> 7 & 0x7F, token >> 14 & 0x7F, token >> 21 & 0x7F]); }
  function decodePong(payload) {
    if (payload.length < 9) throw new Error(`a PONG of ${payload.length} bytes`);
    const b = unpack7(payload.slice(4, 9));
    return { token: payload[0] | payload[1] << 7 | payload[2] << 14 | payload[3] << 21, now: (b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24) >>> 0 };
  }
  // app.c q8_mul: n ticks of q8 (samples a tick x256) in samples, the device's 32-bit arithmetic exactly
  function q8Mul(n, q8) { return (n * Math.floor(q8 / 256) + Math.floor((n * (q8 & 255) + 128) / 256)) >>> 0; }
  // STATE: what plays and when (app.c). Tick t sounds at sample start + q8Mul(t, q8), start = at - q8Mul(tick, q8)
  function decodeState(payload) {
    const b = unpack7(payload);
    if (b.length < STATE_N) throw new Error(`a STATE of ${b.length} bytes, not ${STATE_N}`);
    const u32 = i => (b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24) >>> 0;
    const st = { seq: b[0], flags: b[1], active: b[2], songNo: b[3], song: songFrom(b.slice(4, 4 + SONG_N)),
      tick: u32(32), at: u32(36), q8: u32(40), now: u32(44) };
    st.running = !!(st.flags & SF.RUN);
    st.muted = !!(st.flags & SF.MUTE);
    st.next = !!(st.flags & SF.NEXT);
    st.start = (st.at - q8Mul(st.tick, st.q8)) >>> 0;
    return st;
  }

  function fromRecord(b) {
    if (b.length !== RECORD_N) throw new Error(`${b.length} bytes, not ${RECORD_N}`);
    if (b[0] !== FORMAT) throw new Error(`record format ${b[0]}, this page knows ${FORMAT}`);
    const c = crc16(b.slice(0, -2));
    if (b[RECORD_N - 2] !== (c & 0xFF) || b[RECORD_N - 1] !== c >> 8) throw new Error("the record's CRC does not match");
    const songs = [];
    for (let i = 0; i < NSLOT; i++) songs.push(songFrom(b.slice(4 + i * SONG_N, 4 + (i + 1) * SONG_N)));
    const s = { songs, active: b[1], switch_now: b[2] === 1, page: b[3] };
    check(s);
    return s;
  }
  function frames(s) {   // SET_LOAD frames
    const r = toRecord(s), out = [];
    for (let i = 0; i < FRAMES; i++) out.push(frame(T.SET_LOAD, [i, FRAMES, ...pack7(r.slice(i * CHUNK, (i + 1) * CHUNK))]));
    return out;
  }
  function assembler() {   // SET payloads back into a setlist: feed() returns it after the last
    let buf = [], next = 0;
    return {
      feed(p) {
        if (p.length < 3 || p[1] !== FRAMES) throw new Error(`a record in ${p[1]} parts, not ${FRAMES}`);
        if (p[0] !== next) throw new Error(`frame ${p[0]} where ${next} was due`);
        buf = buf.concat(unpack7(p.slice(2)).slice(0, CHUNK));
        if (++next < FRAMES) return null;
        return fromRecord(buf.slice(0, RECORD_N));
      },
    };
  }

  // ---------------------------------------------------------------- the file ---
  function toJSON(s, firmware) {
    check(s);
    const songs = [];
    s.songs.forEach((g, i) => {
      if (!g.used) return;
      const n = g.beats * g.div, cl = { accent_every: g.accent_every };
      LAYERS.forEach((k, j) => { cl[k] = g.mix[j]; });
      songs.push({ key: i + 1, bpm: g.bpm, mode: MODES[g.mode], sound: SOUNDS[g.sound],
        grid: { beats: g.beats, div: g.div, steps: g.steps.slice(0, n).map(v => LEVELS[v]).join("") }, classic: cl });
    });
    return JSON.stringify({ app: "METRONOME", kind: "setlist", format: FORMAT, firmware: firmware || "0.3",
      active: s.active + 1, switch: SWITCH[s.switch_now ? 1 : 0], page: PAGES[s.page], songs }, null, 2);
  }
  function fromJSON(text) {
    let d;
    try { d = JSON.parse(text); } catch (e) { throw new Error(`not JSON: ${e.message}`); }
    if (!d || d.app !== "METRONOME" || d.kind !== "setlist") throw new Error("not a METRONOME setlist");
    if (d.format !== FORMAT) throw new Error(`setlist format ${d.format}, this page knows ${FORMAT}`);
    const s = setlist();
    s.songs = s.songs.map(() => song());
    for (const e of d.songs || []) {
      const k = e.key | 0;
      if (k < 1 || k > NSLOT) throw new Error(`key ${e.key}`);
      const gr = e.grid || {}, cl = e.classic || {}, beats = gr.beats ?? 4, div = gr.div ?? 1;
      let steps = beats >= 1 && beats <= 9 && div >= 1 && div <= 4 ? gridDefault(beats, div) : new Array(NSTEPS).fill(0);
      if (gr.steps !== undefined) {
        const t = String(gr.steps);
        if (t.length > beats * div) throw new Error(`key ${k}: ${t.length} steps in a bar of ${beats * div}`);
        steps = stepsOf(t);
        if (steps.includes(-1)) throw new Error(`key ${k}: steps "${t}" (use . g N A)`);
        while (steps.length < NSTEPS) steps.push(0);
      }
      const mode = MODES.indexOf(e.mode ?? "grid"), sound = SOUNDS.indexOf(e.sound ?? "click");
      if (mode < 0 || sound < 0) throw new Error(`key ${k}: mode or sound`);
      s.songs[k - 1] = song({ used: true, mode, bpm: e.bpm, beats, div, accent_every: cl.accent_every ?? 4, sound,
        mix: LAYERS.map(n => cl[n] ?? (n === "quarter" ? 7 : 0)), steps });
    }
    s.active = (d.active ?? 1) - 1;
    s.switch_now = SWITCH.indexOf(d.switch ?? "next bar") === 1;
    s.page = Math.max(0, PAGES.indexOf(d.page ?? "set"));
    check(s);
    return s;
  }

  // ------------------------------------------------------ what sounds when ---
  // metro.c mt_hits: a beat is 12 ticks; GRID plays its steps, CLASSIC the quarter (accented every BEAT beats) and
  // the eighth, triplet and sixteenth layers between the beats. Each: [voice, volume 1..7]
  const V = { ACCENT: 0, NORMAL: 1, GHOST: 2, E8: 3, E3: 4, E16: 5 };
  function hits(g, tick) {
    const out = [], inb = tick % 12, beat = Math.floor(tick / 12);
    if (g.mode === 0) {
      const per = 12 / g.div;
      if (inb % per) return out;
      const lvl = g.steps[Math.floor(tick / per) % (g.beats * g.div)];
      if (lvl) out.push([lvl === 3 ? V.ACCENT : lvl === 2 ? V.NORMAL : V.GHOST, 7]);
      return out;
    }
    const add = (v, vol) => { if (vol > 0) out.push([v, vol]); };
    if (!inb) add(g.accent_every && beat % g.accent_every === 0 ? V.ACCENT : V.NORMAL, g.mix[0]);
    else {
      if (inb === 6) add(V.E8, g.mix[1]);
      if (inb % 4 === 0) add(V.E3, g.mix[2]);
      if (inb % 3 === 0) add(V.E16, g.mix[3]);
    }
    return out;
  }
  function barTicks(g) { return (g.mode === 0 ? g.beats : (g.accent_every || 1)) * 12; }
  // app.c SND / V_AMP: each voice's pitch (Hz), loudness, and the sound's decay (ms)
  const SND = [{ hz: [1568, 1047, 1047, 784, 880, 659], tau: 16 }, { hz: [1175, 784, 784, 587, 659, 494], tau: 7 },
    { hz: [2093, 1568, 1568, 1319, 1397, 1175], tau: 45 }];
  const V_AMP = [0.55, 0.38, 0.13, 0.30, 0.30, 0.26];

  const MT = { T, DONE, FORMAT, NSLOT, NSTEPS, RECORD_N, CHUNK, FRAMES, BPM_MIN, BPM_MAX, MODES, SOUNDS, LAYERS, LEVELS,
    MARKS, PRESETS, DIV_NAME, marking, markingJump, gridDefault, presetApply, presetMatch, meterText, song, setlist,
    check, crc16, pack7, unpack7, frame, parse, toRecord, fromRecord, frames, assembler, toJSON, fromJSON,
    V, hits, barTicks, SND, V_AMP, helloAck: () => frame(T.HELLO_ACK, [VER]),
    C, SF, LIVE_MINOR, PING_MINOR, SR, TPB, songFrom, ctrl, q8Mul, decodeState, ping, decodePong };
  if (typeof module === "object" && module.exports) module.exports = MT;
  else root.MT = MT;
})(typeof window !== "undefined" ? window : globalThis);
