"""Framed swarm payloads carried over Meshtastic.

Every telemetry, control, and ACK blob is wrapped so the same bytes work as:

* a PRIVATE_APP (256) payload via the Meshtastic client/protobuf serial API
* a SERIAL_APP (64) payload via Serial Module SIMPLE mode

Layout (little-endian):
    magic u16 = 0x32DB (bytes DB 32)
    type  u8
    len   u16
    payload[len]
    crc16 u16  (CCITT-FALSE over magic..payload)
"""
from __future__ import annotations

import os
import struct

MAGIC = b"\xDB\x32"
MSG_TELEMETRY = 1
MSG_CONTROL = 2
MSG_ACK = 3

PORT_SERIAL_APP = 64
PORT_PRIVATE_APP = 256
DEFAULT_PORT = PORT_PRIVATE_APP

HEADER_LEN = 5
CRC_LEN = 2
MIN_FRAME_LEN = HEADER_LEN + CRC_LEN


def mesh_portnum() -> int:
    raw = os.environ.get("MESHTASTIC_PORTNUM")
    if raw:
        return int(raw)
    return DEFAULT_PORT


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_frame(msg_type: int, payload: bytes) -> bytes:
    if not 0 <= int(msg_type) <= 0xFF:
        raise ValueError("msg_type must fit in uint8")
    payload = bytes(payload)
    if len(payload) > 0xFFFF:
        raise ValueError("payload too large")
    header = MAGIC + bytes([int(msg_type)]) + struct.pack("<H", len(payload))
    body = header + payload
    return body + struct.pack("<H", crc16_ccitt(body))


def decode_frame(data) -> tuple[int, bytes] | None:
    if not data:
        return None
    if isinstance(data, str):
        data = data.encode("latin1")
    if not isinstance(data, (bytes, bytearray, memoryview)):
        return None
    raw = bytes(data)
    if len(raw) < MIN_FRAME_LEN or raw[:2] != MAGIC:
        return None
    msg_type = raw[2]
    payload_len = struct.unpack_from("<H", raw, 3)[0]
    total = HEADER_LEN + payload_len + CRC_LEN
    if len(raw) < total:
        return None
    payload = raw[HEADER_LEN:HEADER_LEN + payload_len]
    crc_got = struct.unpack_from("<H", raw, HEADER_LEN + payload_len)[0]
    if crc_got != crc16_ccitt(raw[: HEADER_LEN + payload_len]):
        return None
    return msg_type, payload


def find_frame(buffer: bytes) -> tuple[int, tuple[int, bytes] | None, bytes]:
    """Scan a byte stream for the first valid frame.

    Returns (consumed_count, decoded_or_None, remaining).
    """
    raw = bytes(buffer)
    magic_at = raw.find(MAGIC)
    if magic_at < 0:
        keep = min(len(raw), 1)
        return len(raw) - keep, None, raw[-keep:] if keep else b""
    if magic_at > 0:
        return magic_at, None, raw[magic_at:]
    if len(raw) < MIN_FRAME_LEN:
        return 0, None, raw
    payload_len = struct.unpack_from("<H", raw, 3)[0]
    total = HEADER_LEN + payload_len + CRC_LEN
    if len(raw) < total:
        return 0, None, raw
    decoded = decode_frame(raw[:total])
    if decoded is None:
        return 1, None, raw[1:]
    return total, decoded, raw[total:]


def portnum_matches(portnum, expected) -> bool:
    if portnum == expected:
        return True
    value = getattr(portnum, "value", None)
    if value == expected:
        return True
    name = getattr(portnum, "name", None)
    if isinstance(name, str) and name.upper() in {
        "SERIAL_APP",
        "PRIVATE_APP",
    }:
        if expected in {PORT_SERIAL_APP, PORT_PRIVATE_APP}:
            return True
    if isinstance(portnum, str):
        compact = portnum.upper().replace("PORTNUM.", "")
        aliases = {
            "SERIAL_APP": PORT_SERIAL_APP,
            "PRIVATE_APP": PORT_PRIVATE_APP,
        }
        if aliases.get(compact) == expected:
            return True
        try:
            return int(portnum) == expected
        except ValueError:
            return False
    try:
        return int(portnum) == expected
    except (TypeError, ValueError):
        return False


def is_swarm_port(portnum) -> bool:
    return (
        portnum_matches(portnum, PORT_SERIAL_APP)
        or portnum_matches(portnum, PORT_PRIVATE_APP)
        or portnum_matches(portnum, 100)
        or portnum_matches(portnum, 101)
        or portnum_matches(portnum, 102)
    )
