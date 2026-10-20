// Smart Mirror Weight Tracker — integrated firmware.
// BLE scale listener on core 0 (passive scan); UI on core 1.
//
// Hardware: per-board profile in board.h (currently the 4" E32R40T: ST7796S
// 480x320 + XPT2046 touch, speaker amp, RGB LED, battery gauge, ToF on I2C).
// Scale: OKOK / Chipsea BLE broadcast scale (okok_scale.*) — no pairing, no keys.
//
// Screen (landscape 480x320):
//   top bar  — scale status, clock, battery
//   content  — WEIGHT (big number, delta, trend) / GRAPH (tap to cycle range) /
//              SETUP (2x4 grid of touch tiles)
//   tab bar  — Weight · Graph · Setup

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

#include "battery.h"
#include "buzzer.h"
#include "cloud.h"
#include "config.h"
#include "history.h"
#include "okok_scale.h"
#include "timekeeper.h"

namespace Scale = OkokScale;

// ── Scale task → UI ─────────────────────────────────────────────────────────
struct WEvt { float kg; bool stable; };
struct SEvt { char s[24]; };
static QueueHandle_t weightQ, statusQ;

static TFT_eSPI tft = TFT_eSPI();
static VL53L0X tof;
static bool tofOk = false;

// ── Geometry (landscape SCR_W x SCR_H from board.h) ─────────────────────────
static const int TOP_H = 32;                  // status bar
static const int TAB_H = 52;                  // bottom tab bar
static const int TAB_Y = SCR_H - TAB_H;       // 268
static const int CONTENT_Y = TOP_H;           // content area: TOP_H..TAB_Y

// ── Colors ──────────────────────────────────────────────────────────────────
static const uint16_t COL_BG = TFT_BLACK;
static const uint16_t COL_BAR = 0x10A2;       // status bar
static const uint16_t COL_TILE = 0x18E3;      // settings tiles
static const uint16_t COL_LIVE = TFT_CYAN;
static const uint16_t COL_STABLE = TFT_GREEN;
static const uint16_t COL_NONE = 0x7BEF;
static const uint16_t COL_UP = TFT_ORANGE;
static const uint16_t COL_DOWN = TFT_GREEN;
static const uint16_t COL_AXIS = 0x39C7;
static const uint16_t COL_TABSEL = TFT_NAVY;

// ── UI state ────────────────────────────────────────────────────────────────
enum Screen { SCR_TODAY = 0, SCR_GRAPH = 1, SCR_SETTINGS = 2, SCR_COUNT = 3 };
static int screen = SCR_GRAPH;          // graph is the resting/default screen
static int lastPullN = -2;              // -2 never, -1 failed, >=0 readings pulled

// ── Settings (persisted in NVS) ─────────────────────────────────────────────
static bool  setUnitLb = false;         // display kg (false) or lb (true)
static int   setSound  = SOUND_MID;     // speaker volume: Off / Low / Mid / High
static float setGoalKg = 0;             // goal weight in kg (0 = none)
static String savedScaleMac;            // scale MAC as stored in NVS

// Settings tiles (2 columns x 4 rows), each a touch target.
enum MenuItem { MI_SYNC, MI_UNIT, MI_SOUND, MI_GOAL, MI_CALIBRATE, MI_SCALE, MI_OTA,
                MI_WIFI, MI_COUNT };
static const int TILE_X[2] = { 6, 244 };
static const int TILE_W = 230, TILE_H = 52, TILE_GAP = 6, TILE_Y0 = CONTENT_Y + 4;
static int tileY(int row) { return TILE_Y0 + row * (TILE_H + TILE_GAP); }
// Goal tile -/+ buttons (x offsets inside the tile).
static const int GB_MINUS_DX = 112, GB_PLUS_DX = 172, GB_W = 52;

// Touch calibration (XPT2046), default from board.h; NVS overrides it.
static uint16_t calData[5] = TOUCH_CAL_DEFAULT;

// OTA "update mode"
static bool otaMode = false;
static bool wantOta = false;                 // request to enter OTA mode (from menu/serial)
static uint32_t otaUntil = 0;
static const uint32_t OTA_WINDOW_MS = 300000;  // 5 min listen window

// Watchdog: an independent task reboots the device if the main loop or the
// scale task stops ticking (a hang), so a wall-mounted unit recovers itself.
static volatile uint32_t mainTick = 0;
static const uint32_t MAIN_STALL_MS = 60000;   // main loop silent this long → reboot
static const uint32_t BLE_STALL_MS  = 150000;  // scale task silent this long → reboot

static Preferences prefs;
static void settingsLoad() {
  prefs.begin("mirror", true);
  setUnitLb = prefs.getBool("unit", false);
  setSound  = prefs.getInt("sound", SOUND_MID);
  setGoalKg = prefs.getFloat("goal", 0);
  savedScaleMac = prefs.getString("scalemac", "");
  prefs.end();
  buzzerSetLevel(setSound);
}
static void settingsSave() {
  prefs.begin("mirror", false);
  prefs.putBool("unit", setUnitLb);
  prefs.putInt("sound", setSound);
  prefs.putFloat("goal", setGoalKg);
  prefs.end();
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
  if (prefs.getBytesLength(TOUCH_CAL_KEY) == sizeof(calData))
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
static uint32_t todayUntil = 0;         // show TODAY until this millis, then revert
static uint32_t lastWeighMs = 0;        // last time any scale weight event arrived
static const uint32_t TODAY_SHOW_MS = 30000;
static const int RANGES[] = {30, 60, 9999};
static int rangeIdx = 0;

static char statusStr[24] = "Booting";
static bool present = false;
static float liveKg = -1;       // last live value (cyan)
static int liveState = 0;       // 0 none, 1 live, 2 stable
// Redraw requests, from biggest to smallest (only what changed gets repainted):
static bool dirty = true;          // whole screen (boot, wake, after full-screen modes)
static bool contentDirty = false;  // content area + tab bar (screen switch, new data)
static bool todayDirty = false;    // weight screen values, drawn in place (no clear)
static bool scaleTileDirty = false;// Settings "Scale" tile (scale MAC learned)

// Battery (read every few seconds; -1 = not read yet).
static int batPct = -1;
static float batV = 0;

// Rejected-reading confirmation ("that's not you — tap to log anyway")
static float pendingKg = 0;
static uint32_t pendingUntil = 0;
static bool pendingActive() { return pendingUntil != 0 && millis() <= pendingUntil; }

// Confirm-button geometry.
static const int BTN_Y = CONTENT_Y + 156, BTN_H = 64;
static const int BTN_L_X = 60, BTN_R_X = 250, BTN_W = 170;

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

// ── Top status bar: scale status (left), clock (centre), battery (right) ────
static void drawBatIcon(int x, int y, int pct) {
  const int w = 34, h = 16;
  tft.drawRoundRect(x, y, w, h, 2, TFT_WHITE);
  tft.fillRect(x + w, y + 5, 3, h - 10, TFT_WHITE);           // terminal nub
  uint16_t c = pct > 50 ? TFT_GREEN : pct > 20 ? TFT_YELLOW : TFT_RED;
  tft.fillRect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, c);
}

// Each status-bar field repaints in place: text is drawn with its background
// and padded to a fixed width, so the old value is overwritten without a flash.
static void drawTopStatus() {
  bool reading = strcmp(statusStr, "Reading") == 0;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(reading ? COL_LIVE : TFT_WHITE, COL_BAR);
  tft.setTextPadding(170);
  tft.drawString(statusStr, 10, TOP_H / 2, 2);
  tft.setTextPadding(0);
}

static void drawTopClock() {
  if (!timeIsSet()) return;
  char hm[8];
  time_t t = nowEpoch();
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(hm, sizeof(hm), "%H:%M", &tm);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BAR);
  tft.setTextPadding(tft.textWidth("88:88", 4));
  tft.drawString(hm, SCR_W / 2, TOP_H / 2, 4);
  tft.setTextPadding(0);
}

static void drawTopBattery() {
  if (batPct < 0) return;
  tft.fillRect(SCR_W - 100, 0, 100, TOP_H, COL_BAR);
  drawBatIcon(SCR_W - 44, (TOP_H - 16) / 2, batPct);
  char p[8];
  snprintf(p, sizeof(p), "%d%%", batPct);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BAR);
  tft.drawString(p, SCR_W - 50, TOP_H / 2, 2);
}

static void drawTopBar() {
  tft.fillRect(0, 0, SCR_W, TOP_H, COL_BAR);
  drawTopStatus();
  drawTopClock();
  drawTopBattery();
}

// ── Bottom tab bar ──────────────────────────────────────────────────────────
static void drawTabs() {
  const char *names[3] = {"Weight", "Graph", "Setup"};
  int cw = SCR_W / 3;
  for (int i = 0; i < 3; i++) {
    int x = i * cw;
    int w = (i == 2) ? (SCR_W - x) : cw;
    bool sel = (screen == i);
    tft.fillRect(x, TAB_Y, w, TAB_H, sel ? COL_TABSEL : COL_BG);
    tft.drawRect(x, TAB_Y, w, TAB_H, COL_AXIS);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(sel ? TFT_CYAN : COL_NONE, sel ? COL_TABSEL : COL_BG);
    tft.drawString(names[i], x + w / 2, TAB_Y + TAB_H / 2, 4);
  }
}

static void fmtDate(uint32_t ts, char *out, size_t n);

// ── Today / Weight screen ───────────────────────────────────────────────────
// Values are drawn in place (background-filled, padded text) so live updates
// overwrite the previous number instead of clearing the area first.

// Big weight — live value if measuring, else last stored.
static void drawTodayWeight() {
  float kg = (liveState ? liveKg : (historyCount() ? historyLast().kg : -1));
  uint16_t c = liveState == 2 ? COL_STABLE : liveState == 1 ? COL_LIVE : COL_NONE;
  tft.setTextColor(c, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(tft.textWidth("888.8", 8) + 16);
  if (kg <= 0)
    tft.drawString("--.-", SCR_W / 2, CONTENT_Y + 78, 8);
  else
    tft.drawFloat(toDisp(kg), 1, SCR_W / 2, CONTENT_Y + 78, 8);
  tft.setTextPadding(0);
}

// Unit, change since last reading, and trend.
static void drawTodayDetails() {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_NONE, COL_BG);
  tft.setTextPadding(80);
  tft.drawString(unitStr(), SCR_W / 2, CONTENT_Y + 140, 4);

  // The two lines below follow the weigh-in: nothing claims "saved" until a
  // valid reading has actually been stored.
  tft.setTextPadding(SCR_W - 20);
  char d[32] = " ";   // a space (not "") so the padding still clears old text
  uint16_t dc = COL_NONE;
  const char *t = " ";
  char tbuf[32];
  uint16_t tc = COL_NONE;

  if (liveState == 1) {                       // on the scale, still settling
    t = "Measuring... stand still";
    tc = COL_LIVE;
  } else if (historyCount() > 0) {
    if (historyCount() > 1) {
      float delta = historyLast().kg - historyAt(historyCount() - 2).kg;
      snprintf(d, sizeof(d), "%+.1f %s vs last", toDisp(delta), unitStr());
      dc = delta > 0.05f ? COL_UP : (delta < -0.05f ? COL_DOWN : COL_NONE);
    }
    Trend tr = historyTrend();
    if (tr != TREND_NONE) {
      t = trendText(tr);
      tc = tr == TREND_UP ? COL_UP : tr == TREND_DOWN ? COL_DOWN : COL_NONE;
    } else if (liveState == 2) {              // just stored, too few for a trend
      t = "Reading saved";
      tc = COL_STABLE;
    } else {                                  // just browsing the Weight tab
      char dt[10];
      fmtDate(historyLast().ts, dt, sizeof(dt));
      snprintf(tbuf, sizeof(tbuf), "Last weigh-in %s", dt);
      t = tbuf;
    }
  }
  tft.setTextColor(dc, COL_BG);
  tft.drawString(d, SCR_W / 2, CONTENT_Y + 176, 4);
  tft.setTextColor(tc, COL_BG);
  tft.drawString(t, SCR_W / 2, CONTENT_Y + 210, 4);
  tft.setTextPadding(0);
}

static void drawToday(bool full) {
  if (full) tft.fillRect(0, CONTENT_Y, SCR_W, TAB_Y - CONTENT_Y, COL_BG);

  // Rejected-reading confirmation prompt takes over the content area.
  if (pendingActive()) {
    tft.fillRect(0, CONTENT_Y, SCR_W, TAB_Y - CONTENT_Y, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_UP, COL_BG);
    tft.drawString("Not recognized", SCR_W / 2, CONTENT_Y + 26, 4);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawFloat(toDisp(pendingKg), 1, SCR_W / 2, CONTENT_Y + 82, 7);
    tft.setTextColor(COL_NONE, COL_BG);
    tft.drawString("Is this you?", SCR_W / 2, CONTENT_Y + 134, 4);
    tft.fillRoundRect(BTN_L_X, BTN_Y, BTN_W, BTN_H, 10, TFT_DARKGREEN);
    tft.fillRoundRect(BTN_R_X, BTN_Y, BTN_W, BTN_H, 10, 0x4208);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString("Log it", BTN_L_X + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
    tft.setTextColor(TFT_WHITE, 0x4208);
    tft.drawString("Skip", BTN_R_X + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
    return;
  }

  drawTodayWeight();
  drawTodayDetails();
}

// ── Graph screen ────────────────────────────────────────────────────────────
static void fmtDate(uint32_t ts, char *out, size_t n) {
  if (ts < 1700000000UL) { snprintf(out, n, "--"); return; }
  time_t t = (time_t)ts;
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(out, n, "%d/%m", &tm);
}

// Whole calendar days between the last weigh-in and now (0 = weighed today).
// Returns -1 when unknown (no readings, no clock, or an unclocked last reading).
static int daysSinceLast() {
  if (historyCount() == 0 || !timeIsSet()) return -1;
  uint32_t lastTs = historyLast().ts;
  if (lastTs < 1700000000UL) return -1;
  time_t tl = (time_t)lastTs, tn = nowEpoch();
  struct tm a, b;
  localtime_r(&tl, &a); a.tm_hour = a.tm_min = a.tm_sec = 0;
  localtime_r(&tn, &b); b.tm_hour = b.tm_min = b.tm_sec = 0;
  double d = difftime(mktime(&b), mktime(&a)) / 86400.0;
  if (d < 0) d = 0;
  return (int)(d + 0.5);
}

// Centred "days since last weigh-in" nudge.
static void drawDaysSince(int y) {
  int d = daysSinceLast();
  if (d < 0) return;
  char s[32];
  tft.setTextDatum(MC_DATUM);
  if (d == 0) {
    tft.setTextColor(COL_STABLE, COL_BG);
    tft.drawString("Weighed today", SCR_W / 2, y, 2);
  } else {
    tft.setTextColor(COL_UP, COL_BG);
    snprintf(s, sizeof(s), "%d day%s since weigh-in", d, d == 1 ? "" : "s");
    tft.drawString(s, SCR_W / 2, y, 2);
  }
}

static void drawGraph() {
  tft.fillRect(0, CONTENT_Y, SCR_W, TAB_Y - CONTENT_Y, COL_BG);
  int n = historyCount();
  int range = RANGES[rangeIdx];
  const int hy = CONTENT_Y + 16;          // header row centre

  // Header: range (left), days-since (centre), latest value (right).
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BG);
  char title[20];
  if (range >= 9999) snprintf(title, sizeof(title), "All (%d)", n);
  else snprintf(title, sizeof(title), "Last %d", range);
  tft.drawString(title, 10, hy, 2);
  drawDaysSince(hy);

  if (n < 2) {
    tft.setTextColor(COL_NONE, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("need 2+ readings", SCR_W / 2, (CONTENT_Y + TAB_Y) / 2, 4);
    return;
  }

  int show = n < range ? n : range;
  int start = n - show;

  float mn = 1e9f, mx = -1e9f;
  for (int i = start; i < n; i++) {
    float k = historyAt(i).kg;
    if (k < mn) mn = k;
    if (k > mx) mx = k;
  }
  if (setGoalKg > 0) {  // keep the goal line on-screen
    if (setGoalKg < mn) mn = setGoalKg;
    if (setGoalKg > mx) mx = setGoalKg;
  }
  if (mx - mn < 1.0f) { float m = (mx + mn) / 2; mn = m - 0.5f; mx = m + 0.5f; }
  float pad = (mx - mn) * 0.1f;
  mn -= pad; mx += pad;
  float mid = (mn + mx) / 2;

  // Plot box — left margin for Y labels, bottom margin for X labels.
  int x0 = 60, y0 = CONTENT_Y + 40, x1 = SCR_W - 10, y1 = TAB_Y - 26;
  tft.drawFastHLine(x0, y1, x1 - x0, COL_AXIS);
  tft.drawFastVLine(x0, y0, y1 - y0, COL_AXIS);

  // Latest value, header right.
  char lb[16];
  bool up = historyAt(n - 1).kg > historyAt(start).kg + 0.1f;
  uint16_t lc = up ? COL_UP : COL_DOWN;
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(lc, COL_BG);
  snprintf(lb, sizeof(lb), "%.1f %s", toDisp(historyLast().kg), unitStr());
  tft.drawString(lb, x1, hy, 4);

  // Y-axis labels: max / mid / min.
  tft.setTextColor(COL_NONE, COL_BG);
  tft.setTextDatum(MR_DATUM);
  snprintf(lb, sizeof(lb), "%.1f", toDisp(mx));  tft.drawString(lb, x0 - 5, y0, 2);
  snprintf(lb, sizeof(lb), "%.1f", toDisp(mid)); tft.drawString(lb, x0 - 5, (y0 + y1) / 2, 2);
  snprintf(lb, sizeof(lb), "%.1f", toDisp(mn));  tft.drawString(lb, x0 - 5, y1, 2);
  tft.drawFastHLine(x0 + 1, (y0 + y1) / 2, x1 - x0 - 1, COL_AXIS);  // mid gridline

  // Goal line (dashed magenta) if a goal is set.
  if (setGoalKg > 0) {
    int gy = y1 - (int)((setGoalKg - mn) * (y1 - y0) / (mx - mn));
    if (gy > y0 && gy < y1) {
      for (int x = x0 + 1; x < x1; x += 8) tft.drawFastHLine(x, gy, 4, TFT_MAGENTA);
      tft.setTextColor(TFT_MAGENTA, COL_BG);
      tft.setTextDatum(BR_DATUM);
      tft.drawString("goal", x1, gy - 2, 2);
    }
  }

  // X-axis labels (dates): oldest left, newest right.
  char d0[10], d1[10];
  fmtDate(historyAt(start).ts, d0, sizeof(d0));
  fmtDate(historyAt(n - 1).ts, d1, sizeof(d1));
  tft.setTextColor(COL_NONE, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(d0, x0, y1 + 6, 2);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(d1, x1, y1 + 6, 2);
  tft.setTextColor(COL_AXIS, COL_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("tap graph: range", (x0 + x1) / 2, y1 + 6, 2);

  // The line.
  int prevx = 0, prevy = 0;
  for (int i = start; i < n; i++) {
    float k = historyAt(i).kg;
    int px = (show == 1) ? x0 : x0 + (long)(i - start) * (x1 - x0) / (show - 1);
    int py = y1 - (int)((k - mn) * (y1 - y0) / (mx - mn));
    if (i > start) {
      tft.drawLine(prevx, prevy, px, py, lc);
      tft.drawLine(prevx, prevy + 1, px, py + 1, lc);   // 2 px thick
    }
    tft.fillCircle(px, py, 3, lc);
    prevx = px; prevy = py;
  }
}

// ── Settings screen (2 x 4 tiles) ───────────────────────────────────────────
static void drawTile(int idx, const char *label, const char *value) {
  int x = TILE_X[idx % 2], y = tileY(idx / 2);
  tft.fillRoundRect(x, y, TILE_W, TILE_H, 8, COL_TILE);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_NONE, COL_TILE);
  tft.drawString(label, x + 10, y + 4, 2);
  tft.setTextColor(TFT_WHITE, COL_TILE);
  tft.drawString(value, x + 10, y + 22, 4);
}

// Draw one settings tile (used alone when only that tile changed).
static void drawSettingTile(int i) {
  char v[24];
  switch (i) {
    case MI_SYNC:
      if (!cloudConfigured()) snprintf(v, sizeof(v), "Offline");
      else if (lastPullN >= 0) snprintf(v, sizeof(v), "Synced %d", lastPullN);
      else if (lastPullN == -1) snprintf(v, sizeof(v), "Failed - retry");
      else snprintf(v, sizeof(v), "Sync now");
      drawTile(MI_SYNC, cloudConfigured() ? "Google Sheet" : "Cloud (no sheet set)", v);
      break;
    case MI_UNIT:
      drawTile(MI_UNIT, "Units", unitStr());
      break;
    case MI_SOUND:
      drawTile(MI_SOUND, "Sound (tap to change)", buzzerLevelName(setSound));
      break;
    case MI_GOAL: {
      if (setGoalKg > 0) snprintf(v, sizeof(v), "%.0f", toDisp(setGoalKg));
      else snprintf(v, sizeof(v), "Off");
      char gl[16];
      snprintf(gl, sizeof(gl), "Goal (%s)", unitStr());
      drawTile(MI_GOAL, gl, v);
      int x = TILE_X[MI_GOAL % 2], y = tileY(MI_GOAL / 2);   // -/+ buttons
      tft.fillRoundRect(x + GB_MINUS_DX, y + 6, GB_W, TILE_H - 12, 6, TFT_NAVY);
      tft.fillRoundRect(x + GB_PLUS_DX, y + 6, GB_W, TILE_H - 12, 6, TFT_NAVY);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_NAVY);
      tft.drawString("-", x + GB_MINUS_DX + GB_W / 2, y + TILE_H / 2, 4);
      tft.drawString("+", x + GB_PLUS_DX + GB_W / 2, y + TILE_H / 2, 4);
    } break;
    case MI_CALIBRATE:
      drawTile(MI_CALIBRATE, "Touch", "Calibrate");
      break;
    case MI_SCALE: {
      const char *mac = Scale::learnedMac();
      if (mac[0]) snprintf(v, sizeof(v), "%.8s", mac + 9);   // last 3 bytes
      else snprintf(v, sizeof(v), "Auto");
      drawTile(MI_SCALE, mac[0] ? "Scale (tap = forget)" : "Scale (learns 1st)", v);
    } break;
    case MI_OTA:
      drawTile(MI_OTA, "Firmware " FW_VERSION, "Update");
      break;
    case MI_WIFI:
      drawTile(MI_WIFI, "WiFi", "Reset");
      break;
  }
}

static void drawSettings() {
  tft.fillRect(0, CONTENT_Y, SCR_W, TAB_Y - CONTENT_Y, COL_BG);
  for (int i = 0; i < MI_COUNT; i++) drawSettingTile(i);
}

// Content area + tab bar (the status bar is left alone).
static void renderContent() {
  if (screen == SCR_TODAY) drawToday(true);
  else if (screen == SCR_GRAPH) drawGraph();
  else drawSettings();
  drawTabs();
}

// Whole screen.
static void render() {
  drawTopBar();
  renderContent();
}

// ── Display power (backlight on/off + wake/sleep) ───────────────────────────
static bool displayOn = true;
static uint32_t lastWakeMs = 0;

static void backlightBegin() {
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);   // on
}

static void displayWake() {
  lastWakeMs = millis();
  if (!displayOn) {
    displayOn = true;
    digitalWrite(PIN_TFT_BL, HIGH);
    dirty = true;                    // full redraw after being dark
  }
}

static void displaySleep() {
  if (displayOn) {
    displayOn = false;
    tft.fillScreen(TFT_BLACK);
    digitalWrite(PIN_TFT_BL, LOW);   // real backlight off on this board
  }
}

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

static void recordStable(float kg) {
  bool haveDelta = historyCount() > 0;
  float delta = haveDelta ? (kg - historyLast().kg) : 0;
  uint32_t ts = timeIsSet() ? (uint32_t)nowEpoch() : 0;
  historyAdd(ts, kg);
  cloudQueue(ts, kg);   // queue for Google Sheet sync (flushed when idle)
  liveKg = kg;
  liveState = 2;
  ledGreenUntil = millis() + 5000;
  buzzerAccept();
  Serial.printf("[ui] stored %.2f kg delta=%+.2f trend=%s\n", kg, delta,
                trendText(historyTrend()));
  if (screen == SCR_TODAY) todayDirty = true;   // weight + delta + trend in place
  else contentDirty = true;                     // e.g. graph gets the new point
}

// ── Touch calibration routine (blocking; run from Settings or serial 'c') ───
static void runCalibrate() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.drawString("Touch calibration", 20, 120, 4);
  tft.setTextColor(COL_NONE, COL_BG);
  tft.drawString("Tap each corner arrow tip as it appears.", 20, 160, 2);
  delay(1200);
  mainTick = millis();                    // feed watchdog before the blocking call
  tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 20);
  tft.setTouch(calData);
  calSave();
  mainTick = millis();
  Serial.print("[touch] saved calData = { ");
  for (int i = 0; i < 5; i++) Serial.printf("%u%s", calData[i], i < 4 ? ", " : " ");
  Serial.println("};");
  buzzerAccept();
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_STABLE, COL_BG);
  tft.drawString("Calibrated", SCR_W / 2, SCR_H / 2, 4);
  delay(900);
  dirty = true;
}

// ── OTA update mode (web upload — browser/PC POSTs the .bin TO the device) ──
static WebServer otaServer(80);

static void drawOta() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, COL_BG);
  tft.drawString("Update mode", 20, 30, 4);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.drawString("Browse to:", 20, 90, 2);
  tft.setTextColor(TFT_YELLOW, COL_BG);
  tft.drawString("http://" + WiFi.localIP().toString() + "/", 20, 114, 4);
  tft.setTextColor(COL_NONE, COL_BG);
  tft.drawString("and upload the firmware .bin there", 20, 160, 2);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.drawString("Tap anywhere to cancel", 20, 200, 4);
}

static void stopOtaMode() {
  otaMode = false;
  otaServer.stop();
  wifiOff();
  screen = SCR_GRAPH;
  dirty = true;
}

// Health readout — serial and (during OTA mode) the web page.
static void diagPrint() {
  Serial.printf("[diag] fw=%s board=%s up=%lus heap=%u min=%u readings=%d last=%.2f "
                "bat=%.2fV(%d%%) scale=%s cloud=%s wifi=%d rssi=%d lastPull=%d\n",
                FW_VERSION, BOARD_NAME, (unsigned long)(millis() / 1000),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                historyCount(), historyCount() ? historyLast().kg : 0.0, batV, batPct,
                Scale::learnedMac()[0] ? Scale::learnedMac() : "auto",
                cloudConfigured() ? "sheet" : "off",
                WiFi.status() == WL_CONNECTED, (int)WiFi.RSSI(), lastPullN);
}

static String diagHtml() {
  char buf[768];
  snprintf(buf, sizeof(buf),
           "<!doctype html><html><body style='font-family:sans-serif;text-align:center'>"
           "<h2>Smart Mirror &mdash; fw %s (%s)</h2>"
           "<p>uptime %lus &middot; heap %u (min %u)<br>"
           "readings %d &middot; last %.1f kg &middot; battery %.2f V (%d%%)<br>"
           "scale %s &middot; wifi %s &middot; %d dBm &middot; %s</p>"
           "<form method='POST' action='/update' enctype='multipart/form-data'>"
           "<input type='file' name='f' accept='.bin'><br><br>"
           "<input type='submit' value='Upload firmware'></form></body></html>",
           FW_VERSION, BOARD_NAME, (unsigned long)(millis() / 1000),
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
           historyCount(), historyCount() ? historyLast().kg : 0.0, batV, batPct,
           Scale::learnedMac()[0] ? Scale::learnedMac() : "auto",
           WiFi.SSID().c_str(), (int)WiFi.RSSI(),
           WiFi.localIP().toString().c_str());
  return String(buf);
}

static void startOtaMode() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, COL_BG);
  tft.drawString("WiFi up...", SCR_W / 2, SCR_H / 2, 4);
  tft.setTextDatum(TL_DATUM);

  if (!wifiConnect(12000)) {
    wifiOff();
    tft.fillScreen(COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_UP, COL_BG);
    tft.drawString("WiFi failed", SCR_W / 2, SCR_H / 2, 4);
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
          tft.fillScreen(COL_BG);
          tft.setTextDatum(MC_DATUM);
          tft.setTextColor(TFT_CYAN, COL_BG);
          tft.drawString("Updating...", SCR_W / 2, 110, 4);
          tft.setTextDatum(TL_DATUM);
          Update.begin(UPDATE_SIZE_UNKNOWN);
        } else if (up.status == UPLOAD_FILE_WRITE) {
          Update.write(up.buf, up.currentSize);
          mainTick = millis();   // keep the watchdog fed during the flash write
          char b[20];
          snprintf(b, sizeof(b), "%u KB", (unsigned)(up.totalSize / 1024));
          tft.fillRect(0, 150, SCR_W, 30, COL_BG);
          tft.setTextDatum(MC_DATUM);
          tft.setTextColor(TFT_WHITE, COL_BG);
          tft.drawString(b, SCR_W / 2, 165, 4);
          tft.setTextDatum(TL_DATUM);
        } else if (up.status == UPLOAD_FILE_END) {
          bool ok = Update.end(true);
          Serial.printf("[ota] end ok=%d size=%u\n", ok, (unsigned)up.totalSize);
          tft.fillScreen(COL_BG);
          tft.setTextDatum(MC_DATUM);
          tft.setTextColor(ok ? COL_STABLE : COL_UP, COL_BG);
          tft.drawString(ok ? "Done! rebooting" : "Update failed", SCR_W / 2, SCR_H / 2, 4);
          tft.setTextDatum(TL_DATUM);
        }
      });
  otaServer.begin();

  otaMode = true;
  otaUntil = millis() + OTA_WINDOW_MS;
  drawOta();
  Serial.printf("[ota] web updater at http://%s/ , %lus window\n",
                WiFi.localIP().toString().c_str(), OTA_WINDOW_MS / 1000);
}

// Independent watchdog: reboots if the main loop or scale task stops ticking.
static void watchdogTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(20000));   // boot grace period
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    uint32_t now = millis();
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

static void showBusy(const char *msg) {
  tft.fillRect(0, CONTENT_Y, SCR_W, TAB_Y - CONTENT_Y, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, COL_BG);
  tft.drawString(msg, SCR_W / 2, (CONTENT_Y + TAB_Y) / 2, 4);
  tft.setTextDatum(TL_DATUM);
}

// Act on a tap at (tx,ty) while the display is awake and not in OTA mode.
static void handleTap(int tx, int ty) {
  // Rejected-reading confirmation: Log it / Skip buttons.
  if (pendingActive()) {
    if (ty >= BTN_Y && ty <= BTN_Y + BTN_H) {
      if (tx >= BTN_L_X && tx <= BTN_L_X + BTN_W) recordStable(pendingKg);  // Log it
      pendingUntil = 0;
      screen = SCR_TODAY;
      todayUntil = millis() + TODAY_SHOW_MS;
      contentDirty = true;          // replace the prompt with the weight screen
    }
    return;
  }

  // Bottom tab bar → switch screen (content + tabs only; status bar stays).
  if (ty >= TAB_Y) {
    int t = tx / (SCR_W / 3);
    if (t < 0) t = 0;
    if (t > 2) t = 2;
    if (t != screen) {
      screen = t;
      todayUntil = (screen == SCR_TODAY) ? (millis() + TODAY_SHOW_MS) : 0;
      contentDirty = true;
    }
    return;
  }
  if (ty < CONTENT_Y) return;   // status bar is not interactive

  // Content-area taps, per screen.
  if (screen == SCR_GRAPH) {
    rangeIdx = (rangeIdx + 1) % (int)(sizeof(RANGES) / sizeof(RANGES[0]));
    drawGraph();                    // content only
  } else if (screen == SCR_SETTINGS) {
    int col = tx < SCR_W / 2 ? 0 : 1;
    int row = (ty - TILE_Y0) / (TILE_H + TILE_GAP);
    if (row < 0 || row > 3) return;
    if (ty > tileY(row) + TILE_H) return;          // tapped the gap
    int i = row * 2 + col;
    int tileX = TILE_X[col];
    switch (i) {
      case MI_SYNC:
        if (!cloudConfigured()) {
          // No sheet on this device: explain how to add one (WiFi setup page).
          showBusy("Add a sheet: Setup > WiFi > Reset");
          delay(2200);
          drawSettings();
          break;
        }
        showBusy("Syncing from sheet...");
        lastPullN = cloudPull();
        (lastPullN >= 0) ? buzzerAccept() : buzzerReject();
        drawSettings();
        break;
      case MI_UNIT:
        setUnitLb = !setUnitLb;
        settingsSave();
        drawSettingTile(MI_UNIT);
        drawSettingTile(MI_GOAL);   // goal is shown in the selected unit
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
        int dx = tx - tileX;
        bool minus = dx >= GB_MINUS_DX && dx < GB_MINUS_DX + GB_W;
        bool plus  = dx >= GB_PLUS_DX && dx < GB_PLUS_DX + GB_W;
        if (minus || plus) {
          float g;
          if (setGoalKg <= 0)   // first tap: start from the latest weight, rounded
            g = roundf(toDisp(historyCount() ? historyLast().kg : 100.0f));
          else
            g = roundf(toDisp(setGoalKg)) + (plus ? 1 : -1);
          if (g < 1) g = 1;
          setGoalKg = fromDisp(g);
        } else {                                                    // label → goal off
          setGoalKg = 0;
        }
        settingsSave();
        drawSettingTile(MI_GOAL);
      } break;
      case MI_CALIBRATE:
        runCalibrate(); break;
      case MI_SCALE:
        // Forget the learned scale; the next scale that locks a reading is learned.
        Scale::setLearnedMac("");
        scaleMacSave("");
        Serial.println("[okok] scale forgotten — will learn the next one");
        drawSettingTile(MI_SCALE);
        break;
      case MI_OTA:
        wantOta = true; break;
      case MI_WIFI: {
        showBusy("WiFi reset...");
        WiFiManager wm; wm.resetSettings();
        delay(400);
        ESP.restart();
      } break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Smart Mirror  fw=" FW_VERSION "  board=" BOARD_NAME " ===");

  ledBegin();
  buzzerBegin(PIN_SPEAKER, PIN_SPEAKER_EN);
  batteryBegin();
  tft.init();
  tft.setRotation(1);           // landscape
  tft.fillScreen(COL_BG);
  backlightBegin();
  lastWakeMs = millis();
  calLoad();                    // board default, or the user's saved calibration

  // Hold a finger on the screen at power-on to wipe WiFi + reopen the portal.
  delay(60);
  bool forcePortal = (tft.getTouchRawZ() > 600);

  batV = batteryVolts();
  batPct = batteryPercent(batV);
  setStatus("Syncing time");
  drawTopBar();
  drawToday(true);
  drawTabs();

  // Show captive-portal join instructions on the TFT when it opens.
  setWifiPortalCallback([](const char *ap, const char *ip) {
    tft.fillScreen(COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_CYAN, COL_BG);
    tft.drawString("WiFi setup", 20, 30, 4);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawString("1. On your phone, join WiFi:", 20, 90, 2);
    tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.drawString(ap, 40, 114, 4);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawString("2. Then open:", 20, 170, 2);
    tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.drawString(ip, 40, 194, 4);
  });

  cloudBegin();     // per-device sheet config (prefills the WiFi setup page)
  timeSyncAtBoot(forcePortal);
  historyBegin();
  settingsLoad();   // units / mute / goal / learned scale from NVS

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
  dirty = true;
  buzzerAccept();

  mainTick = millis();
  xTaskCreatePinnedToCore(watchdogTask, "wdt", 3072, nullptr, 1, nullptr, 1);
}

static uint32_t lastTof = 0;
static uint32_t lastLiveDraw = 0;
static float drawnLiveKg = -1;
static uint32_t lastBat = 0;
static int lastMinute = -1;

void loop() {
  mainTick = millis();   // feed the watchdog

  // Serial dev hooks: o=OTA, p=pull, d=diag, c=calibrate touch.
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'o' || c == 'O') wantOta = true;
    else if (c == 'p' || c == 'P') { lastPullN = cloudPull(); contentDirty = true; }
    else if (c == 'd' || c == 'D') diagPrint();
    else if (c == 'c' || c == 'C') { displayWake(); runCalibrate(); }
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

  // Which status-bar fields changed this pass (each repaints on its own).
  bool statusChanged = false, clockChanged = false, batChanged = false;

  // Status from the scale task.
  SEvt se;
  while (xQueueReceive(statusQ, &se, 0) == pdTRUE) {
    setStatus(se.s);
    statusChanged = true;
    // A weigh-in that ended without locking: drop the stale live value.
    if (strcmp(se.s, "Ready") == 0 && liveState == 1) {
      liveState = 0;
      todayDirty = true;
    }
  }

  // Weight from the scale (the driver reports one final reading per stand).
  // A weigh-in always wakes the display — even at night — since stepping on
  // the scale is deliberate.
  WEvt we;
  bool gotStable = false;
  while (xQueueReceive(weightQ, &we, 0) == pdTRUE) {
    lastWeighMs = millis();            // note scale activity → hold off WiFi sync
    if (we.stable) {
      if (validateReading(we.kg)) {
        recordStable(we.kg);                     // accepted → store + sync + chime
      } else {
        pendingKg = we.kg;                       // rejected → await confirmation
        pendingUntil = millis() + REJECT_CONFIRM_MS;
        liveState = 0;                           // nothing saved (Log it saves)
        buzzerReject();
        contentDirty = true;                     // show the Log it / Skip prompt
      }
      gotStable = true;
    } else {
      if (liveState != 1) todayDirty = true;     // entering "Measuring..." state
      liveKg = we.kg;
      liveState = 1;
    }
    // Pop up the weight screen for 30 s, then it reverts to the graph.
    if (screen != SCR_TODAY) { screen = SCR_TODAY; contentDirty = true; }
    todayUntil = millis() + TODAY_SHOW_MS;
    displayWake();
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
    displayWake();
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
        if (!present && mm < PRESENCE_MM) present = true;
        else if (present && mm > PRESENCE_CLEAR_MM) present = false;
        if (present && !quietHours()) displayWake();  // keep awake while present
      }
    }
  }

  // Battery every 10 s.
  if (millis() - lastBat > 10000) {
    lastBat = millis();
    batV = batteryVolts();
    int p = batteryPercent(batV);
    if (p != batPct) { batPct = p; batChanged = true; }
  }

  // Clock in the status bar: redraw when the minute changes.
  if (timeIsSet()) {
    time_t t = nowEpoch();
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_min != lastMinute) { lastMinute = tm.tm_min; clockChanged = true; }
  }

  // A rejected reading that wasn't confirmed within the window → discard, go to graph.
  static bool wasPending = false;
  bool pend = pendingActive();
  if (wasPending && !pend) {
    pendingUntil = 0;
    screen = SCR_GRAPH;
    todayUntil = 0;
    contentDirty = true;
  }
  wasPending = pend;

  // Auto-revert from the weight screen back to the graph after 30 s. The
  // "just saved" state ends here, so a later visit shows the idle view.
  if (screen == SCR_TODAY && todayUntil != 0 && millis() > todayUntil) {
    screen = SCR_GRAPH;
    todayUntil = 0;
    if (liveState == 2) liveState = 0;
    contentDirty = true;
  }

  // Sleep the display after inactivity — but stay awake during the 30 s window.
  bool inTodayWindow = (todayUntil != 0 && millis() <= todayUntil);
  if (displayOn && !inTodayWindow && millis() - lastWakeMs > DISPLAY_TIMEOUT_MS)
    displaySleep();

  // Cloud sync: flush the queue when idle (WiFi comes up briefly). This BLOCKS
  // the loop for the WiFi-connect duration and lights up the radio, which starves
  // the BLE scan — so sync only when clearly idle, and back off hard whenever
  // WiFi is unavailable (else a stuck reading retries forever, making frequent
  // blocking windows that swallow scale readings).
  static uint32_t lastSyncMs = 0;
  static uint32_t syncBackoffMs = 20000;   // grows on failure, resets on success
  bool recentWeigh = (lastWeighMs != 0 && millis() - lastWeighMs < 20000);
  bool busy = (strcmp(statusStr, "Reading") == 0) || present || recentWeigh;
  if (cloudHasPending() && !busy && (millis() - lastSyncMs > syncBackoffMs)) {
    setStatus("Syncing");
    if (displayOn) drawTopStatus();
    int sent = cloudFlush();    // blocking: WiFi up → POST → WiFi off
    lastSyncMs = millis();
    if (sent > 0) syncBackoffMs = 20000;                       // success → fast again
    else { syncBackoffMs *= 3; if (syncBackoffMs > 900000) syncBackoffMs = 900000; }
    setStatus("Ready");
    statusChanged = true;
  }

  ledUpdate();

  // Redraw only what changed (and only while the display is awake). Requests
  // made while it's dark are dropped: waking always repaints the whole screen.
  if (displayOn) {
    if (dirty) {
      render();
      dirty = contentDirty = todayDirty = scaleTileDirty = false;
    } else {
      if (statusChanged) drawTopStatus();
      if (clockChanged) drawTopClock();
      if (batChanged) drawTopBattery();

      if (contentDirty) {
        renderContent();
        contentDirty = todayDirty = scaleTileDirty = false;
      } else if (screen == SCR_TODAY && !pendingActive()) {
        if (gotStable || todayDirty) {
          drawTodayWeight();
          drawTodayDetails();
          todayDirty = false;
        } else if (liveState == 1 && liveKg != drawnLiveKg && millis() - lastLiveDraw > 200) {
          drawTodayWeight();          // new live value only, ~5 Hz max
          drawnLiveKg = liveKg;
          lastLiveDraw = millis();
        }
      } else if (screen == SCR_SETTINGS && scaleTileDirty) {
        drawSettingTile(MI_SCALE);
        scaleTileDirty = false;
      }
    }
  }

  delay(5);
}
