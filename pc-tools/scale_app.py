"""
Xiaomi S200 live weight display.

A simple desktop window that shows your weight in real time while you stand on
the scale. It runs the working `gatt_mi.py` reader as a child process and reads
its output live -- so the proven BLE/auth code is untouched; this is just the
face on top of it.

    Big number  = current reading (live while you're on the scale)
    Blue        = measuring (weight still settling)
    Green       = stable / locked reading
    Grey        = waiting for the scale / between weigh-ins

Just run it:
    python scale_app.py
(Optionally set SCALE_MAC / SCALE_TOKEN env vars to override the built-ins.)
"""

import os
import queue
import re
import subprocess
import sys
import threading
import tkinter as tk

# Defaults for this user's scale; env vars override.
SCALE_MAC = os.getenv("SCALE_MAC", "D0:7B:6F:91:88:E8")
# Your 12-byte Mi token (hex). Set the SCALE_TOKEN env var, or paste yours here.
SCALE_TOKEN = os.getenv("SCALE_TOKEN", "000000000000000000000000")

HERE = os.path.dirname(os.path.abspath(__file__))
READER = os.path.join(HERE, "gatt_mi.py")

# Colors
BG = "#0f1117"
FG_DIM = "#8a90a2"
COL_WAIT = "#8a90a2"
COL_BUSY = "#f0a020"
COL_LIVE = "#4aa3ff"
COL_STABLE = "#33cc77"
COL_ERROR = "#ff5555"

RE_INTERMEDIATE = re.compile(r"intermediate weight:\s*([\d.]+)\s*kg")
RE_STABLE = re.compile(r"STABLE WEIGHT:\s*([\d.]+)\s*kg")


class ScaleApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.q: "queue.Queue[tuple]" = queue.Queue()
        self.proc = None
        self._stop = False

        root.title("Xiaomi S200 Scale")
        root.configure(bg=BG)
        root.geometry("560x360")
        root.minsize(420, 280)

        # Status pill at top
        self.status_var = tk.StringVar(value="Starting…")
        self.status = tk.Label(root, textvariable=self.status_var, font=("Segoe UI", 14),
                               fg=COL_WAIT, bg=BG)
        self.status.pack(pady=(28, 0))

        # Big weight number
        self.weight_var = tk.StringVar(value="––.–")
        self.weight = tk.Label(root, textvariable=self.weight_var,
                               font=("Segoe UI", 96, "bold"), fg=COL_WAIT, bg=BG)
        self.weight.pack(pady=(6, 0))

        self.unit = tk.Label(root, text="kg", font=("Segoe UI", 24), fg=FG_DIM, bg=BG)
        self.unit.pack()

        # Sub-line: live vs locked, last stable value
        self.sub_var = tk.StringVar(value="")
        self.sub = tk.Label(root, textvariable=self.sub_var, font=("Segoe UI", 12),
                            fg=FG_DIM, bg=BG)
        self.sub.pack(pady=(10, 0))

        # Footer
        self.footer = tk.Label(root, text=f"{SCALE_MAC}", font=("Consolas", 9),
                               fg="#4a4f5e", bg=BG)
        self.footer.pack(side="bottom", pady=8)

        self.last_stable = None

        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.start_reader()
        self.root.after(80, self.poll)

    # ---- child process management ----
    def start_reader(self):
        env = dict(os.environ)
        env["SCALE_MAC"] = SCALE_MAC
        env["SCALE_TOKEN"] = SCALE_TOKEN
        env["PYTHONUNBUFFERED"] = "1"
        env["PYTHONIOENCODING"] = "utf-8"      # child prints ✓ → … safely
        env.pop("MQTT_HOST", None)             # no MQTT for the GUI

        flags = 0
        if os.name == "nt":
            flags = subprocess.CREATE_NO_WINDOW  # don't pop a console

        self.proc = subprocess.Popen(
            [sys.executable, "-u", READER],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=env, encoding="utf-8", errors="replace",
            bufsize=1, creationflags=flags,
        )
        t = threading.Thread(target=self._reader_thread, daemon=True)
        t.start()

    def _reader_thread(self):
        for line in self.proc.stdout:
            if self._stop:
                break
            self.q.put(("line", line.rstrip("\n")))
        self.q.put(("exit", None))

    # ---- UI update loop (main thread) ----
    def poll(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "line":
                    self.handle_line(payload)
                elif kind == "exit":
                    self.set_status("Reader stopped", COL_ERROR)
        except queue.Empty:
            pass
        if not self._stop:
            self.root.after(80, self.poll)

    def handle_line(self, line: str):
        m = RE_STABLE.search(line)
        if m:
            kg = float(m.group(1))
            self.last_stable = kg
            self.show_weight(kg, COL_STABLE)
            self.set_status("Stable reading ✓", COL_STABLE)
            self.sub_var.set("Locked")
            return

        m = RE_INTERMEDIATE.search(line)
        if m:
            kg = float(m.group(1))
            self.show_weight(kg, COL_LIVE)
            self.set_status("Measuring…", COL_LIVE)
            self.sub_var.set("Live — hold still")
            return

        # Status keywords
        low = line.lower()
        if "waiting for" in low and "advertise" in low:
            self.set_status("Waiting for scale — step on it", COL_WAIT)
            self.reset_number()
        elif "connecting" in low or "advertising" in low:
            self.set_status("Connecting…", COL_BUSY)
        elif "auth complete" in low:
            self.set_status("Authenticated ✓", COL_BUSY)
        elif "stand on the scale" in low:
            self.set_status("Step on the scale", COL_LIVE)
        elif "go silent" in low or "asleep" in low:
            self.set_status("Weigh-in done — waiting", COL_WAIT)
            if self.last_stable is not None:
                self.sub_var.set(f"Last: {self.last_stable:.2f} kg")
        elif "server proof mismatch" in low:
            self.set_status("Auth key mismatch (check token)", COL_ERROR)
        elif "error" in low and "read failed" not in low:
            self.set_status("Error — see console", COL_ERROR)

    def show_weight(self, kg: float, color: str):
        self.weight_var.set(f"{kg:.2f}")
        self.weight.config(fg=color)
        self.unit.config(fg=color)

    def reset_number(self):
        self.weight_var.set("––.–")
        self.weight.config(fg=COL_WAIT)
        self.unit.config(fg=FG_DIM)
        self.sub_var.set("")

    def set_status(self, text: str, color: str):
        self.status_var.set(text)
        self.status.config(fg=color)

    def on_close(self):
        self._stop = True
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
            except Exception:
                pass
        self.root.destroy()


def main():
    if not os.path.exists(READER):
        print(f"Cannot find {READER}")
        sys.exit(1)
    root = tk.Tk()
    ScaleApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
