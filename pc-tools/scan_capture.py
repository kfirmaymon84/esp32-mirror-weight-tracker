"""
Change-aware BLE capture — built to make the Xiaomi Body Scale S200 pop out.

Most nearby devices beacon the same bytes over and over (phones, Fast Pair,
etc). A scale is different: when you step on it, it either appears fresh or its
payload changes every reading as the weight settles. This tool prints ONLY when
a device is new or its advertised data changed, so the scale is easy to spot.

Everything (including unchanged repeats) is written to a timestamped log file
so we can analyze the exact byte deltas afterward.

Workflow:
    1. Run:  python scan_capture.py
    2. Wait ~5s for the background to settle (it prints known devices once).
    3. Step on the scale. Watch for a device that appears / keeps changing.
    4. Note its address, step off, Ctrl+C to stop.

Options:
    --seconds N     stop automatically after N seconds (default: run forever)
    --log PATH      log file path (default: capture_<timestamp>.log)
"""

import argparse
import asyncio
from datetime import datetime

from bleak import BleakScanner


def hexbytes(b: bytes) -> str:
    return b.hex(" ")


def short_uuid(uuid: str) -> str:
    """Extract the 16-bit part of a standard BLE UUID.

    Standard UUIDs look like '0000fe95-0000-1000-8000-00805f9b34fb'; the
    meaningful 16-bit id is 'fe95' (chars 4-8), NOT the trailing base-UUID
    bytes. Non-standard 128-bit UUIDs are returned in full.
    """
    base_tail = "-0000-1000-8000-00805f9b34fb"
    if uuid.lower().endswith(base_tail) and uuid.startswith("0000"):
        return uuid[4:8]
    return uuid


def payload_signature(adv) -> str:
    """A stable string of everything interesting in an advertisement."""
    parts = []
    for cid, data in sorted(adv.manufacturer_data.items()):
        parts.append(f"m{cid:04x}={data.hex()}")
    for uuid, data in sorted(adv.service_data.items()):
        parts.append(f"s{short_uuid(uuid)}={data.hex()}")
    return "|".join(parts)


def describe(device, adv, tag) -> str:
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    name = adv.local_name or device.name or "(no name)"
    lines = [f"[{ts}] {tag:7} {device.address}  RSSI={adv.rssi:>4}  {name!r}"]
    for cid, data in adv.manufacturer_data.items():
        lines.append(f"                    mfr 0x{cid:04X} ({len(data):>2}B): {hexbytes(data)}")
    for uuid, data in adv.service_data.items():
        lines.append(f"                    svc {short_uuid(uuid)} ({len(data):>2}B): {hexbytes(data)}")
    return "\n".join(lines)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--log", type=str, default="")
    args = ap.parse_args()

    log_path = args.log or f"capture_{datetime.now():%Y%m%d_%H%M%S}.log"
    log = open(log_path, "w", encoding="utf-8")

    last_sig = {}   # address -> last payload signature we saw

    def emit(text):
        print(text)
        log.write(text + "\n")
        log.flush()

    def callback(device, adv):
        sig = payload_signature(adv)
        addr = device.address
        prev = last_sig.get(addr)

        # Always log the raw line to file (full history for later analysis).
        raw = describe(device, adv, "raw")
        log.write(raw + "\n")

        if prev is None:
            last_sig[addr] = sig
            emit(describe(device, adv, "NEW"))
            emit("")
        elif sig != prev:
            last_sig[addr] = sig
            emit(describe(device, adv, "CHANGED"))
            emit("")
        log.flush()

    emit(f"Change-aware capture. Logging to: {log_path}")
    emit("Let the background settle (~5s), then STEP ON THE SCALE.")
    emit("Watch for NEW / CHANGED devices.  Ctrl+C to stop.\n")

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
        log.close()
        print(f"\nSaved full log to {log_path}")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
