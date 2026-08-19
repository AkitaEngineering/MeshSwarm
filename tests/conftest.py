import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


@pytest.fixture(autouse=True)
def _isolate_side_effects(tmp_path, monkeypatch):
    monkeypatch.setenv("MESHTASTIC_TELEMETRY_LOG", str(tmp_path / "drone_telemetry.jsonl"))
    monkeypatch.delenv("MESHTASTIC_API_TOKEN", raising=False)
    monkeypatch.delenv("MESHTASTIC_GEOFENCE_LAT", raising=False)
    monkeypatch.delenv("MESHTASTIC_GEOFENCE_LON", raising=False)
    monkeypatch.delenv("MESHTASTIC_GEOFENCE_RADIUS_M", raising=False)
