"""
Generic BLE advertisement scanner.

Step 1 of the Xiaomi Body Scale S200 project: find the scale and see its raw
advertisement payload. Run this, then step on the scale so it wakes up and
starts broadcasting. Watch for a device whose data changes as you weigh.

Usage:
    python scan_all.py                # scan forever, print every device
    python scan_all.py --seconds 30   # scan for 30s then stop
    python scan_all.py --filter mi    # only show devices whose name/addr contains "mi"
"""

import argparse
import asyncio
from datetime import datetime

from bleak import BleakScanner


def hexbytes(b: bytes) -> str:
    return b.hex(" ")


def describe(device, adv) -> str:
    lines = []
    ts = datetime.now().strftime("%H:%M:%S")
    name = adv.local_name or device.name or "(no name)"
    lines.append(f"[{ts}] {device.address}  RSSI={adv.rssi:>4} dBm  {name!r}")

    if adv.manufacturer_data:
        for company_id, data in adv.manufacturer_data.items():
            lines.append(
                f"           mfr  0x{company_id:04X} ({len(data):>2}B): {hexbytes(data)}"
            )
    if adv.service_data:
        for uuid, data in adv.service_data.items():
            short = uuid.split("-")[0][-4:]
            lines.append(
                f"           svc  {short} ({len(data):>2}B): {hexbytes(data)}"
            )
    if adv.service_uuids:
        lines.append(f"           uuids: {', '.join(adv.service_uuids)}")
    return "\n".join(lines)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=0, help="0 = run forever")
    ap.add_argument("--filter", type=str, default="", help="substring match on name/address")
    args = ap.parse_args()

    filt = args.filter.lower()

    def callback(device, adv):
        name = (adv.local_name or device.name or "").lower()
        if filt and filt not in name and filt not in device.address.lower():
            return
        print(describe(device, adv))
        print()

    print("Scanning for BLE advertisements...  (Ctrl+C to stop)")
    print("Tip: step on the scale now and watch which device's data changes.\n")

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


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
