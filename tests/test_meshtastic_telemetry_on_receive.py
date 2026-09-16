import meshtastic_telemetry as mt
import meshtastic_crypto as mc
from mesh_frame import MSG_TELEMETRY

KEY = mc.DEFAULT_KEY


def make_telemetry_wire(seq, lat, lon, alt, bat, drone_id):
    plaintext = mc.pack_telemetry_plaintext(seq, lat, lon, alt, bat, drone_id)
    return mc.encrypt_blob(KEY, plaintext, MSG_TELEMETRY)


def make_packet(portnum, wire):
    return {"decoded": {"data": {"portnum": portnum, "payload": wire}}}


class EnumLikePort:
    value = mt.DRONE_TELEMETRY_DATA_TYPE


def test_on_receive_accepts_and_rejects_stale():
    mt.drone_data.pop(5, None)
    mt.last_telemetry_seq.pop(5, None)
    wire = make_telemetry_wire(1, 1.0, 2.0, 3.0, 11.1, 5)
    pkt = make_packet(mt.DRONE_TELEMETRY_DATA_TYPE, wire)
    mt.on_receive(pkt, None)
    assert 5 in mt.drone_data
    last = mt.last_telemetry_seq.get(5)
    assert last == 1

    # same seq (replay) ignored
    mt.on_receive(pkt, None)
    assert mt.last_telemetry_seq.get(5) == 1

    # higher seq accepted
    wire2 = make_telemetry_wire(2, 1.1, 2.1, 3.1, 10.5, 5)
    pkt2 = make_packet(mt.DRONE_TELEMETRY_DATA_TYPE, wire2)
    mt.on_receive(pkt2, None)
    assert mt.last_telemetry_seq.get(5) == 2
    assert mt.drone_data[5]["latitude"] == 1.1


def test_telemetry_is_logged(tmp_path, monkeypatch):
    mt.drone_data.pop(6, None)
    mt.last_telemetry_seq.pop(6, None)
    log_path = tmp_path / "drone_telemetry.jsonl"
    monkeypatch.setenv("MESHTASTIC_TELEMETRY_LOG", str(log_path))
    wire = make_telemetry_wire(1, 1.0, 2.0, 3.0, 11.1, 6)
    mt.on_receive(make_packet(mt.DRONE_TELEMETRY_DATA_TYPE, wire), None)
    text = log_path.read_text(encoding="utf-8")
    assert '"drone_id":6' in text
    assert '"latitude":1.0' in text


def test_geofence_marks_outside_position(monkeypatch):
    mt.drone_data.pop(9, None)
    mt.last_telemetry_seq.pop(9, None)
    monkeypatch.setenv("MESHTASTIC_GEOFENCE_LAT", "40.0")
    monkeypatch.setenv("MESHTASTIC_GEOFENCE_LON", "-74.0")
    monkeypatch.setenv("MESHTASTIC_GEOFENCE_RADIUS_M", "50")
    wire = make_telemetry_wire(4, 41.0, -74.0, 10.0, 12.0, 9)
    mt.on_receive(make_packet(mt.DRONE_TELEMETRY_DATA_TYPE, wire), None)
    assert mt.drone_data[9]["geofence_ok"] is False


def test_on_receive_accepts_enum_like_portnum():
    mt.drone_data.pop(8, None)
    mt.last_telemetry_seq.pop(8, None)
    wire = make_telemetry_wire(3, 1.0, 2.0, 3.0, 11.1, 8)
    pkt = make_packet(EnumLikePort(), wire)

    mt.on_receive(pkt, None)

    assert mt.last_telemetry_seq.get(8) == 3
