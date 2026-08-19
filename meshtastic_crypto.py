"""
Utility helpers for AES key loading, sequence persistence and plaintext packing
(kept independent from GUI modules so they can be unit-tested).
"""
import os
import struct
import threading

# Default AES-128 test key (example). Use secure provisioning in production.
DEFAULT_KEY = bytes([
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C,
])

try:
    import keyring
except Exception:
    keyring = None


_seq_lock = threading.Lock()


def load_key(default_key: bytes, require_configured: bool = False) -> bytes:
    """Load AES key from (in order): file (MESHTASTIC_AES_KEY_FILE),
    env (MESHTASTIC_AES_KEY), system keyring (keyring.get_password),
    fallback to default_key unless require_configured is true.

    Keys are expected as 32-hex-character strings (16 bytes).
    """
    # 1) file
    keyfile = os.environ.get("MESHTASTIC_AES_KEY_FILE")
    if keyfile and os.path.exists(keyfile):
        try:
            with open(keyfile, "r", encoding="utf-8") as f:
                data = f.read().strip()
            key = bytes.fromhex(data)
            if len(key) == 16:
                return key
        except Exception:
            pass

    # 2) env var
    env = os.environ.get("MESHTASTIC_AES_KEY")
    if env:
        try:
            key = bytes.fromhex(env.strip())
            if len(key) == 16:
                return key
        except Exception:
            pass

    # 3) keyring (optional)
    if keyring is not None:
        try:
            v = keyring.get_password("meshtastic", "aes_key")
            if v:
                key = bytes.fromhex(v.strip())
                if len(key) == 16:
                    return key
        except Exception:
            pass

    if require_configured:
        raise RuntimeError(
            "No configured AES key found. Set MESHTASTIC_AES_KEY_FILE, "
            "MESHTASTIC_AES_KEY, or a system keyring entry."
        )

    # fallback
    return default_key


# Sequence persistence helpers (used by control clients)
def load_seq(seq_file: str = ".control_seq") -> int:
    try:
        with open(seq_file, "r", encoding="utf-8") as f:
            return int(f.read().strip())
    except Exception:
        return 0


def save_seq(seq: int, seq_file: str = ".control_seq") -> None:
    try:
        tmp_file = f"{seq_file}.tmp"
        with open(tmp_file, "w", encoding="utf-8") as f:
            f.write(str(int(seq)))
        os.replace(tmp_file, seq_file)
    except Exception:
        pass


def next_seq(seq_file: str = ".control_seq") -> int:
    with _seq_lock:
        seq = load_seq(seq_file) + 1
        save_seq(seq, seq_file)
        return seq


# Key persistence helpers
def save_key_to_file(key: bytes, path: str) -> bool:
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(key.hex())
        return True
    except Exception:
        return False


def save_key_to_keyring(key: bytes) -> bool:
    if keyring is None:
        return False
    try:
        keyring.set_password("meshtastic", "aes_key", key.hex())
        return True
    except Exception:
        return False


# Plaintext packing/unpacking helpers
def pack_control_plaintext(seq: int, drone_id: int, command: int) -> bytes:
    if not 0 <= int(seq) <= 0xFFFFFFFF:
        raise ValueError("seq must fit in uint32")
    if not 0 <= int(drone_id) <= 0xFF:
        raise ValueError("drone_id must fit in uint8")
    if not -0x80000000 <= int(command) <= 0x7FFFFFFF:
        raise ValueError("command must fit in int32")
    return struct.pack("<IBi", int(seq), int(drone_id), int(command))


def unpack_control_plaintext(b: bytes):
    return struct.unpack("<IBi", b)


def _coord_e7(value) -> int:
    """Encode degrees as MAVLink-style 1e-7 integers."""
    scaled = int(round(float(value) * 10000000.0))
    if not -2147483648 <= scaled <= 2147483647:
        raise ValueError("coordinate out of range")
    return scaled


def _alt_mm(value) -> int:
    scaled = int(round(float(value) * 1000.0))
    if not -2147483648 <= scaled <= 2147483647:
        raise ValueError("altitude out of range")
    return scaled


def pack_telemetry_plaintext(
    seq: int,
    latitude: float,
    longitude: float,
    altitude: float,
    battery: float,
    *attitude_and_id,
) -> bytes:
    if len(attitude_and_id) == 1:
        roll, pitch, yaw, drone_id = 0.0, 0.0, 0.0, attitude_and_id[0]
    elif len(attitude_and_id) == 4:
        roll, pitch, yaw, drone_id = attitude_and_id
    else:
        raise TypeError(
            "expected telemetry fields as (seq, lat, lon, alt, battery, "
            "drone_id) or (seq, lat, lon, alt, battery, roll, pitch, yaw, "
            "drone_id)"
        )

    if not 0 <= int(seq) <= 0xFFFFFFFF:
        raise ValueError("seq must fit in uint32")
    if not 0 <= int(drone_id) <= 0xFF:
        raise ValueError("drone_id must fit in uint8")

    return struct.pack(
        "<IiiiffffB",
        int(seq),
        _coord_e7(latitude),
        _coord_e7(longitude),
        _alt_mm(altitude),
        float(battery),
        float(roll),
        float(pitch),
        float(yaw),
        int(drone_id),
    )


def unpack_telemetry_plaintext(b: bytes):
    seq, lat_e7, lon_e7, alt_mm, battery, roll, pitch, yaw, drone_id = struct.unpack(
        "<IiiiffffB", b
    )
    return (
        seq,
        lat_e7 / 10000000.0,
        lon_e7 / 10000000.0,
        alt_mm / 1000.0,
        battery,
        roll,
        pitch,
        yaw,
        drone_id,
    )
