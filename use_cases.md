# Use cases: MeshSwarm ground control

MeshSwarm is a **low-rate encrypted telemetry and command link** over Meshtastic LoRa. It is not an autopilot, video link, or inner-loop controller. Telemetry is on the order of one packet every five seconds. The command set is RTL, land, emergency land, counter sync, and heartbeat.

It can be useful where cellular and Wi-Fi are unavailable, if the flight controller’s own failsafes are configured and the link has been bench-tested with props off.

## 1. Off-grid position monitoring

A small number of aircraft can report lat/lon, altitude, attitude, and battery to a GCS map over the mesh. If one radio hops through another Meshtastic node, telemetry may still arrive after a delay.

The operator can issue RTL or land per aircraft, or broadcast those commands to drone id 255.

## 2. Site geofence and lost-link RTL

The companion computer can command RTL when:

* the aircraft is outside a circular geofence
* battery voltage is below the configured threshold
* the GCS link is lost after it was once acquired
* the aircraft is armed and never hears the GCS
* GPS was valid and then goes stale while armed

These are MAVLink `COMMAND_LONG` messages retried until `COMMAND_ACK`. They are not a replacement for the flight controller’s radio-loss failsafe.

## 3. Large-area survey support

On farms or other large sites without Wi-Fi, the GCS can watch battery and position on ISM-band LoRa while the autopilot flies its own mission. MeshSwarm does not upload waypoints or camera payloads.

## 4. Temporary mesh relay tracking

If a drone is used as a Meshtastic relay, the GCS can show where that node is and command it home before the battery is empty. Extending responder radios “tens of miles” depends on antennas, terrain, and duty cycle; it is not guaranteed by this firmware.

## 5. Encrypted command link

Telemetry and commands are AES-128-GCM with replay counters. That stops casual spoofing of the swarm payload. It does not hide that LoRa traffic exists, and a single shared swarm key (the default) means one extracted key can impersonate the fleet. Use per-drone keys (`MESHTASTIC_AES_KEY_<id>`) when that matters.

MeshSwarm does not implement custom “reposition to this sector” MAVLink, thermal cameras, or stealth against a determined RF adversary.
