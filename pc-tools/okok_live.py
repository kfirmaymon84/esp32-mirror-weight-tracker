"""
Live weight display for the OKOK / Chipsea broadcast scale (model P-01).

Standalone test tool — proves the scale works before we touch any firmware.
It passively listens for the scale's BLE advertisement (NO pairing, NO app, NO
connection) and shows the weight in a big window: cyan while measuring, GREEN
when the reading locks (stable).

Advertisement format (15 bytes, manufacturer data incl. the 2 company-id bytes):
    offset 0    1    2..3       4..5    6  7   8      9..14
           C0  ctr  impedance  weight  0A 11 flag    MAC
    weight = bytes[4:6] big-endian / 100  -> kg
    flag   = 0x24 measuring, 0x25 = stabilized/final

Run:  python okok_live.py        (then step on the scale)
No arguments needed — it auto-detects the scale by its packet signature.
"""

import asyncio
import queue
import threading
import time
import tkinter as tk

from bleak import BleakScanner

updates = queue.Queue()      # BLE thread -> GUI thread


def parse_okok(cid: int, data: bytes):
    """Return (weight_kg, stable, mac_str, raw_hex) or None if not our scale.

    Layout (15 bytes): C0 ctr W_hi W_lo M_hi M_lo 0A 11 flag MAC(6)
      [2:4] = weight, uint16 BE, 0.01 kg   (live value; climbs then locks)
      [4:6] = 0x0000 while measuring, 0x1770 marker once locked
      [8]   = flag: 0x24 measuring, 0x25 = stabilized/final
    After the lock frame the scale reuses [2:4] for impedance, so we only trust
    [2:4] as weight when it's a weight frame: stable (0x25) or marker still 0.
    """
    # bleak splits the first 2 advertisement bytes off as company_id (LE).
    full = bytes([cid & 0xFF, (cid >> 8) & 0xFF]) + data
    if len(full) != 15:
        return None
    if full[0] != 0xC0 or full[6] != 0x0A or full[7] != 0x11:
        return None
    stable = full[8] == 0x25
    marker = int.from_bytes(full[4:6], "big")
    if not stable and marker != 0:
        return None   # post-lock impedance frame — not a weight reading
    weight = int.from_bytes(full[2:4], "big") / 100.0
    mac = ":".join(f"{b:02X}" for b in full[9:15])
    return weight, stable, mac, full.hex(" ")


def ble_callback(device, adv):
    for cid, data in adv.manufacturer_data.items():
        parsed = parse_okok(cid, data)
        if parsed:
            weight, stable, mac, raw = parsed
            updates.put({
                "weight": weight, "stable": stable, "mac": mac,
                "raw": raw, "rssi": adv.rssi, "t": time.time(),
            })


def ble_thread():
    async def run():
        scanner = BleakScanner(detection_callback=ble_callback)
        await scanner.start()
        await asyncio.Event().wait()   # run until the process exits
    asyncio.run(run())


class App:
    IDLE_MS = 3000   # no packet for this long -> "step on the scale"

    def __init__(self, root):
        self.root = root
        root.title("OKOK Scale - Live")
        root.configure(bg="#0b0b0f")
        root.geometry("560x380")

        self.last_stable_kg = None
        self.last_t = 0

        self.status = tk.Label(root, text="Scanning...", font=("Segoe UI", 20),
                               fg="#9aa0a6", bg="#0b0b0f")
        self.status.pack(pady=(28, 0))

        self.weight = tk.Label(root, text="--.-", font=("Segoe UI", 110, "bold"),
                               fg="#5a6066", bg="#0b0b0f")
        self.weight.pack(pady=(4, 0))

        self.unit = tk.Label(root, text="kg", font=("Segoe UI", 28),
                             fg="#9aa0a6", bg="#0b0b0f")
        self.unit.pack()

        self.last = tk.Label(root, text="", font=("Segoe UI", 16),
                             fg="#53d769", bg="#0b0b0f")
        self.last.pack(pady=(14, 0))

        self.info = tk.Label(root, text="step on the scale...",
                             font=("Consolas", 11), fg="#5a6066", bg="#0b0b0f")
        self.info.pack(side="bottom", pady=12)

        self.poll()

    def poll(self):
        latest = None
        try:
            while True:
                latest = updates.get_nowait()
        except queue.Empty:
            pass

        if latest:
            self.last_t = latest["t"]
            kg = latest["weight"]
            self.weight.config(text=f"{kg:.1f}")
            if latest["stable"]:
                self.weight.config(fg="#53d769")      # green = locked
                self.status.config(text="STABLE", fg="#53d769")
                if kg > 0:
                    self.last_stable_kg = kg
                    self.last.config(text=f"✓ last stable: {kg:.1f} kg")
            else:
                self.weight.config(fg="#39c5ff")      # cyan = measuring
                self.status.config(text="Measuring...", fg="#39c5ff")
            self.info.config(
                text=f"MAC {latest['mac']}   RSSI {latest['rssi']} dBm")

        # Idle fade when no recent packet (scale asleep / nobody on it).
        if self.last_t and (time.time() - self.last_t) * 1000 > self.IDLE_MS:
            self.weight.config(fg="#5a6066")
            self.status.config(text="Step on the scale", fg="#9aa0a6")

        self.root.after(80, self.poll)


def main():
    threading.Thread(target=ble_thread, daemon=True).start()
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
