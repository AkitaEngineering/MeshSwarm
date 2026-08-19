import time

import meshtastic_control as control
import meshtastic_telemetry as telemetry
from qa_simulator import SimulatedMeshtasticInterface


def test_simulator_emits_telemetry_and_ack(monkeypatch, tmp_path):
    seq_file = tmp_path / "seq"
    monkeypatch.setattr(control, "next_seq", lambda seq_file=str(seq_file): 123)

    sim = SimulatedMeshtasticInterface(drone_id=4, telemetry_interval=60.0)
    control.interface = sim
    sim.onReceive += telemetry.on_receive
    sim.onReceive += control._on_receive_control

    sim.emit_telemetry()
    assert telemetry.drone_data[4]["battery"] == 12.4

    assert control.send_control_command(4, control.COMMAND_RTL) == 123

    deadline = time.time() + 1.0
    while time.time() < deadline:
        if control._acks.get((4, 123)) is not None:
            break
        time.sleep(0.01)

    assert control._acks[(4, 123)][0] == control.ACK_STATUS_ACCEPTED
