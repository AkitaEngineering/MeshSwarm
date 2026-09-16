import os
import struct
import threading

import pytest

import meshtastic_crypto as mc

DEFAULT = bytes(range(16))
TEST_HEX = "00112233445566778899aabbccddeeff"
TEST_KEY = bytes.fromhex(TEST_HEX)


def test_load_key_from_env_file(tmp_path):
    p = tmp_path / "key.hex"
    p.write_text(TEST_HEX)
    os.environ["MESHTASTIC_AES_KEY_FILE"] = str(p)
    try:
        k = mc.load_key(DEFAULT)
        assert k == TEST_KEY
    finally:
        del os.environ["MESHTASTIC_AES_KEY_FILE"]


def test_load_key_from_env_var():
    os.environ["MESHTASTIC_AES_KEY"] = TEST_HEX
    try:
        k = mc.load_key(DEFAULT)
        assert k == TEST_KEY
    finally:
        del os.environ["MESHTASTIC_AES_KEY"]


def test_load_key_requires_configured_key(monkeypatch):
    monkeypatch.delenv("MESHTASTIC_AES_KEY_FILE", raising=False)
    monkeypatch.delenv("MESHTASTIC_AES_KEY", raising=False)
    monkeypatch.setattr(mc, "keyring", None)

    with pytest.raises(RuntimeError):
        mc.load_key(DEFAULT, require_configured=True)


def test_load_key_rejects_missing_key_file(monkeypatch):
    monkeypatch.setenv("MESHTASTIC_AES_KEY_FILE", "/no/such/meshswarm-key.hex")
    monkeypatch.delenv("MESHTASTIC_AES_KEY", raising=False)
    monkeypatch.setattr(mc, "keyring", None)
    with pytest.raises(RuntimeError, match="not found"):
        mc.load_key(DEFAULT)


def test_load_key_rejects_malformed_key_file(tmp_path, monkeypatch):
    path = tmp_path / "key.hex"
    path.write_text("not-hex")
    monkeypatch.setenv("MESHTASTIC_AES_KEY_FILE", str(path))
    monkeypatch.delenv("MESHTASTIC_AES_KEY", raising=False)
    monkeypatch.setattr(mc, "keyring", None)
    with pytest.raises(RuntimeError, match="not hexadecimal"):
        mc.load_key(DEFAULT)


def test_load_key_rejects_wrong_length_env(monkeypatch):
    monkeypatch.delenv("MESHTASTIC_AES_KEY_FILE", raising=False)
    monkeypatch.setenv("MESHTASTIC_AES_KEY", "aabb")
    monkeypatch.setattr(mc, "keyring", None)
    with pytest.raises(RuntimeError, match="16 bytes"):
        mc.load_key(DEFAULT)


def test_save_key_to_file_is_private(tmp_path):
    path = tmp_path / "k.hex"
    assert mc.save_key_to_file(TEST_KEY, str(path))
    assert (path.stat().st_mode & 0o777) == 0o600


def test_pack_unpack_control():
    seq, drone_id, cmd = 123, 7, 42
    b = mc.pack_control_plaintext(seq, drone_id, cmd)
    assert struct.calcsize("<IBi") == len(b)
    s2, d2, c2 = mc.unpack_control_plaintext(b)
    assert (s2, d2, c2) == (seq, drone_id, cmd)


def test_pack_unpack_telemetry():
    seq = 5
    lat, lon, alt, bat, roll, pitch, yaw = 1.1, 2.2, 3.3, 11.1, 0.1, 0.2, 0.3
    drone_id = 3
    b = mc.pack_telemetry_plaintext(seq, lat, lon, alt, bat, roll, pitch, yaw, drone_id)
    assert struct.calcsize("<IiiiffffB") == len(b)
    s2, lat2, lon2, alt2, bat2, r2, p2, y2, id2 = mc.unpack_telemetry_plaintext(b)
    assert s2 == seq and id2 == drone_id
    assert pytest.approx(lat) == lat2


def test_seq_persistence(tmp_path):
    seq_file = str(tmp_path / "seq.test")
    # ensure starting at zero
    assert mc.load_seq(seq_file) == 0
    s1 = mc.next_seq(seq_file)
    assert s1 == 1
    s2 = mc.next_seq(seq_file)
    assert s2 == 2
    assert mc.load_seq(seq_file) == 2


def test_next_seq_is_thread_safe(tmp_path):
    seq_file = str(tmp_path / "seq.threaded")
    results = []

    def worker():
        results.append(mc.next_seq(seq_file))

    threads = [threading.Thread(target=worker) for _ in range(25)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    assert sorted(results) == list(range(1, 26))
    assert mc.load_seq(seq_file) == 25


def test_pack_control_rejects_out_of_range_fields():
    with pytest.raises(ValueError):
        mc.pack_control_plaintext(1, 256, 1)
    with pytest.raises(ValueError):
        mc.pack_control_plaintext(0x100000000, 1, 1)


def test_aes_gcm_roundtrip_control():
    seq, drone_id, cmd = 99, 2, 7
    plaintext = mc.pack_control_plaintext(seq, drone_id, cmd)
    blob = mc.encrypt_blob(TEST_KEY, plaintext, 2)
    assert mc.decrypt_blob(TEST_KEY, blob, 2) == plaintext


def test_aad_mismatch_fails():
    plaintext = mc.pack_control_plaintext(1, 1, 1)
    blob = mc.encrypt_blob(TEST_KEY, plaintext, 2)
    with pytest.raises(Exception):
        mc.decrypt_blob(TEST_KEY, blob, 1)


def test_load_per_drone_keys(monkeypatch):
    monkeypatch.setenv("MESHTASTIC_AES_KEY_3", TEST_HEX)
    monkeypatch.setenv("MESHTASTIC_AES_KEY_FILE", "/unused")
    keys = mc.load_per_drone_keys()
    assert keys[3] == TEST_KEY
    assert 1 not in keys


def test_decrypt_blob_any_tries_second_key():
    other = bytes.fromhex("ffeeddccbbaa99887766554433221100")
    plaintext = b"hello-aad-world!!"
    blob = mc.encrypt_blob(other, plaintext, 1)
    assert mc.decrypt_blob_any([TEST_KEY, other], blob, 1) == plaintext


def test_per_drone_key_used_for_command(monkeypatch):
    import meshtastic_control as control

    monkeypatch.setenv("MESHTASTIC_AES_KEY_7", TEST_HEX)
    monkeypatch.setattr(control, "KEY", b"\x00" * 16)
    assert control.key_for_command(7) == TEST_KEY
    assert control.key_for_command(1) == b"\x00" * 16
    assert control.key_for_command(255) == b"\x00" * 16
