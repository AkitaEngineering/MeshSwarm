"""Telemetry receive helpers for the ground-control station."""
from __future__ import annotations

import json
import math
import os
import threading
import time
from typing import Any

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM as _AESGCM
except ImportError:  # pragma: no cover - exercised only in minimal installs
    AESGCM: Any = None
else:
    AESGCM = _AESGCM

from meshtastic_crypto import DEFAULT_KEY, load_key, unpack_telemetry_plaintext
from mesh_frame import MSG_TELEMETRY, decode_frame, is_swarm_port

REQUIRE_CONFIGURED_KEY = (
    os.environ.get("MESHTASTIC_PRODUCTION") == "1"
    or os.environ.get("MESHTASTIC_REQUIRE_KEY") == "1"
)
KEY = load_key(DEFAULT_KEY, require_configured=REQUIRE_CONFIGURED_KEY)
DRONE_TELEMETRY_DATA_TYPE = 256

drone_data: dict[int, dict[str, Any]] = {}
last_telemetry_seq: dict[int, int] = {}
_data_lock = threading.Lock()
_log_lock = threading.Lock()


def _decoded_data(packet):
    decoded = packet.get("decoded") or {}
    return decoded.get("data") or decoded


def _geofence_config():
    lat = os.environ.get("MESHTASTIC_GEOFENCE_LAT")
    lon = os.environ.get("MESHTASTIC_GEOFENCE_LON")
    radius = os.environ.get("MESHTASTIC_GEOFENCE_RADIUS_M")
    if not lat or not lon or not radius:
        return None
    try:
        return float(lat), float(lon), float(radius)
    except ValueError:
        return None


def haversine_m(lat1, lon1, lat2, lon2) -> float:
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlmb = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlmb / 2) ** 2
    return 2 * r * math.asin(min(1.0, math.sqrt(a)))


def geofence_status(lat, lon):
    cfg = _geofence_config()
    if cfg is None:
        return None, None
    center_lat, center_lon, radius = cfg
    if radius <= 0:
        return None, None
    distance = haversine_m(center_lat, center_lon, lat, lon)
    return distance <= radius, distance


def _telemetry_log_path() -> str | None:
    if os.environ.get("MESHTASTIC_TELEMETRY_LOG_DISABLE") == "1":
        return None
    return os.environ.get("MESHTASTIC_TELEMETRY_LOG", "drone_telemetry.jsonl")


def _log_telemetry(drone_id, record):
    path = _telemetry_log_path()
    if not path:
        return
    line = json.dumps({"drone_id": int(drone_id), **record}, separators=(",", ":"))
    try:
        with _log_lock:
            with open(path, "a", encoding="utf-8") as handle:
                handle.write(line + "\n")
    except OSError:
        return


def handle_encrypted(payload: bytes) -> None:
    if AESGCM is None or not payload or len(payload) < 12 + 16:
        return

    try:
        plaintext = AESGCM(KEY).decrypt(payload[:12], payload[12:], None)
        seq, lat, lon, alt, bat, roll, pitch, yaw, drone_id = unpack_telemetry_plaintext(
            plaintext
        )
    except Exception:
        return

    with _data_lock:
        last = last_telemetry_seq.get(drone_id, 0)
        if seq <= last:
            return
        last_telemetry_seq[drone_id] = seq
        inside, distance = geofence_status(lat, lon)
        record = {
            "latitude": round(lat, 7),
            "longitude": round(lon, 7),
            "altitude": round(alt, 3),
            "battery": round(bat, 2),
            "roll": round(roll, 3),
            "pitch": round(pitch, 3),
            "yaw": round(yaw, 3),
            "last_seen": time.time(),
            "seq": int(seq),
        }
        if inside is not None:
            record["geofence_ok"] = bool(inside)
            record["geofence_distance_m"] = round(float(distance), 1)
        drone_data[drone_id] = record

    _log_telemetry(drone_id, record)


def snapshot(stale_after=15.0):
    now = time.time()
    with _data_lock:
        out = {}
        for drone_id, record in drone_data.items():
            item = dict(record)
            item["stale"] = (now - float(item.get("last_seen", 0))) > stale_after
            out[str(drone_id)] = item
        return out


def on_receive(packet, interface=None):
    data = _decoded_data(packet)
    payload = data.get("payload")
    if not payload:
        return

    framed = decode_frame(payload)
    if framed is not None:
        msg_type, inner = framed
        if msg_type != MSG_TELEMETRY:
            return
        handle_encrypted(inner)
        return

    if not is_swarm_port(data.get("portnum")):
        return
    handle_encrypted(payload)
