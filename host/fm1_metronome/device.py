# SPDX-License-Identifier: GPL-3.0-or-later
"""Finding the FM-1 running METRONOME on MIDI and talking to it. By what it says, not by the port's name: macOS keeps
the name of the first firmware it saw on a unit (often "Felucca"). After THEORY's and MORSE's (same author)."""

from __future__ import annotations

import queue
import time

from . import protocol as P

HINT = ("Is METRONOME 0.3 or newer running, on a USB data cable? Close other programs that hold the FM-1's MIDI port "
        "(a DAW, M-Flasher, a browser tab with the setlist page).")


def sniff_port(mido, names: list[str], seconds: float = 3.0) -> str | None:
    """The input on which METRONOME says HELLO or HEARTBEAT (once a second)."""
    found: list[str] = []
    ports = []
    for n in names:
        def hear(msg, n=n):
            if msg.type != "sysex" or bytes(msg.data[:3]) != P.HEADER:
                return
            try:
                m = P.parse(bytes([0xF0, *msg.data, 0xF7]))
            except P.ProtocolError:
                return
            if m and m.type in (P.T_HELLO, P.T_HEARTBEAT) and n not in found:
                found.append(n)
        try:
            ports.append(mido.open_input(n, callback=hear))
        except (OSError, IOError):
            continue
    start = time.time()
    while time.time() < start + seconds and not found:
        time.sleep(0.05)
    for p in ports:
        p.close()
    return found[0] if found else None


class MidiLink:
    def __init__(self, port: str | None = None):
        import mido

        ins, outs = mido.get_input_names(), mido.get_output_names()
        name = next((n for n in ins if port and port.lower() in n.lower()), None) if port else sniff_port(mido, ins)
        if not name or name not in outs:
            raise RuntimeError(f"no FM-1 running METRONOME found on MIDI (inputs: {ins}). " + HINT)
        self.name = name
        self.q: queue.Queue[bytes] = queue.Queue()
        self._mido = mido
        self.out = mido.open_output(name)
        self.inp = mido.open_input(name, callback=lambda m: m.type == "sysex" and self.q.put(bytes([0xF0, *m.data, 0xF7])))

    def send(self, frame: bytes) -> None:
        self.out.send(self._mido.Message("sysex", data=list(frame[1:-1])))

    def poll(self, timeout: float = 0.1) -> list[bytes]:
        out = []
        try:
            out.append(self.q.get(timeout=timeout))
            while True:
                out.append(self.q.get_nowait())
        except queue.Empty:
            pass
        return out

    def close(self) -> None:
        self.inp.close()
        self.out.close()
