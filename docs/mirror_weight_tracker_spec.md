# Smart Mirror Weight Tracker — Project Spec

## 1. Concept
A bathroom-mirror-mounted display that shows weight trend over time, reacts to
a person stepping in front of it, listens for a BLE weight reading from an
existing Xiaomi S200 scale, and gives audio + on-screen feedback (buzzer chirp
+ motivational/warning message).

## 2. Architecture (single-node)
One ESP32 handles everything — no second scale-side board is needed since the
S200 already broadcasts weight over BLE. ESP32 also joins home WiFi for cloud
sync and OTA (BLE scan + WiFi coexist fine on ESP32, radio is time-shared).

```
        ┌────────────────────────────────────┐
        │               ESP32                 │
        │                                      │
 ToF ───┼─► presence detect (wake logic)       │
        │                                      │
 BLE ◄──┼─── Xiaomi S200 advertisement          │
        │        (existing decode)             │
        │                                      │
        │  ├─ trend/message engine             │
        │  ├─ LittleFS/NVS history log (buffer)│
        │  ├─ display + buzzer driver          │
        │  ├─ WiFiManager (captive portal)     │
        │  ├─ HTTPS POST → Apps Script → Drive │
        │  └─ OTA (local Arduino OTA + remote  │
        │          pull-based version check)   │
        └───────────────┬────────────────────┘
                         │ SPI
                    TFT display
                (behind two-way mirror)
```

## 2a. WiFi Provisioning
- `WiFiManager` (tzapu) library: on first boot / no saved credentials, ESP32
  opens a captive-portal AP → connect with phone → pick home SSID + password.
- Reset trigger (long-press button, or double-reset-detector) to re-provision
  without reflashing (e.g. if router/network changes).

## 2b. Cloud Sync (Google Drive)
- ESP32 does NOT talk to Google Drive API directly (OAuth2/service-account
  JWT signing is heavy for the chip). Instead:
  **ESP32 → HTTPS POST → Google Apps Script Web App → Google Sheet/File in Drive**
- Apps Script deployed as a public web app under your Google account — no
  OAuth needed on-device, just a POST with `{timestamp, weight}`.
- Local LittleFS ring buffer caches readings if WiFi/cloud is unreachable at
  weigh-in time; flush queue once connectivity returns — no data point lost.
- Optional: Sheet becomes the "backend" for long-term graphing/export outside
  the mirror itself (Sheets charts, Google Data Studio, etc.)

## 2c. OTA Updates
- **Local Arduino OTA**: updates over LAN during development, no re-cabling.
- **Remote pull OTA**: periodic check of a hosted version file/manifest (e.g.
  GitHub Releases); if newer, downloads and flashes new `.bin` over HTTPS.
  Piggybacks on the same WiFi connection used for cloud sync.

## 3. Hardware (BOM starting point)
| Part | Suggestion | Notes |
|---|---|---|
| MCU | ESP32 (WROOM-32, or S3 if more RAM wanted for graphics) | BLE + display driving both needed |
| ToF sensor | VL53L0X or VL53L1X | I2C, mounted top edge pointed down/out at standing zone |
| Display | 2.4"–3.5" SPI TFT w/ touch — ILI9341+XPT2046 (resistive) or ILI9341/ST7789+GT911/FT6236 (capacitive) | Behind two-way acrylic mirror film. Capacitive preferred for wet-finger use in a bathroom; `TFT_eSPI` supports resistive touch directly |
| Buzzer | Passive piezo + PWM drive (or simple active buzzer via MOSFET) | Passive gives you tone variety for good/bad feedback |
| Power | 5V USB-C wall adapter | Bathroom-safe, no battery needed since it's wall-mounted |
| Mirror | Two-way acrylic mirror film or actual smart-mirror glass | Cut to size, display recessed behind it |
| Enclosure | 3D printed frame/bezel (Fusion 360) | Mounts display + ToF + ESP32 behind mirror panel |

## 4. Software / State Machine
- **IDLE**: display off or dim clock/last-reading screen. BLE scan running in background (low duty cycle to save power — not critical since wall-powered).
- **PRESENCE DETECTED** (ToF crosses threshold): only wakes the display during the active window (06:00–23:30, see 4b). Outside that window, ToF events are ignored for wake purposes (display stays off). When armed, wakes display, shows "waiting for reading" animation, extends BLE scan window / increases scan duty cycle.
- **TOUCH DETECTED**: always active regardless of time of day — a tap wakes the display/backlight any time, including during the night quiet-hours window. Touch IRQ is independent of ToF wake logic.
- **READING RECEIVED**: parse weight from S200 advertisement → run through validation gate (4a) → if accepted: buzzer chirp → append to history log → recompute trend → update graph → show message; if rejected: distinct buzzer tone → show "outside expected range" prompt → return to IDLE after N seconds of no presence.
- **NO READING TIMEOUT**: if presence detected but no BLE reading within X seconds, just show current graph (person might just be brushing teeth, not weighing in).

## 4a. Reading Validation / Outlier Rejection
Goal: reject readings from other household members (wife/kids) while still allowing your own legitimate long-term weight changes through.

- **Baseline**: last *accepted* reading, or a short moving average (e.g. last 3 accepted readings) — more robust than a single point.
- **Adaptive tolerance** (not a fixed number):
  `tolerance = base_tolerance + (days_since_last_accepted * max_daily_rate)`
  - `base_tolerance`: tight band for same-day/next-day duplicates (e.g. 1.5–2 kg) — catches a different person immediately.
  - `max_daily_rate`: plausible daily body-weight swing (e.g. 0.15–0.3 kg/day), widens the band the longer you go between weigh-ins so a real gradual change isn't falsely rejected.
  - Cap tolerance at a sane max (e.g. ±8–10% of baseline) so a long gap doesn't make it meaningless.
- **On accept**: normal flow (chirp, log, graph, message).
- **On reject**: separate buzzer tone (distinct from accept chirp) so you know immediately it wasn't logged; show "outside expected range" on screen.
- **Manual override**: on-screen "confirm this is correct" touch button after a rejection to force-accept and reset the baseline — covers real big changes (new diet, illness, etc.) without permanently locking you out. No physical button needed since the display is now touch.
- **No baseline yet** (first-ever reading): always accepted.
- Rejected readings can optionally still be logged to a separate "unrecognized" list for your own debugging, without touching the real trend data.

## 4b. Time-Gated Display Wake (Night Mode)
Goal: don't blind you with a lit-up mirror display if you walk in at 2am, but still let you deliberately wake it if you want to check something.

- **Time source**: NTP sync on boot + periodic resync (timezone Asia/Jerusalem), since ESP32 is already WiFi-connected. Optional: add a DS3231 RTC module (I2C) if you want the schedule to survive extended offline periods without an immediate resync.
- **Active window**: 06:00–23:30 — ToF presence wakes the display/backlight as normal.
- **Quiet hours** (23:30–06:00): ToF presence does NOT wake the display (backlight stays off, no light in a dark bathroom at night). ToF can still run in the background for debug/logging if wanted, it just doesn't trigger a wake.
- **Touch always wins**: regardless of time window, tapping the touchscreen wakes the display — touch controller IRQ is independent of the ToF wake path, so this works even while backlight/display are fully off.
- Window times configurable from the Settings screen (touch UI, section 7a) rather than hardcoded, in case your schedule changes.

## 5. Trend / Message Logic
- Maintain a trailing moving average (e.g. 7-day) alongside raw readings.
- Compare newest reading to the moving average *before* this reading:
  - Increase beyond threshold (e.g. +0.5–1 kg over the average) → cautionary message ("careful, trending up")
  - Stable or decreasing → positive reinforcement ("good work")
  - First-ever reading / insufficient history → neutral "reading captured" message only
- Keep message tone configurable (list of phrases, randomly picked per category) so it doesn't feel repetitive.

## 6. Data Storage
- Store readings as timestamp + weight in LittleFS (simple append-only log or small ring buffer for last N points to keep flash wear low).
- Graph renders from in-RAM cache of last ~30–90 points; full log kept on flash for longer history.

## 7. Display / Graph
- Simple line chart, X = time, Y = weight, with the moving-average line overlaid.
- Color cue tied to trend (e.g. line green while stable/down, amber/red while trending up).
- Idle screen: last reading + delta vs last week, low-key clock/date.

## 7a. Touch UI
Display module includes touch (resistive XPT2046 or capacitive GT911/FT6236 — see BOM). Unlocks a proper multi-screen UI instead of a single static view:

- **Screens** (swipe or tap to navigate):
  - **Today**: current reading, delta vs last week, message
  - **Graph**: trend chart, tappable range toggle (week / month / all-time)
  - **Settings**: goal weight, kg/lb unit toggle, mute buzzer, "check for OTA update now", WiFi reset (re-open captive portal without unplugging)
- **Manual weigh-in entry**: on-screen numeric keypad to log a weight by hand (e.g. S200 battery dead, or want to log something ad hoc).
- **Touch replaces the physical override button** from section 4a — rejected readings get a tap-to-confirm prompt instead of separate hardware.

## 8. Future Extensions (optional, not required for v1)
- **Multi-user profiles (optional, not v1)**: tap-to-select your name on the touchscreen before stepping on the scale; each profile gets its own baseline/tolerance/graph/goal. Would structurally solve the shared-scale problem instead of relying only on the tolerance gate in 4a. Left as a later add-on — v1 ships single-user with tolerance-based rejection.
- Add ESP-NOW or WiFi/MQTT if a second display node is ever wanted (e.g. kitchen, phone dashboard).
- Push history to a phone app or Home Assistant for long-term backup/analysis.
- Add body-fat / other S200 metrics if the BLE payload includes them.

## 9. Open Items to Decide During Build
- Exact ToF detection zone/threshold (distance + hysteresis to avoid false triggers from someone just passing by).
- Display wake/sleep timing to avoid burn-in / needless brightness at night.
- Buzzer tone set (e.g. single chirp = reading captured, two-tone = trend warning).
- Google Sheet vs raw Drive file as the Apps Script sync target.
- Where to host the OTA manifest/binary (GitHub Releases is a free, easy option).
- How aggressive the remote OTA check interval should be (e.g. once a day is plenty).
- Tune `base_tolerance` and `max_daily_rate` values against real-world use (a couple weeks of family data will make this obvious).
- Whether rejected readings get logged at all (for debugging) now that override is a simple tap.
- Resistive vs capacitive touch panel — capacitive costs more but handles wet/damp fingers better in a bathroom setting.

## 10. Task List (build order)

### Hardware & Procurement
1. Pick final BOM (ESP32 variant, touch panel type resistive/capacitive, ToF module, buzzer) and order parts
2. Confirm two-way mirror material/size and mounting approach

### Firmware — Core
3. BLE scan + decode Xiaomi S200 advertisement (port/adapt existing PC decode logic to ESP32)
4. ToF driver + presence detection tuning (distance threshold, hysteresis)
5. Touch driver bring-up (XPT2046 or GT911/FT6236, calibration)
6. Display driver + basic screen rendering (TFT_eSPI or similar)

### Firmware — Logic
7. State machine (idle / presence / touch / reading received / timeout)
8. Reading validation gate (adaptive tolerance, accept/reject, override flow)
9. Trend engine + message selection logic
10. NTP time sync + time-gated wake (night mode window, section 4b)

### Firmware — Connectivity
11. WiFiManager captive portal integration
12. Google Apps Script web app (weight logging endpoint) + ESP32 HTTPS POST client
13. Local LittleFS buffering/retry for offline cloud syncs
14. OTA — local Arduino OTA, then remote pull-based version check

### UI
15. Screen layouts: Today / Graph / Settings (touch navigation)
16. Manual weigh-in keypad entry
17. Graph rendering (line chart + moving average overlay)

### Mechanical
18. Fusion 360 enclosure/bezel design
19. 3D print + test fit behind mirror

### Integration & Testing
20. End-to-end test: full flow from stepping in front of mirror to cloud-logged reading
21. Family testing pass — validate tolerance rejection actually catches wife/kids correctly, tune thresholds
22. Night-mode real-world test (confirm ToF stays quiet, touch wake works in the dark)
