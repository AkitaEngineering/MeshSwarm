"""In-process Meshtastic interface simulator for QA runs without hardware."""
import struct
import threading

import mesh_frame
import meshtastic_control as control
import meshtastic_crypto as crypto
import meshtastic_telemetry as telemetry


class EventHook:
    def __init__(self):
        self._handlers = []

    def __iadd__(self, handler):
        self._handlers.append(handler)
        return self

    def fire(self, packet):
        for handler in list(self._handlers):
            handler(packet, self)


class SimulatedMeshtasticInterface:
    is_simulated = True

    def __init__(self, drone_id=1, telemetry_interval=1.0):
        self.drone_id = int(drone_id)
        self.telemetry_interval = float(telemetry_interval)
        self.onReceive = EventHook()
        self.sent = []
        self._stop = threading.Event()
        self._thread = None
        self._seq = 0
        self.last_control_seq = 0

    def start(self):
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._telemetry_loop, daemon=True)
        self._thread.start()

    def close(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)

    def sendData(
        self,
        data,
        destinationId="^all",
        portNum=256,
        wantAck=False,
        **kwargs,
    ):
        self.sent.append((data, destinationId, portNum, wantAck, kwargs))
        framed = mesh_frame.decode_frame(data)
        if framed is None or framed[0] != mesh_frame.MSG_CONTROL:
            return
        try:
            plaintext = crypto.decrypt_blob(control.KEY, framed[1], mesh_frame.MSG_CONTROL)
            seq, drone_id, command = crypto.unpack_control_plaintext(plaintext)
        except Exception:
            return

        if drone_id not in {self.drone_id, 255}:
            return

        status = control.ACK_STATUS_ACCEPTED
        if command == control.COMMAND_SYNC_REQUEST:
            status = control.ACK_STATUS_SYNC
            if seq > self.last_control_seq:
                self.last_control_seq = seq
        elif command not in {
            control.COMMAND_RTL,
            control.COMMAND_LAND,
            control.COMMAND_EMERGENCY_LAND,
            control.COMMAND_HEARTBEAT,
        }:
            status = control.ACK_STATUS_REJECTED
        elif seq <= self.last_control_seq:
            status = control.ACK_STATUS_REJECTED
        else:
            self.last_control_seq = seq

        ack_plaintext = struct.pack(
            "<IBBI", seq, self.drone_id, status, self.last_control_seq
        )
        self.onReceive.fire(
            {
                "decoded": {
                    "portnum": mesh_frame.mesh_portnum(),
                    "payload": mesh_frame.encode_frame(
                        mesh_frame.MSG_ACK,
                        crypto.encrypt_blob(
                            control.KEY, ack_plaintext, mesh_frame.MSG_ACK
                        ),
                    ),
                }
            }
        )

    def _telemetry_loop(self):
        while not self._stop.wait(self.telemetry_interval):
            self.emit_telemetry()

    def emit_telemetry(self):
        self._seq += 1
        plaintext = crypto.pack_telemetry_plaintext(
            self._seq,
            40.7128 + self._seq * 0.0001,
            -74.0060 - self._seq * 0.0001,
            30.0 + self._seq,
            12.4,
            0.0,
            0.0,
            0.0,
            self.drone_id,
        )
        self.onReceive.fire(
            {
                "decoded": {
                    "portnum": mesh_frame.mesh_portnum(),
                    "payload": mesh_frame.encode_frame(
                        mesh_frame.MSG_TELEMETRY,
                        crypto.encrypt_blob(
                            telemetry.KEY, plaintext, mesh_frame.MSG_TELEMETRY
                        ),
                    ),
                }
            }
        )
