# MeshSwarm

Companion-computer firmware and a ground-control station (GCS) that carry **encrypted MAVLink telemetry and a small command set** over a Meshtastic mesh.

Repository: [github.com/AkitaEngineering/MeshSwarm](https://github.com/AkitaEngineering/MeshSwarm). Previously published as *Meshtastic-Integration-for-DroneBridge32-Swarm*.

This is a control/telemetry link, not a full autopilot. Flight mode still belongs to the flight controller. Bench-test with props off before any flight.

## What it does

* AES-GCM encrypted telemetry (position, altitude, attitude, battery) and commands (RTL, land, emergency land, counter sync, heartbeat)
* Replay protection with persisted sequence numbers
* Signed AES-key provisioning on the ESP32 (`SETKEYSIG`)
* Lost-link, low-battery, and circular geofence RTL on the aircraft
* Web GCS with live map, command ACKs, API token, and JSONL telemetry log
* In-process fake radio (`MESHTASTIC_FAKE=1`) for app QA without hardware

## Architecture

```
GCS (Python)  --USB protobuf-->  Ground Meshtastic node
                                      |
                                   LoRa mesh
                                      |
ESP32 companion  --UART1 PROTO-->  Drone Meshtastic node
       |
     UART2 MAVLink
       |
Flight controller
```

Swarm payloads are an inner frame (`0xDB 0x32` + type + length + AES-GCM blob + CRC16) carried on Meshtastic **PRIVATE_APP (256)**. Configure the drone Meshtastic **Serial Module mode to PROTO** so UART1 is the client API (`0x94 0xC3` ToRadio/FromRadio).

For Serial Module **SIMPLE** mode, compile firmware with `-DMESH_SERIAL_SIMPLE` and run the GCS with `MESHTASTIC_PORTNUM=64` (SERIAL_APP). Receive still accepts both 64 and 256.

## Components

1. **Ground Control Station** — Flask app (`gcs_app.py`) plus Leaflet UI.
2. **ESP32 firmware** — ESP-IDF FreeRTOS node in `main/` (Meshtastic codec, MAVLink heartbeat/commands, fail-safes).

## Installation

```bash
git clone https://github.com/AkitaEngineering/MeshSwarm.git
cd MeshSwarm
```

### ESP32 firmware

1. Install ESP-IDF **v5.x** (v5.5 is the version this tree is built against).
2. Load the IDF environment so `idf.py`, IDF CMake **3.30.2**, Ninja **1.12.1**, and `xtensa-esp-elf-gcc` are on `PATH`. Distro CMake 4.x and `arm-none-eabi-gcc` will not configure this project.

   ```bash
   . $HOME/esp/esp-idf/export.sh          # bash
   source $HOME/esp/esp-idf/export.fish   # fish
   ```

   If `cmake --version` still reports 4.x, install the IDF copies: `python $IDF_PATH/tools/idf_tools.py install cmake ninja`, then source `export` again.
3. From this repository: `idf.py set-target esp32` then `idf.py build` and `idf.py flash monitor`.
4. Wire MAVLink TX/RX to UART2 (pins 17/16) and Meshtastic UART to UART1 (pins 4/5).
5. On the drone Meshtastic node enable the Serial Module in **PROTO** mode at 115200 8N1.

No external MAVLink component is required; the firmware includes a minimal encoder/decoder for HEARTBEAT, COMMAND_LONG, GLOBAL_POSITION_INT, SYS_STATUS, and ATTITUDE.

### Ground station

```bash
pip install -r requirements.txt
python gcs_app.py
```

Open `http://127.0.0.1:5000`. Leaflet assets are served locally; OSM map tiles still need network.

App-level QA without radios:

```bash
MESHTASTIC_FAKE=1 MESHTASTIC_API_TOKEN=qa-token python gcs_app.py
```

## Configuration

* **Drone ID:** default 1. USB console `SETID:2` (persisted in NVS). Rebuild with `-DDRONE_ID=n` for a compile-time default.
* **AES key (GCS), in order:** `MESHTASTIC_AES_KEY_FILE` (32 hex chars), `MESHTASTIC_AES_KEY`, system keyring, compiled NIST test key (dev only). Set `MESHTASTIC_PRODUCTION=1` or `MESHTASTIC_REQUIRE_KEY=1` to refuse the fallback. A configured file or env var that is missing or malformed is an error; it will not silently use the test key.
* **AES key (MCU):** `SETKEYSIG:<32-hex>:<ecdsa-der-hex>` generated with `python scripts/provision_key.py --hex <32 hex chars> --sign-pem /path/to/provisioning_private.pem`. Unsigned `SETKEY:` is compiled out unless `ALLOW_INSECURE_SETKEY`.
* **Provisioning public key:** replace `provisioning_pubkey.h` and `scripts/provisioning_public.pem` before deployment. Do not commit production private keys.
* **API token:** `MESHTASTIC_API_TOKEN`. The UI sends `Authorization: Bearer …` from the header field (stored in browser localStorage). Required for `/api/control` and `/api/telemetry` when set.
* **Bind address:** `MESHTASTIC_GCS_HOST` / `MESHTASTIC_GCS_PORT` (default `127.0.0.1:5000`). Binding beyond localhost requires `MESHTASTIC_API_TOKEN` and a non-default AES key.
* **Serial port:** `MESHTASTIC_SERIAL_PORT` if more than one Meshtastic device is attached.
* **Mesh port:** `MESHTASTIC_PORTNUM` (default 256).
* **Heartbeat:** `MESHTASTIC_HEARTBEAT_INTERVAL` seconds (default 15; `0` disables). Aircraft RTL after `LOST_LINK_TIMEOUT_MS` (default 30000) without a valid command/heartbeat.
* **Command ACK wait:** `MESHTASTIC_ACK_TIMEOUT` seconds (default 5).
* **Geofence (GCS UI):** `MESHTASTIC_GEOFENCE_LAT`, `MESHTASTIC_GEOFENCE_LON`, `MESHTASTIC_GEOFENCE_RADIUS_M`.
* **Geofence (aircraft RTL):** USB console `SETFENCE:<lat>:<lon>:<radius_m>` (NVS). Radius `0` disables.
* **Telemetry log:** `MESHTASTIC_TELEMETRY_LOG` (default `drone_telemetry.jsonl`). Set `MESHTASTIC_TELEMETRY_LOG_DISABLE=1` to turn off.
* **Replay / sync:** sequence files `.control_seq` on the GCS and NVS `ctrl_seq` on the drone. If the GCS counter is behind, use **Sync Counters** (command 4). Sync is accepted even when the GCS seq is stale; the ACK carries the drone’s last seq.

## Commands

| ID | Name | Aircraft action |
|---:|---|---|
| 1 | RTL | `MAV_CMD_NAV_RETURN_TO_LAUNCH` |
| 2 | Land | `MAV_CMD_NAV_LAND` |
| 3 | Emergency land | `MAV_CMD_NAV_LAND` with confirmation=1 |
| 4 | Sync | report/advance control counter; no flight-mode change |
| 5 | Heartbeat | link keepalive (GCS sends this automatically) |

Broadcast drone id **255** addresses the whole swarm.

## Production checklist

The GCS refuses several unsafe combinations on startup:

* `MESHTASTIC_PRODUCTION=1` refuses the compiled test AES key, `MESHTASTIC_FAKE=1`, and a missing API token
* Binding `MESHTASTIC_GCS_HOST` beyond localhost refuses a missing API token and the compiled test AES key
* A set-but-invalid `MESHTASTIC_AES_KEY` / `MESHTASTIC_AES_KEY_FILE` is an error (no silent fallback to the test key)

Operator steps:

1. `pip install -r requirements-dev.txt && flake8 --jobs=1 . && mypy --ignore-missing-imports . && pytest -q`
2. `idf.py build` from a clean checkout with ESP-IDF installed. Do not compile with `ALLOW_INSECURE_DEFAULT_KEY` or `ALLOW_INSECURE_SETKEY`.
3. Replace `provisioning_pubkey.h` and `scripts/provisioning_public.pem`; keep the matching private key offline and never commit it
4. Provision a non-default AES key on the GCS and every drone (`SETKEYSIG`); set `MESHTASTIC_PRODUCTION=1`
5. Set `MESHTASTIC_API_TOKEN` (required for production and for any non-localhost bind)
6. Set unique drone IDs, aircraft geofence (`SETFENCE`), and lost-link timeout for the site
7. Bench-test command ACKs, replay rejection, radio-out RTL, low battery RTL, and geofence RTL with props off

## Tests

Python unit tests cover framing, crypto, GCS routes, pubsub subscribe, fake radio, geofence flags, and logging. They do not replace a hardware-in-the-loop pass.

## Disclaimer

Provided as-is, without warranty. You are responsible for airframe fail-safes, legal operation, and not flying an untested link.
