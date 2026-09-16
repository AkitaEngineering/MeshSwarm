"""Control command helpers for Meshtastic drone nodes."""
from __future__ import annotations

import os
import struct
import threading
from typing import Any

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM as _AESGCM
except ImportError:  # pragma: no cover - exercised only in minimal installs
    AESGCM: Any = None
else:
    AESGCM = _AESGCM

from meshtastic_crypto import (
    DEFAULT_KEY,
    decrypt_blob_any,
    encrypt_blob,
    load_key,
    load_per_drone_keys,
    load_seq,
    next_seq,
    pack_control_plaintext,
    save_seq,
)
from mesh_frame import MSG_ACK, MSG_CONTROL, decode_frame, encode_frame, is_swarm_port, mesh_portnum

REQUIRE_CONFIGURED_KEY = (
    os.environ.get("MESHTASTIC_PRODUCTION") == "1"
    or os.environ.get("MESHTASTIC_REQUIRE_KEY") == "1"
)
KEY = load_key(DEFAULT_KEY, require_configured=REQUIRE_CONFIGURED_KEY)
DRONE_CONTROL_COMMAND_DATA_TYPE = 256
CONTROL_ACK_DATA_TYPE = 256
COMMAND_RTL = 1
COMMAND_LAND = 2
COMMAND_EMERGENCY_LAND = 3
COMMAND_SYNC_REQUEST = 4
COMMAND_HEARTBEAT = 5
ACK_STATUS_ACCEPTED = 1
ACK_STATUS_SYNC = 2
ACK_STATUS_REJECTED = 3
ACK_PLAINTEXT_LEN = 10

interface = None
_acks: dict[tuple[int, int], tuple[int, int]] = {}
_ack_condition = threading.Condition()


def _decoded_data(packet):
    decoded = packet.get("decoded") or {}
    return decoded.get("data") or decoded


def key_for_command(drone_id) -> bytes:
    per = load_per_drone_keys()
    mapped = per.get(int(drone_id))
    return mapped if mapped is not None else KEY


def keys_to_try() -> list[bytes]:
    keys: list[bytes] = []
    for candidate in (KEY, *load_per_drone_keys().values()):
        if candidate not in keys:
            keys.append(candidate)
    return keys


def _encrypt_payload(payload, drone_id):
    if AESGCM is None:
        raise RuntimeError("cryptography is required for encrypted control")
    return encrypt_blob(key_for_command(drone_id), payload, MSG_CONTROL)


def send_control_command(drone_id, command):
    if interface is None:
        raise RuntimeError("Meshtastic not connected")

    seq = next_seq()
    payload = pack_control_plaintext(seq, int(drone_id), int(command))
    interface.sendData(
        encode_frame(MSG_CONTROL, _encrypt_payload(payload, drone_id)),
        portNum=mesh_portnum(),
        wantAck=True,
    )
    return seq


def handle_encrypted_ack(payload: bytes) -> None:
    if AESGCM is None or not payload or len(payload) < 12 + 16:
        return
    try:
        plaintext = decrypt_blob_any(keys_to_try(), payload, MSG_ACK)
        if len(plaintext) != ACK_PLAINTEXT_LEN:
            return
        seq, drone_id, status, last_seq = struct.unpack("<IBBI", plaintext)
    except Exception:
        return

    with _ack_condition:
        _acks[(int(drone_id), int(seq))] = (int(status), int(last_seq))
        if status == ACK_STATUS_SYNC and last_seq > load_seq():
            save_seq(last_seq)
        _ack_condition.notify_all()


def _on_receive_control(packet, received_interface=None):
    data = _decoded_data(packet)
    payload = data.get("payload")
    if not payload:
        return

    framed = decode_frame(payload)
    if framed is not None:
        msg_type, inner = framed
        if msg_type != MSG_ACK:
            return
        handle_encrypted_ack(inner)
        return

    if not is_swarm_port(data.get("portnum")):
        return
    handle_encrypted_ack(payload)


def _ack_lookup(drone_id, seq):
    key = (int(drone_id), int(seq))
    if key in _acks:
        return _acks[key]
    if int(drone_id) != 255:
        return None
    for (ack_drone, ack_seq), value in _acks.items():
        if ack_seq == int(seq):
            return value
    return None


def wait_for_ack(drone_id, seq, timeout=1.5):
    with _ack_condition:
        ok = _ack_condition.wait_for(
            lambda: _ack_lookup(drone_id, seq) is not None,
            timeout=timeout,
        )
        if not ok:
            return None
        return _ack_lookup(drone_id, seq)


def request_seq_sync(drone_id, timeout=2.0):
    seq = send_control_command(drone_id, COMMAND_SYNC_REQUEST)
    ack = wait_for_ack(drone_id, seq, timeout=timeout)
    if ack is None:
        return None
    status, last_seq = ack
    if status != ACK_STATUS_SYNC:
        return None
    return last_seq
