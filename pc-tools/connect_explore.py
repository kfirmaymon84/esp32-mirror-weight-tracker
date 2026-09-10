"""
GATT explorer for the Xiaomi Scale S200 (yunmai.scales.ms106).

The S200 does NOT broadcast weight -- it delivers it over a GATT connection.
This script connects to the scale, dumps every service + characteristic, then
subscribes to ALL notify/indicate characteristics and logs whatever they push.

Do a full weigh-in WHILE this is connected:
    1. Start the script (it connects).
    2. When it says "listening", step on the scale and stay until it locks.
    3. Step off. Watch for notification lines -- that's the weight stream.
    4. Ctrl+C to stop.

Notes / gotchas:
  * The scale must be awake & connectable. If connect fails, tap it / step on
    it to wake it, then rerun immediately.
  * If it's actively connected to your phone's Mi/Xiaomi app, it may refuse a
    2nd connection -- close the app or put the phone out of range first.
  * Standard weight char is 0x2A9D (Weight Measurement) in service 0x181D;
    body-composition is 0x2A9C / 0x181B. Yunmai also uses proprietary UUIDs
    (often the 16-bit 0xFFE0/0xFFE1 or a 0xXXXX under a custom base). We
    subscribe to everything, so we don't have to guess.

Usage:
    python connect_explore.py                       # uses the known S200 address
    python connect_explore.py --address AA:BB:...    # override
    python connect_explore.py --seconds 120          # auto-stop
"""

import argparse
import asyncio
from datetime import datetime

from bleak import BleakClient, BleakScanner

DEFAULT_ADDRESS = "D0:7B:6F:91:88:E8"


def ts() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--address", default=DEFAULT_ADDRESS)
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--log", default="")
    args = ap.parse_args()

    log_path = args.log or f"gatt_{datetime.now():%Y%m%d_%H%M%S}.log"
    log = open(log_path, "w", encoding="utf-8")

    def emit(text=""):
        print(text)
        log.write(text + "\n")
        log.flush()

    emit(f"Looking for {args.address} ... (make sure the scale is awake)")
    # Find the device object first (more reliable than connecting by bare address).
    device = await BleakScanner.find_device_by_address(args.address, timeout=20.0)
    if device is None:
        emit("Could not find the scale advertising. Wake it (step on it) and rerun.")
        log.close()
        return

    emit(f"Found {device.address} {device.name!r}. Connecting...")

    def make_handler(char_uuid, desc):
        def handler(_sender, data: bytearray):
            emit(f"[{ts()}] NOTIFY {char_uuid} ({desc}) {len(data)}B: {data.hex(' ')}")
        return handler

    try:
        async with BleakClient(device, timeout=30.0) as client:
            emit(f"Connected: {client.is_connected}\n")
            emit("=== GATT table ===")
            notif_chars = []
            for service in client.services:
                emit(f"[service] {service.uuid}  {service.description}")
                for ch in service.characteristics:
                    props = ",".join(ch.properties)
                    emit(f"    [char] {ch.uuid}  ({props})  {ch.description}")
                    # Try a one-shot read for readable chars.
                    if "read" in ch.properties:
                        try:
                            val = await client.read_gatt_char(ch)
                            emit(f"           read = {val.hex(' ')}")
                        except Exception as e:
                            emit(f"           read failed: {e}")
                    if "notify" in ch.properties or "indicate" in ch.properties:
                        notif_chars.append(ch)

            emit("\n=== subscribing to notify/indicate characteristics ===")
            for ch in notif_chars:
                try:
                    await client.start_notify(ch, make_handler(ch.uuid, ch.description))
                    emit(f"  subscribed: {ch.uuid}")
                except Exception as e:
                    emit(f"  subscribe failed {ch.uuid}: {e}")

            emit("\n>>> LISTENING. Step on the scale now. Ctrl+C to stop. <<<\n")
            if args.seconds > 0:
                await asyncio.sleep(args.seconds)
            else:
                while client.is_connected:
                    await asyncio.sleep(1)
            emit("\nDisconnected or stopped.")
    except Exception as e:
        emit(f"Connection error: {e}")
    finally:
        log.close()
        print(f"\nSaved to {log_path}")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
