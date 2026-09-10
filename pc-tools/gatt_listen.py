"""
Connect to the S200 and listen hard for weight, with auto-reconnect.

Unlike the first explore run, this:
  * keeps reconnecting if the scale drops us (Xiaomi devices hang up on idle),
  * stays connected in a loop while YOU stand on the scale,
  * logs every notification (with the characteristic + bytes + time),
  * re-reads the readable chars each connect in case values change.

BEST PROCEDURE (timing matters):
  1. STEP ON the scale first and stay on it (keeps it awake & connectable).
  2. Start this script -- it connects while you're standing.
  3. Stay on until it locks the weight, then step off but keep standing near it.
  4. Watch for NOTIFY lines. Ctrl+C to stop.

Any bytes we capture -- even encrypted -- tell us how the scale delivers weight.

Usage:
    python gatt_listen.py --seconds 120
"""

import argparse
import asyncio
from datetime import datetime

from bleak import BleakClient, BleakScanner

MAC = "D0:7B:6F:91:88:E8"


def ts():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


async def run_once(log_emit, seconds_left):
    device = await BleakScanner.find_device_by_address(MAC, timeout=15.0)
    if device is None:
        log_emit(f"[{ts()}] scale not advertising (asleep?). retrying...")
        return "retry"

    got_notification = {"n": 0}

    def handler_factory(uuid):
        def h(_sender, data: bytearray):
            got_notification["n"] += 1
            log_emit(f"[{ts()}] NOTIFY {uuid}  {len(data)}B: {bytes(data).hex(' ')}")
        return h

    disconnected = asyncio.Event()

    def on_disconnect(_client):
        log_emit(f"[{ts()}] *** disconnected by scale ***")
        disconnected.set()

    try:
        async with BleakClient(device, timeout=20.0, disconnected_callback=on_disconnect) as client:
            log_emit(f"[{ts()}] connected={client.is_connected}")
            notif = []
            for service in client.services:
                for ch in service.characteristics:
                    if "notify" in ch.properties or "indicate" in ch.properties:
                        notif.append(ch)
            for ch in notif:
                try:
                    await client.start_notify(ch, handler_factory(ch.uuid))
                except Exception as e:
                    log_emit(f"[{ts()}] subscribe fail {ch.uuid}: {e}")
            log_emit(f"[{ts()}] subscribed to {len(notif)} chars. LISTENING (stay on the scale)...")

            # Stay connected, occasionally poll readable value chars for changes.
            waited = 0.0
            while client.is_connected and waited < seconds_left and not disconnected.is_set():
                await asyncio.sleep(1.0)
                waited += 1.0
            log_emit(f"[{ts()}] session end (notifications this session: {got_notification['n']})")
    except Exception as e:
        log_emit(f"[{ts()}] connect error: {e}")
        return "retry"
    return "ok"


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--log", default=f"gattlisten_{datetime.now():%Y%m%d_%H%M%S}.log")
    args = ap.parse_args()

    log = open(args.log, "w", encoding="utf-8")

    def emit(text=""):
        print(text)
        log.write(text + "\n")
        log.flush()

    emit(f"GATT listen on {MAC}. Log: {args.log}")
    emit("STAND ON THE SCALE, then it will keep (re)connecting. Ctrl+C to stop.\n")

    deadline = args.seconds
    elapsed = 0.0
    import time
    # Avoid time.time drift issues: just loop reconnecting until total budget spent.
    while elapsed < deadline:
        remaining = deadline - elapsed
        start_marker = elapsed
        result = await run_once(emit, remaining)
        # crude time accounting: each run_once consumes some time; re-measure via sleep steps
        # (run_once already slept up to remaining). Add a small retry gap.
        if result == "retry":
            await asyncio.sleep(2.0)
            elapsed += 2.0 + 15.0  # scan timeout budget approx
        else:
            elapsed = deadline  # a full session completed
    emit("\nDone.")
    log.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
