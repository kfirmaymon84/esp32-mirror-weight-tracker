"""
Xiaomi S200 (ms113) GATT reader using the Mi Protocol (miauth).

Full protocol (all writes are Write Commands / response=False):

Pre-auth setup:
  0. Subscribe CCCDs: DATA_RX2, DATA_RX, DEV_INFO (cu(0x001c)), AVDTP — in that order.
  0b. Query DEV_INFO with bytes 00/01/08 01 00/03 (device info, required before auth).

Auth (on AVDTP cu(0x0019) + UPNP cu(0x0010)):
  1. Write 0xa4 to UPNP → MTU exchange on AVDTP.
  2. Subscribe UPNP CCCD, write CMD_LOGIN (24000000) to UPNP.
  3. Write CMD_SEND_KEY (0000000b0100) to AVDTP → RCV_RDY → send 16-byte random_key → RCV_OK.
  4. Receive remote_key (0000020d…) → RCV_ACK.
  5. Receive server_proof (multi-frame 0000000c…) → verify HMAC-SHA256(dev_key, salt_inv).
  6. Write CMD_SEND_INFO (0000000a0100) → RCV_RDY → send client_info → RCV_OK.

Post-auth data exchange (DATA_RX cu(0x001a) ↔ DATA_RX2 cu(0x001b)):
  Each "prop_exchange" (seq 0, 1, 2):
    → DATA_RX: header [00 00 00 00 01 00]
    ← DATA_RX: RCV_RDY [00 00 01 01]
    → DATA_RX: frame [01 00 seq_lo seq_hi] + AES-CCM(app_key/app_iv, seq, payload)
    ← DATA_RX: RCV_OK [00 00 01 00]
    ← DATA_RX2: reply (single-frame [00 00 02 00 dev_seq…ct] or multi-frame header + frames)
    → DATA_RX2: RCV_ACK [00 00 03 00] (single) or RCV_RDY/RCV_OK (multi)

  Exchange 0: PAYLOAD_CAP  (capability negotiation)
  Exchange 1: user profile JSON
  Exchange 2: PAYLOAD_PROP (subscribe weight stream)

Weight streaming (DATA_RX2 multi-frame, ~1 Hz):
  Each update: multi-frame header [00 00 00 00 n 00] → n frames → decrypt with dev_key/dev_iv.
  Type 0x19: intermediate weight, LE uint16 at offset 23, /100 → kg.
  Type 0x22: stable weight, CSV after 0xa0: id,user,weight_10g,impedance,ts; weight_10g/100 → kg.

Key derivation:
  salt        = random_key + remote_key
  derived     = HKDF-SHA256(mi_token, salt=salt, info="mible-login-info", length=64)
  dev_key/app_key/dev_iv/app_iv = derived[0:16/16:32/32:36/36:40]
  AES-CCM nonce = iv(4) + 0x00*4 + seq(4 LE),  tag_length=4

Requirements: SCALE_MAC, SCALE_TOKEN (24 hex chars = 12 bytes mi_token).
Optional:     MQTT_HOST, MQTT_USER, MQTT_PASS, SCALE_HEIGHT/AGE/SEX.

Usage:
  SCALE_MAC=D0:7B:6F:73:3A:84 SCALE_TOKEN=7d0d594b7fd5ef6950b78172 \\
      uv run --with bleak --with cryptography --with paho-mqtt python gatt_mi.py
"""

import asyncio
import hashlib
import hmac
import json
import os
import secrets
import struct
import sys
import time

from bleak import BleakClient, BleakScanner
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.hmac import HMAC
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

MAC = os.getenv("SCALE_MAC", "").lower()
TOKEN_HEX = os.getenv("SCALE_TOKEN", "")
MQTT_HOST = os.getenv("MQTT_HOST", "")
MQTT_USER = os.getenv("MQTT_USER", "")
MQTT_PASS = os.getenv("MQTT_PASS", "")
_MAC_ID = MAC.replace(":", "").lower()
MQTT_STATE_TOPIC     = f"homeassistant/sensor/xiaomi_s200_{_MAC_ID}/state"
MQTT_DISCOVERY_TOPIC = f"homeassistant/sensor/xiaomi_s200_{_MAC_ID}/config"
MQTT_TOPIC = MQTT_STATE_TOPIC  # kept for compatibility

SCALE_HEIGHT = int(os.getenv("SCALE_HEIGHT", "177"))  # cm
SCALE_AGE    = int(os.getenv("SCALE_AGE",    "33"))
SCALE_SEX    = int(os.getenv("SCALE_SEX",    "1"))    # 1=male, 0=female

if not MAC:
    print("Set SCALE_MAC env var (e.g. D0:7B:6F:73:3A:84)")
    sys.exit(1)
if not TOKEN_HEX:
    print("Set SCALE_TOKEN env var — it's the 'token' field from token_extractor output")
    print("  (24 hex chars, e.g. abcd594b7fd5ef6950b78172)")
    sys.exit(1)

MI_TOKEN = bytes.fromhex(TOKEN_HEX)
if len(MI_TOKEN) != 12:
    print(f"SCALE_TOKEN must be 12 bytes (24 hex chars), got {len(MI_TOKEN)}")
    sys.exit(1)

# ── UUIDs / handles ────────────────────────────────────────────────────────────
# These are resolved by UUID during connect, not hard-coded handles.
UUID_UPNP  = "00001013-0000-1000-8000-00805f9b34fb"  # actually 0x0010 UUID → handle 0x0013
UUID_AVDTP = "00001019-0000-1000-8000-00805f9b34fb"  # 0x0019 UUID → handle 0x0016

# Corrected short-form UUIDs expanded:
def cu(n: int) -> str:
    return f"{n:08x}-0000-1000-8000-00805f9b34fb"

UPNP     = cu(0x0010)  # handle 0x0013 — write CMD_LOGIN here
AVDTP    = cu(0x0019)  # handle 0x0016 — auth channel (write + notify)
KEY      = cu(0x0014)  # handle 0x0015 — subscribe here
DATA_RX  = cu(0x001a)  # handle 0x001e — send encrypted commands here
DATA_RX2 = cu(0x001b)  # handle 0x0021 — receive encrypted weight data here

# ── Protocol constants ─────────────────────────────────────────────────────────
CMD_LOGIN     = bytes.fromhex("24000000")
CMD_SEND_KEY  = bytes.fromhex("0000000b0100")
CMD_SEND_INFO = bytes.fromhex("0000000a0100")  # S200 uses 0100 not 0200
RCV_RDY       = bytes.fromhex("00000101")
RCV_OK        = bytes.fromhex("00000100")
RCV_ACK       = bytes.fromhex("00000300")      # S200-specific ack for data frames

# Exchange 1: capability negotiation
PAYLOAD_CAP  = bytes.fromhex("05200200f0")
# Exchange 3: property subscription that triggers weight streaming
PAYLOAD_PROP = bytes.fromhex("0c2004000001050100011005")

# ── Crypto helpers ─────────────────────────────────────────────────────────────

def derive_session_keys(mi_token: bytes, random_key: bytes, remote_key: bytes) -> dict:
    salt = random_key + remote_key
    derived = HKDF(
        algorithm=hashes.SHA256(),
        length=64,
        salt=salt,
        info=b"mible-login-info",
        backend=default_backend(),
    ).derive(mi_token)
    return {
        "dev_key": derived[0:16],
        "app_key": derived[16:32],
        "dev_iv":  derived[32:36],
        "app_iv":  derived[36:40],
        "salt":    salt,
        "salt_inv": remote_key + random_key,
    }


def hmac_sha256(key: bytes, data: bytes) -> bytes:
    h = HMAC(key, hashes.SHA256(), backend=default_backend())
    h.update(data)
    return h.finalize()


def _data_nonce(iv: bytes, seq: int) -> bytes:
    """12-byte AES-CCM nonce: iv(4) + 0x00*4 + seq(4 LE)"""
    return iv + b'\x00' * 4 + struct.pack("<I", seq)


def encrypt_data(key: bytes, iv: bytes, seq: int, plaintext: bytes) -> bytes:
    aesccm = AESCCM(key, tag_length=4)
    nonce = _data_nonce(iv, seq)
    return aesccm.encrypt(nonce, plaintext, None)


def decrypt_data(key: bytes, iv: bytes, seq: int, ciphertext: bytes) -> bytes:
    aesccm = AESCCM(key, tag_length=4)
    nonce = _data_nonce(iv, seq)
    return aesccm.decrypt(nonce, ciphertext, None)


async def send_parcel(client: BleakClient, data: bytes, chunk_size: int = 200):
    """Write data in numbered parcel frames: [frame_num, 0x00] + chunk.

    chunk_size=200 keeps random_key (16 B) and client_info (32 B) each in
    1 frame, matching the n_frames=1 declared in CMD_SEND_KEY / CMD_SEND_INFO.
    """
    for i, offset in enumerate(range(0, len(data), chunk_size)):
        chunk = data[offset:offset + chunk_size]
        await client.write_gatt_char(AVDTP, bytes([i + 1, 0x00]) + chunk)


async def receive_parcel(state: "AuthState", client: BleakClient, cmd_byte: int, timeout: float = 10.0) -> bytes:
    """Receive a data parcel, handling both single-frame and multi-frame variants.

    Single-frame: device sends [00 00 02 CMD data...], we reply RCV_ACK.
    Multi-frame:  device sends [00 00 00 CMD n_lo n_hi], we reply RCV_RDY,
                  device sends N frames [00 00 frm_lo frm_hi data...],
                  we reply RCV_OK.
    """
    single_prefix = bytes([0x00, 0x00, 0x02, cmd_byte])
    notif = await state.wait_for_parcel_header(AVDTP, cmd_byte, timeout=timeout)

    if notif.startswith(single_prefix):
        await client.write_gatt_char(AVDTP, RCV_ACK)
        return notif[4:]

    # Multi-frame header: [00 00 00 CMD n_lo n_hi]
    n_frames = notif[4] + notif[5] * 256
    print(f"[Auth]   multi-frame receive: {n_frames} frames")
    await client.write_gatt_char(AVDTP, RCV_RDY)

    deadline = asyncio.get_event_loop().time() + timeout
    q = state._queue_for(AVDTP)
    payload = b""
    for i in range(n_frames):
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            raise TimeoutError(f"Timeout on frame {i} for cmd={cmd_byte:02x}")
        try:
            frame = await asyncio.wait_for(q.get(), timeout=remaining)
        except asyncio.TimeoutError:
            raise TimeoutError(f"Timeout on frame {i} for cmd={cmd_byte:02x}")
        payload += frame[2:]  # [frm_lo frm_hi data...]

    await client.write_gatt_char(AVDTP, RCV_OK)
    return payload


# ── State ──────────────────────────────────────────────────────────────────────

class AuthState:
    def __init__(self):
        self.notifications: dict[str, list[bytes]] = {}
        self._queues: dict[str, asyncio.Queue] = {}

    def _queue_for(self, uuid: str) -> asyncio.Queue:
        key = uuid.lower()
        if key not in self._queues:
            self._queues[key] = asyncio.Queue()
        return self._queues[key]

    def handler_for(self, uuid: str):
        def handle(_, data: bytearray):
            ts = time.strftime("%H:%M:%S")
            print(f"[{ts}] NOTIFY {uuid[-4:]}: {bytes(data).hex()}")
            b = bytes(data)
            self.notifications.setdefault(uuid.lower(), []).append(b)
            self._queue_for(uuid).put_nowait(b)
        return handle

    async def wait_for(self, uuid: str, prefix: bytes | None = None, timeout: float = 10.0) -> bytes:
        """Wait until a notification on `uuid` starts with `prefix`.

        Always drains from the queue (not history) so notifications are consumed
        exactly once and don't leak into other waiters (e.g. pong_loop).
        """
        deadline = asyncio.get_event_loop().time() + timeout
        q = self._queue_for(uuid)
        while True:
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                raise TimeoutError(
                    f"Timeout waiting for notify on {uuid[-4:]} prefix={prefix and prefix.hex()}")
            try:
                n = await asyncio.wait_for(q.get(), timeout=remaining)
            except asyncio.TimeoutError:
                raise TimeoutError(
                    f"Timeout waiting for notify on {uuid[-4:]} prefix={prefix and prefix.hex()}")
            if prefix is None or n.startswith(prefix):
                return n

    async def wait_for_parcel_header(self, uuid: str, cmd_byte: int, timeout: float = 10.0) -> bytes:
        """Wait for either a single-frame (0x02) or multi-frame header (0x00) for cmd_byte."""
        single_prefix = bytes([0x00, 0x00, 0x02, cmd_byte])
        multi_prefix  = bytes([0x00, 0x00, 0x00, cmd_byte])
        deadline = asyncio.get_event_loop().time() + timeout
        q = self._queue_for(uuid)
        while True:
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                raise TimeoutError(f"Timeout waiting for parcel header cmd={cmd_byte:02x}")
            try:
                n = await asyncio.wait_for(q.get(), timeout=remaining)
            except asyncio.TimeoutError:
                raise TimeoutError(f"Timeout waiting for parcel header cmd={cmd_byte:02x}")
            if n.startswith(single_prefix) or n.startswith(multi_prefix):
                return n


# ── Main auth flow ─────────────────────────────────────────────────────────────

async def authenticate(client: BleakClient, mi_token: bytes) -> dict:
    state = AuthState()

    # Subscribe DATA_RX2 and DATA_RX before auth — required by device (confirmed from btsnoop).
    # Btsnoop order: DATA_RX2 → DATA_RX → DEV_INFO → AVDTP.
    DEV_INFO = cu(0x001c)  # device-info channel — Mi Home queries it before auth
    await client.start_notify(DATA_RX2, state.handler_for(DATA_RX2))
    await client.start_notify(DATA_RX, state.handler_for(DATA_RX))
    await client.start_notify(DEV_INFO, state.handler_for(DEV_INFO))

    # Mi Home sends a 4-command device-info query on cu(0x001c) before auth.
    # Responses are device identifiers (chip=nrf52840, serial, etc.) — we just
    # drain them; not processing the values, just matching Mi Home's exact flow.
    print("[Auth] Device-info init (cu(0x001c)) …")
    for cmd in [b'\x00', b'\x01', b'\x08\x01\x00', b'\x03']:
        try:
            await client.write_gatt_char(DEV_INFO, cmd, response=False)
            resp = await asyncio.wait_for(state._queue_for(DEV_INFO).get(), timeout=2.0)
            print(f"[Auth]   dev_info 0x{cmd.hex()} → {resp.hex()}")
        except Exception as e:
            print(f"[Auth]   dev_info 0x{cmd.hex()} error: {e}")

    await client.start_notify(AVDTP, state.handler_for(AVDTP))
    await asyncio.sleep(0.1)

    # Step 1: init — write 0xa4 to UPNP; scale responds with MTU info on AVDTP
    print("\n[Auth] Step 1: init + MTU exchange")
    await client.write_gatt_char(UPNP, bytes([0xa4]))

    try:
        mtu_notif = await state.wait_for(AVDTP, bytes.fromhex("00000400"), timeout=3.0)
        # MTU reply: 0000 04 00 XX YY — echo back with type 05
        mtu_ack = bytes([0x00, 0x00, 0x05, 0x00]) + mtu_notif[4:]
        await client.write_gatt_char(AVDTP, mtu_ack)
        # Scale sends a large test frame; we mirror it
        big_frame = await state.wait_for(AVDTP, bytes.fromhex("00000401"), timeout=3.0)
        mirror = bytes([0x00, 0x00, 0x05, 0x01]) + big_frame[4:]
        # Write in chunks if necessary (244 bytes may exceed default MTU)
        for i in range(0, len(mirror), 20):
            await client.write_gatt_char(AVDTP, mirror[i:i+20])
        print(f"[Auth]   MTU exchange done ({len(big_frame)} byte test frame)")
    except TimeoutError:
        print("[Auth]   (no MTU exchange — proceeding)")

    # Subscribe to UPNP notifications — required by the scale before CMD_LOGIN
    # (in btsnoop: Mi Home writes 0100 to UPNP's CCCD at this point)
    try:
        await client.start_notify(UPNP, state.handler_for(UPNP))
    except Exception as e:
        print(f"  (UPNP subscribe note: {e})")
    await asyncio.sleep(0.1)

    # Step 2: CMD_LOGIN
    print("[Auth] Step 2: CMD_LOGIN")
    await client.write_gatt_char(UPNP, CMD_LOGIN)

    # Step 3: CMD_SEND_KEY — tells device we're about to send our random key
    print("[Auth] Step 3: CMD_SEND_KEY")
    await client.write_gatt_char(AVDTP, CMD_SEND_KEY)

    # Wait for RCV_RDY
    await state.wait_for(AVDTP, RCV_RDY)
    print("[Auth]   → device is ready to receive key")

    # Step 4: send our 16-byte random key (single frame fits at default MTU)
    random_key = secrets.token_bytes(16)
    print(f"[Auth] Step 4: sending random_key={random_key.hex()}")
    await send_parcel(client, random_key)

    # Wait for RCV_OK
    await state.wait_for(AVDTP, RCV_OK)
    print("[Auth]   → key received by device")

    # Step 5: receive remote_key (device's 16-byte challenge)
    # Single-frame: [00 00 02 0d remote_key...]
    print("[Auth] Step 5: waiting for remote_key")
    notif = await state.wait_for(AVDTP, bytes.fromhex("0000020d"), timeout=10.0)
    remote_key = notif[4:]
    print(f"[Auth]   remote_key={remote_key.hex()}")
    await client.write_gatt_char(AVDTP, RCV_ACK)

    # Step 6: receive server proof (32 bytes — arrives as multi-frame at default MTU)
    # Multi-frame header: [00 00 00 0c n_lo n_hi], then N data frames
    print("[Auth] Step 6: waiting for server proof")
    server_proof = await receive_parcel(state, client, 0x0c, timeout=10.0)
    print(f"[Auth]   server_proof={server_proof.hex()}")

    # Step 7: derive session keys and verify server proof
    print("[Auth] Step 7: deriving session keys")
    keys = derive_session_keys(mi_token, random_key, remote_key)
    expected_proof = hmac_sha256(keys["dev_key"], keys["salt_inv"])
    if expected_proof == server_proof:
        print("[Auth]   ✓ server proof verified!")
    else:
        print(f"[Auth]   ✗ server proof mismatch!")
        print(f"         expected: {expected_proof.hex()}")
        print(f"         got:      {server_proof.hex()}")
        print("[Auth]   Proceeding anyway (mi_token may need adjustment)")

    # Step 8: send our proof to the device (32 bytes → chunked parcel)
    client_info = hmac_sha256(keys["app_key"], keys["salt"])
    print(f"[Auth] Step 8: CMD_SEND_INFO + client_info={client_info.hex()}")
    await client.write_gatt_char(AVDTP, CMD_SEND_INFO)

    await state.wait_for(AVDTP, RCV_RDY)
    await send_parcel(client, client_info)

    await state.wait_for(AVDTP, RCV_OK)
    print("[Auth]   ✓ AUTH COMPLETE!")

    return keys, state


# ── Post-auth data reading ─────────────────────────────────────────────────────

def short(uuid: str) -> str:
    return uuid[:8].lstrip("0") or "0000"


def _parse_data_rx2_frame(notif: bytes) -> tuple[int, bytes]:
    """Parse a DATA_RX2 single-frame notification.

    Format: [00 00 02 00 seq_lo seq_hi] + ciphertext
    Returns (seq, ciphertext).
    """
    return struct.unpack_from("<H", notif, 4)[0], notif[6:]


async def receive_data_rx2(
    client: BleakClient,
    data_rx2_q: asyncio.Queue,
    timeout: float = 10.0,
) -> tuple[int, bytes]:
    """Receive one logical DATA_RX2 message, handling single and multi-frame.

    Single-frame: [00 00 02 00 seq_lo seq_hi ct...]
    Multi-frame header: [00 00 00 00 n_lo n_hi]
      Frame 0: [frm_num(2)] [dev_seq(2)] [ct_part...]
      Frame N: [frm_num(2)] [ct_part...]

    Returns (dev_seq, ciphertext).  Sends the appropriate ACK(s) to DATA_RX2.
    """
    notif = await asyncio.wait_for(data_rx2_q.get(), timeout=timeout)

    if len(notif) >= 4 and notif[2] == 0x02:
        # Single-frame
        dev_seq = struct.unpack_from("<H", notif, 4)[0]
        ct = notif[6:]
        await client.write_gatt_char(DATA_RX2, RCV_ACK, response=False)
        return dev_seq, ct

    # Multi-frame header
    n_frames = struct.unpack_from("<H", notif, 4)[0] if len(notif) >= 6 else 0
    print(f"  [rx2 multi-frame n={n_frames}]")
    await client.write_gatt_char(DATA_RX2, RCV_RDY, response=False)
    dev_seq = 0
    ct = b""
    for i in range(n_frames):
        frame = await asyncio.wait_for(data_rx2_q.get(), timeout=5.0)
        if i == 0:
            dev_seq = struct.unpack_from("<H", frame, 2)[0]
            ct += frame[4:]   # skip [frm_num(2)] [dev_seq(2)]
        else:
            ct += frame[2:]   # skip [frm_num(2)]
    await client.write_gatt_char(DATA_RX2, RCV_OK, response=False)
    return dev_seq, ct


async def prop_exchange(
    client: BleakClient,
    keys: dict,
    state: "AuthState",
    data_rx2_q: asyncio.Queue,
    seq: int,
    payload: bytes,
    label: str,
) -> bytes:
    """Encrypt `payload`, send parcel on DATA_RX, receive+decrypt reply on DATA_RX2."""
    ct = encrypt_data(keys["app_key"], keys["app_iv"], seq, payload)
    print(f"[Data] {label} seq={seq} plain={payload.hex()} ct={ct.hex()}")

    # Protocol: header → wait RCV_RDY → frame → wait RCV_OK
    # All writes to DATA_RX/DATA_RX2 use Write Command (response=False) — confirmed from btsnoop.
    header = bytes([0x00, 0x00, 0x00, 0x00, 0x01, 0x00])
    await client.write_gatt_char(DATA_RX, header, response=False)

    await state.wait_for(DATA_RX, RCV_RDY, timeout=5.0)
    print(f"[Data]   → RCV_RDY for {label}")

    frame = bytes([0x01, 0x00]) + struct.pack("<H", seq) + ct
    await client.write_gatt_char(DATA_RX, frame, response=False)

    await state.wait_for(DATA_RX, RCV_OK, timeout=5.0)
    print(f"[Data]   → RCV_OK for {label}")

    dev_seq, reply_ct = await receive_data_rx2(client, data_rx2_q, timeout=10.0)
    if not reply_ct:
        print(f"[Data]   reply: empty (dev_seq={dev_seq})")
        return b""
    reply_pt = decrypt_data(keys["dev_key"], keys["dev_iv"], dev_seq, reply_ct)
    print(f"[Data]   reply dev_seq={dev_seq} plain={reply_pt.hex()}")
    return reply_pt


def _build_user_profile(seq: int, height: int, age: int, sex: int) -> bytes:
    """
    Exchange 2 payload: 10-byte header + 1-byte json_len + 0xa0 + JSON.
    Header observed in btsnoop: 8f 20 03 00 05 07 01 01 01 00
    """
    ts = int(time.time())
    profile = {
        "mid": "0",
        "duid": 1,
        "uc": 1,
        "ow": 1,
        "unit": 1,
        "time": ts,
        "ud": [{"duid": 1, "ut": 1, "age": age, "sex": sex, "hi": height, "wt": 600}],
    }
    js = json.dumps(profile, separators=(",", ":")).encode()
    header = bytes.fromhex("8f2003000507010101 00".replace(" ", ""))
    return header + bytes([len(js)]) + b"\xa0" + js


async def listen_for_weight(client: BleakClient, keys: dict, state: "AuthState", duration: float = 300.0):
    """Run the 3-exchange post-auth protocol, then stream weight data."""
    # DATA_RX and DATA_RX2 were subscribed in authenticate() before auth.
    data_rx2_q = state._queue_for(DATA_RX2)

    PING = bytes.fromhex("000001050100")
    PONG = bytes.fromhex("000005040100")

    async def pong_loop():
        """Respond to device keepalives on AVDTP."""
        while True:
            try:
                notif = await asyncio.wait_for(state._queue_for(AVDTP).get(), timeout=6.0)
                ts = time.strftime("%H:%M:%S")
                print(f"[{ts}] AVDTP {notif.hex()}")
                if notif == PING:
                    await client.write_gatt_char(AVDTP, PONG, response=False)
                    print(f"       → pong sent")
            except (asyncio.TimeoutError, TimeoutError):
                pass

    pong_task = asyncio.create_task(pong_loop())

    published_kg: float | None = None

    try:
        # btsnoop shows Mi Home reads firmware right before Exchange 1; do the same.
        t0 = time.monotonic()
        try:
            fw_raw = await client.read_gatt_char(cu(0x0004))
            fw = fw_raw.decode("ascii", errors="replace").rstrip("\x00")
            print(f"[Data] Firmware: {fw!r}  (time since auth: {time.monotonic()-t0:.3f}s)")
        except Exception as e:
            print(f"[Data] Firmware read failed: {e}")

        t1 = time.monotonic()
        print(f"\n[Data] Exchange 1: capability negotiation  (Δ={t1-t0:.3f}s since fw read)")
        await prop_exchange(client, keys, state, data_rx2_q, seq=0, payload=PAYLOAD_CAP, label="CAP")

        print("\n[Data] Exchange 2: user profile")
        user_payload = _build_user_profile(seq=1, height=SCALE_HEIGHT, age=SCALE_AGE, sex=SCALE_SEX)
        await prop_exchange(client, keys, state, data_rx2_q, seq=1, payload=user_payload, label="USER")

        print("\n[Data] Exchange 3: subscribe weight stream")
        await prop_exchange(client, keys, state, data_rx2_q, seq=2, payload=PAYLOAD_PROP, label="PROP")

        print(f"\n=== Exchanges done — stand on the scale ({duration:.0f}s) ===\n")

        # Now stream incoming DATA_RX2 frames looking for weight.
        # Each frame: [00 00 02 00 seq_lo seq_hi ct...]  ACK each with RCV_ACK → DATA_RX2.
        deadline = asyncio.get_event_loop().time() + duration
        while asyncio.get_event_loop().time() < deadline:
            remaining = deadline - asyncio.get_event_loop().time()
            try:
                dev_seq, frame_ct = await receive_data_rx2(
                    client, data_rx2_q, timeout=min(remaining, 5.0))
            except (asyncio.TimeoutError, TimeoutError):
                continue

            if not frame_ct:
                continue
            try:
                pt = decrypt_data(keys["dev_key"], keys["dev_iv"], dev_seq, frame_ct)
            except Exception as e:
                print(f"  [decrypt err dev_seq={dev_seq}] {e}  ct={frame_ct.hex()}")
                continue

            ts = time.strftime("%H:%M:%S")
            print(f"[{ts}] WEIGHT_FRAME dev_seq={dev_seq} plain={pt.hex()}")

            frame_type = pt[0] if pt else 0xff

            if frame_type == 0x19 and len(pt) >= 25:
                # Intermediate streaming frame: LE uint16 at offset 23 / 100.0
                kg = struct.unpack_from("<H", pt, 23)[0] / 100.0
                print(f"  → intermediate weight: {kg:.2f} kg")

            elif frame_type in (0x22, 0x2b) and len(pt) >= 14:
                # Final stable measurement: CSV after 0xa0 marker
                # format: 2b 20 0e 00 07 05 04 00 01 04 00 [len] a0 [CSV]
                try:
                    a0_pos = pt.index(0xa0)
                    csv_bytes = pt[a0_pos + 1:]
                    csv = csv_bytes.decode("ascii", errors="replace")
                    parts = csv.split(",")
                    kg = int(parts[2]) / 100.0
                    print(f"  → STABLE WEIGHT: {kg:.2f} kg  (CSV: {csv})")
                    # Publish first reading, then any re-weigh that differs by >0.5 kg
                    if published_kg is None or abs(kg - published_kg) > 0.5:
                        publish_weight(kg)
                        published_kg = kg
                except Exception as e:
                    print(f"  [parse err] {e}  plain={pt.hex()}")

    except Exception as e:
        print(f"[Data] Error in exchanges: {e}")
        import traceback; traceback.print_exc()
    finally:
        pong_task.cancel()
        try:
            await pong_task
        except asyncio.CancelledError:
            pass

    return published_kg is not None


# ── MQTT publish ───────────────────────────────────────────────────────────────

def _mqtt_single(topic: str, payload: str, retain: bool = False):
    """Publish one message and block until delivered, then disconnect."""
    import paho.mqtt.publish as mqtt_pub
    # Use `or None` so an unset MQTT_PASS env var (empty string) is treated as
    # absent rather than as an empty-string password, which some brokers reject.
    auth = {"username": MQTT_USER, "password": MQTT_PASS or None} if MQTT_USER else None
    print(f"  MQTT connect → {MQTT_HOST}:1883  user={MQTT_USER!r}  pass={'***' if MQTT_PASS else '(none)'}")
    mqtt_pub.single(topic, payload, hostname=MQTT_HOST, retain=retain, auth=auth)


def publish_discovery():
    if not MQTT_HOST:
        return
    try:
        discovery = {
            "unique_id": f"xiaomi_s200_{_MAC_ID}",
            "name": "Weight",
            "state_topic": MQTT_STATE_TOPIC,
            "value_template": "{{ value_json.mass_kg }}",
            "unit_of_measurement": "kg",
            "device_class": "weight",
            "state_class": "measurement",
            "device": {
                "identifiers": [f"xiaomi_s200_{_MAC_ID}"],
                "name": "Xiaomi S200",
                "manufacturer": "Xiaomi",
                "model": "Mi Smart Scale S200",
                "connections": [["mac", MAC.upper()]],
            },
        }
        _mqtt_single(MQTT_DISCOVERY_TOPIC, json.dumps(discovery), retain=True)
        print(f"  MQTT discovery → {MQTT_DISCOVERY_TOPIC}")
    except Exception as e:
        print(f"  MQTT discovery failed: {e}")


def publish_weight(kg: float):
    if not MQTT_HOST:
        return
    try:
        payload = json.dumps({"mass_kg": round(kg, 2)})
        _mqtt_single(MQTT_STATE_TOPIC, payload)
        print(f"  MQTT → {payload}")
    except Exception as e:
        print(f"  MQTT publish failed: {e}")


# ── Entry point ────────────────────────────────────────────────────────────────

async def _wait_for_advertisement() -> "BLEDevice":
    """Passively listen for BLE ads until the scale is seen. No radio sent to scale."""
    print(f"Passive scan — waiting for {MAC.upper()} to advertise …")
    seen = asyncio.Event()
    found: list = [None]

    def _cb(device, _adv):
        if device.address.upper() == MAC.upper():
            found[0] = device
            seen.set()

    async with BleakScanner(detection_callback=_cb, scanning_mode="active"):
        await seen.wait()

    return found[0]


async def _wait_for_scale_to_sleep(silence_sec: float = 30.0):
    """Passively scan until the scale has been silent for silence_sec seconds.

    Called after a session ends so we don't immediately re-trigger on the
    scale's post-step-off advertising window.
    """
    last_seen = asyncio.get_event_loop().time()

    def _cb(device, _adv):
        nonlocal last_seen
        if device.address.upper() == MAC.upper():
            last_seen = asyncio.get_event_loop().time()

    print("Waiting for scale to go silent (asleep) …")
    async with BleakScanner(detection_callback=_cb, scanning_mode="active"):
        while True:
            await asyncio.sleep(5)
            gap = asyncio.get_event_loop().time() - last_seen
            if gap >= silence_sec:
                print(f"  Scale silent for {gap:.0f}s — asleep. Ready for next session.")
                break


async def run():
    publish_discovery()

    while True:
        try:
            device = await _wait_for_advertisement()
            print(f"Scale advertising: {device.name!r}. Connecting …")

            async with BleakClient(device, timeout=30.0) as client:
                mtu = getattr(client, "mtu_size", "unknown")
                print(f"Connected. (MTU={mtu})")
                keys, state = await authenticate(client, MI_TOKEN)
                # Stay connected for up to 5 min so re-weighing within the same
                # session (e.g. 90 s later) is captured without reconnecting.
                await listen_for_weight(client, keys, state)

        except Exception as e:
            print(f"[run] Error: {e}")
            import traceback; traceback.print_exc()

        # Don't re-scan until the scale is truly asleep, so we don't
        # immediately reconnect just because it's still advertising post-step-off.
        await _wait_for_scale_to_sleep(silence_sec=30)


if __name__ == "__main__":
    asyncio.run(run())
