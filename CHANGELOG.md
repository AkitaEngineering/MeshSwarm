# Changelog

## 0.2.0

Breaking wire change: AES-GCM now authenticates the swarm message type as AAD. GCS and firmware must be upgraded together.

### Firmware
- Retry RTL/Land until MAVLink `COMMAND_ACK` (emergency land uses more retries)
- Parse FC HEARTBEAT armed flag and `COMMAND_ACK`
- RTL if armed and no GCS packet is ever seen (lost-link timeout)
- RTL if a GPS fix is lost while armed (`GPS_LOSS_TIMEOUT_MS`)
- Extract failsafe/seq/haversine for host tests
- Boot warning when the repository test provisioning pubkey is still compiled in

### GCS
- Serve with Waitress instead of the Flask development server
- Reconnect to the Meshtastic serial device with backoff
- Rate-limit `/api/control` (heartbeats exempt)
- Hide live drone counts on `/api/status` until the API token is presented
- Optional per-drone AES keys (`MESHTASTIC_AES_KEY_<id>`)
- Pin runtime Python dependencies

### Tests / CI
- Host C++ tests for mesh frames, MAVLink, and failsafe policy
- GitHub Actions builds ESP-IDF v5.5 firmware and runs host tests
- `workflow_dispatch` so CI can be run by hand

## 0.1.0

Initial MeshSwarm tree: AES-GCM swarm frames, signed `SETKEYSIG`, GCS Flask UI, production startup gates.
