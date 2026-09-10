"""
Xiaomi-only BLE scanner — cuts the flood down to just the scale.

There are dozens of BLE devices beaconing around you. This filters to ONLY
advertisements that carry a Xiaomi signature, so the Body Scale S200 is the
(almost) only thing you'll see:

  * service data under UUID 0xFE95  (classic Xiaomi "MiBeacon")
  * service data under UUID 0x181B / 0x181D (BLE Body Composition / Weight)
  * manufacturer data with a Xiaomi company id (0x038F, 0x0157, ...)

If NOTHING shows up while you're on the scale, the S200 is using some other
signature -- run with --all to dump everything that has service data and we'll
hunt manually. Use --uuid XXXX to force-match a specific 16-bit service UUID.

Usage:
    python xiaomi_scan.py                 # Xiaomi filter, run forever
    python xiaomi_scan.py --seconds 60     # stop after 60s
    python xiaomi_scan.py --uuid 181b      # also match this 16-bit svc uuid
    python xiaomi_scan.py --all            # show every adv that has service data
"""

import argparse
import asyncio
from datetime import datetime

from bleak import BleakScanner

XIAOMI_COMPANY_IDS = {0x038F, 0x0157}          # Xiaomi Inc., Anhui Huami
XIAOMI_SERVICE_UUIDS = {"fe95", "181b", "181d"}  # MiBeacon, Body Composition, Weight
BASE_TAIL = "-0000-1000-8000-00805f9b34fb"


def short_uuid(uuid: str) -> str:
    u = uuid.lower()
    if u.endswith(BASE_TAIL) and u.startswith("0000"):
        return u[4:8]
    return uuid


def hexbytes(b: bytes) -> str:
    return b.hex(" ")


def matches(adv, extra_uuids, show_all) -> bool:
    svc_shorts = {short_uuid(u) for u in adv.service_data}
    if show_all:
        return bool(adv.service_data)
    if svc_shorts & (XIAOMI_SERVICE_UUIDS | extra_uuids):
        return True
    if set(adv.manufacturer_data) & XIAOMI_COMPANY_IDS:
        return True
    return False


def describe(device, adv) -> str:
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    name = adv.local_name or device.name or "(no name)"
    lines = [f"[{ts}] {device.address}  RSSI={adv.rssi:>4}  {name!r}"]
    for cid, data in adv.manufacturer_data.items():
        lines.append(f"        mfr 0x{cid:04X} ({len(data):>2}B): {hexbytes(data)}")
    for uuid, data in adv.service_data.items():
        lines.append(f"        svc {short_uuid(uuid)} ({len(data):>2}B): {hexbytes(data)}")
    return "\n".join(lines)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--uuid", type=str, default="", help="extra 16-bit svc uuid to match, e.g. 181b")
    ap.add_argument("--all", action="store_true", help="show every adv that has service data")
    ap.add_argument("--log", type=str, default="")
    args = ap.parse_args()

    extra = {args.uuid.lower()} if args.uuid else set()
    log_path = args.log or f"xiaomi_{datetime.now():%Y%m%d_%H%M%S}.log"
    log = open(log_path, "w", encoding="utf-8")
    hits = {}  # address -> count

    def callback(device, adv):
        if not matches(adv, extra, args.all):
            return
        hits[device.address] = hits.get(device.address, 0) + 1
        text = describe(device, adv)
        print(text + "\n")
        log.write(text + "\n\n")
        log.flush()

    mode = "ALL service-data" if args.all else "Xiaomi"
    print(f"Scanning ({mode} filter). Logging to {log_path}")
    print("STEP ON THE SCALE now. Ctrl+C to stop.\n")

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
        print("\n--- summary (address: adv count) ---")
        for addr, n in sorted(hits.items(), key=lambda kv: -kv[1]):
            print(f"  {addr}: {n}")
        print(f"\nSaved to {log_path}")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
