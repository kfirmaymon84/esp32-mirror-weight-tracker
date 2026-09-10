#include "history.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <math.h>

#include "config.h"

static const char *PATH = "/history.csv";

static Reading ring[HISTORY_MAX];
static int count = 0;   // number of valid entries in ring (<= HISTORY_MAX)
static int head = 0;    // index of oldest entry when full (ring buffer start)

static void ringPush(uint32_t ts, float kg) {
  if (count < HISTORY_MAX) {
    ring[count++] = {ts, kg};
  } else {
    ring[head] = {ts, kg};
    head = (head + 1) % HISTORY_MAX;
  }
}

static Reading ringGet(int i) {  // i: 0=oldest
  int idx = (count < HISTORY_MAX) ? i : (head + i) % HISTORY_MAX;
  return ring[idx];
}

void historyReload() {
  count = 0;
  head = 0;
  File f = LittleFS.open(PATH, "r");
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.isEmpty()) continue;
      int comma = line.indexOf(',');
      if (comma < 0) continue;
      uint32_t ts = (uint32_t)line.substring(0, comma).toInt();
      float kg = line.substring(comma + 1).toFloat();
      if (kg > 0) ringPush(ts, kg);
    }
    f.close();
  }
  // Sort by timestamp ascending — sheet rows (after a cloud pull) may be
  // unordered; the graph and one-per-day logic expect chronological order.
  // On a fresh reload head==0, so entries live in ring[0..count-1].
  for (int i = 1; i < count; i++) {
    Reading key = ring[i];
    int j = i - 1;
    while (j >= 0 && ring[j].ts > key.ts) {
      ring[j + 1] = ring[j];
      j--;
    }
    ring[j + 1] = key;
  }
  Serial.printf("[hist] loaded %d readings\n", count);
}

void historyBegin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[hist] LittleFS mount failed");
    return;
  }
  historyReload();
}

static bool sameDay(uint32_t a, uint32_t b) {
  if (a < 1700000000UL || b < 1700000000UL) return false;
  time_t ta = (time_t)a, tb = (time_t)b;
  struct tm A, B;
  localtime_r(&ta, &A);
  localtime_r(&tb, &B);
  return A.tm_year == B.tm_year && A.tm_yday == B.tm_yday;
}

static void rewriteFile() {
  File f = LittleFS.open(PATH, "w");
  if (!f) return;
  for (int i = 0; i < count; i++) {
    Reading r = ringGet(i);
    f.printf("%u,%.2f\n", (unsigned)r.ts, r.kg);
  }
  f.close();
}

void historyAdd(uint32_t ts, float kg) {
  // One reading per day: a same-day weigh-in replaces the day's entry, keeping
  // only the latest. When the clock isn't set (ts==0) we can't tell days apart,
  // so consecutive unclocked readings also collapse onto the last one.
  bool replaceLast = false;
  if (count > 0) {
    uint32_t lastTs = ringGet(count - 1).ts;
    if (lastTs < 1700000000UL && ts < 1700000000UL) replaceLast = true;  // both unclocked
    else if (sameDay(lastTs, ts)) replaceLast = true;                    // same calendar day
  }
  if (replaceLast) {
    int idx = (count < HISTORY_MAX) ? (count - 1) : ((head + count - 1) % HISTORY_MAX);
    ring[idx] = {ts, kg};
    rewriteFile();
    Serial.printf("[hist] ~updated today's reading %.2f kg (count=%d)\n", kg, count);
    return;
  }
  ringPush(ts, kg);
  File f = LittleFS.open(PATH, "a");
  if (f) {
    f.printf("%u,%.2f\n", (unsigned)ts, kg);
    f.close();
  }
  Serial.printf("[hist] +reading %.2f kg @ %u (count=%d)\n", kg, (unsigned)ts,
                count);
}

int historyCount() { return count; }
Reading historyAt(int i) { return ringGet(i); }
Reading historyLast() { return ringGet(count - 1); }

float historyAvg(int n, bool skipLast) {
  int end = count - (skipLast ? 1 : 0);  // exclusive
  if (end <= 0) return NAN;
  int start = end - n;
  if (start < 0) start = 0;
  float sum = 0;
  int k = 0;
  for (int i = start; i < end; i++) {
    sum += ringGet(i).kg;
    k++;
  }
  return k ? sum / k : NAN;
}

Trend historyTrend() {
  if (count == 0) return TREND_NONE;
  if (count == 1) return TREND_NONE;
  float avg = historyAvg(TREND_WINDOW, true);  // baseline before newest
  if (isnan(avg)) return TREND_NONE;
  float kg = historyLast().kg;
  if (kg > avg + TREND_UP_KG) return TREND_UP;
  if (kg < avg - TREND_UP_KG) return TREND_DOWN;
  return TREND_STEADY;
}

const char *trendText(Trend t) {
  switch (t) {
    case TREND_UP: return "Trending up";
    case TREND_DOWN: return "Trending down";
    case TREND_STEADY: return "Holding steady";
    default: return "Reading saved";
  }
}
