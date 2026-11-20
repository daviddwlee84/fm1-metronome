# SPDX-License-Identifier: GPL-3.0-or-later
"""METRONOME's SysEx frames (app.c, "a computer: USB or Bluetooth"):

    F0 7D 4D 54 <ver=1> <type> <payload...> F7       ("MT"; every payload byte 7-bit)

device -> host: HELLO 41 (proto, fw major, fw minor, transport), HEARTBEAT 42 (proto, transport),
                SET 44 (index, count, chunk 7-bit packed), SET_DONE 45 (0 loaded, 1 declined, 2 refused),
                STATE 46 (0.4: what plays and when, 48 bytes 7-bit packed; live.py),
                PONG 47 (0.5: PING's 4 bytes, the sample now packed)
host -> device: HELLO_ACK 01 (answer every HELLO and HEARTBEAT), SET_REQ 04, SET_LOAD 05 (as SET),
                CTRL 06 (0.4: START, STOP, PICK song, BPM, MUTE, STATE?; 0.5: PING)

crc16, pack7 and unpack7 are THEORY's and MORSE's wire scheme (same author)."""

from __future__ import annotations

from dataclasses import dataclass

HEADER = bytes([0x7D, 0x4D, 0x54])
VER = 1
T_HELLO_ACK, T_SET_REQ, T_SET_LOAD, T_CTRL = 0x01, 0x04, 0x05, 0x06
T_HELLO, T_HEARTBEAT, T_SET, T_SET_DONE, T_STATE, T_PONG = 0x41, 0x42, 0x44, 0x45, 0x46, 0x47
C_START, C_STOP, C_PICK, C_BPM, C_MUTE, C_STATE, C_PING = 1, 2, 3, 4, 5, 6, 7
LIVE_MINOR = 4                           # the firmware's minor (HELLO) from which STATE and CTRL exist
PING_MINOR = 5                           # ... and PING / PONG
DONE_LOADED, DONE_DECLINED, DONE_REFUSED = 0, 1, 2


class ProtocolError(ValueError):
    pass


@dataclass
class Message:
    type: int
    payload: bytes


def frame(type_: int, payload: bytes = b"") -> bytes:
    if any(b > 0x7F for b in payload):
        raise ProtocolError("payload bytes must be 7-bit")
    return bytes([0xF0, *HEADER, VER, type_, *payload, 0xF7])


def parse(data: bytes) -> Message | None:
    """One of ours (F0 .. F7), or None for anyone else's frame."""
    if len(data) < 7 or data[0] != 0xF0 or data[-1] != 0xF7 or data[1:4] != HEADER:
        return None
    if data[4] != VER:
        raise ProtocolError(f"protocol version {data[4]}, this host knows {VER}")
    return Message(data[5], bytes(data[6:-1]))


def hello_ack() -> bytes:
    return frame(T_HELLO_ACK, bytes([VER]))


def ctrl(op: int, *args: int) -> bytes:
    """CTRL: START, STOP, PICK song (0-15), BPM (30-300, two 7-bit bytes), MUTE 0/1, STATE?, PING t0 t1 t2 t3"""
    if op == C_BPM:
        args = (args[0] & 0x7F, args[0] >> 7)
    return frame(T_CTRL, bytes([op, *args]))


def firmware_of(m: Message) -> tuple[int, int] | None:
    return (m.payload[1], m.payload[2]) if m.type == T_HELLO and len(m.payload) >= 3 else None


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), as the firmware computes it."""
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def pack7(data: bytes) -> bytes:
    """Each 7 bytes: one byte of their high bits (bit j for byte j), then their low 7 bits."""
    out = bytearray()
    for i in range(0, len(data), 7):
        chunk = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << j for j, b in enumerate(chunk)))
        out.extend(b & 0x7F for b in chunk)
    return bytes(out)


def unpack7(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        hi = data[i]
        i += 1
        for j in range(7):
            if i >= len(data):
                break
            out.append(data[i] | ((hi >> j) & 1) << 7)
            i += 1
    return bytes(out)
