# Smart Mirror Weight Tracker

Read body weight from a **Xiaomi Smart Scale S200** over BLE and show it on a
touch display mounted next to a bathroom mirror, with trend tracking, a
days-since-last-weigh-in nudge, and optional Google Sheet sync.

The S200 does **not** broadcast weight — it requires an authenticated, encrypted
GATT connection (Xiaomi's "mible" MIoT protocol), which was reverse-engineered
and reimplemented here on the ESP32.

## Layout

```
esp32-mirror/   ESP32 firmware (PlatformIO) — the product
pc-tools/       Python tools used to crack the protocol + a desktop reader
docs/           Spec, wiring list, apps-script, project brief
captures/       Raw reverse-engineering artifacts (local only, gitignored)
```

## Hardware

ESP32 DevKit v4 (WROOM-32E) · **TZT 2.8" ST7789V 240×320 SPI TFT + XPT2046
resistive touch** · VL53L0X ToF (presence) · passive buzzer. Touch-only (the
original rotary encoder was removed).

Full pin-by-pin wiring: [`docs/wiring.md`](docs/wiring.md).

## Firmware — `esp32-mirror/` (PlatformIO)

First-time setup — provide your own keys:

```
cp esp32-mirror/src/secrets.example.h esp32-mirror/src/secrets.h
# edit secrets.h: scale MAC, Mi token, cloud URL + token
```

`secrets.h` is gitignored. `SCALE_MAC`/`MI_TOKEN` come from the token extractor
(see below); `CLOUD_URL`/`CLOUD_TOKEN` come from your deployed Apps Script.

Build / flash / monitor:

```
pio run -e esp32dev -t upload --upload-port COM5
pio device monitor -p COM5
```

Environments:
- `esp32dev` — the integrated firmware (the product)
- `disp`     — ST7789 + touch display bring-up test
- `bringup`  — original hardware self-test

Source:
- `src/mi_scale.*`  — BLE client + Mi auth + weight decode (core 0 task)
- `src/mi_crypto.*` — HKDF / HMAC-SHA256 / AES-CCM (mbedTLS)
- `src/history.*`   — LittleFS log, one reading per day
- `src/timekeeper.*`— WiFiManager provisioning + NTP time
- `src/cloud.*`     — Google Sheet sync (queue + flush/pull)
- `src/main.cpp`    — touch UI + state machine (core 1)
- `src/config.h`    — pins, WiFi, thresholds  ·  `src/secrets.h` — personal keys

On-device UI: bottom tab bar (**Weight · Graph · Setup**). Setup includes touch
calibration, units, buzzer, goal, cloud sync, WiFi reset, and web-based OTA.

## PC tools — `pc-tools/`

Python (bleak + cryptography). Highlights:
- `gatt_mi.py`    — full PC reader (Mi auth → decrypt → weight); reference for the firmware
- `scale_app.py`  — tkinter desktop live-weight display (wraps gatt_mi.py)
- `xiaomi_scan.py`, `watch_scale.py`, `btsnoop_parse.py` — discovery + protocol analysis

The per-device BLE key + token are pulled with the
[Xiaomi-cloud-tokens-extractor](https://github.com/PiotrMachowski/Xiaomi-cloud-tokens-extractor)
(not included here — clone it separately into `pc-tools/`).

## Protocol notes

- Auth is **token-based** (not ECDH): `salt = random_key + remote_key`, then
  `HKDF-SHA256(mi_token, salt, "mible-login-info", 64)` → dev/app keys + IVs.
- Data records: **AES-CCM**, tag length 4, nonce = `iv(4) + 0x00×4 + seq(4 LE)`.
- Weight frames: `0x19` = live weight, `0x23` = final measurement.

## Status

Working end-to-end: BLE auth + weight capture (persistent connection), one-per-day
history + trend on LittleFS, NTP timestamps, ST7789 touch UI, presence/night-mode,
reject-validation gate, Google Sheet sync, and web OTA. Hardware bench-tested.

## Built with AI

This project — the protocol reverse-engineering, the firmware, the PC tools, and
the docs — was developed with the help of AI coding tools (Claude / Claude Code).
