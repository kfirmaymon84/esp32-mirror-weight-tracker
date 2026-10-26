#pragma once
#include <stdint.h>

// Firmware version shown to people, and the build number the online updater
// compares (a device installs only a higher FW_BUILD). Bump both per release.
#define FW_VERSION "7"
#define FW_BUILD   7

// Hardware profile (pins, screen size, calibration) for the selected board.
#include "board.h"

// ── Scale (OKOK / Chipsea BLE broadcast) ────────────────────────────────────
// No keys: the firmware learns the first scale heard, or Settings → Scale scans
// for nearby scales and pairs the one the user picks.

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

// ── UI palette ──────────────────────────────────────────────────────────────
// 1 = high-contrast "gloss" palette (readable through glossy mirror film),
// 0 = the original Halo style-guide colours.
#ifndef UI_GLOSS_PALETTE
#define UI_GLOSS_PALETTE 1
#endif

// ── Display wake / night mode ────────────────────────────────────────────────
#define DISPLAY_TIMEOUT_MS 15000        // dim off after this with no presence/activity
#define NIGHT_START_MIN   (23 * 60 + 30) // 23:30 — quiet hours begin
#define NIGHT_END_MIN     (6 * 60)       // 06:00 — quiet hours end
// During quiet hours the ToF does NOT wake the display; a touch always does.

// ── WiFi ─────────────────────────────────────────────────────────────────────
// Credentials are provisioned via WiFiManager's captive portal (no hardcoding).
// First boot with no saved network (or holding a finger on the touch screen at
// power-on) opens an access point named below; join it with a phone to pick WiFi.
#define WIFI_AP_NAME  "SmartMirror-Setup"
// Time zone until the user's phone sets one on the setup page (timezones.h).
#define TZ_DEFAULT    "Asia/Jerusalem"

// ── History / trend ─────────────────────────────────────────────────────────
#define HISTORY_MAX      400   // readings kept in RAM (> 1 year at one per day)
#define TREND_WINDOW     7     // moving-average window for trend
#define TREND_UP_KG      0.5f  // above avg by this → "trending up"
#define TREND_ALPHA      0.1f  // EWMA trend: daily pull toward the reading (Trends/Home)
