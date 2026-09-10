// Smart Mirror Weight Tracker — integrated firmware.
// BLE reader on core 0 (persistent connection); UI on core 1.
//
// Display: ST7789V 240x320 SPI TFT with XPT2046 resistive touch, LANDSCAPE
// (rotation 1 → 320x240). Touch-only UI (rotary encoder removed). A bottom tab
// bar switches screens:
//   WEIGHT   — big weight, delta vs last, trend message
//   GRAPH    — weight-over-time line chart (tap chart to cycle 30/60/all)
//   SETUP    — sync, units, buzzer, goal, calibrate touch, OTA, WiFi reset

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

#include "buzzer.h"
#include "cloud.h"
#include "config.h"
#include "history.h"
#include "mi_scale.h"
#include "timekeeper.h"

// ── BLE task → UI ───────────────────────────────────────────────────────────
struct WEvt { float kg; bool stable; int imp; };
struct SEvt { char s[24]; };
static QueueHandle_t weightQ, statusQ;

static TFT_eSPI tft = TFT_eSPI();
static VL53L0X tof;
static bool tofOk = false;

// ── Geometry (landscape 320x240, rotation 1) ────────────────────────────────
static const int SCR_W = 320;
static const int SCR_H = 240;
static const int TAB_H = 40;                 // bottom tab bar height
static const int TAB_Y = SCR_H - TAB_H;      // 200
static const int CONTENT_H = TAB_Y;          // usable content height above tabs

// ── Colors ──────────────────────────────────────────────────────────────────
static const uint16_t COL_BG = TFT_BLACK;
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
static bool  setMute   = false;         // mute buzzer
static float setGoalKg = 0;             // goal weight in kg (0 = none)

// Settings menu rows (each is a touch target).
enum MenuItem { MI_SYNC, MI_UNIT, MI_BUZZER, MI_GOAL, MI_CALIBRATE, MI_OTA, MI_WIFI, MI_COUNT };
static const int SET_Y0 = 42;           // first row top
static const int SET_RH = 22;           // row height
static int rowY(int i) { return SET_Y0 + i * SET_RH; }
// Goal -/+ button geometry (right side of the goal row).
static const int GB_MINUS_X = 214, GB_PLUS_X = 264, GB_W = 44;

// Touch calibration data (XPT2046). Loaded from NVS; must be calibrated for the
// CURRENT rotation (landscape). Run Settings → Calibrate touch (or serial 'c')
// on first landscape boot. calData[4] encodes orientation.
static uint16_t calData[5] = { 300, 3600, 300, 3600, 7 };

// OTA "update mode"
static bool otaMode = false;
static bool wantOta = false;                 // request to enter OTA mode (from menu/serial)
static uint32_t otaUntil = 0;
static const uint32_t OTA_WINDOW_MS = 300000;  // 5 min listen window

// Watchdog: an independent task reboots the device if the main loop or the BLE
// task stops ticking (a hang), so a wall-mounted unit recovers itself.
static volatile uint32_t mainTick = 0;
static const uint32_t MAIN_STALL_MS = 60000;   // main loop silent this long → reboot
static const uint32_t BLE_STALL_MS  = 150000;  // BLE task silent this long → reboot

static Preferences prefs;
static void settingsLoad() {
  prefs.begin("mirror", true);
  setUnitLb = prefs.getBool("unit", false);
  setMute   = prefs.getBool("mute", false);
  setGoalKg = prefs.getFloat("goal", 0);
  prefs.end();
  buzzerSetMute(setMute);
}
static void settingsSave() {
  prefs.begin("mirror", false);
  prefs.putBool("unit", setUnitLb);
  prefs.putBool("mute", setMute);
  prefs.putFloat("goal", setGoalKg);
  prefs.end();
}

// Touch calibration persistence (5 x uint16 blob in NVS).
static bool haveCal = false;
static void calLoad() {
  prefs.begin("mirror", true);
  if (prefs.getBytesLength("calL") == sizeof(calData)) {
    prefs.getBytes("calL", calData, sizeof(calData));
    haveCal = true;
  }
  prefs.end();
  tft.setTouch(calData);
}
static void calSave() {
  prefs.begin("mirror", false);
  prefs.putBytes("calL", calData, sizeof(calData));
  prefs.end();
  haveCal = true;
}

// Display-unit helpers (data is always stored in kg).
static float toDisp(float kg) { return setUnitLb ? kg * 2.20462f : kg; }
static const char *unitStr() { return setUnitLb ? "lb" : "kg"; }
static uint32_t todayUntil = 0;         // show TODAY until this millis, then revert
static uint32_t lastWeighMs = 0;        // last time any BLE weight event arrived
static const uint32_t TODAY_SHOW_MS = 30000;
static const int RANGES[] = {30, 60, 9999};
static int rangeIdx = 0;

static char statusStr[24] = "Booting";
static bool present = false;
static float liveKg = -1;       // last live value (cyan)
static int liveState = 0;       // 0 none, 1 live, 2 stable
static bool dirty = true;       // full redraw needed

// Rejected-reading confirmation ("that's not you — tap to log anyway")
static float pendingKg = 0;
static uint32_t pendingUntil = 0;
static bool pendingActive() { return pendingUntil != 0 && millis() <= pendingUntil; }

// Confirm-button geometry (landscape).
static const int BTN_Y = 150, BTN_H = 44;
static const int BTN_L_X = 40, BTN_R_X = 170, BTN_W = 110;

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

// ── Today / Weight screen ───────────────────────────────────────────────────
static void drawToday(bool full) {
  if (full) tft.fillRect(0, 0, SCR_W, CONTENT_H, COL_BG);

  // Rejected-reading confirmation prompt takes over the content area.
  if (pendingActive()) {
    tft.fillRect(0, 0, SCR_W, CONTENT_H, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_UP, COL_BG);
    tft.drawString("Not recognized", SCR_W / 2, 24, 4);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawFloat(toDisp(pendingKg), 1, SCR_W / 2, 80, 6);
    tft.setTextColor(COL_NONE, COL_BG);
    tft.drawString("Is this you?", SCR_W / 2, 128, 2);
    // Two buttons: Log it (green) / Skip (grey)
    tft.fillRoundRect(BTN_L_X, BTN_Y, BTN_W, BTN_H, 8, TFT_DARKGREEN);
    tft.fillRoundRect(BTN_R_X, BTN_Y, BTN_W, BTN_H, 8, 0x4208);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString("Log it", BTN_L_X + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
    tft.setTextColor(TFT_WHITE, 0x4208);
    tft.drawString("Skip", BTN_R_X + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
    return;
  }

  // status line
  tft.fillRect(0, 0, SCR_W, 24, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.drawString(statusStr, 6, 4, 2);

  // big weight — live value if measuring, else last stored
  float kg = (liveState ? liveKg : (historyCount() ? historyLast().kg : -1));
  uint16_t c = liveState == 2 ? COL_STABLE
               : liveState == 1 ? COL_LIVE
               : COL_NONE;
  tft.fillRect(0, 40, SCR_W, 92, COL_BG);
  tft.setTextColor(c, COL_BG);
  tft.setTextDatum(MC_DATUM);
  if (kg <= 0)
    tft.drawString("--.-", SCR_W / 2, 86, 8);
  else
    tft.drawFloat(toDisp(kg), 1, SCR_W / 2, 86, 8);
  tft.setTextColor(COL_NONE, COL_BG);
  tft.drawString(unitStr(), SCR_W / 2, 146, 4);

  // delta + trend
  tft.fillRect(0, 164, SCR_W, CONTENT_H - 164, COL_BG);
  tft.setTextDatum(MC_DATUM);
  if (historyCount() > 0) {
    Trend tr = historyTrend();
    if (historyCount() > 1) {
      float delta = historyLast().kg - historyAt(historyCount() - 2).kg;
      char d[28];
      snprintf(d, sizeof(d), "%+.1f %s vs last", toDisp(delta), unitStr());
      tft.setTextColor(delta > 0.05f ? COL_UP : (delta < -0.05f ? COL_DOWN : COL_NONE), COL_BG);
      tft.drawString(d, SCR_W / 2, 172, 2);
    }
    uint16_t tc = tr == TREND_UP ? COL_UP : tr == TREND_DOWN ? COL_DOWN : COL_NONE;
    tft.setTextColor(tc, COL_BG);
    tft.drawString(trendText(tr), SCR_W / 2, 190, 2);
  }
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

// Top-centre "days since last weigh-in" nudge, shared by graph + weight screens.
static void drawDaysSince(int y) {
  int d = daysSinceLast();
  if (d < 0) return;
  char s[24];
  tft.setTextDatum(TC_DATUM);
  if (d == 0) {
    tft.setTextColor(COL_STABLE, COL_BG);
    tft.drawString("Weighed today", SCR_W / 2, y, 2);
  } else {
    tft.setTextColor(COL_UP, COL_BG);
    snprintf(s, sizeof(s), "%dd since weigh-in", d);
    tft.drawString(s, SCR_W / 2, y, 2);
  }
}

static void drawGraph() {
  tft.fillRect(0, 0, SCR_W, CONTENT_H, COL_BG);
  int n = historyCount();
  int range = RANGES[rangeIdx];

  // title (top-left) + latest value (top-right)
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BG);
  char title[20];
  if (range >= 9999) snprintf(title, sizeof(title), "All (%d)", n);
  else snprintf(title, sizeof(title), "Last %d", range);
  tft.drawString(title, 6, 4, 2);

  // days-since-last counter, centred in the top strip
  drawDaysSince(4);

  if (n < 2) {
    tft.setTextColor(COL_NONE, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("need 2+ readings", SCR_W / 2, CONTENT_H / 2, 4);
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

  // plot box — left margin for Y labels, bottom margin for X labels
  int x0 = 46, y0 = 26, x1 = SCR_W - 8, y1 = CONTENT_H - 20;
  tft.drawFastHLine(x0, y1, x1 - x0, COL_AXIS);
  tft.drawFastVLine(x0, y0, y1 - y0, COL_AXIS);

  // latest value, top-right
  char lb[14];
  tft.setTextDatum(TR_DATUM);
  bool up = historyAt(n - 1).kg > historyAt(start).kg + 0.1f;
  uint16_t lc = up ? COL_UP : COL_DOWN;
  tft.setTextColor(lc, COL_BG);
  snprintf(lb, sizeof(lb), "%.1f %s", toDisp(historyLast().kg), unitStr());
  tft.drawString(lb, x1, 4, 2);

  // Y-axis labels: max / mid / min, right-aligned just left of the axis
  tft.setTextColor(COL_NONE, COL_BG);
  tft.setTextDatum(MR_DATUM);
  snprintf(lb, sizeof(lb), "%.1f", toDisp(mx));  tft.drawString(lb, x0 - 3, y0, 1);
  snprintf(lb, sizeof(lb), "%.1f", toDisp(mid)); tft.drawString(lb, x0 - 3, (y0 + y1) / 2, 1);
  snprintf(lb, sizeof(lb), "%.1f", toDisp(mn));  tft.drawString(lb, x0 - 3, y1, 1);
  // faint mid gridline
  tft.drawFastHLine(x0 + 1, (y0 + y1) / 2, x1 - x0 - 1, COL_AXIS);

  // goal line (dashed magenta) if a goal is set
  if (setGoalKg > 0) {
    int gy = y1 - (int)((setGoalKg - mn) * (y1 - y0) / (mx - mn));
    if (gy > y0 && gy < y1) {
      for (int x = x0 + 1; x < x1; x += 6) tft.drawFastHLine(x, gy, 3, TFT_MAGENTA);
      tft.setTextColor(TFT_MAGENTA, COL_BG);
      tft.setTextDatum(TR_DATUM);
      tft.drawString("goal", x1, gy - 9, 1);
    }
  }

  // X-axis labels (dates): oldest at left, newest at right
  char d0[10], d1[10];
  fmtDate(historyAt(start).ts, d0, sizeof(d0));
  fmtDate(historyAt(n - 1).ts, d1, sizeof(d1));
  tft.setTextColor(COL_NONE, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(d0, x0, y1 + 4, 1);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(d1, x1, y1 + 4, 1);

  // the line
  int prevx = 0, prevy = 0;
  for (int i = start; i < n; i++) {
    float k = historyAt(i).kg;
    int px = (show == 1) ? x0 : x0 + (long)(i - start) * (x1 - x0) / (show - 1);
    int py = y1 - (int)((k - mn) * (y1 - y0) / (mx - mn));
    if (i > start) tft.drawLine(prevx, prevy, px, py, lc);
    tft.fillCircle(px, py, 2, lc);
    prevx = px; prevy = py;
  }

  // hint
  tft.setTextColor(COL_AXIS, COL_BG);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("tap: range", x1, 20, 1);
}

// ── Settings screen ─────────────────────────────────────────────────────────
static void drawSettings() {
  tft.fillRect(0, 0, SCR_W, CONTENT_H, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, COL_BG);
  tft.drawString("Settings", 8, 6, 4);

  char row[28];
  for (int i = 0; i < MI_COUNT; i++) {
    int y = rowY(i);
    switch (i) {
      case MI_SYNC:      snprintf(row, sizeof(row), "Sync now"); break;
      case MI_UNIT:      snprintf(row, sizeof(row), "Units: %s", unitStr()); break;
      case MI_BUZZER:    snprintf(row, sizeof(row), "Buzzer: %s", setMute ? "off" : "on"); break;
      case MI_GOAL:
        if (setGoalKg > 0) snprintf(row, sizeof(row), "Goal: %.1f %s", toDisp(setGoalKg), unitStr());
        else snprintf(row, sizeof(row), "Goal: off");
        break;
      case MI_CALIBRATE: snprintf(row, sizeof(row), "Calibrate touch"); break;
      case MI_OTA:       snprintf(row, sizeof(row), "Update (OTA)"); break;
      case MI_WIFI:      snprintf(row, sizeof(row), "WiFi reset"); break;
    }
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(row, 10, y + 4, 2);
    // Goal row carries -/+ touch buttons on the right.
    if (i == MI_GOAL) {
      tft.fillRoundRect(GB_MINUS_X, y + 1, GB_W, SET_RH - 3, 4, TFT_NAVY);
      tft.fillRoundRect(GB_PLUS_X, y + 1, GB_W, SET_RH - 3, 4, TFT_NAVY);
      tft.setTextColor(TFT_WHITE, TFT_NAVY);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("-", GB_MINUS_X + GB_W / 2, y + SET_RH / 2, 4);
      tft.drawString("+", GB_PLUS_X + GB_W / 2, y + SET_RH / 2, 4);
    }
    tft.drawFastHLine(6, y + SET_RH - 1, SCR_W - 12, 0x2104);
  }
}

static void render(bool full) {
  if (screen == SCR_TODAY) drawToday(full);
  else if (screen == SCR_GRAPH) drawGraph();
  else drawSettings();
  drawTabs();
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
    tft.fillScreen(TFT_BLACK);       // blank to black — reads as a plain mirror
    digitalWrite(PIN_TFT_BL, LOW);   // also cut BL, in case it IS wired to GPIO
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
  buzzerAccept();
  Serial.printf("[ui] stored %.2f kg delta=%+.2f trend=%s\n", kg, delta,
                trendText(historyTrend()));
  dirty = true;  // refresh whichever screen is up (graph gets the new point)
}

// ── Touch calibration routine (blocking; run from Settings or serial 'c') ───
static void runCalibrate() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.drawString("Touch calibration", 12, 14, 4);
  tft.setTextColor(COL_NONE, COL_BG);
  tft.drawString("Tap each corner arrow as it appears.", 12, 52, 2);
  mainTick = millis();                    // feed watchdog before the blocking call
  tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 20);
  tft.setTouch(calData);
  calSave();
  mainTick = millis();
  Serial.print("[touch] saved landscape calData = { ");
  for (int i = 0; i < 5; i++) Serial.printf("%u%s", calData[i], i < 4 ? ", " : " ");
  Serial.println("};");
  buzzerAccept();
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_STABLE, COL_BG);
  tft.drawString("Calibrated", SCR_W / 2, CONTENT_H / 2, 4);
  delay(900);
  dirty = true;
}

// ── OTA update mode (web upload — browser/PC POSTs the .bin TO the device) ──
static WebServer otaServer(80);

static void drawOta() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, COL_BG);
  tft.drawString("Update mode", 8, 8, 4);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.drawString("Browse to:", 8, 52, 2);
  tft.setTextColor(TFT_YELLOW, COL_BG);
  tft.drawString("http://" + WiFi.localIP().toString() + "/", 8, 76, 4);
  tft.setTextColor(COL_NONE, COL_BG);
  tft.drawString("upload .bin there", 8, 120, 2);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.drawString("tap = cancel", 8, 150, 2);
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
  Serial.printf("[diag] fw=%s up=%lus heap=%u min=%u readings=%d last=%.2f "
                "wifi=%d rssi=%d lastPull=%d\n",
                FW_VERSION, (unsigned long)(millis() / 1000),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                historyCount(), historyCount() ? historyLast().kg : 0.0,
                WiFi.status() == WL_CONNECTED, (int)WiFi.RSSI(), lastPullN);
}

static String diagHtml() {
  char buf[640];
  snprintf(buf, sizeof(buf),
           "<!doctype html><html><body style='font-family:sans-serif;text-align:center'>"
           "<h2>Smart Mirror &mdash; fw %s</h2>"
           "<p>uptime %lus &middot; heap %u (min %u)<br>"
           "readings %d &middot; last %.1f kg<br>"
           "wifi %s &middot; %d dBm &middot; %s</p>"
           "<form method='POST' action='/update' enctype='multipart/form-data'>"
           "<input type='file' name='f' accept='.bin'><br><br>"
           "<input type='submit' value='Upload firmware'></form></body></html>",
           FW_VERSION, (unsigned long)(millis() / 1000),
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
           historyCount(), historyCount() ? historyLast().kg : 0.0,
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
    tft.setTextColor(COL_UP, COL_BG);
    tft.drawString("WiFi failed", 8, 80, 4);
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
          tft.drawString("Updating...", SCR_W / 2, 70, 4);
          tft.setTextDatum(TL_DATUM);
          Update.begin(UPDATE_SIZE_UNKNOWN);
        } else if (up.status == UPLOAD_FILE_WRITE) {
          Update.write(up.buf, up.currentSize);
          mainTick = millis();   // keep the watchdog fed during the flash write
          char b[20];
          snprintf(b, sizeof(b), "%u KB", (unsigned)(up.totalSize / 1024));
          tft.fillRect(0, 118, SCR_W, 28, COL_BG);
          tft.setTextDatum(MC_DATUM);
          tft.setTextColor(TFT_WHITE, COL_BG);
          tft.drawString(b, SCR_W / 2, 130, 4);
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

// Independent watchdog: reboots if the main loop or BLE task stops ticking.
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
    uint32_t bh = MiScale::heartbeat();
    if (bh != 0 && now - bh > BLE_STALL_MS) {
      Serial.println("[wdt] BLE task stalled -> reboot");
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

// Act on a tap at (tx,ty) while the display is awake and not in OTA mode.
static void handleTap(int tx, int ty) {
  // Rejected-reading confirmation: Log it / Skip buttons.
  if (pendingActive()) {
    if (ty >= BTN_Y && ty <= BTN_Y + BTN_H) {
      if (tx >= BTN_L_X && tx <= BTN_L_X + BTN_W) recordStable(pendingKg);  // Log it
      pendingUntil = 0;
      screen = SCR_TODAY;
      todayUntil = millis() + TODAY_SHOW_MS;
      dirty = true;
    }
    return;
  }

  // Bottom tab bar → switch screen.
  if (ty >= TAB_Y) {
    int t = tx / (SCR_W / 3);
    if (t < 0) t = 0; if (t > 2) t = 2;
    if (t != screen) {
      screen = t;
      todayUntil = (screen == SCR_TODAY) ? (millis() + TODAY_SHOW_MS) : 0;
      dirty = true;
    }
    return;
  }

  // Content-area taps, per screen.
  if (screen == SCR_GRAPH) {
    rangeIdx = (rangeIdx + 1) % (int)(sizeof(RANGES) / sizeof(RANGES[0]));
    dirty = true;
  } else if (screen == SCR_SETTINGS) {
    if (ty < SET_Y0) return;
    int i = (ty - SET_Y0) / SET_RH;
    if (i < 0 || i >= MI_COUNT) return;
    switch (i) {
      case MI_SYNC: {
        tft.fillRect(0, 0, SCR_W, CONTENT_H, COL_BG);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_CYAN, COL_BG);
        tft.drawString("Syncing...", SCR_W / 2, CONTENT_H / 2, 4);
        tft.setTextDatum(TL_DATUM);
        lastPullN = cloudPull();
        (lastPullN >= 0) ? buzzerAccept() : buzzerReject();
        dirty = true;
      } break;
      case MI_UNIT:
        setUnitLb = !setUnitLb; settingsSave(); dirty = true; break;
      case MI_BUZZER:
        setMute = !setMute; buzzerSetMute(setMute); settingsSave(); dirty = true; break;
      case MI_GOAL: {
        // -/+ buttons on the right; anything else on the row toggles off.
        if (tx >= GB_MINUS_X && tx < GB_MINUS_X + GB_W) {          // "-"
          if (setGoalKg <= 0) setGoalKg = historyCount() ? historyLast().kg : 100.0f;
          setGoalKg -= 0.5f;
          if (setGoalKg < 0) setGoalKg = 0;
        } else if (tx >= GB_PLUS_X && tx < GB_PLUS_X + GB_W) {     // "+"
          if (setGoalKg <= 0) setGoalKg = historyCount() ? historyLast().kg : 100.0f;
          else setGoalKg += 0.5f;
        } else {                                                  // label toggles goal off
          setGoalKg = 0;
        }
        settingsSave();
        dirty = true;
      } break;
      case MI_CALIBRATE:
        runCalibrate(); break;
      case MI_OTA:
        wantOta = true; break;
      case MI_WIFI: {
        tft.fillRect(0, 0, SCR_W, CONTENT_H, COL_BG);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_CYAN, COL_BG);
        tft.drawString("WiFi reset...", SCR_W / 2, CONTENT_H / 2, 4);
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
  Serial.println("\n=== Smart Mirror  fw=" FW_VERSION " ===");

  buzzerBegin(PIN_BUZZER);
  tft.init();
  tft.setRotation(1);           // landscape 320x240
  tft.fillScreen(COL_BG);
  backlightBegin();             // backlight on
  lastWakeMs = millis();
  calLoad();                    // touch calibration from NVS (applies setTouch)
  if (!haveCal)
    Serial.println("[touch] no landscape calibration yet — run Settings>Calibrate (or serial 'c')");

  // Hold a finger on the screen at power-on to wipe WiFi + reopen the portal.
  delay(60);
  bool forcePortal = (tft.getTouchRawZ() > 600);

  setStatus("Syncing time");
  drawToday(true);
  drawTabs();

  // Show captive-portal join instructions on the TFT when it opens.
  setWifiPortalCallback([](const char *ap, const char *ip) {
    tft.fillScreen(COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_CYAN, COL_BG);
    tft.drawString("WiFi setup", 8, 8, 4);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawString("Join WiFi:", 8, 52, 2);
    tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.drawString(ap, 8, 74, 4);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawString("then open", 8, 120, 2);
    tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.drawString(ip, 8, 142, 4);
  });

  timeSyncAtBoot(forcePortal);
  historyBegin();
  cloudBegin();
  settingsLoad();   // units / mute / goal from NVS

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  tof.setTimeout(500);
  tofOk = tof.init();
  if (tofOk) tof.startContinuous();

  weightQ = xQueueCreate(8, sizeof(WEvt));
  statusQ = xQueueCreate(8, sizeof(SEvt));
  MiScale::setWeightCallback([](float kg, bool st, const char *csv, int imp) {
    WEvt e{kg, st, imp};
    xQueueSend(weightQ, &e, 0);
  });
  MiScale::setStatusCallback([](const char *s) {
    SEvt e;
    strncpy(e.s, s, sizeof(e.s) - 1);
    e.s[sizeof(e.s) - 1] = 0;
    xQueueSend(statusQ, &e, 0);
  });
  MiScale::startTask();

  setStatus("Ready");
  dirty = true;
  buzzerTone(2000, 120);

  mainTick = millis();
  xTaskCreatePinnedToCore(watchdogTask, "wdt", 3072, nullptr, 1, nullptr, 1);
}

static uint32_t lastTof = 0;
static uint32_t lastLiveDraw = 0;

void loop() {
  mainTick = millis();   // feed the watchdog

  // Serial dev hooks: o=OTA, p=pull, d=diag, c=calibrate touch.
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'o' || c == 'O') wantOta = true;
    else if (c == 'p' || c == 'P') { lastPullN = cloudPull(); dirty = true; }
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

  bool statusChanged = false;

  // Status from BLE
  SEvt se;
  while (xQueueReceive(statusQ, &se, 0) == pdTRUE) {
    setStatus(se.s);
    statusChanged = true;
  }

  // Weight from BLE (BLE layer de-dups stable readings). A weigh-in always
  // wakes the display — even at night — since stepping on the scale is deliberate.
  WEvt we;
  bool gotStable = false;
  while (xQueueReceive(weightQ, &we, 0) == pdTRUE) {
    lastWeighMs = millis();            // note scale activity → hold off WiFi sync
    if (we.stable) {
      if (validateReading(we.kg)) {
        recordStable(we.kg);                     // accepted → store + sync + chirp
      } else {
        pendingKg = we.kg;                       // rejected → await confirmation
        pendingUntil = millis() + REJECT_CONFIRM_MS;
        buzzerReject();
        dirty = true;
      }
      gotStable = true;
    } else {
      liveKg = we.kg;
      liveState = 1;
    }
    // Pop up the weight window for 30 s, then it reverts to the graph.
    if (screen != SCR_TODAY) { screen = SCR_TODAY; dirty = true; }
    todayUntil = millis() + TODAY_SHOW_MS;
    displayWake();
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

  // ToF presence — wakes the display only OUTSIDE quiet hours
  if (millis() - lastTof > 150) {
    lastTof = millis();
    if (tofOk) {
      int mm = tof.readRangeContinuousMillimeters();
      if (!tof.timeoutOccurred() && mm > 0) {
        bool p = present;
        if (!present && mm < PRESENCE_MM) p = true;
        else if (present && mm > PRESENCE_CLEAR_MM) p = false;
        present = p;   // used only for the sync-busy guard + presence wake now
        if (present && !quietHours()) displayWake();  // keep awake while present
      }
    }
  }

  // A rejected reading that wasn't confirmed within the window → discard, go to graph.
  static bool wasPending = false;
  bool pend = pendingActive();
  if (wasPending && !pend) {
    pendingUntil = 0;
    screen = SCR_GRAPH;
    todayUntil = 0;
    dirty = true;
  }
  wasPending = pend;

  // Auto-revert from the weight window back to the graph after 30 s.
  if (screen == SCR_TODAY && todayUntil != 0 && millis() > todayUntil) {
    screen = SCR_GRAPH;
    todayUntil = 0;
    dirty = true;
  }

  // Sleep the display after inactivity — but stay awake during the 30 s window.
  bool inTodayWindow = (todayUntil != 0 && millis() <= todayUntil);
  if (displayOn && !inTodayWindow && millis() - lastWakeMs > DISPLAY_TIMEOUT_MS)
    displaySleep();

  // Cloud sync: flush the queue when idle (WiFi comes up briefly). This BLOCKS
  // the loop for the WiFi-connect duration and lights up the radio, which starves
  // the BLE weigh-in stream — so we must sync only when clearly idle, and back
  // off hard whenever WiFi is unavailable (else a stuck reading retries forever,
  // making frequent blocking windows that swallow scale readings).
  static uint32_t lastSyncMs = 0;
  static uint32_t syncBackoffMs = 20000;   // grows on failure, resets on success
  bool recentWeigh = (lastWeighMs != 0 && millis() - lastWeighMs < 20000);
  bool busy = (strcmp(statusStr, "Reading") == 0) || present || recentWeigh;
  if (cloudHasPending() && !busy && (millis() - lastSyncMs > syncBackoffMs)) {
    setStatus("Syncing");
    if (displayOn && screen == SCR_TODAY) drawToday(false);
    int sent = cloudFlush();    // blocking: WiFi up → POST → WiFi off
    lastSyncMs = millis();
    if (sent > 0) syncBackoffMs = 20000;                       // success → fast again
    else { syncBackoffMs *= 3; if (syncBackoffMs > 900000) syncBackoffMs = 900000; }
    setStatus("Ready");
    statusChanged = true;
  }

  // Redraw (only when the display is awake)
  if (displayOn) {
    if (dirty) {
      render(true);
      dirty = false;
    } else if (screen == SCR_TODAY) {
      if (statusChanged || gotStable) {
        drawToday(false);
      } else if (liveState == 1 && !pendingActive() && millis() - lastLiveDraw > 200) {
        drawToday(false);           // throttle live updates to ~5 Hz
        lastLiveDraw = millis();
      }
    }
  }

  delay(5);
}
