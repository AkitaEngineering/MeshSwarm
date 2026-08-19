from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import pytest

pytest.importorskip("flask")
import gcs_app  # noqa: E402
import mesh_frame  # noqa: E402
import meshtastic_control  # noqa: E402
import meshtastic_crypto as crypto  # noqa: E402
import meshtastic_telemetry  # noqa: E402


class DummyInterface:
    def __init__(self):
        self.sent = []

    def sendData(
        self,
        data,
        destinationId="^all",
        portNum=256,
        wantAck=False,
        **kwargs,
    ):
        self.sent.append((data, destinationId, portNum, wantAck, kwargs))


def _install_dummy_interface(monkeypatch):
    dummy = DummyInterface()
    monkeypatch.setattr(gcs_app, "interface", dummy)
    monkeypatch.setattr(meshtastic_control, "interface", dummy)
    return dummy


def test_control_route_sends_encrypted_command(monkeypatch, tmp_path):
    dummy = _install_dummy_interface(monkeypatch)
    seq_file = tmp_path / "seq"
    monkeypatch.setattr(
        meshtastic_control,
        "next_seq",
        lambda seq_file=str(seq_file): 7,
    )

    client = gcs_app.app.test_client()
    response = client.post(
        "/api/control", json={"drone_id": 3, "command": 1, "wait_ack": False}
    )

    assert response.status_code == 200
    body = response.get_json()
    assert body["success"] is True
    assert body["seq"] == 7
    assert len(dummy.sent) == 1

    wire, destination, port, want_ack, kwargs = dummy.sent[0]
    assert destination == "^all"
    assert port == mesh_frame.DEFAULT_PORT
    assert want_ack is True
    assert kwargs == {}
    framed = mesh_frame.decode_frame(wire)
    assert framed is not None
    msg_type, payload = framed
    assert msg_type == mesh_frame.MSG_CONTROL
    plaintext = AESGCM(meshtastic_control.KEY).decrypt(payload[:12], payload[12:], None)
    assert crypto.unpack_control_plaintext(plaintext) == (7, 3, 1)


def test_control_route_rejects_invalid_command(monkeypatch):
    _install_dummy_interface(monkeypatch)

    client = gcs_app.app.test_client()
    response = client.post("/api/control", json={"drone_id": 3, "command": 99})

    assert response.status_code == 400
    assert response.get_json()["error"] == "unsupported command"


def test_control_route_requires_token_when_configured(monkeypatch):
    _install_dummy_interface(monkeypatch)
    monkeypatch.setenv("MESHTASTIC_API_TOKEN", "secret-token")

    client = gcs_app.app.test_client()
    response = client.post("/api/control", json={"drone_id": 3, "command": 1})

    assert response.status_code == 401

    authorized = client.post(
        "/api/control",
        json={"drone_id": 3, "command": 1, "wait_ack": False},
        headers={"Authorization": "Bearer secret-token"},
    )
    assert authorized.status_code == 200


def test_telemetry_route_requires_token_when_configured(monkeypatch):
    monkeypatch.setenv("MESHTASTIC_API_TOKEN", "secret-token")
    client = gcs_app.app.test_client()
    assert client.get("/api/telemetry").status_code == 401
    ok = client.get("/api/telemetry", headers={"X-API-Token": "secret-token"})
    assert ok.status_code == 200


def test_status_route_reports_disconnected():
    client = gcs_app.app.test_client()
    payload = client.get("/api/status").get_json()
    assert payload["connected"] is False
    assert "auth_required" in payload


def test_subscribe_uses_pubsub_when_no_onreceive(monkeypatch):
    subscribed = {}

    class DummyPub:
        @staticmethod
        def subscribe(handler, topic):
            subscribed["handler"] = handler
            subscribed["topic"] = topic

    class BareIface:
        def sendData(self, *args, **kwargs):
            return None

    monkeypatch.setattr(gcs_app, "_pubsub", DummyPub)
    monkeypatch.setattr(gcs_app, "_receive_subscribed", False)
    assert gcs_app._subscribe_receive(BareIface()) is True
    assert subscribed["topic"] == "meshtastic.receive"
    assert subscribed["handler"] is gcs_app.on_receive


def test_on_receive_dispatches_framed_telemetry(monkeypatch):
    meshtastic_telemetry.drone_data.clear()
    meshtastic_telemetry.last_telemetry_seq.clear()
    plaintext = crypto.pack_telemetry_plaintext(9, 1.0, 2.0, 3.0, 12.0, 1)
    nonce = b"\x11" * 12
    blob = nonce + AESGCM(meshtastic_telemetry.KEY).encrypt(nonce, plaintext, None)
    packet = {
        "decoded": {
            "portnum": "PRIVATE_APP",
            "payload": mesh_frame.encode_frame(mesh_frame.MSG_TELEMETRY, blob),
        }
    }
    gcs_app.on_receive(packet, None)
    assert 1 in meshtastic_telemetry.drone_data
    assert meshtastic_telemetry.drone_data[1]["seq"] == 9
