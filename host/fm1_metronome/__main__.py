# SPDX-License-Identifier: GPL-3.0-or-later
"""python -m fm1_metronome backup FILE | restore FILE | watch [--start|--stop] [--port NAME]   (PYTHONPATH=host)

The setlist of an FM-1 running METRONOME 0.3 or newer into a JSON file, and back (the FM-1 asks first: PLAY loads
it, HOME keeps what is there). The web page web/index.html does the same in Chrome or Edge. watch (0.4): every
STATE the FM-1 sends, as it comes, with the time it came (for measuring the link); --start / --stop first."""

from __future__ import annotations

import argparse
import sys
import time

from . import __version__
from . import live as L
from . import protocol as P
from . import setlist as S


class Session:
    """Answers HELLO and HEARTBEAT (the FM-1 counts the computer gone 10 s after its last frame); gathers SET."""

    def __init__(self, link, log=print):
        self.link, self.log = link, log
        self.connected = False
        self.firmware: tuple[int, int] | None = None
        self.asm: S.Assembler | None = None
        self.setlist: S.Setlist | None = None
        self.error: str | None = None
        self.done: int | None = None
        self.states: list[tuple[float, L.State]] = []
        self.watch = False               # print each STATE as it comes (watch)

    def pump(self, timeout: float = 0.1) -> None:
        for data in self.link.poll(timeout):
            try:
                m = P.parse(data)
            except P.ProtocolError as e:
                self.error = str(e)
                continue
            if m is None:
                continue
            if m.type in (P.T_HELLO, P.T_HEARTBEAT):
                self.firmware = P.firmware_of(m) or self.firmware
                self.link.send(P.hello_ack())
                self.connected = True
            elif m.type == P.T_SET and self.asm is not None:
                try:
                    s = self.asm.feed(m.payload)
                except S.SetlistError as e:
                    self.error, self.asm = str(e), None
                    continue
                if s is not None:
                    self.setlist, self.asm = s, None
            elif m.type == P.T_SET_DONE and m.payload:
                self.done = m.payload[0]
            elif m.type == P.T_STATE:
                try:
                    st = L.decode(m.payload)
                except P.ProtocolError as e:
                    self.error = str(e)
                    continue
                self.states.append((time.time(), st))
                if self.watch:
                    self.log(f"[metronome] {time.strftime('%H:%M:%S')} {L.describe(st)}")

    def wait_connected(self, seconds: float = 4.0) -> bool:
        end = time.time() + seconds
        while time.time() < end and not self.connected:
            self.pump()
        return self.connected

    def backup(self, seconds: float = 5.0) -> S.Setlist:
        self.asm, self.setlist, self.error = S.Assembler(), None, None
        self.link.send(P.frame(P.T_SET_REQ))
        end = time.time() + seconds
        while time.time() < end and self.setlist is None and self.error is None:
            self.pump()
        if self.setlist is None:
            raise RuntimeError(self.error or "no setlist came back (is it METRONOME 0.3 or newer?)")
        return self.setlist

    def restore(self, s: S.Setlist, seconds: float = 120.0) -> int:
        self.done = None
        for f in S.frames(s):
            self.link.send(f)
            time.sleep(0.03)             # the FM-1 takes one frame at a time
            self.pump(0)
        self.log("on the FM-1: PLAY loads it, HOME keeps what is there")
        end = time.time() + seconds
        while time.time() < end and self.done is None:
            self.pump()
        if self.done is None:
            raise RuntimeError("no answer from the FM-1")
        return self.done


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="fm1_metronome", description=f"METRONOME's computer side {__version__}")
    ap.add_argument("--port", help="the MIDI port's name, or part of it (default: whichever speaks METRONOME)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("backup", help="the setlist into a JSON file").add_argument("file")
    sub.add_parser("restore", help="a JSON file onto the FM-1 (it asks first)").add_argument("file")
    w = sub.add_parser("watch", help="print each STATE (0.4) as it comes; Ctrl-C ends")
    g = w.add_mutually_exclusive_group()
    g.add_argument("--start", action="store_true", help="CTRL START first")
    g.add_argument("--stop", action="store_true", help="CTRL STOP first")
    w.add_argument("--seconds", type=float, default=0, help="stop after this long (0: until Ctrl-C)")
    a = ap.parse_args(argv)
    from .device import MidiLink, HINT
    try:
        link = MidiLink(a.port)
    except RuntimeError as e:
        print(f"[metronome] {e}", file=sys.stderr)
        return 1
    s = Session(link)
    try:
        if not s.wait_connected():
            print(f"[metronome] {link.name} is quiet. " + HINT, file=sys.stderr)
            return 1
        if a.cmd == "watch":
            if s.firmware and s.firmware < (0, P.LIVE_MINOR):
                print(f"[metronome] METRONOME {s.firmware[0]}.{s.firmware[1]} has no live link (0.4 and newer)",
                      file=sys.stderr)
                return 1
            s.watch = True
            s.link.send(P.ctrl(P.C_START if a.start else P.C_STOP if a.stop else P.C_STATE))
            end = time.time() + a.seconds if a.seconds else None
            try:
                while end is None or time.time() < end:
                    s.pump()
            except KeyboardInterrupt:
                pass
            return 0
        if a.cmd == "backup":
            sl = s.backup()
            fw = f"{s.firmware[0]}.{s.firmware[1]}" if s.firmware else "0.3"
            with open(a.file, "w") as f:
                f.write(S.to_json(sl, fw) + "\n")
            print(f"[metronome] {S.summary(sl)} -> {a.file}")
        else:
            with open(a.file) as f:
                sl = S.from_json(f.read())
            print(f"[metronome] sending {S.summary(sl)}")
            done = s.restore(sl)
            print("[metronome] " + {P.DONE_LOADED: "loaded", P.DONE_DECLINED: "declined on the FM-1",
                                    P.DONE_REFUSED: "refused: the FM-1 found it damaged"}.get(done, f"answer {done}"))
            return 0 if done == P.DONE_LOADED else 1
    except (RuntimeError, S.SetlistError, OSError) as e:
        print(f"[metronome] {e}", file=sys.stderr)
        return 1
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
