# SPDX-License-Identifier: GPL-3.0-or-later
"""The live link (0.4): the STATEs and CTRLs of the firmware's own test (tests/basic.txt), decoded here; web/tests
decode the same ones."""

from fm1_metronome import live as L
from fm1_metronome import protocol as P

from test_setlist import frames_in_basic


def states():
    return [L.decode(P.parse(f).payload) for f in frames_in_basic("expect_sysex", P.T_STATE)]


def test_three_states_in_basic():
    assert len(states()) == 3


def test_stopped_state_is_the_active_song():
    s = states()[0]
    assert (s.seq, s.flags, s.active, s.song_no) == (0, 0, 4, 4)
    assert (s.song.used, s.song.mode, s.song.bpm, s.song.accent_every, s.song.mix) == (True, 1, 150, 3, [7, 4, 0, 2])
    assert (s.tick, s.at, s.q8) == (0, 0, 0)


def test_started_state_anchors_tick_0_ahead_of_now():
    s = states()[1]
    assert s.running and not s.muted and not s.next and s.seq == 1
    assert s.q8 == 44100 * 60 * 256 // 150 // 12          # mt_tick_q8 at 150 BPM
    assert round(s.bpm()) == 150
    ahead = (s.at - s.now) & 0xFFFFFFFF                   # tr_start: tick 0 50 ms after the press
    assert 0.020 * L.SR <= ahead <= 0.050 * L.SR       # (sent a few ms after the start)
    assert s.tick_at(12) - s.tick_at(0) == L.q8_mul(12, s.q8)   # a beat


def test_periodic_state_keeps_the_anchor():
    a, b = states()[1], states()[2]
    assert b.seq == 2 and (b.at, b.tick, b.q8) == (a.at, a.tick, a.q8)
    assert 0.95 * L.SR <= (b.now - a.now) & 0xFFFFFFFF <= 1.1 * L.SR


def test_ctrl_frames_match_basic():
    sent = set(frames_in_basic("sysex", P.T_CTRL))
    for f in (P.ctrl(P.C_START), P.ctrl(P.C_STOP), P.ctrl(P.C_PICK, 0), P.ctrl(P.C_BPM, 100),
              P.ctrl(P.C_MUTE, 1), P.ctrl(P.C_MUTE, 0)):
        assert f in sent, f.hex(" ")
