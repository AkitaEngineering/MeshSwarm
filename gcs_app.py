import hashlib
import hmac
import os
import sys
import threading
from typing import Any

from flask import Flask, jsonify, render_template, request

import meshtastic.serial_interface

import mesh_frame
import meshtastic_control
import meshtastic_crypto
import meshtastic_telemetry

_pubsub: Any
try:
    from pubsub import pub as _pubsub
except ImportError:  # pragma: no cover - pubsub ships with meshtastic
    _pubsub = None

app = Flask(__name__)

interface = None
interface_error = None
_receive_subscribed = False
_heartbeat_stop = threading.Event()


def _configured_api_token():
    raw = os.environ.get("MESHTASTIC_API_TOKEN")
    if raw is None:
        return None
    token = raw.strip()
    return token or None


def _is_loopback_host(host: str) -> bool:
    return (host or "").strip().lower() in {
        "127.0.0.1",
        "localhost",
        "::1",
        "ip6-localhost",
    }


def _tokens_match(supplied: str, expected: str) -> bool:
    if not isinstance(supplied, str) or not isinstance(expected, str):
        return False
    left = hashlib.sha256(supplied.encode("utf-8")).digest()
    right = hashlib.sha256(expected.encode("utf-8")).digest()
    return hmac.compare_digest(left, right)


def validate_startup(host: str | None = None) -> None:
    """Refuse unsafe GCS combinations before the radio thread starts."""
    if host is None:
        host = os.environ.get("MESHTASTIC_GCS_HOST", "127.0.0.1")
    production = os.environ.get("MESHTASTIC_PRODUCTION") == "1"
    fake = os.environ.get("MESHTASTIC_FAKE") == "1"
    token = _configured_api_token()
    public = not _is_loopback_host(host)
    using_default = meshtastic_control.KEY == meshtastic_crypto.DEFAULT_KEY

    if production and fake:
        raise RuntimeError("MESHTASTIC_FAKE=1 is not allowed when MESHTASTIC_PRODUCTION=1")
    if (production or public) and not token:
        raise RuntimeError(
            "MESHTASTIC_API_TOKEN is required in production and when binding "
            "beyond localhost"
        )
    if (production or public) and using_default:
        raise RuntimeError(
            "Refusing to start with the compiled default AES key"
        )


def _control_authorized():
    token = _configured_api_token()
    if not token:
        return True

    auth_header = request.headers.get("Authorization", "")
    if auth_header.startswith("Bearer "):
        supplied = auth_header.removeprefix("Bearer ").strip()
    else:
        supplied = request.headers.get("X-API-Token", "")

    return _tokens_match(supplied, token)


def _api_authorized():
    return _control_authorized()


def on_receive(packet, received_interface=None):
    data = packet.get("decoded") or {}
    if "data" in data and isinstance(data["data"], dict):
        data = data["data"]
    payload = data.get("payload")
    if not payload:
        return

    framed = mesh_frame.decode_frame(payload)
    if framed is not None:
        msg_type, inner = framed
        if msg_type == mesh_frame.MSG_TELEMETRY:
            meshtastic_telemetry.handle_encrypted(inner)
        elif msg_type == mesh_frame.MSG_ACK:
            meshtastic_control.handle_encrypted_ack(inner)
        return

    meshtastic_telemetry.on_receive(packet, received_interface)
    meshtastic_control._on_receive_control(packet, received_interface)


def _subscribe_receive(iface):
    global _receive_subscribed
    hooked = False
    if hasattr(iface, "onReceive"):
        try:
            iface.onReceive += on_receive
            hooked = True
        except Exception:
            hooked = False
    if _pubsub is not None and not _receive_subscribed:
        _pubsub.subscribe(on_receive, "meshtastic.receive")
        _receive_subscribed = True
        hooked = True
    return hooked


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/status")
def get_status():
    stale_after = float(os.environ.get("MESHTASTIC_STALE_AFTER", "15"))
    snapshot = meshtastic_telemetry.snapshot(stale_after=stale_after)
    live = sum(1 for item in snapshot.values() if not item.get("stale"))
    return jsonify(
        {
            "connected": interface is not None,
            "error": interface_error,
            "auth_required": bool(_configured_api_token()),
            "drone_count": len(snapshot),
            "live_drones": live,
            "portnum": mesh_frame.mesh_portnum(),
        }
    )


@app.route("/api/telemetry")
def get_telemetry():
    if not _api_authorized():
        return jsonify({"error": "unauthorized"}), 401
    stale_after = float(os.environ.get("MESHTASTIC_STALE_AFTER", "15"))
    return jsonify(meshtastic_telemetry.snapshot(stale_after=stale_after))


@app.route("/api/control", methods=["POST"])
def send_command():
    data = request.get_json(silent=True) or {}

    if not _control_authorized():
        return jsonify({"error": "unauthorized"}), 401

    if interface is None:
        return jsonify({"error": "Meshtastic not connected"}), 500

    try:
        drone_id = int(data.get("drone_id", 0))
        command = int(data.get("command", 0))
    except (TypeError, ValueError):
        return jsonify({"error": "drone_id and command must be integers"}), 400

    if not 0 <= drone_id <= 255:
        return jsonify({"error": "drone_id must be between 0 and 255"}), 400
    allowed_commands = {
        meshtastic_control.COMMAND_RTL,
        meshtastic_control.COMMAND_LAND,
        meshtastic_control.COMMAND_EMERGENCY_LAND,
        meshtastic_control.COMMAND_SYNC_REQUEST,
        meshtastic_control.COMMAND_HEARTBEAT,
    }
    if command not in allowed_commands:
        return jsonify({"error": "unsupported command"}), 400

    try:
        seq = meshtastic_control.send_control_command(drone_id, command)
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500

    wait_ack = bool(data.get("wait_ack", command != meshtastic_control.COMMAND_HEARTBEAT))
    timeout = float(data.get("ack_timeout", os.environ.get("MESHTASTIC_ACK_TIMEOUT", "5")))
    ack_status = None
    last_seq = None
    if wait_ack:
        ack = meshtastic_control.wait_for_ack(drone_id, seq, timeout=timeout)
        if ack is not None:
            ack_status, last_seq = ack

    return jsonify(
        {
            "success": True,
            "seq": seq,
            "ack_status": ack_status,
            "last_seq": last_seq,
        }
    )


def _heartbeat_loop():
    raw = os.environ.get("MESHTASTIC_HEARTBEAT_INTERVAL", "15")
    try:
        interval = float(raw)
    except ValueError:
        interval = 15.0
    if interval <= 0:
        return
    while not _heartbeat_stop.wait(interval):
        if interface is None:
            continue
        try:
            meshtastic_control.send_control_command(
                255, meshtastic_control.COMMAND_HEARTBEAT
            )
        except Exception:
            continue


def meshtastic_thread():
    global interface, interface_error
    try:
        if os.environ.get("MESHTASTIC_FAKE") == "1":
            from qa_simulator import SimulatedMeshtasticInterface

            interval = float(os.environ.get("MESHTASTIC_FAKE_INTERVAL", "1.0"))
            drone_id = int(os.environ.get("MESHTASTIC_FAKE_DRONE_ID", "1"))
            interface = SimulatedMeshtasticInterface(drone_id, interval)
        else:
            port = os.environ.get("MESHTASTIC_SERIAL_PORT") or None
            interface = meshtastic.serial_interface.SerialInterface(devPath=port)
            if not hasattr(interface, "sendData"):
                raise RuntimeError("No Meshtastic serial device with sendData")

        meshtastic_control.interface = interface
        _subscribe_receive(interface)
        if hasattr(interface, "start"):
            interface.start()
        interface_error = None
    except Exception as exc:
        interface = None
        meshtastic_control.interface = None
        interface_error = str(exc)
        print(f"Meshtastic interface failed: {exc}")


if __name__ == "__main__":
    host = os.environ.get("MESHTASTIC_GCS_HOST", "127.0.0.1")
    port = int(os.environ.get("MESHTASTIC_GCS_PORT", "5000"))
    validate_startup(host)
    if meshtastic_control.KEY == meshtastic_crypto.DEFAULT_KEY:
        print(
            "WARNING: using compiled NIST test AES key. "
            "Set MESHTASTIC_PRODUCTION=1 and provision a real key before flight.",
            file=sys.stderr,
        )
    t = threading.Thread(target=meshtastic_thread, daemon=True)
    t.start()
    hb = threading.Thread(target=_heartbeat_loop, daemon=True)
    hb.start()
    app.run(host=host, port=port, debug=False, threaded=True)
