// SPDX-License-Identifier: GPL-3.0-or-later
// The FM-1 running METRONOME, from the page: over USB (Web MIDI) or Bluetooth (Web Bluetooth, BLE-MIDI directly, no
// system setup), the same frames either way. After MORSE's page (same author): the port is found by what it says
// (HELLO / HEARTBEAT once a second), not by its name; USB wins when both answer; Bluetooth's packets are timed by
// their BLE-MIDI timestamps (the FM-1's clock), mapped onto this page's. Every HELLO and HEARTBEAT is answered here.
//   const link = MTLink.create({ onFrame(m, timeStamp), onStatus(text, kind), onConnect(link), onLost(link) })
//   link.usb(); link.ble(); link.send(frame); link.firmware ([major, minor]); link.via ("USB" / "Bluetooth")
// blePackets and bleParser are pure (web/tests). Loads in a page (window.MTLink) and in node.
"use strict";
(function (root) {
  const HEAD = [0x7D, 0x4D, 0x54], T_HELLO = 0x41, T_HEARTBEAT = 0x42;
  const BLE_SVC = "03b80e5a-ede8-4b33-a751-6ce34ec4c700", BLE_IO = "7772e5db-3868-4112-a1a9-f2669d106bf3", BLE_PKT = 20;

  function ours(d) { return d.length >= 7 && d[0] === 0xF0 && d[1] === HEAD[0] && d[2] === HEAD[1] && d[3] === HEAD[2]; }
  function transportOf(d) {   // HELLO: proto, major, minor, transport; HEARTBEAT: proto, transport (1 Bluetooth)
    const pl = d.slice(6, d.length - 1);
    return d[5] === T_HELLO ? pl[3] : d[5] === T_HEARTBEAT ? pl[1] : undefined;
  }

  // ---- BLE-MIDI: one SysEx frame (F0 .. F7) into packets of at most 20 bytes (the page cannot learn the MTU)
  function blePackets(f, ms) {
    const t = Math.floor(ms) & 0x1FFF, hi = 0x80 | (t >> 7 & 0x3F), lo = 0x80 | (t & 0x7F), pk = [];
    let p = [hi, lo, 0xF0];
    for (const b of f.slice(1, -1)) { if (p.length >= BLE_PKT) { pk.push(p); p = [hi]; } p.push(b); }
    if (p.length + 2 > BLE_PKT) { pk.push(p); p = [hi]; }
    p.push(lo, 0xF7); pk.push(p);
    return pk;
  }
  function dataLen(s) { const h = s & 0xF0; return h === 0xC0 || h === 0xD0 || s === 0xF1 || s === 0xF3 ? 1 : h < 0xF0 || s === 0xF2 ? 2 : 0; }
  // packets back into messages, each with the time it was sent on this page's clock: feed(packet, arrived) -> [{data, timeStamp}]
  function bleParser() {
    let sx = null, run = 0, off = null;
    const unwrap = (ts, arrived) => Math.round((arrived - (off ?? arrived - ts) - ts) / 8192) * 8192 + ts;
    return {
      feed(p, arrived) {
        if (!(p[0] & 0x80)) return [];
        let hi = p[0] & 0x3F, lastLo = -1, ts = 0, i = 1;
        const got = [], emit = m => got.push([m, ts]);
        const stamp = lo => { if (lastLo >= 0 && lo < lastLo) hi = (hi + 1) & 0x3F; lastLo = lo; ts = hi << 7 | lo; };
        if (sx) while (i < p.length && p[i] < 0x80) sx.push(p[i++]);   // a SysEx going on from the last packet
        while (i < p.length) {
          if (p[i] & 0x80) { stamp(p[i++] & 0x7F); if (i >= p.length) break; }
          let s = p[i];
          if (s === 0xF7 && sx) { sx.push(0xF7); emit(sx); sx = null; i++; continue; }
          if (s < 0x80) { s = run; if (!s) break; } else i++;   // running status
          if (s === 0xF0) { sx = [0xF0]; while (i < p.length && p[i] < 0x80) sx.push(p[i++]); continue; }
          if (s >= 0xF8) continue;   // real time
          const n = dataLen(s);
          if (i + n > p.length) break;
          if (s < 0xF0) run = s;
          emit([s, ...p.slice(i, i + n)]);
          i += n;
        }
        if (!got.length) return [];
        // the packet came when its newest message was sent, or later: the smallest such lag is the clocks' offset
        const newest = unwrap(got[got.length - 1][1], arrived);
        if (off === null || arrived - newest < off) off = arrived - newest;
        return got.map(([m, t]) => ({ data: Uint8Array.from(m), timeStamp: unwrap(t, arrived) + off }));
      },
    };
  }

  function create(h) {
    const L = { firmware: null, via: null, name: "", connected: false, send: () => {} };
    let midi = null, inp = null, out = null, hunting = 0, bleDev = null, bleQ = Promise.resolve(), lastAt = 0, watch = 0;
    const status = (t, k) => h.onStatus && h.onStatus(t, k || "");
    function idle() {
      if (inp) inp.onmidimessage = null;
      inp = out = null;
      const was = L.connected;
      L.firmware = L.via = null; L.connected = false; L.send = () => {};
      clearInterval(watch);
      if (was && h.onLost) h.onLost(L);
    }
    function onMessage(e) {
      const d = Array.from(e.data);
      if (!ours(d)) return;
      lastAt = performance.now();
      if (d[5] === T_HELLO || d[5] === T_HEARTBEAT) {
        if (d[5] === T_HELLO && d.length >= 10) L.firmware = [d[7], d[8]];
        L.via = transportOf(d) === 1 ? "Bluetooth" : "USB";
        L.send([0xF0, ...HEAD, 1, 0x01, 1, 0xF7]);   // HELLO_ACK, every time
        if (!L.connected) {   // (only HEARTBEATs: another page answered it lately; the firmware stays unknown)
          L.connected = true;
          clearInterval(watch);
          watch = setInterval(() => {   // a heartbeat each second: three missed, it is gone
            if (L.connected && performance.now() - lastAt > 3500) { status("The FM-1 went quiet: connect again.", "bad"); idle(); }
          }, 1000);
          if (h.onConnect) h.onConnect(L);
        }
      }
      if (h.onFrame) h.onFrame({ type: d[5], payload: d.slice(6, d.length - 1) }, e.timeStamp, d);
    }
    function outputFor(i) {
      const outs = [...midi.outputs.values()];
      return outs.find(o => o.name === i.name && o.manufacturer === i.manufacturer)
        || outs.find(o => o.name === i.name) || (outs.length === 1 ? outs[0] : null);
    }
    function hunt() {
      idle();
      const ins = [...midi.inputs.values()];
      if (!ins.length) { status("No MIDI inputs: connect the FM-1 by a USB data cable (or pair it over Bluetooth MIDI).", "bad"); return; }
      status("Looking for the FM-1 running METRONOME…", "dim");
      const heardOn = new Map();   // every port that speaks our frames, by its link; USB wins, Bluetooth waits for it
      let waiting = null;
      const take = () => {
        if (inp || !heardOn.size) return;
        const i = [...heardOn.keys()].sort((a, b) => heardOn.get(a) - heardOn.get(b))[0], o = outputFor(i);
        if (!o) { status(`Found METRONOME on "${i.name}" but not its output port.`, "bad"); return; }
        for (const j of ins) j.onmidimessage = null;
        inp = i; out = o; L.name = i.name;
        L.send = f => out.send(f);
        inp.onmidimessage = onMessage;   // its next HELLO or HEARTBEAT (each second) connects
        status("Found it: connecting…", "dim");
      };
      for (const i of ins) i.onmidimessage = e => {
        const d = Array.from(e.data);
        if (inp || !ours(d)) return;
        heardOn.set(i, transportOf(d) === 1 ? 1 : 0);
        if (transportOf(d) !== 1) take();
        else if (!waiting) waiting = setTimeout(take, 1300);
      };
      clearTimeout(hunting);
      hunting = setTimeout(() => {
        if (!inp) status("No FM-1 running METRONOME answered. Is it on, on USB, and is no other program (a DAW, M-Flasher, another tab) holding its port?", "bad");
      }, 3000);
    }
    L.usb = async () => {
      if (!navigator.requestMIDIAccess) {
        status("This browser has no Web MIDI (Safari, and every browser on iPhone and iPad): use Chrome or Edge on a computer.", "bad");
        return;
      }
      try {
        midi = midi || await navigator.requestMIDIAccess({ sysex: true });
        midi.onstatechange = e => { if (e.port.state === "connected" && !inp && !bleDev) hunt(); };   // plugged in, restarted
        bleDrop();
        hunt();
      } catch (err) { status(`MIDI access was refused (SysEx must be allowed for this page): ${err.message}`, "bad"); }
    };
    function bleDrop() {
      const d = bleDev; bleDev = null;
      if (d && d.gatt.connected) d.gatt.disconnect();
    }
    L.ble = async () => {
      let dev;
      try { dev = await navigator.bluetooth.requestDevice({ filters: [{ services: [BLE_SVC] }] }); }
      catch (err) { if (err.name !== "NotFoundError") status(`Bluetooth: ${err.message}`, "bad"); return; }
      try {
        if (midi) for (const i of midi.inputs.values()) i.onmidimessage = null;
        clearTimeout(hunting);
        idle();
        status(`Connecting to ${dev.name || "the FM-1"} over Bluetooth…`, "dim");
        const io = await (await (await dev.gatt.connect()).getPrimaryService(BLE_SVC)).getCharacteristic(BLE_IO);
        const parser = bleParser();
        bleQ = Promise.resolve();
        io.addEventListener("characteristicvaluechanged", e => {
          const v = e.target.value;
          for (const m of parser.feed(new Uint8Array(v.buffer, v.byteOffset, v.byteLength), performance.now())) onMessage(m);
        });
        dev.addEventListener("gattserverdisconnected", () => {
          if (bleDev !== dev) return;
          bleDev = null; idle();
          status("The Bluetooth link was lost: connect again.", "bad");
        });
        await io.startNotifications();
        bleDev = dev; L.name = dev.name || "FM-1";
        L.send = f => { for (const pk of blePackets(f, performance.now())) bleQ = bleQ.then(() => io.writeValueWithoutResponse(Uint8Array.from(pk))); };
        status("Connected over Bluetooth: waiting for METRONOME's HELLO…", "dim");
        hunting = setTimeout(() => { if (bleDev === dev && !L.connected) status("Connected, but METRONOME said nothing: is it METRONOME 0.4 or newer, with Bluetooth on (SET EDIT, K4)?", "bad"); }, 3000);
      } catch (err) { bleDev = null; idle(); status(`Bluetooth: ${err.message}`, "bad"); }
    };
    L.bleAvailable = typeof navigator !== "undefined" && !!navigator.bluetooth;
    L.midiAvailable = typeof navigator !== "undefined" && !!navigator.requestMIDIAccess;
    L.pace = () => (L.via === "Bluetooth" ? 60 : 30);   // ms between the setlist's frames (Bluetooth's queue is short)
    return L;
  }

  const MTLink = { create, blePackets, bleParser, BLE_PKT };
  if (typeof module === "object" && module.exports) module.exports = MTLink;
  else root.MTLink = MTLink;
})(typeof window !== "undefined" ? window : globalThis);
