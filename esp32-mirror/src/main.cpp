// Smart Mirror Weight Tracker — integrated firmware.
// BLE scale listener on core 0 (passive scan); UI on core 1.
//
// Hardware: per-board profile in board.h (currently the 4" E32R40T: ST7796S
// 480x320 + XPT2046 touch, speaker amp, RGB LED, ToF on I2C; USB powered).
// Scale: selected at build time via scale.h — OKOK broadcast (default, no keys) or
// the Xiaomi S200 (-D SCALE_S200, personal builds using the keys in secrets.h).
//
// UI "Halo" (landscape 480x320, black = mirror, one mint accent, amber for
// attention, smooth Outfit/Inter fonts from halo_fonts.h):
//   rail screens — 64 px icon rail (Home · Trends · Settings) + content
//     Home     — halo ring (goal progress) around the latest weight, week pill,
//                Goal / Streak / 30-day sparkline column. The resting screen.
//     Trends   — EWMA trend line over raw dots, 14D / 30D / 90D / All
//     Settings — 2x4 tiles
//   full-screen — Measuring (comet arc + live number), Result (30 s), Is-this-you
//                 (20 s), Night glance (quiet hours), Wi-Fi setup (QR code)

#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <Update.h>
#include <VL53L0X.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <math.h>
#include <qrcode.h>

#include "buzzer.h"
#include "cloud.h"
#include "config.h"
#include "halo_fonts.h"
#include "history.h"
#include "ota_update.h"
#include "scale.h"
#include "timekeeper.h"


// ── Scale task → UI ─────────────────────────────────────────────────────────
struct WEvt { float kg; bool stable; };
struct SEvt { char s[24]; };
static QueueHandle_t weightQ, statusQ;

static TFT_eSPI tft = TFT_eSPI();
static VL53L0X tof;
static bool tofOk = false;

// ── Halo palette (RGB565) ───────────────────────────────────────────────────
// Behind glossy mirror film the reflection adds a floor of light over the whole
// panel: anything dimmer than it vanishes. The gloss palette (default) keeps the
// black ground but lifts every other tone — secondary text, tracks, borders,
// highlights and the faint glows — above that floor. UI_GLOSS_PALETTE 0 restores
// the original Halo style-guide values.
#if UI_GLOSS_PALETTE
static const uint16_t C_INK     = 0x0000;   // black = the mirror
static const uint16_t C_PANEL   = 0x0000;   // tile fill (a dark fill is invisible under gloss)
static const uint16_t C_TRACK   = 0x31C8;   // ring track, tile border   #353A42
static const uint16_t C_LINE    = 0x3A09;   // dividers, outlines         #3C424A
static const uint16_t C_TEXT    = 0xFFFF;   //                            #FFFFFF
static const uint16_t C_TEXT2   = 0xCE9B;   //                            #CDD2D8
static const uint16_t C_TEXT3   = 0xA556;   //                            #A2A8B0
static const uint16_t C_MINT    = 0x6FFA;   // the one accent             #6BFFD0
static const uint16_t C_CYAN    = 0x5E9F;   // live / measuring           #5AD2FF
static const uint16_t C_AMBER   = 0xFE0C;   // attention — there is no red #FFC266
static const uint16_t C_MINTDIM = 0x1AC8;   // selected rail item, pills  #1B5A47
static const uint16_t C_NIGHT   = 0xB409;   // night glance (dim room: unchanged)
static const uint16_t C_EDGE    = 0x2966;   // rail border                #2A2E35
static const uint16_t C_CHIP    = 0x2146;   // icon discs, chart grid     #272B32
static const uint16_t C_BTNEDGE = 0x5B0D;   // outline button, off bar    #5A6069
static const uint16_t C_DASH    = 0x8452;   // goal pill dashes           #858B95
static const uint16_t C_DOT     = 0x3CF0;   // past streak days           #3E9E80
static const uint16_t C_NIGHT2  = 0xA389;   // night secondary text       #A07048
// Opacity of the soft effects (glows, comet tail, chart area, halo rings).
static const float A_GLOW = 0.30f, A_TAIL1 = 0.35f, A_TAIL2 = 0.65f, A_AREA = 0.38f;
static const float A_HALO1 = 0.25f, A_HALO2 = 0.50f, A_RAWDOT = 1.0f;
#else
static const uint16_t C_INK     = 0x0000;   // black = the mirror
static const uint16_t C_PANEL   = 0x0862;   // tile fill
static const uint16_t C_TRACK   = 0x18E4;   // ring track, tile border
static const uint16_t C_LINE    = 0x1904;   // dividers, outlines
static const uint16_t C_TEXT    = 0xF7BE;
static const uint16_t C_TEXT2   = 0xA556;
static const uint16_t C_TEXT3   = 0x8431;
static const uint16_t C_MINT    = 0x5F98;   // the one accent
static const uint16_t C_CYAN    = 0x3E5F;   // live / measuring
static const uint16_t C_AMBER   = 0xFD8B;   // attention — there is no red
static const uint16_t C_MINTDIM = 0x0944;   // selected rail item, pills
static const uint16_t C_NIGHT   = 0xB409;   // night glance
static const uint16_t C_EDGE    = 0x1082;   // rail border            #111316
static const uint16_t C_CHIP    = 0x10C3;   // icon discs, chart grid #15181C
static const uint16_t C_BTNEDGE = 0x2966;   // outline button, off bar #2B2F35
static const uint16_t C_DASH    = 0x4A6A;   // goal pill dashes       #4A4F57
static const uint16_t C_DOT     = 0x2B4A;   // past streak days       #2C6B57
static const uint16_t C_NIGHT2  = 0xA389;   // night secondary text   #A07048
static const float A_GLOW = 0.16f, A_TAIL1 = 0.18f, A_TAIL2 = 0.45f, A_AREA = 0.22f;
static const float A_HALO1 = 0.12f, A_HALO2 = 0.30f, A_RAWDOT = 0.75f;
#endif

// ── Geometry ────────────────────────────────────────────────────────────────
static const int RAIL_W = 64;                 // icon rail incl. its 1 px border
static const int MAIN_X = RAIL_W + 22;        // content left edge (rail screens)
static const int RAIL_BTN_X = 4, RAIL_BTN_W = 56, RAIL_BTN_H = 60;
static const int RAIL_BTN_Y0 = 62, RAIL_BTN_PITCH = 68;

// ── UI state ────────────────────────────────────────────────────────────────
enum Screen { SCR_HOME = 0, SCR_TRENDS = 1, SCR_SETTINGS = 2 };
enum Mode { MODE_RAIL, MODE_MEASURE, MODE_RESULT, MODE_CONFIRM, MODE_NIGHT, MODE_PAIR,
            MODE_ACCOUNT };
static int screen = SCR_HOME;           // rail screen; Home is the resting screen
static int mode = MODE_RAIL;            // what fills the display
static int lastPullN = -2;              // -2 never, -1 failed, >=0 readings pulled
static time_t lastSyncAt = 0;           // last successful account sync (epoch)
static bool syncing = false;            // cloud flush in progress (Wi-Fi icon cyan)

// ── Settings (persisted in NVS) ─────────────────────────────────────────────
static bool  setUnitLb = false;         // display kg (false) or lb (true)
static int   setSound  = SOUND_MID;     // speaker volume: Off / Low / Mid / High
static float setGoalKg = 0;             // goal weight in kg (0 = none)
#if defined(SCALE_S200)
static const float HEIGHT_DEFAULT_CM = USER_HEIGHT_CM;   // personal build: owner's height
#else
static const float HEIGHT_DEFAULT_CM = 0;                // product: unset until entered
#endif
static float setHeightCm = 0;           // body height for BMI (0 = not set)
static String savedScaleMac;            // scale MAC as stored in NVS
// Units / sound / goal / height also live in the account (users/<uid>/settings/
// profile): changed here = dirty until uploaded; the newer side wins on sync.
static uint32_t setUpdated = 0;         // epoch of the last change on this device
static bool setDirty = false;           // changed since the last upload

// Settings tiles (2 columns x 4 rows), each a touch target.
enum MenuItem { MI_SYNC, MI_UNIT, MI_GOAL, MI_HEIGHT, MI_SOUND, MI_CALIBRATE, MI_SCALE,
                MI_OTA, MI_WIFI, MI_COUNT };
static const int TILE_ROWS = (MI_COUNT + 1) / 2;
static const int TILE_Y0 = 50, TILE_W = 185, TILE_H = 46, TILE_GAP = 6;
// Text rows inside a tile: small label (top), value (middle line), big-number baseline.
static const int TILE_LAB_Y = 6, TILE_VAL_Y = TILE_H - 15, TILE_BASE_Y = TILE_H - 8;
static int tileX(int i) { return MAIN_X + (i % 2) * (TILE_W + TILE_GAP); }
static int tileY(int i) { return TILE_Y0 + (i / 2) * (TILE_H + TILE_GAP); }
// Goal tile -/+ buttons (offsets inside the tile, 40x40 touch targets).
static const int GB_MINUS_DX = TILE_W - 8 - 40 - 6 - 40, GB_PLUS_DX = TILE_W - 8 - 40;
static const int GB_DY = (TILE_H - 40) / 2, GB_W = 40;

// Trends range control.
static const int RANGES[] = {14, 30, 90, 0};       // days; 0 = all
static const char *RANGE_NAMES[] = {"14D", "30D", "90D", "All"};
static int rangeIdx = 0;
static const int SEG_X = 276, SEG_Y = 10, SEG_BW = 46, SEG_BH = 40;

// Touch calibration (XPT2046), default from board.h; NVS overrides it.
static uint16_t calData[5] = TOUCH_CAL_DEFAULT;

// OTA "update mode"
static bool otaMode = false;
static bool wantOta = false;                 // request to enter OTA mode (from menu/serial)
static uint32_t otaUntil = 0;
static const uint32_t OTA_WINDOW_MS = 300000;  // 5 min listen window

// Online updates (product builds): checked soon after boot, then every 12 h
// (1 h after a failed check), only after the display has been dark 10 min.
static char otaNote[20] = "";                // Settings tile: "Up to date", ...
static uint32_t lastOtaCheckMs = 0;
static uint32_t otaIntervalMs = 0;           // 0 = not checked yet this boot
static const uint32_t OTA_FIRST_CHECK_MS = 120000;
static const uint32_t OTA_IDLE_MS = 600000;  // display dark this long before a check

// Watchdog: an independent task reboots the device if the main loop or the
// scale task stops ticking (a hang), so a wall-mounted unit recovers itself.
static volatile uint32_t mainTick = 0;
static volatile uint32_t wdtGraceUntil = 0;  // long blocking jobs (update) pause it
static const uint32_t MAIN_STALL_MS = 60000;   // main loop silent this long → reboot
static const uint32_t BLE_STALL_MS  = 150000;  // scale task silent this long → reboot

static Preferences prefs;
static void settingsLoad() {
  prefs.begin("mirror", true);
  setUnitLb = prefs.getBool("unit", false);
  setSound  = prefs.getInt("sound", SOUND_MID);
  // getFloat() logs an error for a key a new device hasn't saved yet: check first.
  setGoalKg = prefs.isKey("goal") ? prefs.getFloat("goal", 0) : 0;
  setHeightCm = prefs.isKey("height") ? prefs.getFloat("height", 0) : HEIGHT_DEFAULT_CM;
  savedScaleMac = prefs.getString("scalemac", "");
  setUpdated = prefs.getUInt("setts", 0);
  setDirty = prefs.getBool("setdirty", false);
  prefs.end();
  buzzerSetLevel(setSound);
}
static void settingsStore() {           // persist as they are (no "changed" mark)
  prefs.begin("mirror", false);
  prefs.putBool("unit", setUnitLb);
  prefs.putInt("sound", setSound);
  prefs.putFloat("goal", setGoalKg);
  prefs.putFloat("height", setHeightCm);
  prefs.putUInt("setts", setUpdated);
  prefs.putBool("setdirty", setDirty);
  prefs.end();
}
static void settingsSave() {            // the user changed a setting on this device
  setUpdated = timeIsSet() ? (uint32_t)nowEpoch() : 0;
  setDirty = true;
  settingsStore();
}
static void scaleMacSave(const char *mac) {
  prefs.begin("mirror", false);
  prefs.putString("scalemac", mac);
  prefs.end();
  savedScaleMac = mac;
}

// Touch calibration persistence (5 x uint16 blob in NVS, per-board key).
static void calLoad() {
  prefs.begin("mirror", true);
  if (prefs.isKey(TOUCH_CAL_KEY) && prefs.getBytesLength(TOUCH_CAL_KEY) == sizeof(calData))
    prefs.getBytes(TOUCH_CAL_KEY, calData, sizeof(calData));
  prefs.end();
  tft.setTouch(calData);
}
static void calSave() {
  prefs.begin("mirror", false);
  prefs.putBytes(TOUCH_CAL_KEY, calData, sizeof(calData));
  prefs.end();
}

// Display-unit helpers (data is always stored in kg).
static float toDisp(float kg) { return setUnitLb ? kg * 2.20462f : kg; }
static float fromDisp(float v) { return setUnitLb ? v / 2.20462f : v; }
static const char *unitStr() { return setUnitLb ? "lb" : "kg"; }

static uint32_t lastWeighMs = 0;        // last time any scale weight event arrived
static uint32_t resultUntil = 0;        // Result screen auto-return (millis)
static const uint32_t RESULT_SHOW_MS = 30000;
static const uint32_t MEASURE_IDLE_MS = 8000;   // no scale events → leave Measuring

static char statusStr[24] = "Booting";
static bool present = false;
static float liveKg = -1;       // last live value
static int liveState = 0;       // 0 none, 1 live, 2 stable
static float liveHist[5];       // recent live values → stability bars
static int liveN = 0;
// Redraw requests, from biggest to smallest (only what changed gets repainted):
static bool dirty = true;          // whole screen (boot, wake, mode change)
static bool contentDirty = false;  // rail content area (screen switch, new data)
static bool todayDirty = false;    // in-place values (live weight, Home numbers)
static bool scaleTileDirty = false;// Settings "Scale" tile (scale MAC learned)


// Rejected-reading confirmation ("is this you?")
static float pendingKg = 0;
static uint32_t pendingUntil = 0;
static bool pendingActive() { return pendingUntil != 0 && millis() <= pendingUntil; }

// Confirm buttons (60 high, primary on the right).
static const int CB_Y = 188, CB_H = 60, CB_L_X = 52, CB_R_X = 248, CB_W = 180;

// Accept a reading only if it's within an adaptive band of the recent baseline.
static bool validateReading(float kg) {
  if (historyCount() == 0) return true;             // first-ever reading
  float baseline = historyAvg(3, false);            // avg of last up-to-3
  if (isnan(baseline)) baseline = historyLast().kg;
  float days = 0;
  uint32_t lastTs = historyLast().ts;
  if (timeIsSet() && lastTs > 1700000000UL) {
    long dt = (long)nowEpoch() - (long)lastTs;
    if (dt > 0) days = dt / 86400.0f;
  }
  float tol = VALIDATE_BASE_KG + days * VALIDATE_RATE_KG_DAY;
  float cap = VALIDATE_CAP_PCT * baseline;
  if (tol > cap) tol = cap;
  bool ok = fabsf(kg - baseline) <= tol;
  Serial.printf("[valid] kg=%.2f base=%.2f days=%.1f tol=%.2f -> %s\n", kg,
                baseline, days, tol, ok ? "ACCEPT" : "REJECT");
  return ok;
}

static void setStatus(const char *s) {
  strncpy(statusStr, s, sizeof(statusStr) - 1);
  statusStr[sizeof(statusStr) - 1] = 0;
}

// ── RGB status LED (back of the board) ──────────────────────────────────────
//   blue = measuring, green = reading saved (5 s), red = waiting for "Log it?"
enum LedColor { LED_OFF, LED_BLUE, LED_GREEN, LED_RED };
static int ledCur = -1;
static uint32_t ledGreenUntil = 0;

static void ledWrite(int c) {
  if (c == ledCur) return;
  ledCur = c;
  digitalWrite(PIN_LED_R, c == LED_RED ? LOW : HIGH);    // active LOW
  digitalWrite(PIN_LED_G, c == LED_GREEN ? LOW : HIGH);
  digitalWrite(PIN_LED_B, c == LED_BLUE ? LOW : HIGH);
}
static void ledBegin() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  ledWrite(LED_OFF);
}
static void ledUpdate() {
  if (pendingActive()) ledWrite(LED_RED);
  else if (ledGreenUntil && millis() < ledGreenUntil) ledWrite(LED_GREEN);
  else if (liveState == 1 && millis() - lastWeighMs < 3000) ledWrite(LED_BLUE);
  else ledWrite(LED_OFF);
}

// ════════════════════════════════════════════════════════════════════════════
// Drawing toolkit
// ════════════════════════════════════════════════════════════════════════════

static uint16_t mix(uint16_t fg, uint16_t bg, float a) {
  if (a <= 0) return bg;
  if (a >= 1) return fg;
  return tft.alphaBlend((uint8_t)(a * 255 + 0.5f), fg, bg);
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ── Fonts ───────────────────────────────────────────────────────────────────
static const uint8_t *curFont = nullptr;
static void useFont(const uint8_t *f) {   // nullptr = back to the built-in font
  if (f == curFont) return;
  if (curFont) tft.unloadFont();
  if (f) tft.loadFont(f);
  curFont = f;
}

// Text in the current font, anchored at (x, y) by `datum` (TFT_eSPI datums incl.
// L/C/R_BASELINE). Glyph boxes are painted with `bg`, and if `pad` is wider than
// the text the slack is painted too — so a new value overwrites the old one in
// place, without clearing first. Returns the text width.
static int text(const char *s, int x, int y, uint8_t datum, uint16_t fg, uint16_t bg,
                int pad = 0) {
  int w = tft.textWidth(s);
  int h = tft.fontHeight();
  int hx = datum % 3, vy = datum / 3;
  int left = hx == 0 ? x : hx == 1 ? x - w / 2 : x - w;
  int top = vy == 0 ? y : vy == 1 ? y - h / 2 : vy == 2 ? y - h : y - tft.gFont.maxAscent;
  if (pad > w) {
    int slack = pad - w;
    int l = hx == 0 ? 0 : hx == 1 ? slack / 2 : slack;   // part of the slack on the left
    if (l) tft.fillRect(left - l, top, l, h, bg);
    if (slack - l) tft.fillRect(left + w, top, slack - l, h, bg);
  }
  tft.setTextColor(fg, bg, true);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(s, left, top);
  return w;
}

// Pixel advance of one character in the current smooth font.
static int advanceOf(uint16_t uc) {
  uint16_t g;
  if (uc == ' ') return tft.gFont.spaceWidth;
  return tft.getUnicodeIndex(uc, &g) ? tft.gxAdvance[g] : tft.gFont.spaceWidth;
}

// Next UTF-8 character of s at byte *i (advances *i).
static uint16_t nextChar(const char *s, int *i) {
  uint8_t c = s[*i];
  int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : 3;
  uint16_t u = len == 1 ? c : len == 2 ? (c & 0x1F) : (c & 0x0F);
  for (int k = 1; k < len && s[*i + k]; k++) u = (u << 6) | (s[*i + k] & 0x3F);
  *i += len;
  return u;
}

// Letter-spaced caps label (the design's 0.14 em tracking). Top-anchored;
// align 0 = left, 1 = centre, 2 = right. `pad` > 0 repaints that width first.
static int label(const char *s, int x, int top, int align, uint16_t fg, uint16_t bg,
                 int pad = 0, int track = 2) {
  int w = 0, n = strlen(s);
  for (int i = 0; i < n;) {
    w += advanceOf(nextChar(s, &i));
    if (i < n) w += track;
  }
  int left = align == 0 ? x : align == 1 ? x - w / 2 : x - w;
  if (pad > 0) {
    int pl = align == 0 ? x : align == 1 ? x - pad / 2 : x - pad;
    tft.fillRect(pl, top, pad, tft.fontHeight(), bg);
  }
  tft.setTextColor(fg, bg, true);
  tft.setTextDatum(TL_DATUM);
  char one[4];
  int cx = left;
  for (int i = 0; i < n;) {
    int st = i;
    uint16_t u = nextChar(s, &i);
    memcpy(one, s + st, i - st);
    one[i - st] = 0;
    if (u != ' ') tft.drawString(one, cx, top);
    cx += advanceOf(u) + track;
  }
  return w;
}

// Word-wrapped text (current font), lines `lh` apart, at most maxLines.
static int textWrap(const char *s, int x, int y, int w, int lh, uint16_t fg, uint16_t bg,
                    int maxLines) {
  char line[96] = "", trial[96];
  int lines = 0;
  const char *p = s;
  while (*p && lines < maxLines) {
    const char *e = p;
    while (*e && *e != ' ') e++;
    snprintf(trial, sizeof(trial), "%s%s%.*s", line, line[0] ? " " : "", (int)(e - p), p);
    if (line[0] && tft.textWidth(trial) > w) {
      text(line, x, y + lines * lh, TL_DATUM, fg, bg, w);
      lines++;
      snprintf(line, sizeof(line), "%.*s", (int)(e - p), p);
    } else {
      strlcpy(line, trial, sizeof(line));
    }
    p = *e ? e + 1 : e;
  }
  if (line[0] && lines < maxLines) text(line, x, y + (lines++) * lh, TL_DATUM, fg, bg, w);
  return lines;
}

// Hero number with its unit ("82.4" + "kg"), centred on cx, number baseline at
// `base`. Five-glyph values ("100.0", most lb values) switch to the smaller
// `nfLong` so they still fit inside the ring. The band [cx-band/2, cx+band/2] x
// the number's height is fully repainted (glyph boxes + slack), so a live value
// updates in place without a flash.
static void heroNumber(const char *num, const char *unit, int cx, int base,
                       const uint8_t *nf, const uint8_t *nfLong, const uint8_t *uf,
                       uint16_t fg, uint16_t ufg, uint16_t bg, int band) {
  // Band height comes from the big font so a shorter long-value line still
  // erases a previous big one.
  useFont(nf);
  int top = base - tft.gFont.maxAscent, hn = tft.fontHeight(), bot = top + hn;
  if (strlen(num) >= 5) useFont(nfLong);
  int wn = tft.textWidth(num), nt = base - tft.gFont.maxAscent, nh = tft.fontHeight();
  useFont(uf);
  int wu = tft.textWidth(unit), au = tft.gFont.maxAscent, hu = tft.fontHeight();
  const int gap = 5;
  int total = wn + gap + wu;
  int left = cx - total / 2;
  int bl = cx - band / 2, br = cx + band / 2;
  if (left > bl) tft.fillRect(bl, top, left - bl, hn, bg);
  if (left + total < br) tft.fillRect(left + total, top, br - left - total, hn, bg);
  // Number (its glyph boxes cover nt..nt+nh; fill the rest of the band above/below).
  if (nt > top) tft.fillRect(left, top, wn, nt - top, bg);
  if (nt + nh < bot) tft.fillRect(left, nt + nh, wn, bot - nt - nh, bg);
  useFont(strlen(num) >= 5 ? nfLong : nf);
  text(num, left, nt, TL_DATUM, fg, bg);
  // Gap, then the unit on the same baseline.
  int ux = left + wn + gap, ut = base - au;
  tft.fillRect(left + wn, top, gap, hn, bg);
  if (ut > top) tft.fillRect(ux, top, wu, ut - top, bg);
  if (ut + hu < bot) tft.fillRect(ux, ut + hu, wu, bot - ut - hu, bg);
  useFont(uf);
  text(unit, ux, ut, TL_DATUM, ufg, bg);
}

// ── Vector icons ────────────────────────────────────────────────────────────
// A tiny anti-aliased rasteriser: icons are lists of stroked segments / arcs and
// filled discs / rounded boxes (signed-distance coverage), rendered into a small
// buffer over a flat background and pushed in one go. Coordinates are given in
// the icon's SVG viewBox units and scaled.
enum { SH_SEG, SH_ARC, SH_DISC, SH_RBOX };
struct Shp {
  uint8_t k;
  uint16_t col;
  float a, b, c, d, e, f;
  float x0, y0, x1, y1;   // bounding box (screen px), for a quick reject
};
struct Vec {
  float s, ox, oy;
  uint16_t col;
  int n = 0;
  Shp sh[16];
  Vec(float scale, float x, float y, uint16_t c) : s(scale), ox(x), oy(y), col(c) {}
  Shp &add(uint8_t k, float x0, float y0, float x1, float y1) {
    Shp &p = sh[n < 15 ? n++ : n];
    p.k = k; p.col = col;
    p.x0 = x0 - 1; p.y0 = y0 - 1; p.x1 = x1 + 1; p.y1 = y1 + 1;
    return p;
  }
  float X(float v) const { return ox + v * s; }
  float Y(float v) const { return oy + v * s; }
  void seg(float x1, float y1, float x2, float y2, float w) {
    float hw = w * s / 2;
    Shp &p = add(SH_SEG, fminf(X(x1), X(x2)) - hw, fminf(Y(y1), Y(y2)) - hw,
                 fmaxf(X(x1), X(x2)) + hw, fmaxf(Y(y1), Y(y2)) + hw);
    p.a = X(x1); p.b = Y(y1); p.c = X(x2); p.d = Y(y2); p.e = hw;
  }
  void poly(const float *pt, int npts, float w) {
    for (int i = 0; i + 1 < npts; i++) seg(pt[2 * i], pt[2 * i + 1], pt[2 * i + 2], pt[2 * i + 3], w);
  }
  // Arc: degrees, 0 = 12 o'clock, clockwise; round caps.
  void arc(float cx, float cy, float r, float start, float sweep, float w) {
    float hw = w * s / 2, R = r * s;
    Shp &p = add(SH_ARC, X(cx) - R - hw, Y(cy) - R - hw, X(cx) + R + hw, Y(cy) + R + hw);
    p.a = X(cx); p.b = Y(cy); p.c = R; p.d = start; p.e = sweep; p.f = hw;
  }
  void circle(float cx, float cy, float r, float w) { arc(cx, cy, r, 0, 360, w); }
  void disc(float cx, float cy, float r) {
    float R = r * s;
    Shp &p = add(SH_DISC, X(cx) - R, Y(cy) - R, X(cx) + R, Y(cy) + R);
    p.a = X(cx); p.b = Y(cy); p.c = R;
  }
  // Rounded box; w = stroke width, or 0 for filled.
  void rbox(float x, float y, float bw, float bh, float r, float w) {
    float hw = w * s / 2;
    Shp &p = add(SH_RBOX, X(x) - hw, Y(y) - hw, X(x + bw) + hw, Y(y + bh) + hw);
    p.a = X(x + bw / 2); p.b = Y(y + bh / 2); p.c = bw * s / 2; p.d = bh * s / 2;
    p.e = r * s; p.f = hw;
  }
};

static float shapeCover(const Shp &p, float x, float y) {
  if (x < p.x0 || x > p.x1 || y < p.y0 || y > p.y1) return 0;
  switch (p.k) {
    case SH_SEG: {
      float vx = p.c - p.a, vy = p.d - p.b, wx = x - p.a, wy = y - p.b;
      float l2 = vx * vx + vy * vy;
      float t = l2 > 0 ? clamp01((wx * vx + wy * vy) / l2) : 0;
      float dx = wx - t * vx, dy = wy - t * vy;
      return clamp01(p.e + 0.5f - sqrtf(dx * dx + dy * dy));
    }
    case SH_ARC: {
      float dx = x - p.a, dy = y - p.b;
      float dist = sqrtf(dx * dx + dy * dy);
      float ang = atan2f(dx, -dy) * (180.0f / PI);
      if (ang < 0) ang += 360;
      float rel = fmodf(ang - p.d + 720.0f, 360.0f);
      float d;
      if (p.e >= 360 || rel <= p.e) {
        d = fabsf(dist - p.c);
      } else {   // beyond the ends: distance to the nearer cap centre
        float a0 = p.d * (PI / 180), a1 = (p.d + p.e) * (PI / 180);
        float ex0 = p.a + p.c * sinf(a0) - x, ey0 = p.b - p.c * cosf(a0) - y;
        float ex1 = p.a + p.c * sinf(a1) - x, ey1 = p.b - p.c * cosf(a1) - y;
        d = sqrtf(fminf(ex0 * ex0 + ey0 * ey0, ex1 * ex1 + ey1 * ey1));
      }
      return clamp01(p.f + 0.5f - d);
    }
    case SH_DISC: {
      float dx = x - p.a, dy = y - p.b;
      return clamp01(p.c + 0.5f - sqrtf(dx * dx + dy * dy));
    }
    case SH_RBOX: {
      float qx = fabsf(x - p.a) - (p.c - p.e), qy = fabsf(y - p.b) - (p.d - p.e);
      float mx = fmaxf(qx, 0), my = fmaxf(qy, 0);
      float sd = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0) - p.e;
      return p.f > 0 ? clamp01(p.f + 0.5f - fabsf(sd)) : clamp01(0.5f - sd);
    }
  }
  return 0;
}

static uint16_t vbuf[2048];   // shared pixel buffer (icons, ring spans, chart columns)

static void vecDraw(const Vec &v, int x, int y, int w, int h, uint16_t bg) {
  int rows = (int)(sizeof(vbuf) / sizeof(vbuf[0])) / w;
  for (int y0 = 0; y0 < h; y0 += rows) {
    int hh = (h - y0 < rows) ? h - y0 : rows;
    for (int j = 0; j < hh; j++)
      for (int i = 0; i < w; i++) {
        uint16_t c = bg;
        float px = x + i + 0.5f, py = y + y0 + j + 0.5f;
        for (int k = 0; k < v.n; k++) {
          float a = shapeCover(v.sh[k], px, py);
          if (a > 0) c = mix(v.sh[k].col, c, a);
        }
        vbuf[j * w + i] = c;
      }
    tft.pushImage(x, y + y0, w, hh, vbuf);
  }
}

enum Icon { IC_HOME, IC_TRENDS, IC_SETTINGS, IC_WIFI, IC_DOWN, IC_UP, IC_FLAT, IC_CHECK,
            IC_PERSON, IC_BT, IC_CLOUD, IC_RULER, IC_SPEAKER, IC_TOUCH, IC_SCALE,
            IC_DOWNLOAD, IC_MOON, IC_MINUS, IC_PLUS };

// Draw a 24-unit line icon at (x, y), `size` px square.
static void icon(int id, int x, int y, int size, uint16_t fg, uint16_t bg) {
  Vec v(size / 24.0f, x, y, fg);
  switch (id) {
    case IC_HOME: {
      static const float p[] = {4, 11, 12, 4, 20, 11, 20, 20, 15, 20, 15, 14, 9, 14, 9, 20, 4, 20, 4, 11};
      v.poly(p, 10, 2);
    } break;
    case IC_TRENDS: {
      static const float a[] = {3, 17, 9, 11, 13, 15, 21, 7}, b[] = {15, 7, 21, 7, 21, 13};
      v.poly(a, 4, 2); v.poly(b, 3, 2);
    } break;
    case IC_SETTINGS:
      v.seg(4, 7, 13, 7, 2); v.seg(19, 7, 20, 7, 2); v.seg(4, 17, 7, 17, 2); v.seg(13, 17, 20, 17, 2);
      v.circle(16, 7, 2.5f, 2); v.circle(10, 17, 2.5f, 2);
      break;
    case IC_WIFI:
      v.arc(12, 20.18f, 15, 318.2f, 83.6f, 2); v.arc(12, 20.1f, 10, 319.5f, 81, 2);
      v.arc(12, 20, 5, 323.1f, 73.8f, 2); v.disc(12, 19.5f, 1.5f);
      break;
    case IC_DOWN: {
      static const float p[] = {6, 13, 12, 19, 18, 13};
      v.seg(12, 5, 12, 19, 3); v.poly(p, 3, 3);
    } break;
    case IC_UP: {
      static const float p[] = {6, 11, 12, 5, 18, 11};
      v.seg(12, 5, 12, 19, 3); v.poly(p, 3, 3);
    } break;
    case IC_FLAT: {
      static const float p[] = {13, 6, 19, 12, 13, 18};
      v.seg(5, 12, 19, 12, 3); v.poly(p, 3, 3);
    } break;
    case IC_CHECK: {
      static const float p[] = {5, 12, 10, 17, 19, 7};
      v.poly(p, 3, 3);
    } break;
    case IC_PERSON:
      v.circle(12, 8, 4, 2.4f); v.arc(12, 21, 8, 270, 180, 2.4f);
      break;
    case IC_BT: {
      static const float p[] = {7, 7, 17, 17, 12, 22, 12, 2, 17, 7, 7, 17};
      v.poly(p, 6, 2);
    } break;
    case IC_CLOUD:
      v.arc(6.7f, 13.02f, 5, 176.6f, 180, 2); v.arc(12.08f, 9.97f, 6, 288.8f, 151.9f, 2);
      v.arc(17.75f, 13.5f, 4.5f, 3.2f, 180, 2); v.seg(17.5f, 18, 7, 18, 2);
      break;
    case IC_RULER:
      v.rbox(2, 8, 20, 8, 1.5f, 2);
      v.seg(6, 8, 6, 11, 2); v.seg(10, 8, 10, 12, 2); v.seg(14, 8, 14, 11, 2); v.seg(18, 8, 18, 12, 2);
      break;
    case IC_SPEAKER: {
      static const float p[] = {11, 5, 6, 9, 3, 9, 3, 15, 6, 15, 11, 19, 11, 5};
      v.poly(p, 7, 2); v.arc(11.93f, 12, 5, 45.6f, 88.8f, 2); v.arc(12.28f, 12, 9, 43.7f, 92.6f, 2);
    } break;
    case IC_TOUCH:
      v.circle(12, 12, 6, 2);
      v.seg(12, 2, 12, 6, 2); v.seg(12, 18, 12, 22, 2); v.seg(2, 12, 6, 12, 2); v.seg(18, 12, 22, 12, 2);
      break;
    case IC_SCALE:
      v.rbox(3, 4, 18, 16, 4, 2); v.arc(12, 12.15f, 4, 311.5f, 97, 2); v.seg(12, 9.5f, 13.2f, 8, 2);
      break;
    case IC_DOWNLOAD: {
      static const float p[] = {7, 10, 12, 15, 17, 10};
      v.seg(12, 4, 12, 15, 2); v.poly(p, 3, 2); v.seg(5, 20, 19, 20, 2);
    } break;
    case IC_MOON:
      v.arc(12.64f, 11.36f, 8, 113.1f, 223.8f, 2); v.arc(16.86f, 7.14f, 8, 156.9f, 136.2f, 2);
      break;
    case IC_MINUS:
      v.seg(5, 12, 19, 12, 2.6f);
      break;
    case IC_PLUS:
      v.seg(5, 12, 19, 12, 2.6f); v.seg(12, 5, 12, 19, 2.6f);
      break;
  }
  vecDraw(v, x, y, size, size, bg);
}

// The rail logo: a small halo ring (22 px).
static void drawLogo(int x, int y) {
  Vec v(1, x, y, C_LINE);
  v.circle(11, 11, 8, 3);
  v.col = C_MINT;
  v.arc(11, 11, 8, 0, 252, 3);
  vecDraw(v, x, y, 22, 22, C_INK);
}

// ── Halo ring renderer ──────────────────────────────────────────────────────
// Rings are composited per pixel (track, progress arcs with round caps and a
// mint→cyan diagonal gradient, glow + end dots) and pushed as horizontal spans
// covering only the annulus [rIn, rOut], so whatever is drawn inside the ring
// (the number) is never touched.
struct RingArc {
  float r, hw, start, sweep;   // centre radius, half width, degrees (0 = top, cw)
  uint16_t c0, c1;             // c0 → c1 diagonal gradient (c0 == c1 = flat)
  float alpha;
  bool caps;
};
struct RingDot {
  float ang, r, rad;           // position (deg, radius) and dot radius
  uint16_t col;
  float alpha;
};

static void drawHalo(int cx, int cy, float rIn, float rOut, const RingArc *arcs, int na,
                     const RingDot *dots, int nd, float gradR, uint16_t bg = C_INK) {
  float dpx[6], dpy[6];
  for (int k = 0; k < nd && k < 6; k++) {
    float a = dots[k].ang * (PI / 180);
    dpx[k] = cx + dots[k].r * sinf(a);
    dpy[k] = cy - dots[k].r * cosf(a);
  }
  float capx[8][2], capy[8][2];
  for (int k = 0; k < na && k < 8; k++)
    for (int e = 0; e < 2; e++) {
      float a = (arcs[k].start + (e ? arcs[k].sweep : 0)) * (PI / 180);
      capx[k][e] = cx + arcs[k].r * sinf(a);
      capy[k][e] = cy - arcs[k].r * cosf(a);
    }
  int r0 = (int)ceilf(rOut);
  for (int y = cy - r0; y <= cy + r0; y++) {
    float fy = y + 0.5f - cy;
    if (fabsf(fy) > rOut) continue;
    float xo = sqrtf(rOut * rOut - fy * fy);
    float xi = fabsf(fy) < rIn ? sqrtf(rIn * rIn - fy * fy) : -1;
    for (int side = 0; side < (xi >= 0 ? 2 : 1); side++) {
      int xa, xb;
      if (xi < 0) { xa = (int)floorf(cx - xo); xb = (int)ceilf(cx + xo); }
      else if (side == 0) { xa = (int)floorf(cx - xo); xb = (int)floorf(cx - xi); }
      else { xa = (int)ceilf(cx + xi); xb = (int)ceilf(cx + xo); }
      int w = xb - xa;
      if (w <= 0) continue;
      for (int i = 0; i < w; i++) {
        float px = xa + i + 0.5f, dx = px - cx;
        float dist = sqrtf(dx * dx + fy * fy);
        float ang = -1;                 // computed lazily: only pixels on an arc need it
        uint16_t c = bg;
        for (int k = 0; k < na; k++) {
          const RingArc &A = arcs[k];
          float d = fabsf(dist - A.r);
          if (d > A.hw + 1) continue;   // off this arc's stroke (caps lie within it too)
          if (ang < 0) {
            ang = atan2f(dx, -fy) * (180.0f / PI);
            if (ang < 0) ang += 360;
          }
          float rel = ang - A.start;
          while (rel < 0) rel += 360;
          while (rel >= 360) rel -= 360;
          if (!(A.sweep >= 360 || rel <= A.sweep)) {
            if (!A.caps) continue;
            float e0x = capx[k][0] - px, e0y = capy[k][0] - (y + 0.5f);
            float e1x = capx[k][1] - px, e1y = capy[k][1] - (y + 0.5f);
            d = sqrtf(fminf(e0x * e0x + e0y * e0y, e1x * e1x + e1y * e1y));
          }
          float cov = clamp01(A.hw + 0.5f - d) * A.alpha;
          if (cov <= 0) continue;
          uint16_t col = A.c0;
          if (A.c1 != A.c0) {
            float t = clamp01((dx + gradR + fy + gradR) / (4 * gradR));
            col = mix(A.c1, A.c0, t);
          }
          c = mix(col, c, cov);
        }
        for (int k = 0; k < nd; k++) {
          float ex = px - dpx[k], ey = y + 0.5f - dpy[k];
          float cov = clamp01(dots[k].rad + 0.5f - sqrtf(ex * ex + ey * ey)) * dots[k].alpha;
          if (cov > 0) c = mix(dots[k].col, c, cov);
        }
        vbuf[i] = c;
      }
      tft.pushImage(xa, y, w, 1, vbuf);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Data helpers
// ════════════════════════════════════════════════════════════════════════════
static const char *DAY_NAMES[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};
static const char *MON_NAMES[] = {"January", "February", "March", "April", "May", "June",
                                  "July", "August", "September", "October", "November",
                                  "December"};

static bool localTm(time_t t, struct tm &tm) {
  if (t < 1700000000) return false;
  localtime_r(&t, &tm);
  return true;
}
static void fmtHM(time_t t, char *out, size_t n) {
  struct tm tm;
  if (localTm(t, tm)) snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
  else snprintf(out, n, "--:--");
}
static void fmtDayMon(uint32_t ts, char *out, size_t n) {   // "21 Sep"
  struct tm tm;
  if (localTm(ts, tm)) snprintf(out, n, "%d %.3s", tm.tm_mday, MON_NAMES[tm.tm_mon]);
  else snprintf(out, n, "--");
}

// Is moving by `delta` kg good news? Toward the goal if one is set, else down.
static bool goodDir(float delta) {
  if (setGoalKg > 0 && historyCount()) return (setGoalKg - historyTrendLast()) * delta > 0;
  return delta < 0;
}

// Goal progress 0..1: how far the latest weight has come from the first
// weigh-in toward the goal (the whole journey; changing the goal never resets it).
static float goalProgress() {
  if (setGoalKg <= 0 || historyCount() == 0) return -1;
  float start = historyAt(0).kg;
  float cur = historyLast().kg;
  float span = start - setGoalKg;
  if (fabsf(span) < 0.05f) return 1;
  return clamp01((start - cur) / span);
}

// "1.8" — absolute value, one decimal, in display units.
static void fmtAbs(float kg, char *out, size_t n) { snprintf(out, n, "%.1f", fabsf(toDisp(kg))); }

// ════════════════════════════════════════════════════════════════════════════
// Rail
// ════════════════════════════════════════════════════════════════════════════
static void drawRailButton(int i) {
  static const int ICONS[3] = {IC_HOME, IC_TRENDS, IC_SETTINGS};
  static const char *NAMES[3] = {"Home", "Trends", "Settings"};
  bool sel = (mode == MODE_RAIL && screen == i);
  int x = RAIL_BTN_X, y = RAIL_BTN_Y0 + i * RAIL_BTN_PITCH;
  uint16_t bg = sel ? C_MINTDIM : C_INK, fg = sel ? C_MINT : C_TEXT3;
  if (sel) tft.fillSmoothRoundRect(x, y, RAIL_BTN_W, RAIL_BTN_H, 14, bg, C_INK);
  else tft.fillRect(x, y, RAIL_BTN_W, RAIL_BTN_H, C_INK);
  icon(ICONS[i], x + (RAIL_BTN_W - 22) / 2, y + 10, 22, fg, bg);
  useFont(F_I10);
  text(NAMES[i], x + RAIL_BTN_W / 2, y + 37, TC_DATUM, fg, bg);
}

static void drawRail() {
  tft.fillRect(0, 0, RAIL_W - 1, SCR_H, C_INK);
  tft.drawFastVLine(RAIL_W - 1, 0, SCR_H, C_EDGE);
  drawLogo(21, 16);
  for (int i = 0; i < 3; i++) drawRailButton(i);
}

// ════════════════════════════════════════════════════════════════════════════
// Home
// ════════════════════════════════════════════════════════════════════════════
static const int HOME_CX = MAIN_X + 106, HOME_CY = 177;   // ring centre
static const int HOME_COL_X = MAIN_X + 212 + 22, HOME_COL_R = SCR_W - 20;

static void drawHomeHeaderLeft() {
  struct tm tm;
  bool ok = localTm(nowEpoch(), tm);
  const char *greet = !ok ? "Hello" : tm.tm_hour < 12 ? "Good morning"
                      : tm.tm_hour < 18 ? "Good afternoon" : "Good evening";
  char date[40];
  if (ok) snprintf(date, sizeof(date), "%s, %d %s", DAY_NAMES[tm.tm_wday], tm.tm_mday,
                   MON_NAMES[tm.tm_mon]);
  else snprintf(date, sizeof(date), "Clock not set yet");
  useFont(F_I15B);
  text(greet, MAIN_X, 14, TL_DATUM, C_TEXT, C_INK, 200);
  useFont(F_I13);
  text(date, MAIN_X, 34, TL_DATUM, C_TEXT2, C_INK, 200);
}

// Top-right status cluster: Wi-Fi · clock (right-aligned to the edge).
static int homeClockX = 0;   // left edge of the clock slot (set by drawHomeClock)
static void drawHomeClock() {
  char hm[8];
  fmtHM(nowEpoch(), hm, sizeof(hm));
  useFont(F_O18);
  int slot = tft.textWidth("00:00") + 2;
  homeClockX = HOME_COL_R - slot;
  text(hm, HOME_COL_R, 25, MR_DATUM, C_TEXT, C_INK, slot);
}
static void drawHomeWifi() {
  if (!wifiSavedSsid()[0]) return;     // never set up: no icon
  icon(IC_WIFI, homeClockX - 10 - 16, 17, 16, syncing ? C_CYAN : C_TEXT2, C_INK);
}

// Ring + everything inside it.
static void drawHomeRing() {
  float p = goalProgress();
  RingArc arcs[2] = {
      {98, 5, 0, 360, C_TRACK, C_TRACK, 1, false},
      {98, 5, 0, p > 0 ? p * 360 : 0, C_MINT, C_CYAN, 1, true},
  };
  RingDot dots[4] = {
      {0, 98, 2.5f, C_TEXT3, 1},                    // start tick
      {p * 360, 98, 13, C_CYAN, A_GLOW},             // end glow
      {p * 360, 98, 8, C_INK, 1},                   // end dot (black rim)
      {p * 360, 98, 6, C_TEXT, 1},
  };
  bool hasArc = p > 0.001f;
  drawHalo(HOME_CX, HOME_CY, 84, 111, arcs, hasArc ? 2 : 1, dots, hasArc ? 4 : 0, 98);
}

static void drawHomeCenter() {
  int n = historyCount();
  char lab[24] = "NO WEIGH-INS YET", num[12] = "--.-";
  if (n) {
    Reading r = historyLast();
    snprintf(num, sizeof(num), "%.1f", toDisp(r.kg));
    long dToday = historyDay(nowEpoch()), d = historyDay(r.ts);
    struct tm tm;
    if (d < 0 || dToday < 0) snprintf(lab, sizeof(lab), "LAST WEIGH-IN");
    else if (d == dToday) {
      localTm(r.ts, tm);
      snprintf(lab, sizeof(lab), tm.tm_hour < 12 ? "THIS MORNING" : "TODAY");
    } else if (d == dToday - 1) snprintf(lab, sizeof(lab), "YESTERDAY");
    else {
      localTm(r.ts, tm);
      snprintf(lab, sizeof(lab), "LAST \xC2\xB7 %d %.3s", tm.tm_mday, MON_NAMES[tm.tm_mon]);
      for (char *c = lab; *c; c++) *c = toupper((unsigned char)*c);
    }
  }
  useFont(F_I11B);
  label(lab, HOME_CX, 119, 1, C_TEXT2, C_INK, 140);
  if (n) {
    heroNumber(num, unitStr(), HOME_CX, 186, F_O62, F_O52, F_I15B, C_TEXT, C_TEXT2, C_INK, 160);
  } else {
    tft.fillRect(HOME_CX - 80, 136, 160, 64, C_INK);
    useFont(F_I15B);
    text("Step on the scale", HOME_CX, 168, MC_DATUM, C_TEXT2, C_INK);
  }

  // Week pill: trend change over the last 7 days.
  tft.fillRect(HOME_CX - 70, 206, 140, 28, C_INK);   // stays inside the ring's hole
  float wk = n ? historyTrendLast() - historyTrendDaysAgo(7, nowEpoch()) : NAN;
  if (isnan(wk)) return;
  char t[32], a[8];
  int ic = IC_FLAT;
  if (fabsf(toDisp(wk)) < 0.05f) snprintf(t, sizeof(t), "Steady this week");
  else {
    fmtAbs(wk, a, sizeof(a));
    snprintf(t, sizeof(t), "%s %s this week", a, unitStr());
    ic = wk < 0 ? IC_DOWN : IC_UP;
  }
  bool good = goodDir(wk) && ic != IC_FLAT;
  uint16_t bg = good ? C_MINTDIM : C_CHIP, fg = good ? C_MINT : C_TEXT2;
  useFont(F_I11B);
  int tw = tft.textWidth(t), w = 10 + 12 + 5 + tw + 10;
  if (w > 140) w = 140;
  int x = HOME_CX - w / 2;
  tft.fillSmoothRoundRect(x, 208, w, 25, 12, bg, C_INK);
  icon(ic, x + 10, 208 + 6, 12, fg, bg);
  text(t, x + 27, 208 + 13, ML_DATUM, fg, bg);
}

static void drawHomeColumn() {
  const int x = HOME_COL_X, w = HOME_COL_R - HOME_COL_X;
  uint32_t now = nowEpoch();
  int n = historyCount();
  tft.fillRect(x, 52, w + 1, 250, C_INK);
  char v[24], s[48], a[8];

  // GOAL
  useFont(F_I11B);
  label("GOAL", x, 58, 0, C_TEXT2, C_INK);
  if (setGoalKg > 0) {
    snprintf(v, sizeof(v), "%.1f", toDisp(setGoalKg));
    useFont(F_O26);
    int vw = text(v, x, 94, L_BASELINE, C_TEXT, C_INK);
    useFont(F_I13);
    text(unitStr(), x + vw + 4, 94, L_BASELINE, C_TEXT2, C_INK);
    float p = goalProgress();
    if (n && p >= 1) {
      text("Goal reached", x, 102, TL_DATUM, C_MINT, C_INK);
    } else if (n) {
      fmtAbs(historyLast().kg - setGoalKg, a, sizeof(a));
      snprintf(s, sizeof(s), "%s %s to go \xC2\xB7 %d%%", a, unitStr(), (int)(p * 100 + 0.5f));
      text(s, x, 102, TL_DATUM, C_TEXT2, C_INK);
    }
  } else {
    useFont(F_I15B);
    text("Not set", x, 76, TL_DATUM, C_TEXT2, C_INK);
    useFont(F_I13);
    text("Set it in Settings", x, 102, TL_DATUM, C_TEXT3, C_INK);
  }
  tft.drawFastHLine(x, 126, w, C_LINE);

  // STREAK
  useFont(F_I11B);
  label("STREAK", x, 139, 0, C_TEXT2, C_INK);
  int streak = historyStreak(now);
  snprintf(v, sizeof(v), "%d", streak);
  useFont(F_O26);
  int vw = text(v, x, 175, L_BASELINE, C_TEXT, C_INK);
  useFont(F_I13);
  text(streak == 1 ? "day" : "days", x + vw + 4, 175, L_BASELINE, C_TEXT2, C_INK);
  uint8_t mask = historyWeekMask(now);
  for (int i = 0; i < 7; i++) {
    int cx = x + 5 + i * 16, cy = 193;
    bool on = mask & (1 << i);
    if (i == 6) {
      if (on) {
        tft.fillSmoothCircle(cx + 1, cy, 9, mix(C_MINT, C_INK, A_GLOW), C_INK);
        tft.fillSmoothCircle(cx + 1, cy, 6, C_MINT, mix(C_MINT, C_INK, A_GLOW));
      } else {
        tft.fillSmoothCircle(cx + 1, cy, 6, C_LINE, C_INK);
      }
    } else {
      tft.fillSmoothCircle(cx, cy, 5, on ? C_DOT : C_TRACK, C_INK);
    }
  }
  tft.drawFastHLine(x, 212, w, C_LINE);

  // 30 DAYS sparkline (trend weight)
  useFont(F_I11B);
  label("30 DAYS", x, 225, 0, C_TEXT2, C_INK);
  float d30 = n ? historyTrendLast() - historyTrendDaysAgo(30, now) : NAN;
  if (n && isnan(d30) && n >= 2) d30 = historyTrendLast() - historyTrendAt(0);
  if (!isnan(d30)) {
    fmtAbs(d30, a, sizeof(a));
    snprintf(s, sizeof(s), "%s%s %s", fabsf(toDisp(d30)) < 0.05f ? "" : d30 < 0 ? "−" : "+",
             a, unitStr());
    text(s, HOME_COL_R, 225, TR_DATUM, goodDir(d30) ? C_MINT : C_TEXT2, C_INK);
  }
  const int sy0 = 246, sy1 = 282;
  int i0 = n;
  if (timeIsSet()) {
    while (i0 > 0 && historyAt(i0 - 1).ts + 30UL * 86400 >= now) i0--;
  } else {
    i0 = n > 30 ? n - 30 : 0;
  }
  if (i0 > 0) i0--;                          // include the trend value entering the window
  if (n - i0 < 2) {
    useFont(F_I11);
    text("Weigh in daily to see a trend", x, sy0 + 12, TL_DATUM, C_TEXT3, C_INK);
    return;
  }
  float mn = 1e9f, mx = -1e9f;
  for (int i = i0; i < n; i++) {
    float t = historyTrendAt(i);
    mn = fminf(mn, t); mx = fmaxf(mx, t);
  }
  if (mx - mn < 0.5f) { float m = (mx + mn) / 2; mn = m - 0.25f; mx = m + 0.25f; }
  uint32_t t0 = timeIsSet() ? now - 30UL * 86400 : 0;
  float px = 0, py = 0;
  for (int i = i0; i < n; i++) {
    float fx = timeIsSet() ? x + (float)((long)historyAt(i).ts - (long)t0) * w / (30.0f * 86400)
                           : x + (float)(i - i0) * w / (n - 1 - i0);
    if (fx < x) fx = x;
    float fy = sy0 + (mx - historyTrendAt(i)) * (sy1 - sy0) / (mx - mn);
    if (i > i0) tft.drawWideLine(px, py, fx, fy, 2, C_MINT, C_INK);
    px = fx; py = fy;
  }
}

// BMI pill under the goal ring: "BMI 24.1". Mint in the healthy band (18.5-25),
// amber outside it (the Halo palette has no red). No height: a quiet hint instead.
static void drawHomeBmi() {
  const int cy = HOME_CY + 111 + 15;               // just below the ring
  tft.fillRect(HOME_CX - 115, cy - 13, 230, 27, C_INK);
  if (!historyCount()) return;
  char t[40];
  useFont(F_I11B);
  if (setHeightCm <= 0) {
    text("Add your height in Settings for BMI", HOME_CX, cy, MC_DATUM, C_TEXT3, C_INK);
    return;
  }
  float m = setHeightCm / 100.0f;
  float bmi = historyLast().kg / (m * m);
  bool healthy = bmi >= 18.5f && bmi < 25.0f;    // colour only — no category text
  snprintf(t, sizeof(t), "BMI %.1f", bmi);
  uint16_t bg = healthy ? C_MINTDIM : C_CHIP, fg = healthy ? C_MINT : C_AMBER;
  int w = tft.textWidth(t) + 24;
  tft.fillSmoothRoundRect(HOME_CX - w / 2, cy - 12, w, 25, 12, bg, C_INK);
  text(t, HOME_CX, cy + 1, MC_DATUM, fg, bg);
}

static void drawHome() {
  drawHomeHeaderLeft();
  drawHomeClock();
  drawHomeWifi();
  drawHomeRing();
  drawHomeCenter();
  drawHomeColumn();
  drawHomeBmi();
}

// ════════════════════════════════════════════════════════════════════════════
// Trends
// ════════════════════════════════════════════════════════════════════════════
static const int CH_X0 = 96, CH_X1 = 456, CH_YT = 112, CH_YB = 232, CH_BASE = 244;

static void drawRangeControl() {
  int w = 4 * SEG_BW + 4, h = SEG_BH + 4;
  tft.fillSmoothRoundRect(SEG_X, SEG_Y, w + 2, h + 2, 12, C_LINE, C_INK);
  tft.fillSmoothRoundRect(SEG_X + 1, SEG_Y + 1, w, h, 11, C_PANEL, C_LINE);
  useFont(F_I11B);
  for (int i = 0; i < 4; i++) {
    int bx = SEG_X + 3 + i * SEG_BW, by = SEG_Y + 3;
    bool sel = i == rangeIdx;
    uint16_t bg = sel ? C_LINE : C_PANEL;
    if (sel) tft.fillSmoothRoundRect(bx, by, SEG_BW, SEG_BH, 10, bg, C_PANEL);
    else tft.fillRect(bx, by, SEG_BW, SEG_BH, C_PANEL);
    text(RANGE_NAMES[i], bx + SEG_BW / 2, by + SEG_BH / 2, MC_DATUM, sel ? C_TEXT : C_TEXT2, bg);
  }
}

// Dashed-outline pill "Goal 78.0 kg ↓" (right-aligned), shown when the goal is
// outside the chart's range.
static void drawGoalPill(bool below) {
  char s[24];
  snprintf(s, sizeof(s), "Goal %.1f %s", toDisp(setGoalKg), unitStr());
  useFont(F_I11);
  int tw = tft.textWidth(s), w = 9 + tw + 5 + 11 + 9, h = 22;
  int x = CH_X1 - w, y = 116;
  tft.drawSmoothRoundRect(x, y, 11, 10, w - 1, h - 1, C_DASH, C_INK);
  for (int gx = x + 12; gx < x + w - 14; gx += 6) {      // dash the straight edges
    tft.drawFastHLine(gx, y, 3, C_INK);
    tft.drawFastHLine(gx, y + h - 1, 3, C_INK);
  }
  text(s, x + 9, y + h / 2, ML_DATUM, C_TEXT2, C_INK);
  icon(below ? IC_DOWN : IC_UP, x + 9 + tw + 5, y + 5, 11, C_TEXT2, C_INK);
}

static void drawTrends() {
  useFont(F_I22B);
  text("Trends", MAIN_X, SEG_Y + (SEG_BH + 6) / 2, ML_DATUM, C_TEXT, C_INK);
  drawRangeControl();

  tft.fillRect(RAIL_W, 58, SCR_W - RAIL_W, SCR_H - 58, C_INK);
  int n = historyCount();
  uint32_t now = nowEpoch();
  int days = RANGES[rangeIdx];
  bool clocked = timeIsSet();

  // Readings in range: [i0, n).
  int i0 = 0;
  uint32_t tStart = 0;
  if (clocked) {
    if (days > 0) tStart = now - (uint32_t)days * 86400UL;
    else {
      for (int i = 0; i < n; i++)
        if (historyAt(i).ts > 1700000000UL) { tStart = historyAt(i).ts; break; }
      if (tStart == 0 || now - tStart < 86400) tStart = now - 7UL * 86400;
    }
    while (i0 < n && historyAt(i0).ts < tStart) i0++;
  } else if (days > 0 && n > days) {
    i0 = n - days;
  }
  int m = n - i0;

  if (m == 0) {
    useFont(F_I13);
    text(n ? "No weigh-ins in this range" : "No weigh-ins yet", (CH_X0 + CH_X1) / 2, 180,
         MC_DATUM, C_TEXT3, C_INK);
    return;
  }

  // Headline: latest + change of the trend over the range.
  char s[48], a[8];
  snprintf(s, sizeof(s), "%.1f", toDisp(historyLast().kg));
  useFont(F_O26);
  int x = MAIN_X + text(s, MAIN_X, 84, L_BASELINE, C_TEXT, C_INK);
  useFont(F_I13);
  x += 6 + text(unitStr(), x + 6, 84, L_BASELINE, C_TEXT2, C_INK);
  float from = (i0 > 0) ? historyTrendAt(i0 - 1) : historyTrendAt(i0);
  float dTr = historyTrendLast() - from;
  if (m >= 2 || i0 > 0) {
    fmtAbs(dTr, a, sizeof(a));
    char span[16];
    if (days > 0) snprintf(span, sizeof(span), "in %d days", days);
    else snprintf(span, sizeof(span), "overall");
    snprintf(s, sizeof(s), "%s%s %s %s", fabsf(toDisp(dTr)) < 0.05f ? "" : dTr < 0 ? "−" : "+",
             a, unitStr(), span);
    text(s, x + 12, 84, L_BASELINE, goodDir(dTr) ? C_MINT : C_TEXT2, C_INK);
  }

  // Value range over raw + trend points (display units).
  float mn = 1e9f, mx = -1e9f;
  for (int i = i0; i < n; i++) {
    float r = toDisp(historyAt(i).kg), t = toDisp(historyTrendAt(i));
    mn = fminf(mn, fminf(r, t)); mx = fmaxf(mx, fmaxf(r, t));
  }
  if (mx - mn < 1.0f) { float c = (mx + mn) / 2; mn = c - 0.5f; mx = c + 0.5f; }
  float padv = (mx - mn) * 0.08f;
  mn -= padv; mx += padv;
  auto yOf = [&](float v) { return CH_YT + (mx - v) * (CH_YB - CH_YT) / (mx - mn); };
  auto xOf = [&](int i) -> float {
    if (clocked) {
      float f = (float)((long)historyAt(i).ts - (long)tStart) / (float)(now - tStart);
      return CH_X0 + clamp01(f) * (CH_X1 - CH_X0);
    }
    return m < 2 ? CH_X1 : CH_X0 + (float)(i - i0) * (CH_X1 - CH_X0) / (m - 1);
  };

  // Grid: 3–5 lines on a "nice" step.
  static const float STEPS[] = {0.2f, 0.5f, 1, 2, 5, 10, 20};
  float step = 20;
  for (float st : STEPS) if ((mx - mn) / st <= 4.5f) { step = st; break; }
  useFont(F_I10);
  bool gridRow[CH_BASE + 1] = {false};
  for (float g = ceilf(mn / step) * step; g <= mx; g += step) {
    int gy = (int)(yOf(g) + 0.5f);
    if (gy < CH_YT - 4 || gy > CH_BASE - 4) continue;
    tft.drawFastHLine(CH_X0, gy, CH_X1 - CH_X0, C_CHIP);
    gridRow[gy] = true;
    snprintf(s, sizeof(s), step < 1 ? "%.1f" : "%.0f", g);
    text(s, CH_X0 - 6, gy, MR_DATUM, C_TEXT3, C_INK);
  }

  // Area under the trend line: mint fading to black, painted column by column.
  float areaTop = 1e9f;
  for (int i = i0; i < n; i++) areaTop = fminf(areaTop, yOf(toDisp(historyTrendAt(i))));
  int xa = (int)xOf(i0), xb = (int)xOf(n - 1);
  int seg = i0;
  for (int cx = xa; cx <= xb && m >= 2; cx++) {
    while (seg < n - 2 && xOf(seg + 1) < cx) seg++;
    float x0 = xOf(seg), x1 = xOf(seg + 1);
    float f = x1 > x0 ? clamp01((cx - x0) / (x1 - x0)) : 0;
    float ty = yOf(toDisp(historyTrendAt(seg) + f * (historyTrendAt(seg + 1) - historyTrendAt(seg))));
    int y0 = (int)ceilf(ty);
    int h = CH_BASE - y0;
    if (h <= 0) continue;
    for (int j = 0; j < h; j++) {
      int yy = y0 + j;
      float al = A_AREA * (1 - (yy - areaTop) / (CH_BASE - areaTop));
      vbuf[j] = mix(C_MINT, gridRow[yy] ? C_CHIP : C_INK, al);
    }
    tft.pushImage(cx, y0, 1, h, vbuf);
  }
  tft.drawFastHLine(CH_X0, CH_BASE, CH_X1 - CH_X0, C_LINE);

  // Goal: dashed line in range, else a pill pointing to it.
  if (setGoalKg > 0) {
    float g = toDisp(setGoalKg);
    if (g >= mn && g <= mx) {
      int gy = (int)(yOf(g) + 0.5f);
      for (int gx = CH_X0; gx < CH_X1; gx += 8) tft.drawFastHLine(gx, gy, 4, C_DASH);
      useFont(F_I10);
      text("Goal", CH_X1, gy - 2, BR_DATUM, C_TEXT3, C_INK);
    } else {
      drawGoalPill(g < mn);
    }
  }

  // Each reading's pull on the trend: a thin line from the dot to the trend at
  // the same moment — mint when it pulls toward the goal, amber when away.
  uint16_t pullGood = mix(C_MINT, C_INK, 0.55f), pullBad = mix(C_AMBER, C_INK, 0.55f);
  for (int i = i0; i < n; i++) {
    float d = historyAt(i).kg - historyTrendAt(i);
    int rx = (int)xOf(i), ry = (int)yOf(toDisp(historyAt(i).kg));
    int ty = (int)yOf(toDisp(historyTrendAt(i)));
    if (abs(ry - ty) > 3)
      tft.drawFastVLine(rx, min(ry, ty), abs(ry - ty), goodDir(d) ? pullGood : pullBad);
  }

  // Raw readings: faint dots.
  uint16_t dotCol = mix(C_TEXT3, C_INK, A_RAWDOT);
  for (int i = i0; i < n; i++) {
    float rx = xOf(i), ry = yOf(toDisp(historyAt(i).kg));
    tft.fillSmoothCircle((int)rx, (int)ry, 2, dotCol, C_INK);
  }

  // Trend line + end marker.
  float ex = xOf(n - 1), ey = yOf(toDisp(historyTrendLast()));
  tft.fillSmoothCircle((int)ex, (int)ey, 11, mix(C_MINT, C_INK, A_GLOW), C_INK);
  uint16_t lineBg = mix(C_MINT, C_INK, A_AREA * 0.55f);
  for (int i = i0 + 1; i < n; i++)
    tft.drawWideLine(xOf(i - 1), yOf(toDisp(historyTrendAt(i - 1))), xOf(i),
                     yOf(toDisp(historyTrendAt(i))), 2.5f, C_MINT, lineBg);
  tft.fillSmoothCircle((int)ex, (int)ey, 6, C_INK, mix(C_MINT, C_INK, A_GLOW));
  tft.fillSmoothCircle((int)ex, (int)ey, 4, C_TEXT, C_INK);
  snprintf(s, sizeof(s), "%.1f", toDisp(historyTrendLast()));
  useFont(F_I11B);
  int ly = ey + 24 <= CH_BASE - 2 ? (int)ey + 24 : (int)ey - 14;
  text(s, (int)ex - 10, ly, R_BASELINE, C_TEXT, C_INK);

  // Legend, top-right above the chart: • Weigh-ins  ━ Trend.
  useFont(F_I10);
  int lx = CH_X1 - text("Trend", CH_X1, 100, MR_DATUM, C_TEXT2, C_INK) - 6;
  tft.drawWideLine(lx - 14, 100, lx, 100, 2.5f, C_MINT, C_INK);
  lx -= 14 + 12;
  lx -= text("Weigh-ins", lx, 100, MR_DATUM, C_TEXT2, C_INK) + 7;
  tft.fillSmoothCircle(lx, 100, 2, dotCol, C_INK);

  // X labels.
  useFont(F_I10);
  if (clocked) {
    fmtDayMon(tStart, s, sizeof(s));
    text(s, CH_X0, 250, TL_DATUM, C_TEXT3, C_INK);
    fmtDayMon(tStart + (now - tStart) / 2, s, sizeof(s));
    text(s, (CH_X0 + CH_X1) / 2, 250, TC_DATUM, C_TEXT3, C_INK);
    text("Today", CH_X1, 250, TR_DATUM, C_TEXT3, C_INK);
  }

  // Stats: Average · Lowest · Weigh-ins.
  float sum = 0, lo = 1e9f;
  for (int i = i0; i < n; i++) { sum += historyAt(i).kg; lo = fminf(lo, historyAt(i).kg); }
  const int colW = (CH_X1 - MAIN_X + 12) / 3;
  const char *labs[3] = {"AVERAGE", "LOWEST", "WEIGH-INS"};
  for (int c = 0; c < 3; c++) {
    int sx = MAIN_X + c * colW;
    useFont(F_I10);
    label(labs[c], sx, 272, 0, C_TEXT2, C_INK);
    const char *unit = unitStr();
    char ub[20];
    if (c == 0) snprintf(s, sizeof(s), "%.1f", toDisp(sum / m));
    else if (c == 1) snprintf(s, sizeof(s), "%.1f", toDisp(lo));
    else {
      snprintf(s, sizeof(s), "%d", m);
      long span = clocked ? (long)((now - tStart) / 86400UL) + (days > 0 ? 0 : 1) : 0;
      if (span > 0) snprintf(ub, sizeof(ub), "of %ld days", span);
      else snprintf(ub, sizeof(ub), "total");
      unit = ub;
    }
    useFont(F_O18);
    int vw = text(s, sx, 303, L_BASELINE, C_TEXT, C_INK);
    useFont(F_I11);
    text(unit, sx + vw + 4, 303, L_BASELINE, C_TEXT2, C_INK);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Settings
// ════════════════════════════════════════════════════════════════════════════
// Tile frame: 1 px border, panel fill, optional icon disc. Returns text x.
static int tileFrame(int i, int ic) {
  int x = tileX(i), y = tileY(i);
  tft.fillSmoothRoundRect(x, y, TILE_W, TILE_H, 14, C_TRACK, C_INK);
  tft.fillSmoothRoundRect(x + 1, y + 1, TILE_W - 2, TILE_H - 2, 13, C_PANEL, C_TRACK);
  if (ic < 0) return x + 14;
  tft.fillSmoothCircle(x + 12 + 16, y + TILE_H / 2, 16, C_CHIP, C_PANEL);
  icon(ic, x + 12 + 16 - 9, y + TILE_H / 2 - 9, 18, C_TEXT2, C_CHIP);
  return x + 12 + 32 + 10;
}

// Two-line tile body: small label + value (optionally coloured, with a dot).
static void tileText(int i, int tx, const char *lab, const char *val, uint16_t vc,
                     int dotCol = -1) {
  int y = tileY(i);
  useFont(F_I11);
  text(lab, tx, y + TILE_LAB_Y, TL_DATUM, C_TEXT2, C_PANEL);
  int vx = tx;
  if (dotCol >= 0) {
    tft.fillSmoothCircle(tx + 3, y + TILE_VAL_Y, 3, dotCol, C_PANEL);
    vx += 13;
  }
  useFont(F_I15B);
  text(val, vx, y + TILE_VAL_Y, ML_DATUM, vc, C_PANEL);
}

// Account tile status line; returns its dot colour.
static char drawnSync[32] = "";
static int syncStatus(char *v, size_t n) {
  if (!cloudAvailable()) { snprintf(v, n, "Off"); return C_TEXT3; }
  if (cloudSignInLost()) { snprintf(v, n, "Sign in again"); return C_AMBER; }
  if (!cloudConfigured()) { snprintf(v, n, "Sign in"); return C_TEXT3; }
  if (lastPullN == -1) { snprintf(v, n, "Sync failed"); return C_AMBER; }
  if (cloudPendingCount() > 0) { snprintf(v, n, "%d to sync", cloudPendingCount()); return C_TEXT2; }
  if (lastSyncAt > 0 && timeIsSet()) {
    long m = (long)(nowEpoch() - lastSyncAt) / 60;
    if (m < 1) snprintf(v, n, "Synced now");
    else if (m < 60) snprintf(v, n, "Synced %ldm ago", m);
    else if (m < 48 * 60) snprintf(v, n, "Synced %ldh ago", m / 60);
    else snprintf(v, n, "Synced %ldd ago", m / 1440);
    return C_MINT;
  }
  snprintf(v, n, "Sync now");
  return C_TEXT3;
}

static void drawSettingTile(int i) {
  char v[32], l[40];
  int x = tileX(i), y = tileY(i);
  switch (i) {
    case MI_SYNC: {
      int tx = tileFrame(i, IC_CLOUD);
      int dot = syncStatus(drawnSync, sizeof(drawnSync));
      tileText(i, tx, "Account", drawnSync, C_TEXT, dot);
    } break;
    case MI_UNIT: {
      int tx = tileFrame(i, IC_RULER);
      useFont(F_I15B);
      text("Units", tx, y + TILE_H / 2, ML_DATUM, C_TEXT, C_PANEL);
      // kg | lb segmented pill on the right.
      int sw = 32, sx = x + TILE_W - 12 - 2 * sw - 4, sy = y + (TILE_H - 28) / 2;
      tft.fillSmoothRoundRect(sx, sy, 2 * sw + 4, 28, 9, C_CHIP, C_PANEL);
      int on = setUnitLb ? 1 : 0;
      tft.fillSmoothRoundRect(sx + 2 + on * sw, sy + 2, sw, 24, 7, C_BTNEDGE, C_CHIP);
      useFont(F_I11B);
      text("kg", sx + 2 + sw / 2, sy + 14, MC_DATUM, on ? C_TEXT2 : C_TEXT, on ? C_CHIP : C_BTNEDGE);
      text("lb", sx + 2 + sw + sw / 2, sy + 14, MC_DATUM, on ? C_TEXT : C_TEXT2, on ? C_BTNEDGE : C_CHIP);
    } break;
    case MI_SOUND: {
      static const char *NAMES[4] = {"Off", "Low", "Medium", "High"};
      int tx = tileFrame(i, IC_SPEAKER);
      useFont(F_I11);
      text("Sound", tx, y + TILE_LAB_Y, TL_DATUM, C_TEXT2, C_PANEL);
      useFont(F_I15B);
      int vw = text(NAMES[setSound], tx, y + TILE_VAL_Y, ML_DATUM, C_TEXT, C_PANEL);
      for (int b = 0; b < 3; b++) {   // volume bars
        int bh = 6 + b * 3, bx = tx + vw + 8 + b * 6;
        tft.fillRect(bx, y + TILE_VAL_Y + 6 - bh, 4, bh, b < setSound ? C_MINT : C_BTNEDGE);
      }
    } break;
    case MI_GOAL: {
      int tx = tileFrame(i, -1);
      useFont(F_I11);
      text("Goal", tx, y + TILE_LAB_Y, TL_DATUM, C_TEXT2, C_PANEL);
      if (setGoalKg > 0) {
        snprintf(v, sizeof(v), "%.0f", toDisp(setGoalKg));
        useFont(F_O18);
        int vw = text(v, tx, y + TILE_BASE_Y, L_BASELINE, C_TEXT, C_PANEL);
        useFont(F_I15B);
        text(unitStr(), tx + vw + 4, y + TILE_BASE_Y, L_BASELINE, C_TEXT, C_PANEL);
      } else {
        useFont(F_I15B);
        text("Off", tx, y + TILE_VAL_Y, ML_DATUM, C_TEXT2, C_PANEL);
      }
      for (int b = 0; b < 2; b++) {
        int bx = x + (b ? GB_PLUS_DX : GB_MINUS_DX);
        tft.fillSmoothRoundRect(bx, y + GB_DY, GB_W, 40, 10, C_CHIP, C_PANEL);
        icon(b ? IC_PLUS : IC_MINUS, bx + 12, y + GB_DY + 12, 16, C_TEXT, C_CHIP);
      }
    } break;
    case MI_HEIGHT: {
      int tx = tileFrame(i, -1);
      useFont(F_I11);
      text("Height \xC2\xB7 for BMI", tx, y + TILE_LAB_Y, TL_DATUM, C_TEXT2, C_PANEL);
      if (setHeightCm <= 0) {
        useFont(F_I15B);
        text("Off", tx, y + TILE_VAL_Y, ML_DATUM, C_TEXT2, C_PANEL);
      } else if (setUnitLb) {                     // feet + inches (Inter has ' and ")
        int in = (int)roundf(setHeightCm / 2.54f);
        snprintf(v, sizeof(v), "%d' %d\"", in / 12, in % 12);
        useFont(F_I15B);
        text(v, tx, y + TILE_VAL_Y, ML_DATUM, C_TEXT, C_PANEL);
      } else {
        snprintf(v, sizeof(v), "%.0f", setHeightCm);
        useFont(F_O18);
        int vw = text(v, tx, y + TILE_BASE_Y, L_BASELINE, C_TEXT, C_PANEL);
        useFont(F_I15B);
        text("cm", tx + vw + 4, y + TILE_BASE_Y, L_BASELINE, C_TEXT, C_PANEL);
      }
      for (int b = 0; b < 2; b++) {
        int bx = x + (b ? GB_PLUS_DX : GB_MINUS_DX);
        tft.fillSmoothRoundRect(bx, y + GB_DY, GB_W, 40, 10, C_CHIP, C_PANEL);
        icon(b ? IC_PLUS : IC_MINUS, bx + 12, y + GB_DY + 12, 16, C_TEXT, C_CHIP);
      }
    } break;
    case MI_CALIBRATE:
      tileText(i, tileFrame(i, IC_TOUCH), "Touch", "Calibrate", C_TEXT);
      break;
    case MI_SCALE: {
      const char *mac = Scale::learnedMac();
      if (mac[0]) {
        snprintf(v, sizeof(v), "%.8s", mac + 9);   // last 3 bytes
        for (char *c = v; *c; c++) *c = toupper((unsigned char)*c);
      } else snprintf(v, sizeof(v), "Pair");
      // Tapping opens the pairing screen (scan + pick); unpaired = first one is learned.
      tileText(i, tileFrame(i, IC_SCALE),
               mac[0] ? "Scale \xC2\xB7 paired" : "Scale \xC2\xB7 any", v, C_TEXT);
    } break;
    case MI_OTA:
      if (otaAutoEnabled())       // online updates: tap = check (and install) now
        tileText(i, tileFrame(i, IC_DOWNLOAD), "Firmware " FW_VERSION " \xC2\xB7 auto",
                 otaNote[0] ? otaNote : "Check now", C_TEXT);
      else                        // web upload from a computer on the same Wi-Fi
        tileText(i, tileFrame(i, IC_DOWNLOAD), "Firmware " FW_VERSION, "Update", C_TEXT);
      break;
    case MI_WIFI:
      if (wifiSavedSsid()[0]) snprintf(l, sizeof(l), "Wi-Fi \xC2\xB7 %.10s", wifiSavedSsid());
      else snprintf(l, sizeof(l), "Wi-Fi");
      tileText(i, tileFrame(i, IC_WIFI), l, "Reset", C_AMBER);
      break;
  }
}

static void drawSettings() {
  useFont(F_I22B);
  text("Settings", MAIN_X, 36, L_BASELINE, C_TEXT, C_INK);
  useFont(F_I11);
  text("Changes save automatically", SCR_W - 16, 36, R_BASELINE, C_TEXT2, C_INK);
  for (int i = 0; i < MI_COUNT; i++) drawSettingTile(i);
}

// ════════════════════════════════════════════════════════════════════════════
// Full-screen modes
// ════════════════════════════════════════════════════════════════════════════
// ── Measuring ───────────────────────────────────────────────────────────────
static const int MEAS_CX = 240, MEAS_CY = 172;
static float drawnLiveKg = -1;
static int drawnStab = -1;

static void drawMeasureClock() {
  char hm[8];
  fmtHM(nowEpoch(), hm, sizeof(hm));
  useFont(F_O18);
  text(hm, SCR_W - 20, 24, MR_DATUM, C_TEXT, C_INK, tft.textWidth("00:00") + 2);
}

// Comet: three overlapping arcs (fading cyan tail, mint head) chasing round.
static void drawComet() {
  float head = 138.7f + (millis() % 2400) * (360.0f / 2400);
  RingArc arcs[4] = {
      {116, 3, 0, 360, C_TRACK, C_TRACK, 1, false},
      {116, 3, head - 108.7f, 108.7f, C_CYAN, C_CYAN, A_TAIL1, true},
      {116, 3, head - 69.2f, 69.2f, C_CYAN, C_CYAN, A_TAIL2, true},
      {116, 3, head - 34.6f, 34.6f, C_MINT, C_MINT, 1, true},
  };
  for (int k = 1; k < 4; k++) if (arcs[k].start < 0) arcs[k].start += 360;
  RingDot dots[2] = {{head, 116, 14, C_MINT, A_GLOW}, {head, 116, 6, C_TEXT, 1}};
  drawHalo(MEAS_CX, MEAS_CY, 108, 131, arcs, 4, dots, 2, 116);
}

static int stability() {
  if (liveN == 0) return 0;
  int k = 0;
  float last = liveHist[(liveN - 1) % 5];
  for (int i = 0; i < 5 && i < liveN; i++)
    if (fabsf(liveHist[(liveN - 1 - i) % 5] - last) <= 0.1f) k++;
    else break;
  return k;
}

static void drawMeasureValue() {
  char num[12];
  if (liveKg > 0) snprintf(num, sizeof(num), "%.1f", toDisp(liveKg));
  else snprintf(num, sizeof(num), "--.-");
  heroNumber(num, unitStr(), MEAS_CX, 190, F_O80L, F_O64L, F_I15B, C_TEXT, C_TEXT2, C_INK, 212);
  drawnLiveKg = liveKg;
  int st = stability();
  if (st != drawnStab) {
    for (int i = 0; i < 5; i++)
      tft.fillSmoothRoundRect(MEAS_CX - 53 + i * 22, 208, 18, 4, 2, i < st ? C_CYAN : C_LINE, C_INK);
    drawnStab = st;
  }
}

static void drawMeasure() {
  icon(IC_BT, 20, 16, 15, C_CYAN, C_INK);
  useFont(F_I11);
  text("Scale connected", 42, 23, ML_DATUM, C_TEXT2, C_INK);
  drawMeasureClock();
  drawComet();
  useFont(F_I11B);
  label("MEASURING", MEAS_CX, 103, 1, C_CYAN, C_INK);
  drawnStab = -1;
  drawMeasureValue();
  useFont(F_I13);
  text("Hold still…", MEAS_CX, 222, TC_DATUM, C_TEXT2, C_INK);
}

// ── Result ──────────────────────────────────────────────────────────────────
struct ResultInfo {
  float kg;
  uint32_t ts;
  bool hasPrev; float dPrev; int prevDays;
  bool hasAvg; float dAvg;
  int streak;
  char msg[80];
};
static ResultInfo res;
static int drawnBarW = -1;

// Compare against history BEFORE the new reading is added.
static void resultBegin(float kg, uint32_t ts) {
  res = {};
  res.kg = kg; res.ts = ts;
  long today = historyDay(ts);
  for (int i = historyCount() - 1; i >= 0; i--) {
    Reading r = historyAt(i);
    long d = historyDay(r.ts);
    if (today >= 0 && d == today) continue;     // replaced by this reading
    res.hasPrev = true;
    res.dPrev = kg - r.kg;
    res.prevDays = (today >= 0 && d >= 0) ? (int)(today - d) : -1;
    break;
  }
  float avg = historyAvgPrevDays(7, ts);
  if (!isnan(avg)) { res.hasAvg = true; res.dAvg = kg - avg; }
}

// After it is added: streak + the monthly message.
static void resultFinish() {
  uint32_t now = res.ts;
  res.streak = historyStreak(now);
  int n = historyCount();
  float d30 = historyTrendLast() - historyTrendDaysAgo(30, now);
  if (isnan(d30) && n >= 2) {
    uint32_t t0 = historyAt(0).ts;
    if (t0 > 1700000000UL && now - t0 >= 7UL * 86400) d30 = historyTrendLast() - historyTrendAt(0);
  }
  char a[8];
  if (n < 2) snprintf(res.msg, sizeof(res.msg), "First weigh-in saved. Welcome!");
  else if (isnan(d30)) snprintf(res.msg, sizeof(res.msg), "Saved. Your trend shows after a week of weigh-ins.");
  else if (fabsf(d30) < 0.3f) snprintf(res.msg, sizeof(res.msg), "Holding steady this month.");
  else {
    fmtAbs(d30, a, sizeof(a));
    snprintf(res.msg, sizeof(res.msg), "%s %s %s this month. %s", d30 < 0 ? "Down" : "Up", a,
             unitStr(), goodDir(d30) ? "Nice and steady." : "One day at a time.");
  }
}

static void resultDelta(int x, int top, const char *lab, float d) {
  useFont(F_I11B);
  label(lab, x, top, 0, C_TEXT2, C_INK);
  char s[16], a[8];
  fmtAbs(d, a, sizeof(a));
  snprintf(s, sizeof(s), "%s %s", a, unitStr());
  bool flat = fabsf(toDisp(d)) < 0.05f;
  uint16_t c = (!flat && goodDir(d)) ? C_MINT : C_TEXT;
  icon(flat ? IC_FLAT : d < 0 ? IC_DOWN : IC_UP, x, top + 22, 16, c, C_INK);
  useFont(F_O26);
  text(s, x + 22, top + 39, L_BASELINE, c, C_INK);
}

static void drawResultBar() {
  uint32_t left = resultUntil > millis() ? resultUntil - millis() : 0;
  int w = (int)((uint64_t)SCR_W * left / RESULT_SHOW_MS);
  if (drawnBarW < 0) {
    tft.fillRect(0, SCR_H - 3, SCR_W, 3, C_EDGE);
    drawnBarW = 0;
  }
  if (w > drawnBarW) tft.fillRect(drawnBarW, SCR_H - 3, w - drawnBarW, 3, mix(C_MINT, C_INK, 0.7f));
  else if (w < drawnBarW) tft.fillRect(w, SCR_H - 3, drawnBarW - w, 3, C_EDGE);
  drawnBarW = w;
}

static void drawResult() {
  const int cx = 154, cy = 162;
  RingArc arcs[3] = {
      {121, 0.75f, 0, 360, C_MINT, C_MINT, A_HALO1, false},
      {112, 1, 0, 360, C_MINT, C_MINT, A_HALO2, false},
      {98, 5, 0, 360, C_MINT, C_CYAN, 1, false},
  };
  drawHalo(cx, cy, 91, 123, arcs, 3, nullptr, 0, 98);

  useFont(F_I11B);
  int lw = label("SAVED", cx + 10, 112, 1, C_MINT, C_INK);
  icon(IC_CHECK, cx + 10 - lw / 2 - 20, 111, 14, C_MINT, C_INK);
  char num[12], s[40];
  snprintf(num, sizeof(num), "%.1f", toDisp(res.kg));
  heroNumber(num, unitStr(), cx, 180, F_O62, F_O52, F_I15B, C_TEXT, C_TEXT2, C_INK, 170);
  struct tm tm;
  if (localTm(res.ts, tm)) snprintf(s, sizeof(s), "%02d:%02d \xC2\xB7 %s", tm.tm_hour, tm.tm_min, DAY_NAMES[tm.tm_wday]);
  else s[0] = 0;
  useFont(F_I11);
  if (s[0]) text(s, cx, 194, TC_DATUM, C_TEXT2, C_INK);

  // Right column.
  const int x = 300, w = SCR_W - 22 - x;
  int y = 56;
  if (res.hasPrev) {
    char lab[28];
    if (res.prevDays == 1) snprintf(lab, sizeof(lab), "VS YESTERDAY");
    else if (res.prevDays > 1) snprintf(lab, sizeof(lab), "VS %d DAYS AGO", res.prevDays);
    else snprintf(lab, sizeof(lab), "VS LAST WEIGH-IN");
    resultDelta(x, y, lab, res.dPrev);
  }
  y += 60;
  if (res.hasAvg) resultDelta(x, y, "VS 7-DAY AVERAGE", res.dAvg);
  y += 52;
  tft.drawFastHLine(x, y, w, C_LINE);
  y += 14;
  useFont(F_I13);
  int lines = textWrap(res.msg, x, y, w, 19, C_TEXT, C_INK, 3);
  y += lines * 19 + 12;
  if (res.streak > 0) {
    snprintf(s, sizeof(s), "Day %d streak", res.streak);
    useFont(F_I11B);
    int pw = 8 + 8 + 6 + tft.textWidth(s) + 10;
    tft.fillSmoothRoundRect(x, y, pw, 26, 13, C_LINE, C_INK);
    tft.fillSmoothRoundRect(x + 1, y + 1, pw - 2, 24, 12, C_INK, C_LINE);
    tft.fillSmoothCircle(x + 12, y + 13, 5, mix(C_MINT, C_INK, A_HALO2), C_INK);
    tft.fillSmoothCircle(x + 12, y + 13, 4, C_MINT, mix(C_MINT, C_INK, A_HALO2));
    text(s, x + 22, y + 13, ML_DATUM, C_TEXT, C_INK);
  }
  drawnBarW = -1;
  drawResultBar();
}

// ── Is this you? ────────────────────────────────────────────────────────────
static int drawnCountdown = -1;

static void drawConfirmBar() {
  uint32_t left = pendingUntil > millis() ? pendingUntil - millis() : 0;
  int bw = CB_R_X + CB_W - CB_L_X;
  int w = (int)((uint64_t)bw * left / REJECT_CONFIRM_MS);
  tft.fillRect(CB_L_X, 266, w, 3, C_AMBER);
  tft.fillRect(CB_L_X + w, 266, bw - w, 3, C_TRACK);
  int sec = (left + 999) / 1000;
  if (sec != drawnCountdown) {
    char s[48];
    snprintf(s, sizeof(s), "Not logged unless you confirm \xC2\xB7 %d s", sec);
    useFont(F_I11);
    text(s, SCR_W / 2, 275, TC_DATUM, C_TEXT2, C_INK, 260);
    drawnCountdown = sec;
  }
}

static void drawConfirm() {
  useFont(F_I11B);
  int lw = label("UNRECOGNIZED READING", SCR_W / 2 + 10, 34, 1, C_AMBER, C_INK);
  icon(IC_PERSON, SCR_W / 2 + 10 - lw / 2 - 21, 33, 14, C_AMBER, C_INK);
  char num[12], s[64], a[8];
  snprintf(num, sizeof(num), "%.1f", toDisp(pendingKg));
  heroNumber(num, unitStr(), SCR_W / 2, 103, F_O62, F_O52, F_I15B, C_TEXT, C_TEXT2, C_INK, 220);
  useFont(F_I22B);
  text("Is this you?", SCR_W / 2, 122, TC_DATUM, C_TEXT, C_INK);
  float base = historyAvg(3, false);
  if (!isnan(base)) {
    fmtAbs(pendingKg - base, a, sizeof(a));
    snprintf(s, sizeof(s), "That’s %s %s %s your recent weigh-ins.", a, unitStr(),
             pendingKg > base ? "above" : "below");
    useFont(F_I13);
    text(s, SCR_W / 2, 154, TC_DATUM, C_TEXT2, C_INK);
  }
  // Not me (outline) · Yes, log it (mint, primary).
  tft.fillSmoothRoundRect(CB_L_X, CB_Y, CB_W, CB_H, 16, C_BTNEDGE, C_INK);
  tft.fillSmoothRoundRect(CB_L_X + 2, CB_Y + 2, CB_W - 4, CB_H - 4, 14, C_INK, C_BTNEDGE);
  tft.fillSmoothRoundRect(CB_R_X, CB_Y, CB_W, CB_H, 16, C_MINT, C_INK);
  useFont(F_I15B);
  text("Not me", CB_L_X + CB_W / 2, CB_Y + CB_H / 2, MC_DATUM, C_TEXT, C_INK);
  int tw = tft.textWidth("Yes, log it"), gx = CB_R_X + (CB_W - (18 + 8 + tw)) / 2;
  icon(IC_CHECK, gx, CB_Y + CB_H / 2 - 9, 18, C_INK, C_MINT);
  text("Yes, log it", gx + 26, CB_Y + CB_H / 2, ML_DATUM, C_INK, C_MINT);
  drawnCountdown = -1;
  drawConfirmBar();
}

// ── Night glance ────────────────────────────────────────────────────────────
static void drawNightClock() {
  char hm[8];
  fmtHM(nowEpoch(), hm, sizeof(hm));
  useFont(F_O60L);
  text(hm, 240, 154, C_BASELINE, C_NIGHT, C_INK, tft.textWidth("00:00") + 4);
}

static void drawNight() {
  float p = goalProgress();
  RingArc arcs[2] = {
      {104, 1, 0, 360, C_NIGHT, C_NIGHT, 0.18f, false},
      {104, 1, 0, p > 0 ? p * 360 : 0, C_NIGHT, C_NIGHT, 0.55f, true},
  };
  drawHalo(240, 150, 101, 107, arcs, p > 0.001f ? 2 : 1, nullptr, 0, 104);
  drawNightClock();
  char s[48];
  if (historyCount()) snprintf(s, sizeof(s), "Last weigh-in %.1f %s", toDisp(historyLast().kg), unitStr());
  else snprintf(s, sizeof(s), "No weigh-ins yet");
  useFont(F_I13);
  text(s, 240, 176, TC_DATUM, C_NIGHT2, C_INK);
  snprintf(s, sizeof(s), "Quiet hours until %02d:%02d \xC2\xB7 backlight 15%%", NIGHT_END_MIN / 60,
           NIGHT_END_MIN % 60);
  useFont(F_I11);
  int tw = tft.textWidth(s), x = (SCR_W - (13 + 6 + tw)) / 2;
  icon(IC_MOON, x, 291, 13, C_NIGHT2, C_INK);
  text(s, x + 19, 298, ML_DATUM, C_NIGHT2, C_INK);
}

// ── Wi-Fi setup (captive portal open) ───────────────────────────────────────
// accountOnly: opened from Settings → Account to sign in (Wi-Fi already set).
static void drawSetup(const char *ap, bool accountOnly) {
  tft.fillScreen(C_INK);
  const int x = 28, colW = 288 - 24 - x;
  useFont(F_I11B);
  label(accountOnly ? "SETTINGS \xC2\xB7 ACCOUNT" : "SETUP", x, 52, 0, C_MINT, C_INK);
  useFont(F_I22B);
  text(accountOnly ? "Sign in" : "Connect to Wi-Fi", x, 78, TL_DATUM, C_TEXT, C_INK);
  static const char *SETUP[3] = {"Scan the code with your phone camera", "Pick your home Wi-Fi",
                                 cloudAvailable() ? "Sign in or create your account (optional)"
                                                  : "Save: the mirror carries on by itself"};
  static const char *SIGNIN[3] = {"Scan the code with your phone camera", "Tap Setup",
                                  "Sign in or create your account, then Save"};
  const char *const *STEPS = accountOnly ? SIGNIN : SETUP;
  int y = 120;
  for (int i = 0; i < 3; i++) {
    tft.fillSmoothCircle(x + 10, y + 10, 10, C_CHIP, C_INK);
    char d[2] = {(char)('1' + i), 0};
    useFont(F_I11B);
    text(d, x + 10, y + 10, MC_DATUM, C_TEXT, C_CHIP);
    useFont(F_I13);
    int lines = textWrap(STEPS[i], x + 30, y + 2, colW - 30, 17, C_TEXT2, C_INK, 2);
    y += lines * 17 + 12;
  }
  // "Or join <AP>" chip.
  char s[48];
  snprintf(s, sizeof(s), "Or join ");
  useFont(F_I11);
  int w1 = tft.textWidth(s);
  useFont(F_I11B);
  int w2 = tft.textWidth(ap);
  int cw = 10 + 14 + 7 + w1 + w2 + 10;
  tft.fillSmoothRoundRect(x, y + 2, cw, 28, 10, C_LINE, C_INK);
  tft.fillSmoothRoundRect(x + 1, y + 3, cw - 2, 26, 9, C_INK, C_LINE);
  icon(IC_WIFI, x + 10, y + 9, 14, C_TEXT2, C_INK);
  useFont(F_I11);
  text(s, x + 31, y + 16, ML_DATUM, C_TEXT2, C_INK);
  useFont(F_I11B);
  text(ap, x + 31 + w1, y + 16, ML_DATUM, C_TEXT, C_INK);

  // QR code: joins the setup network (open AP).
  const int qx = 288, qy = 64, qs = 168;
  tft.fillSmoothRoundRect(qx, qy, qs, qs, 16, C_TEXT, C_INK);
  char payload[64];
  snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ap);
  QRCode qr;
  static uint8_t qrData[106];   // qrcode_getBufferSize(3)
  if (qrcode_initText(&qr, qrData, 3, ECC_MEDIUM, payload) == 0) {
    int mod = 5, off = (qs - qr.size * mod) / 2;
    for (int my = 0; my < qr.size; my++)
      for (int mx = 0; mx < qr.size; mx++)
        if (qrcode_getModule(&qr, mx, my))
          tft.fillRect(qx + off + mx * mod, qy + off + my * mod, mod, mod, C_INK);
  }
  useFont(F_I11);
  text("Point your camera here", qx + qs / 2, qy + qs + 10, TC_DATUM, C_TEXT2, C_INK);
}

// ════════════════════════════════════════════════════════════════════════════
// Screen composition
// ════════════════════════════════════════════════════════════════════════════
static void renderContent() {   // rail content area (the rail itself stays)
  tft.fillRect(RAIL_W, 0, SCR_W - RAIL_W, SCR_H, C_INK);
  if (screen == SCR_HOME) drawHome();
  else if (screen == SCR_TRENDS) drawTrends();
  else drawSettings();
}

// ── Scale pairing (Settings → Scale) ────────────────────────────────────────
// Lists the OKOK scales heard in the last few seconds; a scale only broadcasts
// while someone stands on it. Tap one to pair it; the device then ignores all
// other scales (e.g. a neighbour's). "Forget" goes back to learning the first.
static const int PR_X = 28, PR_W = SCR_W - 56, PR_Y0 = 98, PR_H = 38, PR_GAP = 6, PR_ROWS = 4;
static const int PB_Y = 280, PB_H = 34, PB_CW = 110, PB_FW = 150;   // Cancel / Forget buttons
static const uint32_t PAIR_MS = 120000;        // closes itself after 2 min
static Scale::Nearby pairList[PR_ROWS];
static int pairN = 0;
static char pairDrawn[PR_ROWS][48];            // what each row shows (redraw on change)
static uint32_t pairUntil = 0, lastPairPoll = 0;

static int signalBars(int rssi) { return rssi > -60 ? 4 : rssi > -70 ? 3 : rssi > -80 ? 2 : 1; }

static void macTail(const char *mac, char *out, size_t n) {   // "88:E8" style tail
  snprintf(out, n, "%.8s", mac + 9);
  for (char *c = out; *c; c++) *c = toupper((unsigned char)*c);
}

static void drawPairRow(int i) {
  char key[48] = "";
  if (i < pairN)
    snprintf(key, sizeof(key), "%s|%.1f|%d|%d", pairList[i].mac, pairList[i].kg,
             signalBars(pairList[i].rssi), strcmp(pairList[i].mac, Scale::learnedMac()) == 0);
  else if (i == 0) snprintf(key, sizeof(key), "empty");
  if (strcmp(key, pairDrawn[i]) == 0) return;
  strcpy(pairDrawn[i], key);

  int y = PR_Y0 + i * (PR_H + PR_GAP);
  tft.fillRect(PR_X, y, PR_W, PR_H, C_INK);
  if (i >= pairN) {
    if (i == 0) {
      useFont(F_I13);
      text("Looking for scales\xE2\x80\xA6", SCR_W / 2, y + PR_H / 2, MC_DATUM, C_TEXT3, C_INK);
    }
    return;
  }
  const Scale::Nearby &s = pairList[i];
  bool paired = strcmp(s.mac, Scale::learnedMac()) == 0;
  tft.fillSmoothRoundRect(PR_X, y, PR_W, PR_H, 10, paired ? C_MINT : C_LINE, C_INK);
  tft.fillSmoothRoundRect(PR_X + 1, y + 1, PR_W - 2, PR_H - 2, 9, C_INK, paired ? C_MINT : C_LINE);
  icon(IC_SCALE, PR_X + 12, y + 10, 18, paired ? C_MINT : C_TEXT2, C_INK);
  char t[32], tail[12];
  macTail(s.mac, tail, sizeof(tail));
  snprintf(t, sizeof(t), "Scale %s", tail);
  useFont(F_I15B);
  int w = text(t, PR_X + 40, y + PR_H / 2, ML_DATUM, C_TEXT, C_INK);
  if (paired) {
    useFont(F_I11B);
    label("PAIRED", PR_X + 40 + w + 10, y + PR_H / 2 - 6, 0, C_MINT, C_INK);
  }
  snprintf(t, sizeof(t), "%.1f %s", toDisp(s.kg), unitStr());
  useFont(F_I13);
  text(t, PR_X + PR_W - 52, y + PR_H / 2, MR_DATUM, C_TEXT2, C_INK);
  int bars = signalBars(s.rssi);
  for (int b = 0; b < 4; b++) {
    int bh = 5 + b * 4, bx = PR_X + PR_W - 40 + b * 7;
    tft.fillRect(bx, y + PR_H / 2 + 8 - bh, 4, bh, b < bars ? C_MINT : C_BTNEDGE);
  }
}

static void drawPairButtons() {
  tft.fillRect(PR_X, PB_Y, PR_W, PB_H, C_INK);
  tft.fillSmoothRoundRect(PR_X, PB_Y, PB_CW, PB_H, 10, C_CHIP, C_INK);
  useFont(F_I15B);
  text("Done", PR_X + PB_CW / 2, PB_Y + PB_H / 2, MC_DATUM, C_TEXT, C_CHIP);
  if (Scale::learnedMac()[0]) {
    int fx = PR_X + PR_W - PB_FW;
    tft.fillSmoothRoundRect(fx, PB_Y, PB_FW, PB_H, 10, C_CHIP, C_INK);
    text("Forget scale", fx + PB_FW / 2, PB_Y + PB_H / 2, MC_DATUM, C_AMBER, C_CHIP);
  }
}

static void drawPair() {
  useFont(F_I11B);
  label("SETTINGS \xC2\xB7 SCALE", PR_X, 22, 0, C_MINT, C_INK);
  useFont(F_I22B);
  text("Pair your scale", PR_X, 40, TL_DATUM, C_TEXT, C_INK);
  useFont(F_I13);
  text("Step on it to wake it up, then tap it below.", PR_X, 72, TL_DATUM, C_TEXT2, C_INK);
  for (int i = 0; i < PR_ROWS; i++) {
    pairDrawn[i][0] = '\x01';                      // force every row to draw
    drawPairRow(i);
  }
  drawPairButtons();
}

static void openPair() {
  Scale::setPairing(true);
  pairN = 0;
  pairUntil = millis() + PAIR_MS;
  mode = MODE_PAIR;
  dirty = true;
}

static void closePair() {
  Scale::setPairing(false);
  mode = MODE_RAIL;
  screen = SCR_SETTINGS;
  dirty = true;
}

static void handlePairTap(int tx, int ty) {
  if (ty >= PB_Y - 6) {
    if (tx < PR_X + PB_CW + 6) closePair();
    else if (Scale::learnedMac()[0] && tx > PR_X + PR_W - PB_FW - 6) {
      Scale::setLearnedMac("");
      scaleMacSave("");
      Serial.println("[okok] scale forgotten - will learn the next one");
      drawPairButtons();
      for (int i = 0; i < PR_ROWS; i++) drawPairRow(i);
    }
    return;
  }
  int i = (ty - PR_Y0) / (PR_H + PR_GAP);
  if (ty < PR_Y0 || i >= pairN) return;
  Scale::setLearnedMac(pairList[i].mac);
  scaleMacSave(pairList[i].mac);
  Serial.printf("[okok] paired scale %s\n", pairList[i].mac);
  buzzerAccept();
  drawPairRow(i);
  drawPairButtons();
  delay(700);
  closePair();
}

// ── Account (Settings → Account) ────────────────────────────────────────────
// Signed in: email + sync state, Sync now / Sign out (tap twice). Signed out:
// Sign in opens the phone setup page with only the account fields.
static const int AC_X = 28, AB_Y = 272, AB_H = 36;
static const int AB_DONE_X = 28, AB_DONE_W = 110, AB_MAIN_X = 150, AB_MAIN_W = 150;
static const int AB_OUT_X = 312, AB_OUT_W = 140;
static bool acConfirmOut = false;            // "Sign out" tapped once → confirm
static char acNote[80] = "";                 // result of the last action
static bool acNoteOk = true;

static void acButton(int x, int w, const char *t, uint16_t fg) {
  tft.fillSmoothRoundRect(x, AB_Y, w, AB_H, 10, C_CHIP, C_INK);
  useFont(F_I15B);
  text(t, x + w / 2, AB_Y + AB_H / 2, MC_DATUM, fg, C_CHIP);
}

static void drawAccount() {
  tft.fillRect(0, 0, SCR_W, SCR_H, C_INK);
  useFont(F_I11B);
  label("SETTINGS \xC2\xB7 ACCOUNT", AC_X, 22, 0, C_MINT, C_INK);
  useFont(F_I22B);
  text("Your account", AC_X, 40, TL_DATUM, C_TEXT, C_INK);
  bool in = cloudConfigured();
  int y = 92;
  if (in) {
    useFont(F_I15B);
    text(cloudEmail(), AC_X, y, TL_DATUM, C_TEXT, C_INK, SCR_W - 2 * AC_X);
    char v[32];
    uint16_t c = syncStatus(v, sizeof(v));
    useFont(F_I13);
    text(v, AC_X, y + 30, TL_DATUM, c, C_INK);
  } else {
    const char *why = cloudSignInLost()
        ? "Your sign-in has expired. Sign in again to keep your weigh-ins syncing."
        : "Sign in to keep your weigh-ins in your account: safe, and on any new device.";
    textWrap(why, AC_X, y, SCR_W - 2 * AC_X, 19, C_TEXT2, C_INK, 3);
  }
  if (acNote[0]) {
    useFont(F_I13);
    textWrap(acNote, AC_X, 196, SCR_W - 2 * AC_X, 19, acNoteOk ? C_MINT : C_AMBER, C_INK, 2);
  }
  acButton(AB_DONE_X, AB_DONE_W, "Done", C_TEXT);
  acButton(AB_MAIN_X, AB_MAIN_W, in ? "Sync now" : "Sign in", C_TEXT);
  if (in) acButton(AB_OUT_X, AB_OUT_W, acConfirmOut ? "Sure? Tap" : "Sign out", C_AMBER);
}

static void openAccount() {
  acConfirmOut = false;
  acNote[0] = 0;
  mode = MODE_ACCOUNT;
  dirty = true;
}

// Settings follow the account. The newer side wins; right after signing in the
// account wins (signing in on a device makes it yours). An account without
// settings yet gets this device's. Leaves WiFi off. True if the account was reached.
static bool settingsSync(bool accountWins) {
  if (!cloudConfigured()) return false;
  UserSettings c;
  bool found = false;
  bool ok = cloudPullSettings(c, found);
  bool takeAccount = found && (accountWins || c.updated > setUpdated);
  // Changed here before the clock was set (no time to compare): keep the change.
  if (setDirty && !setUpdated && !accountWins) takeAccount = false;
  if (ok && takeAccount) {
    setUnitLb = c.lb;
    setSound = c.sound;
    setGoalKg = c.goalKg;
    setHeightCm = c.heightCm;
    setUpdated = c.updated;
    setDirty = false;
    settingsStore();
    buzzerSetLevel(setSound);
    dirty = true;                                  // units / goal / BMI shown everywhere
    Serial.println("[cloud] settings taken from the account");
  } else if (ok && (setDirty || !found)) {
    UserSettings s;
    s.lb = setUnitLb;
    s.sound = setSound;
    s.goalKg = setGoalKg;
    s.heightCm = setHeightCm;
    s.updated = setUpdated ? setUpdated : (timeIsSet() ? (uint32_t)nowEpoch() : 0);
    if (cloudPushSettings(s)) {
      setUpdated = s.updated;
      setDirty = false;
      settingsStore();
    } else {
      ok = false;
    }
  }
  wifiOff();
  return ok;
}

// Upload what's queued, then take the account's history (and settings) locally.
// justSignedIn: the account's settings win over this device's.
static void syncNow(bool justSignedIn = false) {
  wdtGraceUntil = millis() + 120000;
  if (cloudHasPending()) cloudFlush();
  lastPullN = cloudPull();
  if (lastPullN >= 0) lastSyncAt = nowEpoch();
  settingsSync(justSignedIn);
  wdtGraceUntil = 0;
  mainTick = millis();
}

static void handleAccountTap(int tx, int ty) {
  if (ty < AB_Y - 6) return;
  if (tx < AB_DONE_X + AB_DONE_W + 6) {                       // Done
    mode = MODE_RAIL;
    screen = SCR_SETTINGS;
    dirty = true;
  } else if (tx < AB_MAIN_X + AB_MAIN_W + 6) {                // Sync now / Sign in
    acConfirmOut = false;
    if (cloudConfigured()) {
      snprintf(acNote, sizeof(acNote), "Syncing\xE2\x80\xA6");
      acNoteOk = true;
      drawAccount();
      syncNow();
      acNoteOk = lastPullN >= 0;
      snprintf(acNote, sizeof(acNote), acNoteOk ? "Synced: %d weigh-ins in your account"
                                                : "Couldn't reach your account. Try again later.",
               lastPullN);
    } else {
      wdtGraceUntil = millis() + 200000;                      // the page waits up to 3 min
      bool saved = accountPortal([]() { mainTick = millis(); });
      bool ok = false;
      const char *m = accountSetupMessage(&ok);
      if (saved && ok && cloudConfigured()) syncNow(true);    // offline readings up, history down
      snprintf(acNote, sizeof(acNote), "%s", saved ? m : "Sign-in page closed");
      acNoteOk = saved && ok;
      wdtGraceUntil = 0;
      mainTick = millis();
    }
    dirty = true;
  } else if (cloudConfigured() && tx >= AB_OUT_X - 6) {      // Sign out (tap twice)
    if (!acConfirmOut) {
      acConfirmOut = true;
      acButton(AB_OUT_X, AB_OUT_W, "Sure? Tap", C_AMBER);
      return;
    }
    acConfirmOut = false;
    wdtGraceUntil = millis() + 90000;
    if (setDirty) settingsSync(false);              // don't lose a change made here
    cloudSignOut();
    // The personal settings leave with the account (units / sound stay).
    setGoalKg = 0;
    setHeightCm = HEIGHT_DEFAULT_CM;
    setUpdated = 0;
    setDirty = false;
    settingsStore();
    wdtGraceUntil = 0;
    mainTick = millis();
    lastSyncAt = 0;
    lastPullN = 0;
    snprintf(acNote, sizeof(acNote), "Signed out. This device's weigh-ins and goal were cleared.");
    acNoteOk = true;
    dirty = true;
  }
}

static void render() {          // whole screen for the current mode
  tft.fillScreen(C_INK);
  switch (mode) {
    case MODE_RAIL: drawRail(); renderContent(); break;
    case MODE_MEASURE: drawMeasure(); break;
    case MODE_RESULT: drawResult(); break;
    case MODE_CONFIRM: drawConfirm(); break;
    case MODE_NIGHT: drawNight(); break;
    case MODE_PAIR: drawPair(); break;
    case MODE_ACCOUNT: drawAccount(); break;
  }
}

static void enterMode(int m) {
  mode = m;
  dirty = true;
}

static void goRailScreen(int s) {
  int old = screen;
  screen = s;
  if (mode != MODE_RAIL) { enterMode(MODE_RAIL); return; }
  drawRailButton(old);
  drawRailButton(s);
  contentDirty = true;
}

// ── Display power (PWM backlight + wake/sleep) ──────────────────────────────
static const int BL_CH = 6;                 // LEDC channel (buzzer uses 4)
static const uint8_t BL_DAY = 255, BL_NIGHT_GLANCE = 38, BL_NIGHT_USE = 100;
static bool displayOn = true;
static uint32_t lastWakeMs = 0;
static uint32_t sleptAtMs = 0;              // when the display last went dark

static void backlightBegin() {
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(PIN_TFT_BL, BL_CH);
  ledcWrite(BL_CH, BL_DAY);
}
static void backlight(uint8_t v) { ledcWrite(BL_CH, v); }

// True during the night quiet-hours window (ToF must not wake the display then).
static bool quietHours() {
  if (!timeIsSet()) return false;    // no clock → never treat as night
  time_t t = nowEpoch();
  struct tm tm;
  localtime_r(&t, &tm);
  int m = tm.tm_hour * 60 + tm.tm_min;
  if (NIGHT_START_MIN < NIGHT_END_MIN)
    return m >= NIGHT_START_MIN && m < NIGHT_END_MIN;
  return m >= NIGHT_START_MIN || m < NIGHT_END_MIN;  // wraps midnight
}

// Wake the display. A touch during quiet hours opens the dim night glance;
// otherwise the resting Home screen (a weigh-in then switches to Measuring).
static void displayWake(bool byTouch = false) {
  lastWakeMs = millis();
  if (!displayOn) {
    displayOn = true;
    bool night = quietHours();
    Serial.printf("[disp] wake (%s%s)\n", byTouch ? "touch" : "presence/scale",
                  night ? ", quiet hours" : "");
    if (night && byTouch) {
      mode = MODE_NIGHT;
      backlight(BL_NIGHT_GLANCE);
    } else {
      mode = MODE_RAIL;
      screen = SCR_HOME;
      backlight(night ? BL_NIGHT_USE : BL_DAY);
    }
    dirty = true;                    // full redraw after being dark
  }
}

static void displaySleep() {
  if (displayOn) {
    displayOn = false;
    sleptAtMs = millis();
    Serial.println("[disp] sleep");
    tft.fillScreen(C_INK);
    backlight(0);                    // real backlight off on this board
    mode = MODE_RAIL;                // wake → Home, the resting screen
    screen = SCR_HOME;
  }
}

// Show a final reading on the Result screen; save = store + queue it for sync
// (false for a repeat of the reading just saved: shown again, not stored twice).
static void recordStable(float kg, bool save = true) {
  uint32_t ts = timeIsSet() ? (uint32_t)nowEpoch() : 0;
  resultBegin(kg, ts);
  if (save) {
    historyAdd(ts, kg);
    cloudQueue(ts, kg);   // queue for the account upload (flushed when idle)
  }
  resultFinish();
  liveKg = kg;
  liveState = 2;
  ledGreenUntil = millis() + 5000;
  buzzerAccept();
  Serial.printf("[ui] %s %.2f kg trend=%.2f streak=%d\n", save ? "stored" : "shown (repeat)",
                kg, historyTrendLast(), res.streak);
  resultUntil = millis() + RESULT_SHOW_MS;
  enterMode(MODE_RESULT);
}

// ── Touch calibration routine (blocking; run from Settings or serial 'c') ───
static void runCalibrate() {
  tft.fillScreen(C_INK);
  useFont(F_I22B);
  text("Touch calibration", 24, 110, TL_DATUM, C_TEXT, C_INK);
  useFont(F_I13);
  text("Tap each corner arrow tip as it appears.", 24, 150, TL_DATUM, C_TEXT2, C_INK);
  delay(1200);
  useFont(nullptr);
  mainTick = millis();                    // feed watchdog before the blocking call
  tft.calibrateTouch(calData, C_MINT, C_INK, 20);
  tft.setTouch(calData);
  calSave();
  mainTick = millis();
  Serial.print("[touch] saved calData = { ");
  for (int i = 0; i < 5; i++) Serial.printf("%u%s", calData[i], i < 4 ? ", " : " ");
  Serial.println("};");
  buzzerAccept();
  tft.fillScreen(C_INK);
  useFont(F_I15B);
  text("Calibrated", SCR_W / 2, SCR_H / 2, MC_DATUM, C_MINT, C_INK);
  delay(900);
  dirty = true;
}

// ── OTA update mode (web upload — browser/PC POSTs the .bin TO the device) ──
static WebServer otaServer(80);

static void fullMessage(const char *msg, uint16_t col) {
  tft.fillScreen(C_INK);
  useFont(F_I15B);
  text(msg, SCR_W / 2, SCR_H / 2, MC_DATUM, col, C_INK);
}

static void drawOta() {
  tft.fillScreen(C_INK);
  useFont(F_I11B);
  label("FIRMWARE " FW_VERSION, 28, 44, 0, C_MINT, C_INK);
  useFont(F_I22B);
  text("Update mode", 28, 66, TL_DATUM, C_TEXT, C_INK);
  useFont(F_I13);
  text("On a computer on the same Wi-Fi, browse to", 28, 116, TL_DATUM, C_TEXT2, C_INK);
  useFont(F_I22B);
  text(("http://" + WiFi.localIP().toString() + "/").c_str(), 28, 140, TL_DATUM, C_MINT, C_INK);
  useFont(F_I13);
  text("and upload the firmware .bin there.", 28, 180, TL_DATUM, C_TEXT2, C_INK);
  text("Tap anywhere to cancel", 28, 270, TL_DATUM, C_TEXT3, C_INK);
}

static void stopOtaMode() {
  otaMode = false;
  otaServer.stop();
  wifiOff();
  mode = MODE_RAIL;
  screen = SCR_HOME;
  dirty = true;
}

// Health readout — serial and (during OTA mode) the web page.
static void diagPrint() {
  Serial.printf("[diag] fw=%s board=%s up=%lus heap=%u min=%u readings=%d last=%.2f "
                "scale=%s cloud=%s wifi=%d rssi=%d lastPull=%d\n",
                FW_VERSION, BOARD_NAME, (unsigned long)(millis() / 1000),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                historyCount(), historyCount() ? historyLast().kg : 0.0,
                Scale::learnedMac()[0] ? Scale::learnedMac() : "auto",
                cloudConfigured() ? cloudEmail() : cloudAvailable() ? "signed out" : "off",
                WiFi.status() == WL_CONNECTED, (int)WiFi.RSSI(), lastPullN);
}

static String diagHtml() {
  char buf[768];
  snprintf(buf, sizeof(buf),
           "<!doctype html><html><body style='font-family:sans-serif;text-align:center'>"
           "<h2>Smart Mirror &mdash; fw %s (%s)</h2>"
           "<p>uptime %lus &middot; heap %u (min %u)<br>"
           "readings %d &middot; last %.1f kg<br>"
           "scale %s &middot; wifi %s &middot; %d dBm &middot; %s</p>"
           "<form method='POST' action='/update' enctype='multipart/form-data'>"
           "<input type='file' name='f' accept='.bin'><br><br>"
           "<input type='submit' value='Upload firmware'></form></body></html>",
           FW_VERSION, BOARD_NAME, (unsigned long)(millis() / 1000),
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
           historyCount(), historyCount() ? historyLast().kg : 0.0,
           Scale::learnedMac()[0] ? Scale::learnedMac() : "auto",
           WiFi.SSID().c_str(), (int)WiFi.RSSI(),
           WiFi.localIP().toString().c_str());
  return String(buf);
}

static void startOtaMode() {
  fullMessage("Connecting to Wi-Fi…", C_CYAN);

  if (!wifiConnect(12000)) {
    wifiOff();
    fullMessage("Wi-Fi connection failed", C_AMBER);
    buzzerReject();
    delay(1500);
    stopOtaMode();
    return;
  }

  otaServer.on("/", HTTP_GET, []() { otaServer.send(200, "text/html", diagHtml()); });
  otaServer.on(
      "/update", HTTP_POST,
      []() {  // completion handler
        bool ok = !Update.hasError();
        otaServer.send(200, "text/plain", ok ? "OK - rebooting" : "FAILED");
        delay(600);
        if (ok) ESP.restart();
      },
      []() {  // upload handler (streamed chunks)
        HTTPUpload &up = otaServer.upload();
        if (up.status == UPLOAD_FILE_START) {
          Serial.printf("[ota] receiving %s\n", up.filename.c_str());
          fullMessage("Updating…", C_CYAN);
          Update.begin(UPDATE_SIZE_UNKNOWN);
        } else if (up.status == UPLOAD_FILE_WRITE) {
          Update.write(up.buf, up.currentSize);
          mainTick = millis();   // keep the watchdog fed during the flash write
          char b[20];
          snprintf(b, sizeof(b), "%u KB", (unsigned)(up.totalSize / 1024));
          useFont(F_I13);
          text(b, SCR_W / 2, SCR_H / 2 + 30, TC_DATUM, C_TEXT2, C_INK, 120);
        } else if (up.status == UPLOAD_FILE_END) {
          bool ok = Update.end(true);
          Serial.printf("[ota] end ok=%d size=%u\n", ok, (unsigned)up.totalSize);
          fullMessage(ok ? "Done \xC2\xB7 restarting" : "Update failed", ok ? C_MINT : C_AMBER);
        }
      });
  otaServer.begin();

  otaMode = true;
  otaUntil = millis() + OTA_WINDOW_MS;
  drawOta();
  Serial.printf("[ota] web updater at http://%s/ , %lus window\n",
                WiFi.localIP().toString().c_str(), OTA_WINDOW_MS / 1000);
}

// ── Online update (product builds) ───────────────────────────────────────────
static void showBusy(const char *msg);
static const int UPD_BX = 28, UPD_BW = SCR_W - 56, UPD_BY = 196, UPD_BH = 10;

static void drawUpdating(const char *version) {
  tft.fillScreen(C_INK);
  useFont(F_I11B);
  label("FIRMWARE UPDATE", UPD_BX, 60, 0, C_MINT, C_INK);
  char t[40];
  snprintf(t, sizeof(t), "Updating to %s", version);
  useFont(F_I22B);
  text(t, UPD_BX, 82, TL_DATUM, C_TEXT, C_INK);
  useFont(F_I13);
  text("Keep it plugged in. It restarts by itself.", UPD_BX, 122, TL_DATUM, C_TEXT2, C_INK);
  tft.fillSmoothRoundRect(UPD_BX, UPD_BY, UPD_BW, UPD_BH, UPD_BH / 2, C_TRACK, C_INK);
}

static void drawUpdateProgress(int pct) {
  int w = UPD_BW * pct / 100;
  if (w >= UPD_BH) tft.fillSmoothRoundRect(UPD_BX, UPD_BY, w, UPD_BH, UPD_BH / 2, C_MINT, C_TRACK);
  char t[8];
  snprintf(t, sizeof(t), "%d%%", pct);
  useFont(F_I13);
  text(t, UPD_BX + UPD_BW, UPD_BY + 28, TR_DATUM, C_TEXT2, C_INK, 50);
}

// Check the latest release and install it when it's newer. Interactive (the
// Settings tile) shows each step; the automatic run happens with the display
// off and leaves no trace on screen unless it restarts into the new firmware.
static void runUpdate(bool interactive) {
  wdtGraceUntil = millis() + 240000;     // WiFi + download can block for a while
  if (interactive) showBusy("Checking for updates\xE2\x80\xA6");
  OtaRelease rel;
  OtaResult r = otaCheck(rel);
  bool installed = false;
  if (r == OTA_AVAILABLE) {
    if (interactive) drawUpdating(rel.version);
    installed = otaInstall(rel, [interactive](int pct) {
      mainTick = millis();
      if (interactive) drawUpdateProgress(pct);
    });
  }
  wifiOff();
  lastOtaCheckMs = millis();
  otaIntervalMs = r == OTA_FAILED || (r == OTA_AVAILABLE && !installed) ? 3600000UL : 43200000UL;
  if (installed) {
    if (interactive) fullMessage("Updated \xC2\xB7 restarting", C_MINT);
    delay(800);
    ESP.restart();
  }
  snprintf(otaNote, sizeof(otaNote), "%s", r == OTA_UP_TO_DATE ? "Up to date"
                                          : r == OTA_AVAILABLE ? "Update failed" : "Check failed");
  mainTick = millis();
  wdtGraceUntil = 0;
  if (interactive) {
    if (r != OTA_UP_TO_DATE) buzzerReject();
    dirty = true;                        // the busy / progress screens covered the UI
  }
}

// Independent watchdog: reboots if the main loop or scale task stops ticking.
static void watchdogTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(20000));   // boot grace period
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    uint32_t now = millis();
    if ((int32_t)(wdtGraceUntil - now) > 0) continue;
    if (now - mainTick > MAIN_STALL_MS) {
      Serial.println("[wdt] main loop stalled -> reboot");
      Serial.flush();
      ESP.restart();
    }
    uint32_t bh = Scale::heartbeat();
    if (bh != 0 && now - bh > BLE_STALL_MS) {
      Serial.println("[wdt] scale task stalled -> reboot");
      Serial.flush();
      ESP.restart();
    }
  }
}

// ── Touch input: detect a single tap (down-edge), return screen coords ───────
static bool touchDown = false;
static uint32_t lastTapMs = 0;

static bool getTap(int &tx, int &ty) {
  uint16_t x, y;
  bool now = tft.getTouch(&x, &y);
  bool tap = false;
  if (now) {
    lastWakeMs = millis();                       // any contact keeps display awake
    if (!touchDown && millis() - lastTapMs > 200) {
      tap = true; tx = x; ty = y; lastTapMs = millis();
    }
  }
  touchDown = now;
  return tap;
}

// Message over the Settings content while a blocking action runs.
static void showBusy(const char *msg) {
  tft.fillRect(RAIL_W, 0, SCR_W - RAIL_W, SCR_H, C_INK);
  useFont(F_I15B);
  text(msg, (RAIL_W + SCR_W) / 2, SCR_H / 2, MC_DATUM, C_CYAN, C_INK);
}

static void handleSettingsTap(int tx, int ty) {
  int col = (tx - MAIN_X) / (TILE_W + TILE_GAP), row = (ty - TILE_Y0) / (TILE_H + TILE_GAP);
  if (tx < MAIN_X || col > 1 || ty < TILE_Y0 || row >= TILE_ROWS) return;
  int i = row * 2 + col;
  if (i >= MI_COUNT) return;                                      // empty slot
  if (tx > tileX(i) + TILE_W || ty > tileY(i) + TILE_H) return;   // tapped a gap
  switch (i) {
    case MI_SYNC:
      if (cloudAvailable()) openAccount();   // sign in / sync / sign out
      break;
    case MI_UNIT:
      setUnitLb = !setUnitLb;
      settingsSave();
      drawSettingTile(MI_UNIT);
      drawSettingTile(MI_GOAL);   // goal and height are shown in the selected unit
      drawSettingTile(MI_HEIGHT);
      break;
    case MI_SOUND:
      // Off -> Low -> Mid -> High -> Off; play a sample at the new level.
      setSound = (setSound + 1) % 4;
      buzzerSetLevel(setSound);
      settingsSave();
      drawSettingTile(MI_SOUND);
      buzzerAccept();
      break;
    case MI_GOAL: {
      // Whole numbers only, stepped by 1 in the unit on screen (kg or lb).
      int dx = tx - tileX(i);
      bool minus = dx >= GB_MINUS_DX - 3 && dx < GB_MINUS_DX + GB_W + 3;
      bool plus  = dx >= GB_PLUS_DX - 3;
      if (minus || plus) {
        float g;
        if (setGoalKg <= 0) {   // first tap: start from the latest weight, rounded
          g = roundf(toDisp(historyCount() ? historyLast().kg : 100.0f));
        } else {
          g = roundf(toDisp(setGoalKg)) + (plus ? 1 : -1);
        }
        if (g < 1) g = 1;
        setGoalKg = fromDisp(g);
      } else {                                                    // label → goal off
        setGoalKg = 0;
      }
      settingsSave();
      drawSettingTile(MI_GOAL);
    } break;
    case MI_HEIGHT: {
      // 1 cm steps (1 inch in lb mode); tapping the label clears it (BMI hidden).
      int dx = tx - tileX(i);
      bool minus = dx >= GB_MINUS_DX - 3 && dx < GB_MINUS_DX + GB_W + 3;
      bool plus  = dx >= GB_PLUS_DX - 3;
      if (minus || plus) {
        if (setHeightCm <= 0) {
          setHeightCm = 170;                                   // sensible starting point
        } else if (setUnitLb) {
          int in = (int)roundf(setHeightCm / 2.54f) + (plus ? 1 : -1);
          setHeightCm = in * 2.54f;
        } else {
          setHeightCm = roundf(setHeightCm) + (plus ? 1 : -1);
        }
        if (setHeightCm < 100) setHeightCm = 100;
        if (setHeightCm > 230) setHeightCm = 230;
      } else {
        setHeightCm = 0;
      }
      settingsSave();
      drawSettingTile(MI_HEIGHT);
    } break;
    case MI_CALIBRATE:
      runCalibrate(); break;
    case MI_SCALE:
      if (Scale::canPair()) openPair();      // scan + pick (S200 builds: fixed scale)
      break;
    case MI_OTA:
      if (otaAutoEnabled()) runUpdate(true); // check online now, install if newer
      else wantOta = true;                   // web upload from a computer
      break;
    case MI_WIFI: {
      showBusy("Wi-Fi reset…");
      WiFiManager wm; wm.resetSettings();
      delay(400);
      ESP.restart();
    } break;
  }
}

// Act on a tap at (tx,ty) while the display is awake and not in OTA mode.
static void handleTap(int tx, int ty) {
  switch (mode) {
    case MODE_NIGHT:                 // night glance → normal UI, still dimmed
      backlight(BL_NIGHT_USE);
      mode = MODE_RAIL;
      screen = SCR_HOME;
      dirty = true;
      return;
    case MODE_RESULT:                // done looking → Home
      resultUntil = 0;
      if (liveState == 2) liveState = 0;
      mode = MODE_RAIL;
      screen = SCR_HOME;
      dirty = true;
      return;
    case MODE_CONFIRM:               // Not me / Yes, log it
      if (ty >= CB_Y - 6 && ty <= CB_Y + CB_H + 6) {
        bool yes = tx >= CB_R_X && tx <= CB_R_X + CB_W;
        bool no = tx >= CB_L_X && tx <= CB_L_X + CB_W;
        if (!yes && !no) return;
        pendingUntil = 0;
        if (yes) { recordStable(pendingKg); return; }
        mode = MODE_RAIL;
        screen = SCR_HOME;
        dirty = true;
      }
      return;
    case MODE_MEASURE:
      return;
    case MODE_PAIR:
      handlePairTap(tx, ty);
      return;
    case MODE_ACCOUNT:
      handleAccountTap(tx, ty);
      return;
  }

  // Rail → switch screen.
  if (tx < RAIL_W) {
    int i = (ty - RAIL_BTN_Y0 + RAIL_BTN_PITCH - RAIL_BTN_H) / RAIL_BTN_PITCH;
    int by = RAIL_BTN_Y0 + i * RAIL_BTN_PITCH;
    if (i >= 0 && i <= 2 && ty >= by - 4 && ty <= by + RAIL_BTN_H + 4 && i != screen)
      goRailScreen(i);
    return;
  }

  if (screen == SCR_TRENDS) {
    if (ty >= SEG_Y && ty <= SEG_Y + SEG_BH + 10 && tx >= SEG_X) {
      int i = (tx - SEG_X - 3) / SEG_BW;
      if (i > 3) i = 3;
      if (i >= 0 && i != rangeIdx) {
        rangeIdx = i;
        drawTrends();                // content only
      }
    }
  } else if (screen == SCR_SETTINGS) {
    handleSettingsTap(tx, ty);
  }
}

// Hold a finger on the screen at power-on to wipe WiFi + reopen the portal.
// One pressure reading isn't enough: something resting on the panel (a bezel,
// the case) reads as a press, and that wiped the saved network on ordinary
// restarts. So it takes a valid on-screen touch held for ~2 s, with a prompt.
static bool bootHoldForPortal() {
  uint16_t x, y;
  delay(60);
  if (!tft.getTouch(&x, &y)) return false;
  useFont(F_I15B);
  text("Keep holding to reset Wi-Fi…", SCR_W / 2, SCR_H / 2, MC_DATUM, C_CYAN, C_INK);
  bool held = true;
  for (int i = 0; i < 40 && held; i++) { delay(50); held = tft.getTouch(&x, &y); }
  tft.fillScreen(C_INK);
  Serial.printf("[wifi] boot hold: %s\n", held ? "reset Wi-Fi" : "released, keeping Wi-Fi");
  return held;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== Smart Mirror  fw=%s (build %d)  board=%s  updates=%s ===\n", FW_VERSION,
                FW_BUILD, BOARD_NAME, otaAutoEnabled() ? "online" : "manual");
  otaBootBegin();               // notes a rolled-back update so it isn't retried

  ledBegin();
  buzzerBegin(PIN_SPEAKER, PIN_SPEAKER_EN);
  tft.init();
  tft.setRotation(1);           // landscape
  tft.setSwapBytes(true);       // pushImage takes native RGB565 buffers
  tft.fillScreen(C_INK);
  backlightBegin();
  lastWakeMs = millis();
  calLoad();                    // board default, or the user's saved calibration

  bool forcePortal = bootHoldForPortal();

  {   // boot splash: the halo track + a status line
    RingArc a = {60, 4, 0, 360, C_TRACK, C_TRACK, 1, false};
    drawHalo(SCR_W / 2, 140, 54, 66, &a, 1, nullptr, 0, 60);
    drawLogo(SCR_W / 2 - 11, 129);
    useFont(F_I13);
    text("Connecting…", SCR_W / 2, 228, TC_DATUM, C_TEXT2, C_INK);
  }

  // Show the Wi-Fi setup screen (QR + steps) when the captive portal opens.
  setWifiPortalCallback([](const char *ap, bool accountOnly) { drawSetup(ap, accountOnly); });

  timeZoneBegin();  // local days for the history's trend, even before NTP
  historyBegin();   // mounts the filesystem (a setup-page sign-in may clear the log)
  cloudBegin();     // the saved account (its email prefills the setup page)
  timeSyncAtBoot(forcePortal);
  settingsLoad();   // units / sound / goal / learned scale from NVS

  // Just signed in on the setup page: show the outcome, then upload readings
  // made offline and take over the account's history. A unit that is signed in
  // but has no log (e.g. replacing an older mirror) takes the history too.
  bool acctOk = false;
  const char *acctMsg = accountSetupMessage(&acctOk);
  if (acctMsg[0]) {
    fullMessage(acctMsg, acctOk ? C_MINT : C_AMBER);
    delay(2500);
  }
  if (cloudConfigured() && (acctOk || historyCount() == 0)) {
    Serial.println("[cloud] taking over the account's history");
    if (cloudHasPending()) cloudFlush();
    lastPullN = cloudPull();
    if (lastPullN >= 0) lastSyncAt = nowEpoch();
  }
  // Settings changed on another of the user's devices (or theirs, after sign-in).
  if (cloudConfigured()) settingsSync(acctOk);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  tof.setTimeout(500);
  tofOk = tof.init();
  if (tofOk) tof.startContinuous();
  Serial.printf("[tof] %s\n", tofOk ? "OK" : "not found");

  weightQ = xQueueCreate(16, sizeof(WEvt));
  statusQ = xQueueCreate(8, sizeof(SEvt));
  Scale::setLearnedMac(savedScaleMac.c_str());
  Scale::setWeightCallback([](float kg, bool st) {
    WEvt e{kg, st};
    xQueueSend(weightQ, &e, 0);
  });
  Scale::setStatusCallback([](const char *s) {
    SEvt e;
    strncpy(e.s, s, sizeof(e.s) - 1);
    e.s[sizeof(e.s) - 1] = 0;
    xQueueSend(statusQ, &e, 0);
  });
  Scale::startTask();
  Serial.printf("[okok] scale: %s\n", savedScaleMac.length() ? savedScaleMac.c_str()
                                                              : "auto (learns the first one)");

  setStatus("Ready");
  if (quietHours()) backlight(BL_NIGHT_USE);
  mode = MODE_RAIL;
  screen = SCR_HOME;
  render();          // draw Home first: the boot melody blocks for ~1.7 s
  dirty = false;
  if (!quietHours()) buzzerBoot();   // e.g. no chime after a night-time update

  mainTick = millis();
  xTaskCreatePinnedToCore(watchdogTask, "wdt", 3072, nullptr, 1, nullptr, 1);
}

static uint32_t lastTof = 0;
static uint32_t lastLiveDraw = 0;
static uint32_t lastAnim = 0;
static int lastMinute = -1;
static long lastDay = -2;

void loop() {
  mainTick = millis();   // feed the watchdog
  // Running normally for 30 s: keep this firmware (an update that crashes or
  // restarts before this point is rolled back by the bootloader).
  if (millis() > 30000) otaConfirmHealthy();

  // Serial dev hooks: o=web OTA, u=online update now, p=pull, d=diag, c=calibrate
  // touch, t=raw touch, x=distance sensor, h=history, f=fill demo history (real
  // log backed up), r=restore the real log.
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'o' || c == 'O') wantOta = true;
    else if (c == 'u' && otaAutoEnabled()) { displayWake(); runUpdate(true); }
    else if (c == 'f' && historySeedDemo(nowEpoch()) > 0) {
      if (setGoalKg <= 0) {               // give the ring something to show
        setGoalKg = roundf(historyLast().kg) - 5;
        settingsSave();
      }
      displayWake();
      dirty = true;
    }
    else if (c == 'r' && historyRestoreReal()) { displayWake(); dirty = true; }
    else if (c == 'p' || c == 'P') {
      lastPullN = cloudPull();
      if (lastPullN >= 0) lastSyncAt = nowEpoch();
      contentDirty = true;
    }
    else if (c == 'd' || c == 'D') diagPrint();
    else if (c == 'h') {                  // dump the stored readings
      for (int i = 0; i < historyCount(); i++) {
        Reading r = historyAt(i);
        time_t t = r.ts;
        struct tm tm;
        char d[24] = "unclocked";
        if (localTm(t, tm)) strftime(d, sizeof(d), "%Y-%m-%d %H:%M", &tm);
        Serial.printf("[hist] %3d ts=%lu %s kg=%.2f trend=%.2f\n", i, (unsigned long)r.ts, d,
                      r.kg, historyTrendAt(i));
        mainTick = millis();
      }
    }
    else if (c == 't') {                  // raw touch readout for 8 s
      for (int i = 0; i < 40; i++) {
        uint16_t rx, ry, x, y;
        tft.getTouchRaw(&rx, &ry);
        uint16_t z = tft.getTouchRawZ();
        bool ok = tft.getTouch(&x, &y);
        Serial.printf("[touch] raw x=%4u y=%4u z=%4u -> %s %u,%u\n", rx, ry, z,
                      ok ? "TAP" : "none", ok ? x : 0, ok ? y : 0);
        mainTick = millis();
        delay(200);
      }
    }
    else if (c == 'c' || c == 'C') { displayWake(); runCalibrate(); }
    else if (c == 'x') {                  // distance sensor readout for 8 s
      for (int i = 0; i < 40 && tofOk; i++) {
        int mm = tof.readRangeContinuousMillimeters();
        Serial.printf("[tof] %d mm%s present=%d display=%s quiet=%d\n", mm,
                      tof.timeoutOccurred() ? " TIMEOUT" : "", present,
                      displayOn ? "on" : "off", quietHours());
        mainTick = millis();
        delay(200);
      }
      if (!tofOk) Serial.println("[tof] not found at boot");
    }
  }
  if (wantOta) { wantOta = false; startOtaMode(); }
  if (otaMode) {
    otaServer.handleClient();
    int tx, ty;
    if (getTap(tx, ty)) { buzzerClick(); stopOtaMode(); }
    if (millis() > otaUntil) stopOtaMode();
    delay(2);
    return;
  }

  bool clockChanged = false;

  // Status from the scale task.
  SEvt se;
  while (xQueueReceive(statusQ, &se, 0) == pdTRUE) {
    setStatus(se.s);
    // A weigh-in that ended without locking: drop the stale live value.
    if (strcmp(se.s, "Ready") == 0 && liveState == 1) {
      liveState = 0;
      if (mode == MODE_MEASURE) { mode = MODE_RAIL; screen = SCR_HOME; dirty = true; }
    }
  }

  // Weight from the scale (the driver reports one final reading per stand).
  // A weigh-in always wakes the display — even at night — since stepping on
  // the scale is deliberate.
  WEvt we;
  while (xQueueReceive(weightQ, &we, 0) == pdTRUE) {
    lastWeighMs = millis();            // note scale activity → hold off WiFi sync
    displayWake();
    // Every stand is shown. The same weight again within a minute of the stored
    // reading (stepping straight back on) is shown but not saved a second time.
    uint32_t sinceLast = (timeIsSet() && historyCount()) ? nowEpoch() - historyLast().ts : 9999;
    bool repeat = we.stable && sinceLast < 60 && fabsf(we.kg - historyLast().kg) < 0.15f;
    if (repeat) {
      recordStable(we.kg, false);
    } else if (we.stable) {
      if (validateReading(we.kg)) {
        recordStable(we.kg);                     // accepted → store + sync + Result
      } else {
        pendingKg = we.kg;                       // rejected → await confirmation
        pendingUntil = millis() + REJECT_CONFIRM_MS;
        liveState = 0;                           // nothing saved (Yes, log it saves)
        buzzerReject();
        enterMode(MODE_CONFIRM);
      }
    } else {
      liveKg = we.kg;
      liveHist[liveN % 5] = we.kg;
      liveN++;
      if (liveState != 1 || mode != MODE_MEASURE) {   // a new weigh-in begins
        liveState = 1;
        liveN = 1;
        liveHist[0] = we.kg;
        enterMode(MODE_MEASURE);
      }
    }
  }

  // Persist the scale MAC the driver learned on its first locked reading.
  const char *mac = Scale::learnedMac();
  if (mac[0] && savedScaleMac != mac) {
    scaleMacSave(mac);
    Serial.printf("[okok] saved scale %s\n", mac);
    scaleTileDirty = true;
  }

  // Touch input → wake; if already awake, act on the tap.
  int tx, ty;
  if (getTap(tx, ty)) {
    bool wasOff = !displayOn;
    displayWake(true);
    if (!wasOff) {                 // don't also act on the wake tap
      buzzerClick();
      handleTap(tx, ty);
    }
  }

  // ToF presence — wakes the display only OUTSIDE quiet hours.
  if (millis() - lastTof > 150) {
    lastTof = millis();
    if (tofOk) {
      int mm = tof.readRangeContinuousMillimeters();
      if (!tof.timeoutOccurred() && mm > 0) {
        bool was = present;
        if (!present && mm < PRESENCE_MM) present = true;
        else if (present && mm > PRESENCE_CLEAR_MM) present = false;
        if (present != was) Serial.printf("[tof] %s (%d mm)\n", present ? "present" : "gone", mm);
        if (present && !quietHours()) displayWake();  // keep awake while present
      }
    }
  }

  // Clock: redraw when the minute changes; a new day refreshes Home's labels.
  if (timeIsSet()) {
    time_t t = nowEpoch();
    historyDateUnclocked((uint32_t)t);   // no-op unless a reading awaits its date
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_min != lastMinute) {
      lastMinute = tm.tm_min;
      clockChanged = true;
      long d = historyDay((uint32_t)t);
      if (lastDay != -2 && d != lastDay && mode == MODE_RAIL) contentDirty = true;
      lastDay = d;
    }
  }

  // A rejected reading that wasn't confirmed within the window → discard.
  if (mode == MODE_CONFIRM && !pendingActive()) {
    pendingUntil = 0;
    mode = MODE_RAIL;
    screen = SCR_HOME;
    dirty = true;
  }
  // Result → Home after 30 s; Measuring → Home if the scale went quiet.
  if (mode == MODE_RESULT && millis() > resultUntil) {
    if (liveState == 2) liveState = 0;
    mode = MODE_RAIL;
    screen = SCR_HOME;
    dirty = true;
  }
  if (mode == MODE_MEASURE && millis() - lastWeighMs > MEASURE_IDLE_MS) {
    liveState = 0;
    mode = MODE_RAIL;
    screen = SCR_HOME;
    dirty = true;
  }

  // Pairing: refresh the list of scales heard; close after a while untouched.
  if (mode == MODE_PAIR) {
    if (millis() - lastPairPoll > 500) {
      lastPairPoll = millis();
      pairN = Scale::nearby(pairList, PR_ROWS, 15000);
      if (!dirty) for (int i = 0; i < PR_ROWS; i++) drawPairRow(i);
    }
    if (millis() - lastWakeMs > PAIR_MS) closePair();
  }
  if (mode == MODE_ACCOUNT && millis() - lastWakeMs > 120000) {
    mode = MODE_RAIL;
    screen = SCR_HOME;
    dirty = true;
  }

  // Sleep the display after inactivity — never mid weigh-in / result / prompt.
  bool holdAwake = mode == MODE_MEASURE || mode == MODE_RESULT || mode == MODE_CONFIRM ||
                   mode == MODE_PAIR || mode == MODE_ACCOUNT;
  if (displayOn && !holdAwake && millis() - lastWakeMs > DISPLAY_TIMEOUT_MS)
    displaySleep();

  // Cloud sync: flush the queue when idle (WiFi comes up briefly). This BLOCKS
  // the loop for the WiFi-connect duration and lights up the radio, which starves
  // the BLE scan — so sync only when clearly idle, and back off hard whenever
  // WiFi is unavailable (else a stuck reading retries forever, making frequent
  // blocking windows that swallow scale readings).
  static uint32_t lastSyncMs = 0;
  static uint32_t syncBackoffMs = 20000;   // grows on failure, resets on success
  bool recentWeigh = (lastWeighMs != 0 && millis() - lastWeighMs < 20000);
  bool busy = (strcmp(statusStr, "Reading") == 0) || present || recentWeigh ||
              mode != MODE_RAIL;
  bool settingsDue = setDirty && cloudConfigured();
  if ((cloudHasPending() || settingsDue) && !busy && (millis() - lastSyncMs > syncBackoffMs)) {
    setStatus("Syncing");
    syncing = true;
    if (displayOn && screen == SCR_HOME) drawHomeWifi();
    if (cloudHasPending()) cloudFlush();   // blocking: WiFi up → upload → WiFi off
    // Queue empty: also push a settings change made here / take one made elsewhere.
    bool ok = !cloudHasPending() && settingsSync(false);
    lastSyncMs = millis();
    if (ok) { syncBackoffMs = 20000; lastSyncAt = nowEpoch(); }   // success → fast again
    else { syncBackoffMs *= 3; if (syncBackoffMs > 900000) syncBackoffMs = 900000; }
    setStatus("Ready");
    syncing = false;
    if (displayOn && mode == MODE_RAIL && screen == SCR_HOME) drawHomeWifi();
    if (displayOn && mode == MODE_RAIL && screen == SCR_SETTINGS) drawSettingTile(MI_SYNC);
  }

  // Online update: first after boot, then every 12 h (1 h after a failure),
  // only when nobody is around: the display dark for 10 min. A check blocks the
  // loop (WiFi + HTTPS), so the display can't wake meanwhile; right after
  // someone walked away they may well come straight back.
  bool otaDue = otaIntervalMs ? millis() - lastOtaCheckMs > otaIntervalMs
                              : millis() > OTA_FIRST_CHECK_MS;
  if (otaAutoEnabled() && otaDue && !busy && !displayOn && wifiSavedSsid()[0] &&
      millis() - sleptAtMs > OTA_IDLE_MS)
    runUpdate(false);

  ledUpdate();

  // Redraw only what changed (and only while the display is awake). Requests
  // made while it's dark are dropped: waking always repaints the whole screen.
  if (displayOn) {
    if (dirty) {
      render();
      dirty = contentDirty = todayDirty = scaleTileDirty = false;
    } else if (mode == MODE_RAIL) {
      if (contentDirty) {
        renderContent();
        contentDirty = todayDirty = scaleTileDirty = false;
      } else if (screen == SCR_HOME) {
        if (clockChanged) { drawHomeClock(); drawHomeHeaderLeft(); }
      } else if (screen == SCR_SETTINGS) {
        if (scaleTileDirty) { drawSettingTile(MI_SCALE); scaleTileDirty = false; }
        if (clockChanged) {           // "Synced Xm ago": repaint only if it changed
          char v[32];
          syncStatus(v, sizeof(v));
          if (strcmp(v, drawnSync) != 0) drawSettingTile(MI_SYNC);
        }
      }
    } else if (mode == MODE_MEASURE) {
      if (millis() - lastAnim > 70) { drawComet(); lastAnim = millis(); }
      if (liveKg != drawnLiveKg && millis() - lastLiveDraw > 200) {
        drawMeasureValue();         // new live value only, ~5 Hz max
        lastLiveDraw = millis();
      }
      if (clockChanged) drawMeasureClock();
    } else if (mode == MODE_RESULT) {
      if (millis() - lastAnim > 100) { drawResultBar(); lastAnim = millis(); }
    } else if (mode == MODE_CONFIRM) {
      if (millis() - lastAnim > 100) { drawConfirmBar(); lastAnim = millis(); }
    } else if (mode == MODE_NIGHT) {
      if (clockChanged) drawNightClock();
    }
  }

  delay(5);
}
