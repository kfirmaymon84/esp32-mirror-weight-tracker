# Garmin Connect sync

Pushes Smart Mirror weigh-ins into **Garmin Connect** (which then syncs the
weight to your watch). It reads the readings from your existing Google Sheet —
the ESP32 is not involved and needs no changes.

```
ESP32 → Google Sheet (Apps Script) → garmin_sync.py → Garmin Connect → watch
```

Garmin has no official personal API, so this uses the community
[`python-garminconnect`](https://github.com/cyberjunky/python-garminconnect)
library, which logs into Garmin Connect for you and caches the session token.

## What it does

- Fetches the sheet CSV (`epoch,kg` per line) via the Apps Script `doGet`.
- Keeps one weigh-in per day (the last), skips readings it has already sent
  (tracked in `.sync_state.json`), and skips undated readings (device booted
  with no clock).
- Sends new weigh-ins with `add_weigh_in(weight, unitKey="kg", timestamp=...)`.

## Config

Copy the template and fill in your values:

```bash
cp garmin.env.example garmin.env
# then edit garmin.env
```

`garmin.env` holds your Garmin password and is **gitignored** — it never leaves
your device. `SHEET_URL` / `SHEET_TOKEN` are the same values as `CLOUD_URL` /
`CLOUD_TOKEN` in `esp32-mirror/src/secrets.h`.

## Run on Android (Termux)

1. Install **Termux** from [F-Droid](https://f-droid.org/en/packages/com.termux/)
   (the Play Store build is outdated).
2. Set up Python + the library:
   ```bash
   pkg update && pkg install python
   pip install -r requirements.txt
   ```
3. Create `garmin.env` (see Config above) and paste in your details.
4. First run — verify it works (if your account has 2FA it asks for a code once,
   then caches the session):
   ```bash
   python garmin_sync.py --dry-run   # preview: shows what would be sent
   python garmin_sync.py             # actually push
   ```
5. Schedule it (once or twice a day is plenty for weight):
   ```bash
   pkg install termux-api
   termux-job-scheduler --script ~/…/garmin-sync/garmin_sync.py --period-ms 43200000
   ```
   Also exempt Termux from battery optimization (Android Settings → Apps →
   Termux → Battery → Unrestricted) so it isn't killed in the background.

## Run on a PC or Raspberry Pi (optional, same script)

```bash
pip install -r requirements.txt
cp garmin.env.example garmin.env   # edit it
python garmin_sync.py
```

Schedule with cron (Linux/Pi) or Task Scheduler (Windows) — e.g. twice daily.

## Flags

- `--dry-run` — show what would be sent, send nothing.
- `--all` — ignore local state and resend every dated reading (e.g. first-time
  backfill of your whole history).

## Notes

- Session tokens are cached under `~/.garminconnect` (override with the
  `GARMINTOKENS` env var). Delete that folder to force a fresh login.
- This is Garmin's private API via a community library — it can break if Garmin
  changes their login flow; updating `garminconnect` usually fixes it.
