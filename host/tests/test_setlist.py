# SPDX-License-Identifier: GPL-3.0-or-later
"""METRONOME's computer side: the wire, the record, JSON, and backup / restore against a stand-in FM-1.
The frames are the ones the firmware's own test (tests/basic.txt) sends and expects.
    PYTHONPATH=host pytest -q host/tests"""

import json
from pathlib import Path

import pytest

from fm1_metronome import protocol as P
from fm1_metronome import setlist as S
from fm1_metronome.__main__ import Session

BASIC = Path(__file__).resolve().parents[2] / "tests" / "basic.txt"


def frames_in_basic(kind: str, type_: int) -> list[bytes]:
    """basic.txt's 'sysex' (to the FM-1) or 'expect_sysex' (from it) frames of one type, in order."""
    out = []
    for line in BASIC.read_text().splitlines():
        w = line.split()
        if w and w[0] == kind and w[1:5] == ["F0", "7D", "4D", "54"]:
            f = bytes(int(x, 16) for x in w[1:])
            if f[5] == type_:
                out.append(f)
    return out


def assemble(fr: list[bytes]) -> S.Setlist:
    a, s = S.Assembler(), None
    for f in fr:
        s = a.feed(P.parse(f).payload)
    assert s is not None
    return s


def three_songs() -> S.Setlist:
    s = S.Setlist([S.Song() for _ in range(S.NSLOT)])
    s.songs[0] = S.Song(True, 0, 96, 3, 2, 4, 1, [7, 0, 0, 0], S.grid_default(3, 2))
    s.songs[4] = S.Song(True, 1, 150, 4, 1, 3, 0, [7, 4, 0, 2], S.grid_default(4, 1))
    s.songs[9] = S.Song(True, 0, 200, 7, 1, 4, 2, [7, 0, 0, 0], [3, 1, 2, 1, 2, 1, 1] + [0] * 9)
    s.active, s.switch_now = 4, True
    return s


def test_wire_basics():
    assert P.crc16(b"123456789") == 0x29B1          # CRC-16/CCITT-FALSE check value
    data = bytes(range(256))
    assert P.unpack7(P.pack7(data)) == data
    assert all(b < 0x80 for b in P.pack7(data))
    assert P.hello_ack().hex() == "f07d4d540101" + "01f7"
    with pytest.raises(P.ProtocolError):
        P.frame(P.T_SET, b"\x80")
    assert P.parse(bytes.fromhex("f07d544801410100f7")) is None   # THEORY's, not ours


def test_record_layout():
    assert S.RECORD_N == 454 and S.FRAMES == 10
    r = S.to_record(S.Setlist())
    assert r[:4] == bytes([1, 0, 0, 0]) and r[4:8] == bytes([1, 0, 120, 0])
    assert S.from_record(r) == S.Setlist()
    with pytest.raises(S.SetlistError):
        S.from_record(r[:-1] + bytes([r[-1] ^ 1]))


def test_the_firmware_backup_is_ours():
    """The boot state's setlist as the firmware sends it (basic.txt) is the host's default, byte for byte."""
    fr = frames_in_basic("expect_sysex", P.T_SET)
    assert len(fr) == S.FRAMES
    assert assemble(fr) == S.Setlist()
    assert [f[:5] + bytes([P.T_SET_LOAD]) + f[6:] for f in fr] == S.frames(S.Setlist())


def test_the_firmware_loads_ours():
    """basic.txt sends three_songs() (then once more, then a damaged copy): the same frames the host makes."""
    fr = frames_in_basic("sysex", P.T_SET_LOAD)
    assert fr[:S.FRAMES] == S.frames(three_songs())
    assert fr[S.FRAMES:2 * S.FRAMES] == S.frames(three_songs())
    with pytest.raises(S.SetlistError):
        assemble(fr[2 * S.FRAMES:3 * S.FRAMES])


def test_json_round_trip():
    s = three_songs()
    text = S.to_json(s)
    d = json.loads(text)
    assert [e["key"] for e in d["songs"]] == [1, 5, 10]
    assert d["songs"][2]["grid"]["steps"] == "AgNgNgg" and d["songs"][1]["mode"] == "classic"
    assert d["active"] == 5 and d["switch"] == "now"
    assert S.from_json(text) == s


def test_json_by_hand():
    """A file written by hand (or by the web page) with only what matters."""
    s = S.from_json(json.dumps({"app": "METRONOME", "kind": "setlist", "format": 1, "active": 2,
                                "songs": [{"key": 2, "bpm": 132, "grid": {"beats": 2, "div": 3}},
                                          {"key": 3, "bpm": 60, "mode": "classic", "classic": {"accent_every": 0}}]}))
    assert s.active == 1 and s.songs[1].steps[:6] == [3, 1, 1, 2, 1, 1] and s.songs[2].accent_every == 0
    assert not s.songs[0].used
    for bad in ({"songs": []},                                                     # the active song is not stored
                {"songs": [{"key": 1, "bpm": 20}]},                                # too slow
                {"songs": [{"key": 1, "bpm": 120, "grid": {"beats": 5, "div": 4}}]},   # 20 steps
                {"songs": [{"key": 1, "bpm": 120, "grid": {"steps": "AXN"}}]},
                {"songs": [{"key": 17, "bpm": 120}]}):
        with pytest.raises(S.SetlistError):
            S.from_json(json.dumps({"app": "METRONOME", "kind": "setlist", "format": 1, **bad}))
    with pytest.raises(S.SetlistError):
        S.from_json('{"app": "THEORY"}')


class FakeFM1:
    """A stand-in for the firmware: says HELLO, answers SET_REQ with a setlist, takes SET_LOAD and answers PLAY."""

    def __init__(self, s: S.Setlist, answer: int = P.DONE_LOADED):
        self.s, self.answer, self.out, self.asm = s, answer, [], S.Assembler()
        self.out.append(P.frame(P.T_HELLO, bytes([1, 0, 3, 0])))

    def send(self, f: bytes) -> None:
        m = P.parse(f)
        if m.type == P.T_SET_REQ:
            self.out += [x[:5] + bytes([P.T_SET]) + x[6:] for x in S.frames(self.s)]
        elif m.type == P.T_SET_LOAD:
            got = self.asm.feed(m.payload)
            if got is not None:
                self.s = got if self.answer == P.DONE_LOADED else self.s
                self.out.append(P.frame(P.T_SET_DONE, bytes([self.answer])))

    def poll(self, timeout: float = 0.1) -> list[bytes]:
        o, self.out = self.out, []
        return o


def test_backup_and_restore_session():
    fm = FakeFM1(three_songs())
    ss = Session(fm, log=lambda *_: None)
    assert ss.wait_connected(1) and ss.firmware == (0, 3)
    assert ss.backup(1) == three_songs()
    assert ss.restore(S.Setlist(), 1) == P.DONE_LOADED and fm.s == S.Setlist()
    fm2 = FakeFM1(three_songs(), P.DONE_DECLINED)
    ss2 = Session(fm2, log=lambda *_: None)
    ss2.wait_connected(1)
    assert ss2.restore(S.Setlist(), 1) == P.DONE_DECLINED and fm2.s == three_songs()


FIXTURE = Path(__file__).with_name("three_songs.json")


def test_the_file_the_web_page_reads():
    """three_songs.json is what this host writes; web/tests/setlist_test.js checks the page writes the same."""
    assert FIXTURE.read_text() == S.to_json(three_songs()) + "\n"
