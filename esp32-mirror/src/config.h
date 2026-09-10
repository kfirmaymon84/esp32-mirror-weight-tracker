#pragma once
#include <stdint.h>

// Bump this to visibly confirm a firmware update (printed at boot).
#define FW_VERSION "2"

// ── Secrets (personal, gitignored) ──────────────────────────────────────────
// SCALE_MAC, MI_TOKEN, CLOUD_URL, CLOUD_TOKEN live in secrets.h.
// Copy secrets.example.h -> secrets.h and fill in your own values.
#include "secrets.h"

// ── User profile (sent to the scale during setup; body-comp uses these) ─────
// Weight readout does not depend on these being exact.
#define USER_HEIGHT_CM 177
#define USER_AGE       33
#define USER_SEX       1   // 1 = male, 0 = female

// How long to keep the session open after auth, listening for a stable weight.
#define SESSION_STREAM_MS 30000

// ── Pin map (ESP32 DevKit v4 / WROOM-32E) ───────────────────────────────────
// TFT (ST7789V 240x320 + XPT2046 touch, SPI) pins are set via TFT_eSPI build
// flags in platformio.ini; mirrored here for reference:
//   SCLK=18  MOSI=23  MISO=19  CS=5  DC=16  RST=17  BL=32  TOUCH_CS=4
// Full wiring: docs/wiring.md
#define PIN_TFT_BL   32

#define PIN_I2C_SDA  21
#define PIN_I2C_SCL  22

// Rotary encoder removed (touch-only). GPIO 25/26/27 are free.

#define PIN_BUZZER   33

// ── Presence detection (VL53L0X) ────────────────────────────────────────────
#define PRESENCE_MM      900   // closer than this = someone is here
#define PRESENCE_CLEAR_MM 1100 // must exceed this to count as "gone" (hysteresis)

// ── Reading validation (reject other household members) ─────────────────────
// A reading is accepted if it's within `tolerance` of your recent baseline,
// where tolerance grows the longer it's been since your last accepted reading:
//   tolerance = BASE + days_since_last * RATE   (capped at CAP_PCT of baseline)
#define VALIDATE_BASE_KG        2.0f    // same/next-day band
#define VALIDATE_RATE_KG_DAY    0.25f   // plausible daily drift
#define VALIDATE_CAP_PCT        0.10f   // never wider than ±10% of baseline
#define REJECT_CONFIRM_MS       20000   // window to force-accept a rejected reading

// ── Display wake / night mode ────────────────────────────────────────────────
#define DISPLAY_TIMEOUT_MS 15000        // dim off after this with no presence/activity
#define NIGHT_START_MIN   (23 * 60 + 30) // 23:30 — quiet hours begin
#define NIGHT_END_MIN     (6 * 60)       // 06:00 — quiet hours end
// During quiet hours the ToF does NOT wake the display; the encoder always does.

// ── WiFi ─────────────────────────────────────────────────────────────────────
// Credentials are provisioned via WiFiManager's captive portal (no hardcoding).
// First boot with no saved network (or holding a finger on the touch screen at
// power-on) opens an access point named below; join it with a phone to pick WiFi.
#define WIFI_AP_NAME  "SmartMirror-Setup"
// POSIX timezone for Asia/Jerusalem (with DST rules).
#define TZ_INFO       "IST-2IDT,M3.4.4/26,M10.5.0"

// ── History / trend ─────────────────────────────────────────────────────────
#define HISTORY_MAX      120   // readings kept in RAM (flash log keeps all)
#define TREND_WINDOW     7     // moving-average window for trend
#define TREND_UP_KG      0.5f  // above avg by this → "trending up"

// ── Cloud sync (Google Apps Script web app) ─────────────────────────────────
// CLOUD_URL and CLOUD_TOKEN live in secrets.h (gitignored). See secrets.example.h.
