import os
import struct
import threading

import pytest
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

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
    key = TEST_KEY
    aes = AESGCM(key)
    seq, drone_id, cmd = 99, 2, 7
    plaintext = mc.pack_control_plaintext(seq, drone_id, cmd)
    nonce = os.urandom(12)
    ct = aes.encrypt(nonce, plaintext, None)
    pt = AESGCM(key).decrypt(nonce, ct, None)
    assert pt == plaintext
