import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

# Isolate import-time AES key loading in meshtastic_control / telemetry.
for _name in (
    "MESHTASTIC_PRODUCTION",
    "MESHTASTIC_REQUIRE_KEY",
    "MESHTASTIC_AES_KEY",
    "MESHTASTIC_AES_KEY_FILE",
    "MESHTASTIC_API_TOKEN",
    "MESHTASTIC_FAKE",
):
    os.environ.pop(_name, None)


@pytest.fixture(autouse=True)
def _isolate_side_effects(tmp_path, monkeypatch):
    monkeypatch.setenv("MESHTASTIC_TELEMETRY_LOG", str(tmp_path / "drone_telemetry.jsonl"))
    monkeypatch.delenv("MESHTASTIC_API_TOKEN", raising=False)
    monkeypatch.delenv("MESHTASTIC_PRODUCTION", raising=False)
    monkeypatch.delenv("MESHTASTIC_REQUIRE_KEY", raising=False)
    monkeypatch.delenv("MESHTASTIC_FAKE", raising=False)
    monkeypatch.delenv("MESHTASTIC_AES_KEY", raising=False)
    monkeypatch.delenv("MESHTASTIC_AES_KEY_FILE", raising=False)
    monkeypatch.delenv("MESHTASTIC_GCS_HOST", raising=False)
    monkeypatch.delenv("MESHTASTIC_GEOFENCE_LAT", raising=False)
    monkeypatch.delenv("MESHTASTIC_GEOFENCE_LON", raising=False)
    monkeypatch.delenv("MESHTASTIC_GEOFENCE_RADIUS_M", raising=False)
