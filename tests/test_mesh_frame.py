import mesh_frame


def test_frame_roundtrip_and_crc():
    payload = b"hello-swarm"
    wire = mesh_frame.encode_frame(mesh_frame.MSG_CONTROL, payload)
    decoded = mesh_frame.decode_frame(wire)
    assert decoded == (mesh_frame.MSG_CONTROL, payload)

    tampered = bytearray(wire)
    tampered[-1] ^= 0x01
    assert mesh_frame.decode_frame(bytes(tampered)) is None


def test_find_frame_resyncs_after_garbage():
    payload = b"abc"
    frame = mesh_frame.encode_frame(mesh_frame.MSG_TELEMETRY, payload)
    buf = b"\x00\x01\x02" + frame + b"zz"
    consumed, decoded, rest = mesh_frame.find_frame(buf)
    assert consumed == 3
    consumed, decoded, rest = mesh_frame.find_frame(rest)
    assert decoded == (mesh_frame.MSG_TELEMETRY, payload)
    assert rest == b"zz"


def test_portnum_matches_string_and_int():
    assert mesh_frame.is_swarm_port(256)
    assert mesh_frame.is_swarm_port("PRIVATE_APP")
    assert mesh_frame.is_swarm_port("SERIAL_APP")
    assert mesh_frame.is_swarm_port(64)
    assert not mesh_frame.is_swarm_port("TEXT_MESSAGE_APP")
