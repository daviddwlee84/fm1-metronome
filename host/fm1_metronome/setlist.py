# SPDX-License-Identifier: GPL-3.0-or-later
"""The setlist: METRONOME's record as it goes over the wire (app.c sl_fill / sl_read) and as a JSON file.

The record (454 bytes): a format byte (1), the active song (0-15), the switch (0 next bar, 1 now), the page (0 SET,
1 the song's GRID or CLASSIC page), 16 songs of 28 bytes, a CRC-16/CCITT-FALSE of all before it (low byte first).
A song: used, mode (0 GRID, 1 CLASSIC), bpm (2 bytes, low first), beats a bar (GRID, 1-9), steps a beat (GRID: 1, 2,
3 or 4; beats x steps <= 16), accent every (CLASSIC, 0-9, 0 none), sound (0 CLICK, 1 WOOD, 2 BEEP), the CLASSIC
mix (quarter, eighth, triplet, sixteenth: 0-7 each), 16 GRID steps (0 off, 1 ghost, 2 normal, 3 accent).
It goes as 10 frames of 49 bytes (the last 13), each packed 7-bit."""

from __future__ import annotations

import json
from dataclasses import dataclass, field

from . import protocol as P

FORMAT = 1
NSLOT, NSTEPS, SONG_N = 16, 16, 28
RECORD_N = 1 + 3 + NSLOT * SONG_N + 2
CHUNK = 49
FRAMES = (RECORD_N + CHUNK - 1) // CHUNK
BPM_MIN, BPM_MAX = 30, 300
MODES = ("grid", "classic")
SOUNDS = ("click", "wood", "beep")
LAYERS = ("quarter", "eighth", "triplet", "sixteenth")
LEVELS = ".gNA"                          # off, ghost, normal, accent (as a step string; any case on input)
SWITCH = ("next bar", "now")
PAGES = ("set", "pattern")


class SetlistError(ValueError):
    pass


def grid_default(beats: int, div: int) -> list[int]:
    """metro.c mt_grid_default: 1 accented, the other beats normal, the steps between them ghosts."""
    n = beats * div
    return [0 if i >= n else 3 if i == 0 else 2 if i % div == 0 else 1 for i in range(NSTEPS)]


@dataclass
class Song:
    used: bool = False
    mode: int = 0
    bpm: int = 120
    beats: int = 4
    div: int = 1
    accent_every: int = 4
    sound: int = 0
    mix: list[int] = field(default_factory=lambda: [7, 0, 0, 0])
    steps: list[int] = field(default_factory=lambda: grid_default(4, 1))

    def check(self, where: str) -> None:
        ok = (self.mode in (0, 1) and BPM_MIN <= self.bpm <= BPM_MAX and 1 <= self.beats <= 9 and 1 <= self.div <= 4
              and self.beats * self.div <= NSTEPS and 0 <= self.accent_every <= 9 and 0 <= self.sound < len(SOUNDS)
              and len(self.mix) == 4 and all(0 <= v <= 7 for v in self.mix)
              and len(self.steps) == NSTEPS and all(0 <= v <= 3 for v in self.steps))
        if not ok:
            raise SetlistError(f"{where}: out of range ({self})")


@dataclass
class Setlist:
    songs: list[Song] = field(default_factory=lambda: [Song(used=i == 0) for i in range(NSLOT)])
    active: int = 0
    switch_now: bool = False
    page: int = 0

    def check(self) -> None:
        if len(self.songs) != NSLOT:
            raise SetlistError(f"{len(self.songs)} songs, not {NSLOT}")
        for i, s in enumerate(self.songs):
            s.check(f"song {i + 1}")
        if not 0 <= self.active < NSLOT or not self.songs[self.active].used:
            raise SetlistError(f"the active song ({self.active + 1}) is not stored")
        if self.page not in (0, 1):
            raise SetlistError(f"page {self.page}")


# ------------------------------------------------------------------ the record ---
def to_record(s: Setlist) -> bytes:
    s.check()
    b = bytearray([FORMAT, s.active, int(s.switch_now), s.page])
    for g in s.songs:
        b += bytes([int(g.used), g.mode, g.bpm & 0xFF, g.bpm >> 8, g.beats, g.div, g.accent_every, g.sound,
                    *g.mix, *g.steps])
    c = P.crc16(bytes(b))
    b += bytes([c & 0xFF, c >> 8])
    assert len(b) == RECORD_N
    return bytes(b)


def song_from(d: bytes) -> Song:
    """A song's 28 bytes (the record's, STATE's) as a Song (unchecked)."""
    return Song(bool(d[0]), d[1], d[2] | d[3] << 8, d[4], d[5], d[6], d[7], list(d[8:12]), list(d[12:28]))


def from_record(b: bytes) -> Setlist:
    if len(b) != RECORD_N:
        raise SetlistError(f"{len(b)} bytes, not {RECORD_N}")
    if b[0] != FORMAT:
        raise SetlistError(f"record format {b[0]}, this host knows {FORMAT}")
    c = P.crc16(b[:-2])
    if b[-2] != c & 0xFF or b[-1] != c >> 8:
        raise SetlistError("the record's CRC does not match")
    songs = []
    for i in range(NSLOT):
        d = b[4 + i * SONG_N:4 + (i + 1) * SONG_N]
        if d[0] > 1:
            raise SetlistError(f"song {i + 1}: used = {d[0]}")
        songs.append(song_from(d))
    s = Setlist(songs, b[1], bool(b[2]), b[3])
    if b[2] > 1:
        raise SetlistError(f"switch = {b[2]}")
    s.check()
    return s


def frames(s: Setlist) -> list[bytes]:
    """SET_LOAD frames, as the FM-1 takes them (one after another, index 0 first)."""
    r = to_record(s)
    return [P.frame(P.T_SET_LOAD, bytes([i, FRAMES]) + P.pack7(r[i * CHUNK:(i + 1) * CHUNK])) for i in range(FRAMES)]


class Assembler:
    """SET payloads (index, count, packed chunk) back into a Setlist."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.next = 0

    def feed(self, payload: bytes) -> Setlist | None:
        if len(payload) < 3 or payload[1] != FRAMES:
            raise SetlistError(f"a frame of a record in {payload[1] if len(payload) > 1 else '?'} parts, not {FRAMES}")
        if payload[0] != self.next:
            raise SetlistError(f"frame {payload[0]} where {self.next} was due")
        self.buf += P.unpack7(payload[2:])[:CHUNK]
        self.next += 1
        if self.next < FRAMES:
            return None
        return from_record(bytes(self.buf[:RECORD_N]))


# ------------------------------------------------------------------- the file ---
def to_json(s: Setlist, firmware: str = "0.3") -> str:
    """Stored songs only, by key (1-16); the steps as a string of the bar's steps (. g N A)."""
    s.check()
    songs = []
    for i, g in enumerate(s.songs):
        if not g.used:
            continue
        n = g.beats * g.div
        songs.append({
            "key": i + 1, "bpm": g.bpm, "mode": MODES[g.mode], "sound": SOUNDS[g.sound],
            "grid": {"beats": g.beats, "div": g.div, "steps": "".join(LEVELS[v] for v in g.steps[:n])},
            "classic": {"accent_every": g.accent_every, **{k: v for k, v in zip(LAYERS, g.mix)}},
        })
    return json.dumps({"app": "METRONOME", "kind": "setlist", "format": FORMAT, "firmware": firmware,
                       "active": s.active + 1, "switch": SWITCH[int(s.switch_now)], "page": PAGES[s.page],
                       "songs": songs}, indent=2)


def from_json(text: str) -> Setlist:
    try:
        d = json.loads(text)
    except json.JSONDecodeError as e:
        raise SetlistError(f"not JSON: {e}") from None
    if not isinstance(d, dict) or d.get("app") != "METRONOME" or d.get("kind") != "setlist":
        raise SetlistError("not a METRONOME setlist")
    if d.get("format") != FORMAT:
        raise SetlistError(f"setlist format {d.get('format')}, this host knows {FORMAT}")
    s = Setlist([Song() for _ in range(NSLOT)])
    try:
        for e in d["songs"]:
            k = int(e["key"])
            if not 1 <= k <= NSLOT:
                raise SetlistError(f"key {k}")
            gr, cl = e.get("grid", {}), e.get("classic", {})
            beats, div = int(gr.get("beats", 4)), int(gr.get("div", 1))
            steps = grid_default(beats, div) if 1 <= beats <= 9 and 1 <= div <= 4 else [0] * NSTEPS
            if "steps" in gr:
                txt = str(gr["steps"])
                if len(txt) > beats * div:
                    raise SetlistError(f"key {k}: {len(txt)} steps in a bar of {beats * div}")
                steps = [LEVELS.lower().index(c.lower()) if c.lower() in LEVELS.lower() else -1 for c in txt]
                if -1 in steps:
                    raise SetlistError(f"key {k}: steps {txt!r} (use . g N A)")
                steps += [0] * (NSTEPS - len(steps))
            s.songs[k - 1] = Song(
                True, MODES.index(e.get("mode", "grid")), int(e["bpm"]), beats, div, int(cl.get("accent_every", 4)),
                SOUNDS.index(e.get("sound", "click")), [int(cl.get(k2, 7 if k2 == "quarter" else 0)) for k2 in LAYERS],
                steps)
        s.active = int(d.get("active", 1)) - 1
        s.switch_now = SWITCH.index(d.get("switch", "next bar")) == 1
        s.page = PAGES.index(d.get("page", "set"))
    except (KeyError, TypeError, ValueError) as e:
        if isinstance(e, SetlistError):
            raise
        raise SetlistError(f"a field is missing or wrong: {e}") from None
    s.check()
    return s


def summary(s: Setlist) -> str:
    used = [(i + 1, g.bpm) for i, g in enumerate(s.songs) if g.used]
    return f"{len(used)} songs: " + ", ".join(f"{k}:{b}" for k, b in used)
