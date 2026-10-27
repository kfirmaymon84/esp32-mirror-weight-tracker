# Smart Mirror Weight Tracker — notes for Claude

An ESP32 + touch display that sits by a bathroom mirror: it reads body weight from
an OKOK / Chipsea BLE scale, shows the latest weight, goal ring, BMI and trends,
and syncs each user's weigh-ins and settings to their own cloud account. The
owner is moving toward one public firmware for every user (testers first).

**This repository is PUBLIC.** Never commit or push personal data: no email
addresses, Wi-Fi names, tokens, keys, sheet links or scale MAC addresses.

## Layout

- `esp32-mirror/` — the firmware (PlatformIO, Arduino-ESP32 2.x / IDF 4.4).
  - `src/main.cpp` — UI and app logic ("Halo" UI, 480×320 landscape).
  - `src/board.h` — per-board pins; `src/config.h` — `FW_VERSION`/`FW_BUILD` + tuning.
  - `src/okok_scale.*` (+ `scale.h` alias) — passive BLE scan of the OKOK scale.
  - `src/cloud.*` — Firebase Auth + Firestore over HTTPS; `src/cloud_config.h`
    holds the project's public IDs; `src/google_roots.h` the verified TLS roots.
  - `src/timekeeper.*` — Wi-Fi (WiFiManager captive portal), NTP, time zones
    (`src/timezones.h`), the phone setup pages (Wi-Fi / account / time zone).
  - `src/ota_update.*` + `src/ota_pubkey.h` — signed online updates.
  - `src/history.*` — local weigh-in log (LittleFS) + EWMA trend.
  - `src/buzzer.*` — LEDC tones + DAC-synthesised boot melody.
  - `ota/release.py` — build, sign, publish releases; `ota/check_timezones.py`.
  - `CHANGELOG.md` — one section per firmware version (see Releases).
  - Bring-up / test sketches: `disp4_bringup.cpp`, `probe35.cpp`,
    `melody_audition.cpp` (their own envs; not in the product build).
- `cloud/` — `firestore.rules` (paste into the Firebase console → Publish) and
  `import_sheet.py` (one-time Google-Sheet import; `--remove-under KG` cleanup).
- `enclosure/` — parametric CadQuery case for the 4" board (`build_case.py`
  runs an interference check against the vendor board STEP, which is NOT in
  the repo; `clean_board_step.py` flattens it for Onshape).
- `pc-tools/`, `captures/` (gitignored) — old reverse-engineering tools for the
  Xiaomi S200 scale. Xiaomi support has been REMOVED from the firmware (fw 6).

## Hardware

Two LCDWiki all-in-one boards, ESP32-32E (WROOM-32E), CH340 USB serial:
- **4" E32R40T** (`[env:e32r40t]`): ST7796S 320×480 + XPT2046 touch on the LCD
  SPI bus (TOUCH_CS 33), backlight 27, speaker amp on 26 with enable 4 (active
  LOW), RGB LED 22/16/17 (active LOW), I2C 32/25 (VL53L0X distance sensor).
- **3.5" E32R35T** (`[env:e32r35t]`): same panel wiring but, unlike its
  datasheet, LED R4/G17/B16, amp always on, I2C 21/22.
- Both: 480×320 in rotation 1, `min_spiffs` partitions (two OTA slots).
- Distance sensor wakes the display within 90 cm (clears at 110 cm); a board on
  a desk usually sees "someone present" and so never looks idle.
- Resistive touch: anything pressing the panel (a bezel) reads as a constant
  touch; the boot "hold to reset Wi-Fi" needs a valid touch held ~2 s.

## Build, flash, test

- `pio run -e e32r40t|e32r35t [-t upload]` (pio is in
  `~/.platformio/penv/Scripts/pio.exe`; boards usually on COM3).
- Read serial WITHOUT resetting the board: pyserial with `dtr=False; rts=False`
  before `open()`. Serial commands (115200): `d` diag, `h` history dump,
  `t` raw touch, `x` distance sensor, `c` touch calibration, `u` check for an
  update now, `b` toggle beta channel (no on-screen button, on purpose),
  `k` forget a rolled-back build, `p` pull the account, `o` local web uploader.
- `[env:e32r35t_otatest]` points the updater at a local server
  (`http://<PC hotspot IP>:8000/stable|beta`) to test updates end to end.

## Cloud (Firebase project `smart-mirror-9c760`, free Spark plan)

- Accounts: email/password. The user signs in (or creates the account) on the
  phone setup page; the device keeps only a refresh token, never the password.
- Data: `users/{uid}/readings/{epoch}` = `{ts, at (timestamp), kg, src}` (doc id
  = ts so retries overwrite), `users/{uid}/settings/profile` = units, sound,
  goal, height, `updatedTs`/`updatedAt` (newer side wins; the account wins right
  after sign-in). Device-only: touch calibration, paired scale, time zone.
- Rules in `cloud/firestore.rules`: owner-only, validated fields. Don't add a
  catch-all `{rest=**}` match: it would bypass the readings validation.
- TLS to Google is certificate-verified (`google_roots.h`). Today's chains end
  at GlobalSign Root CA (expires 2028-01-28): add GTS Root R4 before then.
- Large Firestore responses don't fit in RAM next to BLE + TLS: the history
  is pulled in pages of 20, and a cut-short page fails the whole pull.

## Releases (online updates)

- Devices fetch `<board>.json` from GitHub releases of this repo: **stable** =
  `releases/latest/download/`, **beta** = the `beta` pre-release (files replaced
  each time; a beta device takes the newer of both). The manifest is signed
  (ECDSA P-256 over `board:build:sha256`); the image's SHA-256 must match.
- A new image boots "pending" and confirms itself after 30 s; if it crashes
  first, the bootloader rolls back and that build is never retried.
- Automatic checks: overnight 02:00–05:00 local, display dark 10 min, at most
  every 12 h (or any quiet moment after 3 days without a check).
  Settings → Firmware → Check now installs immediately.
- **The private signing key** `esp32-mirror/ota/ota_signing_key.pem` is
  gitignored and exists only on the owner's PC (backed up by the owner).
  Without it no release can be signed. Never commit it, never run `genkey` again.
- To release: add changes under "Unreleased" in `CHANGELOG.md` as you go; then
  bump `FW_VERSION` + `FW_BUILD` in `src/config.h`, rename "Unreleased" to
  `## Firmware <v> (build <n>) — <date>`, commit, push,
  `py -3 ota/release.py beta` (it refuses without the changelog section and
  uses it as the release notes). `promote` publishes the beta files unchanged
  as stable. Both boards always share a version.

## How the owner works

- Commit, push and publish only when the owner asks. Before pushing, scan the
  commits for private data (email, tokens, keys, Wi-Fi names, MAC addresses).
- Product builds must contain no personal data; the owner's devices are set up
  through the same public firmware (account sign-in, scale pairing, `b`).
- UI style: partial redraws (no full-screen repaints for small changes), black
  background, one mint accent, amber for attention, no red.
- Keep `CHANGELOG.md` "Unreleased" up to date with user-visible changes.

## Gotchas

- Windows PowerShell `Get-Content | Set-Content` mangles UTF-8 (box-drawing
  characters become garbage) and adds a BOM: edit files with an editor tool or
  Python, not PowerShell. Printing `→` to the console needs `PYTHONIOENCODING=utf-8`.
- On this core `LittleFS.exists()` and `open()` of a missing file log `[E]`
  errors: check with `stat("/littlefs/...")` first. `Preferences` opened
  read-only on a new namespace, and `getFloat()`/`getBytesLength()` of a
  missing key, also log errors: use read-write `begin()` and `isKey()`.
- After the DAC boot melody, `rtc_gpio_deinit()` the speaker pin or LEDC tones
  stay silent.
- WiFiManager `setMenu()` needs a named vector (non-const reference).

## Where things stand (2026-10-27)

- Firmware 7 is the current beta (no stable release yet). The owner is the only
  tester; their 3.5" board is on the beta channel.
- Next: the owner's 4" home unit still runs the old Xiaomi firmware. Plan: over
  USB, erase flash, flash `[env:e32r40t]`, then on the device sign in on the
  setup page, pair the OKOK scale (Settings → Scale), send `b` over USB, and test
  settings sync between the two devices.
- Ideas raised but not built: each device reporting its firmware version to the
  account; automatic version bumping in `release.py`; a data export/backup
  when there are more users.
