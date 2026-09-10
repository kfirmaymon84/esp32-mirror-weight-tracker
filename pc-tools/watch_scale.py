"""
Watch ONLY the S200 and decode/decrypt any MiBeacon it broadcasts.

We have the bindkey now, so if the scale ever broadcasts an encrypted MiBeacon
measurement object, this decrypts it live. It also logs EVERY advertisement
from the scale (all manufacturer + service data), so if a fresh measurement
triggers a different/one-shot frame, we catch it.

HOW TO GET A CLEAN TEST:
    Do a FRESH, COLD measurement while this runs --
      1. Start this script.
      2. With the scale asleep, step on it so it powers up, weighs you, and
         finalizes the reading. Stay on until the number locks.
      3. Step OFF (the "final" reading is often broadcast at step-off).
      4. Ctrl+C.

If the only thing we ever see is the 12-byte idle beacon
(30 58 04 4c 00 ..MAC.. 08) with object=0, then the S200 does NOT broadcast
weight and we must read it over an authenticated GATT connection instead.

Usage:
    python watch_scale.py                 # run forever
    python watch_scale.py --seconds 90
"""

import argparse
import asyncio
from datetime import datetime

from bleak import BleakScanner

try:
    from Crypto.Cipher import AES  # pycryptodome
except ImportError:
    AES = None

MAC = "D0:7B:6F:91:88:E8"
# Your MiBeacon bindkey (16 bytes hex) from the token extractor — paste yours here.
BINDKEY = bytes.fromhex("00000000000000000000000000000000")
IDLE = bytes.fromhex("3058044c00e888916f7bd008")  # the known static idle beacon


def parse_frctrl(v: bytes):
    frctrl = v[0] + (v[1] << 8)
    return {
        "raw": frctrl,
        "version": frctrl >> 12,
        "encrypted": (frctrl >> 3) & 1,
        "mac_include": (frctrl >> 4) & 1,
        "capability_include": (frctrl >> 5) & 1,
        "object_include": (frctrl >> 6) & 1,
    }


def decrypt_v4_v5(value: bytes, mac_bytes: bytes, key: bytes, obj_start: int):
    """value = raw fe95 service-data value (starts at frctrl). Returns plaintext object bytes."""
    if AES is None:
        return None, "pycryptodome not installed"
    # Emulate ble_monitor indexing: it works on AD with a 4-byte prefix before frctrl.
    data = b"\x00\x00\x00\x00" + value
    i = 4 + obj_start
    try:
        nonce = b"".join([mac_bytes[::-1], data[6:9], data[-7:-4]])
        aad = b"\x11"
        mic = data[-4:]
        cipherpayload = data[i:-7]
        cipher = AES.new(key, AES.MODE_CCM, nonce=nonce, mac_len=4)
        cipher.update(aad)
        return cipher.decrypt_and_verify(cipherpayload, mic), None
    except Exception as e:
        return None, str(e)


def parse_objects(payload: bytes):
    """MiBeacon objects are TLV: [id: 2B LE][len: 1B][data]. Return list of (id, data)."""
    out, idx = [], 0
    while idx + 3 <= len(payload):
        oid = payload[idx] + (payload[idx + 1] << 8)
        ln = payload[idx + 2]
        data = payload[idx + 3: idx + 3 + ln]
        out.append((oid, data))
        idx += 3 + ln
    return out


def ts():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--log", default=f"watch_{datetime.now():%Y%m%d_%H%M%S}.log")
    args = ap.parse_args()

    log = open(args.log, "w", encoding="utf-8")
    mac_bytes = bytes(int(x, 16) for x in MAC.split(":"))
    seen_non_idle = 0
    frame_count = 0

    def emit(text=""):
        print(text)
        log.write(text + "\n")
        log.flush()

    def callback(device, adv):
        nonlocal seen_non_idle, frame_count
        if device.address.upper() != MAC:
            return
        frame_count += 1
        fe95 = None
        for uuid, data in adv.service_data.items():
            if uuid.lower().startswith("0000fe95"):
                fe95 = bytes(data)

        # Log any manufacturer data too (in case a measurement uses it).
        if adv.manufacturer_data:
            for cid, d in adv.manufacturer_data.items():
                emit(f"[{ts()}] MFR 0x{cid:04X}: {bytes(d).hex(' ')}")

        if fe95 is None:
            return

        if fe95 == IDLE:
            # The boring idle beacon; count silently, don't spam.
            if frame_count % 25 == 1:
                emit(f"[{ts()}] (idle beacon x{frame_count})  {fe95.hex(' ')}")
            return

        # Something different! Decode it fully.
        seen_non_idle += 1
        fc = parse_frctrl(fe95)
        emit(f"\n[{ts()}] *** NON-IDLE fe95 FRAME #{seen_non_idle} ***")
        emit(f"   raw: {fe95.hex(' ')}")
        emit(f"   frctrl=0x{fc['raw']:04x} version={fc['version']} "
             f"encrypted={fc['encrypted']} mac_inc={fc['mac_include']} "
             f"cap_inc={fc['capability_include']} obj_inc={fc['object_include']}")

        # Compute where the object/ciphertext starts within the value.
        obj_start = 5  # frctrl(2)+prodid(2)+counter(1)
        if fc["mac_include"]:
            obj_start += 6
        if fc["capability_include"]:
            obj_start += 1

        if fc["object_include"] and fc["encrypted"]:
            plain, err = decrypt_v4_v5(fe95, mac_bytes, BINDKEY, obj_start)
            if plain is None:
                emit(f"   decrypt failed: {err}")
            else:
                emit(f"   decrypted: {plain.hex(' ')}")
                for oid, d in parse_objects(plain):
                    emit(f"     object 0x{oid:04x} ({len(d)}B): {d.hex(' ')}")
        elif fc["object_include"] and not fc["encrypted"]:
            emit("   (unencrypted object)")
            for oid, d in parse_objects(fe95[obj_start:]):
                emit(f"     object 0x{oid:04x} ({len(d)}B): {d.hex(' ')}")

    emit(f"Watching {MAC}. Logging to {args.log}")
    emit("Do a FRESH cold weigh-in now (step on, wait, step off). Ctrl+C to stop.\n")

    scanner = BleakScanner(detection_callback=callback)
    await scanner.start()
    try:
        if args.seconds > 0:
            await asyncio.sleep(args.seconds)
        else:
            while True:
                await asyncio.sleep(3600)
    finally:
        await scanner.stop()
        emit(f"\nDone. total frames from scale={frame_count}, non-idle frames={seen_non_idle}")
        log.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
