#!/usr/bin/env python3
"""Sync Smart Mirror weigh-ins from the Google Sheet to Garmin Connect.

Pipeline:
  ESP32 -> Google Sheet (Apps Script) -> [this script] -> Garmin Connect -> watch

It reads the sheet CSV (one "epoch,kg" line per reading) that the Apps Script
`doGet` already exposes, remembers which calendar dates it has already pushed
(a local state file), and sends any new weigh-ins to Garmin Connect via
`python-garminconnect` (`add_weigh_in`, weight in kg).

Runs anywhere Python runs — Termux on Android, a PC (Task Scheduler), or a Pi.

Config (environment variables, optionally loaded from a local `garmin.env`
file next to this script — KEY=VALUE lines):
    GARMIN_EMAIL=you@example.com
    GARMIN_PASSWORD=your-garmin-password
    SHEET_URL=https://script.google.com/macros/s/XXXX/exec
    SHEET_TOKEN=your-cloud-token
`garmin.env` is gitignored — never commit it.

First Garmin login caches a session token (via garth) under ~/.garminconnect
so MFA isn't re-prompted on later runs.

Usage:
    python garmin_sync.py            # sync new weigh-ins
    python garmin_sync.py --dry-run  # show what would be sent, send nothing
    python garmin_sync.py --all      # ignore state, resend every dated reading
"""

import argparse
import json
import os
import sys
from datetime import datetime

import requests

HERE = os.path.dirname(os.path.abspath(__file__))
ENV_FILE = os.path.join(HERE, "garmin.env")
STATE_FILE = os.path.join(HERE, ".sync_state.json")
TOKENSTORE = os.path.expanduser(os.getenv("GARMINTOKENS", "~/.garminconnect"))

# Readings older than this have no usable clock (device booted without NTP) and
# can't be assigned a date, so we skip them.
MIN_EPOCH = 1_700_000_000  # ~2023-11


def load_config():
    """Env vars win; fall back to KEY=VALUE lines in garmin.env."""
    cfg = {}
    if os.path.exists(ENV_FILE):
        with open(ENV_FILE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip().strip('"').strip("'")
    out = {}
    for key in ("GARMIN_EMAIL", "GARMIN_PASSWORD", "SHEET_URL", "SHEET_TOKEN"):
        out[key] = os.getenv(key, cfg.get(key, ""))
    missing = [k for k, v in out.items() if not v]
    if missing:
        sys.exit(f"[config] missing: {', '.join(missing)}\n"
                 f"Set them as env vars or in {ENV_FILE} "
                 f"(copy garmin.env.example).")
    return out


def fetch_sheet(url, token):
    """Return the sheet as {date -> (epoch, kg)}, keeping the last reading/day."""
    r = requests.get(url, params={"token": token}, timeout=20)
    r.raise_for_status()
    body = r.text.strip()
    if "bad token" in body.lower():
        sys.exit("[sheet] server said 'bad token' — check SHEET_TOKEN.")
    per_day = {}
    for line in body.splitlines():
        line = line.strip()
        if not line or "," not in line:
            continue
        ep_s, kg_s = line.split(",", 1)
        try:
            epoch = int(float(ep_s))
            kg = float(kg_s)
        except ValueError:
            continue
        if kg <= 0:
            continue
        if epoch < MIN_EPOCH:
            print(f"[sheet] skipping undated reading {kg} kg (no clock)")
            continue
        day = datetime.fromtimestamp(epoch).strftime("%Y-%m-%d")
        # keep the reading with the largest epoch for that day (last weigh-in)
        if day not in per_day or epoch > per_day[day][0]:
            per_day[day] = (epoch, kg)
    return per_day


def load_state():
    if os.path.exists(STATE_FILE):
        try:
            with open(STATE_FILE, "r", encoding="utf-8") as f:
                return set(json.load(f).get("synced_dates", []))
        except Exception:
            pass
    return set()


def save_state(synced):
    with open(STATE_FILE, "w", encoding="utf-8") as f:
        json.dump({"synced_dates": sorted(synced)}, f, indent=2)


def garmin_login(email, password):
    from garminconnect import Garmin

    # 1) Try cached session tokens (no password, no MFA prompt).
    try:
        g = Garmin()
        g.login(TOKENSTORE)
        print("[garmin] logged in via cached token")
        return g
    except Exception:
        pass

    # 2) Fresh login. Newer garminconnect supports an MFA prompt callback.
    def prompt_mfa():
        return input("Garmin MFA code: ").strip()

    try:
        g = Garmin(email=email, password=password, prompt_mfa=prompt_mfa)
    except TypeError:
        g = Garmin(email=email, password=password)
    g.login()
    try:
        g.garth.dump(TOKENSTORE)
        print(f"[garmin] logged in; token cached at {TOKENSTORE}")
    except Exception:
        print("[garmin] logged in (token cache not saved)")
    return g


def main():
    ap = argparse.ArgumentParser(description="Sync weigh-ins to Garmin Connect.")
    ap.add_argument("--dry-run", action="store_true",
                    help="show what would be sent, send nothing")
    ap.add_argument("--all", action="store_true",
                    help="ignore local state; resend every dated reading")
    args = ap.parse_args()

    cfg = load_config()
    per_day = fetch_sheet(cfg["SHEET_URL"], cfg["SHEET_TOKEN"])
    if not per_day:
        print("[sync] no dated readings in the sheet — nothing to do.")
        return

    synced = set() if args.all else load_state()
    todo = sorted(d for d in per_day if d not in synced)
    if not todo:
        print(f"[sync] up to date ({len(per_day)} readings, all already synced).")
        return

    print(f"[sync] {len(todo)} new weigh-in(s) to push: {', '.join(todo)}")
    if args.dry_run:
        for day in todo:
            _, kg = per_day[day]
            print(f"  would send {day}: {kg:.2f} kg")
        return

    g = garmin_login(cfg["GARMIN_EMAIL"], cfg["GARMIN_PASSWORD"])

    ok = 0
    for day in todo:
        epoch, kg = per_day[day]
        ts_iso = datetime.fromtimestamp(epoch).replace(microsecond=0).isoformat()
        try:
            g.add_weigh_in(weight=round(kg, 2), unitKey="kg", timestamp=ts_iso)
            synced.add(day)
            ok += 1
            print(f"  sent {day}: {kg:.2f} kg")
        except Exception as e:
            print(f"  FAILED {day}: {kg:.2f} kg -> {e}")
            break  # stop on first failure; keep the rest for next run

    save_state(synced)
    print(f"[sync] done: {ok}/{len(todo)} pushed to Garmin Connect.")


if __name__ == "__main__":
    main()
