# SPDX-License-Identifier: GPL-3.0-or-later
"""The live link (METRONOME 0.4): STATE, what plays and when, so a computer plays along on its own clock.

STATE's 48 bytes (7-bit packed on the wire): seq, flags (1 running, 2 muted, 4 the switch waiting, 8 switch NOW),
the active song, this segment's song, its 28 bytes (as the setlist record's), then u32s low byte first: an anchor
tick and its sample, q8 (samples a tick x256; a tick is a twelfth of a beat), the sample when it was sent. 44.1 kHz.
Tick t of the segment sounds at sample anchor_at + (t - anchor_tick) * q8 / 256, as app.c's q8_mul computes it."""

from __future__ import annotations

from dataclasses import dataclass

from . import protocol as P
from . import setlist as S

SR = 44100
TPB = 12                                 # ticks a beat
STATE_N = 48
F_RUN, F_MUTE, F_NEXT, F_NOW = 1, 2, 4, 8


@dataclass
class State:
    seq: int
    flags: int
    active: int
    song_no: int
    song: S.Song
    tick: int
    at: int
    q8: int
    now: int

    @property
    def running(self) -> bool:
        return bool(self.flags & F_RUN)

    @property
    def muted(self) -> bool:
        return bool(self.flags & F_MUTE)

    @property
    def next(self) -> bool:
        return bool(self.flags & F_NEXT)

    def tick_at(self, t: int) -> int:
        """The sample tick t sounds at (app.c tick_at, the same 32-bit arithmetic)."""
        n = (t - self.tick) & 0xFFFFFFFF
        start = (self.at - q8_mul(self.tick, self.q8)) & 0xFFFFFFFF
        return (start + q8_mul(t, self.q8)) & 0xFFFFFFFF if n < 0x80000000 else start

    def bpm(self) -> float:
        return SR * 60 * 256 / self.q8 / TPB if self.q8 else float(self.song.bpm)


def q8_mul(n: int, q8: int) -> int:
    return (n * (q8 >> 8) + ((n * (q8 & 255) + 128) >> 8)) & 0xFFFFFFFF


def u32(b: bytes, i: int) -> int:
    return b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24


def decode(payload: bytes) -> State:
    b = P.unpack7(payload)
    if len(b) < STATE_N:
        raise P.ProtocolError(f"a STATE of {len(b)} bytes, not {STATE_N}")
    return State(b[0], b[1], b[2], b[3], S.song_from(b[4:4 + S.SONG_N]), u32(b, 32), u32(b, 36), u32(b, 40),
                 u32(b, 44))


def describe(s: State) -> str:
    g = s.song
    what = (f"{g.beats} beats x{g.div}" if g.mode == 0 else f"CLASSIC accent {g.accent_every}")
    state = ("NEXT " if s.next else "") + ("playing" if s.running else "stopped")
    return (f"#{s.seq:3d} {state:13s} song {s.song_no + 1:2d} (active {s.active + 1}) {g.bpm} BPM {what}"
            f"{' muted' if s.muted else ''}  anchor tick {s.tick} @ {s.at}, now {s.now}")
