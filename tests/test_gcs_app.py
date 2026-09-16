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
    plaintext = crypto.decrypt_blob(
        meshtastic_control.KEY, payload, mesh_frame.MSG_CONTROL
    )
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
    assert "version" in payload


def test_status_hides_counts_when_unauthorized(monkeypatch):
    monkeypatch.setenv("MESHTASTIC_API_TOKEN", "secret-token")
    client = gcs_app.app.test_client()
    payload = client.get("/api/status").get_json()
    assert payload["auth_required"] is True
    assert "live_drones" not in payload
    assert "error" not in payload
    authorized = client.get(
        "/api/status", headers={"Authorization": "Bearer secret-token"}
    ).get_json()
    assert "live_drones" in authorized


def test_control_route_rate_limited(monkeypatch):
    _install_dummy_interface(monkeypatch)
    monkeypatch.setenv("MESHTASTIC_CONTROL_RATE", "1")
    monkeypatch.setenv("MESHTASTIC_CONTROL_RATE_WINDOW", "60")
    gcs_app._control_hits.clear()
    client = gcs_app.app.test_client()
    first = client.post(
        "/api/control", json={"drone_id": 3, "command": 1, "wait_ack": False}
    )
    assert first.status_code == 200
    second = client.post(
        "/api/control", json={"drone_id": 3, "command": 1, "wait_ack": False}
    )
    assert second.status_code == 429


def test_interface_is_alive_simulated():
    class Fake:
        is_simulated = True

    assert gcs_app.interface_is_alive(Fake()) is True
    assert gcs_app.interface_is_alive(None) is False

    class Closed:
        stream = type("S", (), {"is_open": False})()

    assert gcs_app.interface_is_alive(Closed()) is False


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


def test_control_route_wrong_length_token_is_unauthorized(monkeypatch):
    _install_dummy_interface(monkeypatch)
    monkeypatch.setenv("MESHTASTIC_API_TOKEN", "secret-token")
    client = gcs_app.app.test_client()
    response = client.post(
        "/api/control",
        json={"drone_id": 3, "command": 1, "wait_ack": False},
        headers={"Authorization": "Bearer x"},
    )
    assert response.status_code == 401


def test_validate_startup_allows_localhost_dev(monkeypatch):
    monkeypatch.delenv("MESHTASTIC_PRODUCTION", raising=False)
    monkeypatch.delenv("MESHTASTIC_API_TOKEN", raising=False)
    monkeypatch.delenv("MESHTASTIC_FAKE", raising=False)
    gcs_app.validate_startup("127.0.0.1")


def test_validate_startup_requires_token_in_production(monkeypatch):
    monkeypatch.setenv("MESHTASTIC_PRODUCTION", "1")
    monkeypatch.delenv("MESHTASTIC_API_TOKEN", raising=False)
    with pytest.raises(RuntimeError, match="MESHTASTIC_API_TOKEN"):
        gcs_app.validate_startup("127.0.0.1")


def test_validate_startup_requires_token_on_public_bind(monkeypatch):
    monkeypatch.delenv("MESHTASTIC_PRODUCTION", raising=False)
    monkeypatch.delenv("MESHTASTIC_API_TOKEN", raising=False)
    with pytest.raises(RuntimeError, match="MESHTASTIC_API_TOKEN"):
        gcs_app.validate_startup("0.0.0.0")


def test_validate_startup_refuses_fake_in_production(monkeypatch):
    monkeypatch.setenv("MESHTASTIC_PRODUCTION", "1")
    monkeypatch.setenv("MESHTASTIC_FAKE", "1")
    monkeypatch.setenv("MESHTASTIC_API_TOKEN", "secret-token")
    with pytest.raises(RuntimeError, match="MESHTASTIC_FAKE"):
        gcs_app.validate_startup("127.0.0.1")


def test_validate_startup_refuses_default_key_on_public_bind(monkeypatch):
    monkeypatch.setenv("MESHTASTIC_API_TOKEN", "secret-token")
    monkeypatch.setattr(meshtastic_control, "KEY", crypto.DEFAULT_KEY)
    with pytest.raises(RuntimeError, match="default AES key"):
        gcs_app.validate_startup("0.0.0.0")


def test_validate_startup_refuses_default_key_in_production(monkeypatch):
    monkeypatch.setenv("MESHTASTIC_PRODUCTION", "1")
    monkeypatch.setenv("MESHTASTIC_API_TOKEN", "secret-token")
    monkeypatch.setattr(meshtastic_control, "KEY", crypto.DEFAULT_KEY)
    with pytest.raises(RuntimeError, match="default AES key"):
        gcs_app.validate_startup("127.0.0.1")


def test_on_receive_dispatches_framed_telemetry(monkeypatch):
    meshtastic_telemetry.drone_data.clear()
    meshtastic_telemetry.last_telemetry_seq.clear()
    plaintext = crypto.pack_telemetry_plaintext(9, 1.0, 2.0, 3.0, 12.0, 1)
    blob = crypto.encrypt_blob(meshtastic_telemetry.KEY, plaintext, mesh_frame.MSG_TELEMETRY)
    packet = {
        "decoded": {
            "portnum": "PRIVATE_APP",
            "payload": mesh_frame.encode_frame(mesh_frame.MSG_TELEMETRY, blob),
        }
    }
    gcs_app.on_receive(packet, None)
    assert 1 in meshtastic_telemetry.drone_data
    assert meshtastic_telemetry.drone_data[1]["seq"] == 9
