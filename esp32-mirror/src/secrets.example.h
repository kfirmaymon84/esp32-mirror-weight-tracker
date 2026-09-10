#pragma once
#include <stdint.h>

// ── SECRETS TEMPLATE ────────────────────────────────────────────────────────
// Copy this file to `secrets.h` (same folder) and fill in your own values.
// `secrets.h` is gitignored so your keys never get committed.

// Your Xiaomi Scale S200 BLE MAC (lowercase, colon-separated).
#define SCALE_MAC "aa:bb:cc:dd:ee:ff"

// 12-byte Mi device token (the "token" field from token_extractor).
static const uint8_t MI_TOKEN[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Google Apps Script web-app /exec URL for cloud sync (leave as-is to disable).
#define CLOUD_URL   "https://script.google.com/macros/s/XXXXXXXX/exec"
// Shared secret; must match TOKEN in the Apps Script.
#define CLOUD_TOKEN "change-me"
