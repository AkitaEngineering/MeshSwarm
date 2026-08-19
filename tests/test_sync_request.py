import os
import struct
import threading
import time

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

import mesh_frame
import meshtastic_control as mc
from meshtastic_crypto import save_seq


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


def simulate_ack(seq, drone_id, status, last_seq, key, delay=0.05):
    time.sleep(delay)
    plaintext = struct.pack("<IBBI", int(seq), int(drone_id), int(status), int(last_seq))
    aes = AESGCM(key)
    nonce = os.urandom(12)
    ct = aes.encrypt(nonce, plaintext, None)
    wire = mesh_frame.encode_frame(mesh_frame.MSG_ACK, nonce + ct)
    pkt = {"decoded": {"portnum": mesh_frame.DEFAULT_PORT, "payload": wire}}
    mc._on_receive_control(pkt, None)


def test_wait_for_ack_matches_broadcast_to_real_drone_id():
    mc._acks.clear()
    with mc._ack_condition:
        mc._acks[(4, 77)] = (mc.ACK_STATUS_ACCEPTED, 77)
        mc._ack_condition.notify_all()
    assert mc.wait_for_ack(255, 77, timeout=0.2) == (mc.ACK_STATUS_ACCEPTED, 77)


def test_request_seq_sync_receives_sync_response(monkeypatch):
    dummy = DummyInterface()
    mc.interface = dummy
    monkeypatch.setattr(mc, "next_seq", lambda seq_file=".control_seq": 0xBEEF)

    t = threading.Thread(target=simulate_ack, args=(0xBEEF, 5, 2, 0xBEEF, mc.KEY, 0.02))
    t.start()

    res = mc.request_seq_sync(5, timeout=1.0)
    t.join()

    assert res == 0xBEEF
    assert dummy.sent[0][2] == mesh_frame.DEFAULT_PORT
    assert dummy.sent[0][3] is True


def test_sync_advances_local_counter_when_drone_is_ahead(monkeypatch, tmp_path):
    dummy = DummyInterface()
    mc.interface = dummy
    seq_file = str(tmp_path / "seq")
    save_seq(3, seq_file)
    monkeypatch.setattr(mc, "next_seq", lambda seq_file=seq_file: 4)

    t = threading.Thread(target=simulate_ack, args=(4, 2, 2, 90, mc.KEY, 0.02))
    t.start()
    res = mc.request_seq_sync(2, timeout=1.0)
    t.join()

    assert res == 90
