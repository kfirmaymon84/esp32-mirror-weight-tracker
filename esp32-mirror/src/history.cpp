#include "history.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <math.h>

#include "config.h"

static const char *PATH = "/history.csv";

static Reading ring[HISTORY_MAX];
static int count = 0;   // number of valid entries in ring (<= HISTORY_MAX)
static int head = 0;    // index of oldest entry when full (ring buffer start)
static float trend[HISTORY_MAX];  // EWMA per reading, logical order (0 = oldest)

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

static void computeTrend() {
  float t = 0;
  uint32_t prevTs = 0;
  for (int i = 0; i < count; i++) {
    Reading r = ringGet(i);
    if (i == 0) {
      t = r.kg;
    } else {
      float days = 1;
      if (r.ts > 1700000000UL && prevTs > 1700000000UL && r.ts > prevTs)
        days = (r.ts - prevTs) / 86400.0f;
      if (days < 1) days = 1;
      t += (1.0f - powf(1.0f - TREND_ALPHA, days)) * (r.kg - t);
    }
    trend[i] = t;
    prevTs = r.ts;
  }
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
  computeTrend();
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
    computeTrend();
    Serial.printf("[hist] ~updated today's reading %.2f kg (count=%d)\n", kg, count);
    return;
  }
  ringPush(ts, kg);
  computeTrend();
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

float historyTrendAt(int i) { return (i >= 0 && i < count) ? trend[i] : NAN; }
float historyTrendLast() { return count ? trend[count - 1] : NAN; }

float historyTrendDaysAgo(int days, uint32_t now) {
  if (now < 1700000000UL) return NAN;
  uint32_t cut = now - (uint32_t)days * 86400UL;
  for (int i = count - 1; i >= 0; i--) {
    uint32_t ts = ringGet(i).ts;
    if (ts > 1700000000UL && ts <= cut) return trend[i];
  }
  return NAN;
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
static long civilDays(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

long historyDay(uint32_t ts) {
  if (ts < 1700000000UL) return -1;
  time_t t = (time_t)ts;
  struct tm tm;
  localtime_r(&t, &tm);
  return civilDays(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

int historyStreak(uint32_t now) {
  long today = historyDay(now);
  if (today < 0 || count == 0) return 0;
  long expect = -1;   // the next (earlier) day the streak needs
  int streak = 0;
  for (int i = count - 1; i >= 0; i--) {
    long d = historyDay(ringGet(i).ts);
    if (d < 0) continue;
    if (expect < 0) {                       // newest clocked reading
      if (d != today && d != today - 1) return 0;
      streak = 1;
      expect = d - 1;
    } else if (d == expect + 1) {
      continue;                             // same day as the one just counted
    } else if (d == expect) {
      streak++;
      expect--;
    } else {
      break;
    }
  }
  return streak;
}

uint8_t historyWeekMask(uint32_t now) {
  long today = historyDay(now);
  if (today < 0) return 0;
  uint8_t m = 0;
  for (int i = count - 1; i >= 0; i--) {
    long d = historyDay(ringGet(i).ts);
    if (d < 0) continue;
    long ago = today - d;
    if (ago > 6) break;
    if (ago >= 0) m |= 1 << (6 - ago);
  }
  return m;
}

float historyAvgPrevDays(int days, uint32_t now) {
  long today = historyDay(now);
  if (today < 0) return NAN;
  float sum = 0;
  int k = 0;
  for (int i = count - 1; i >= 0; i--) {
    long d = historyDay(ringGet(i).ts);
    if (d < 0) continue;
    if (today - d > days) break;
    if (d < today) { sum += ringGet(i).kg; k++; }
  }
  return k ? sum / k : NAN;
}

static const char *REAL_PATH = "/history.real.csv";

int historySeedDemo(uint32_t now) {
  if (now < 1700000000UL) return 0;
  // Keep the real log aside (only the first time, so seeding twice is safe).
  if (!LittleFS.exists(REAL_PATH)) {
    File src = LittleFS.open(PATH, "r");
    File dst = LittleFS.open(REAL_PATH, "w");
    if (src && dst)
      while (src.available()) dst.write(src.read());
    if (src) src.close();
    if (dst) dst.close();
  }
  // Real readings: demo days stop before the first of them; aim the demo's end
  // at the first real weight so the trend leads into it.
  String real;
  uint32_t firstReal = now;
  float endKg = count ? ringGet(count - 1).kg : 80.0f;
  File r = LittleFS.open(REAL_PATH, "r");
  if (r) {
    while (r.available()) {
      String line = r.readStringUntil('\n');
      line.trim();
      int comma = line.indexOf(',');
      if (comma < 0) continue;
      uint32_t ts = (uint32_t)line.substring(0, comma).toInt();
      float kg = line.substring(comma + 1).toFloat();
      if (ts > 1700000000UL && ts < firstReal) { firstReal = ts; endKg = kg; }
      real += line + "\n";
    }
    r.close();
  }
  const int DAYS = 120;
  long lastDay = historyDay(firstReal) - 1;
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return ((seed >> 16) & 0x7FFF) / 32767.0f; };
  File f = LittleFS.open(PATH, "w");
  if (!f) return 0;
  int added = 0;
  for (int d = DAYS; d >= 1; d--) {
    float noise = (rnd() - 0.5f) * 1.2f;
    bool skip = rnd() < 0.12f;                        // a few missed days
    if (skip && d > 9) continue;                      // ...but a clean last week (streak)
    uint32_t ts = firstReal - (uint32_t)d * 86400UL + (uint32_t)(rnd() * 1800) - 900;
    if (historyDay(ts) > lastDay) continue;
    float f01 = (float)d / DAYS;                      // 1 = oldest
    float kg = endKg + 4.6f * f01 + 0.35f * sinf(d * 0.9f) + noise;
    f.printf("%u,%.2f\n", (unsigned)ts, kg);
    added++;
  }
  f.print(real);
  f.close();
  historyReload();
  Serial.printf("[hist] demo: +%d readings (real log kept in %s)\n", added, REAL_PATH);
  return added;
}

bool historyRestoreReal() {
  if (!LittleFS.exists(REAL_PATH)) return false;
  LittleFS.remove(PATH);
  bool ok = LittleFS.rename(REAL_PATH, PATH);
  historyReload();
  Serial.printf("[hist] real log restored (%d readings)\n", count);
  return ok;
}
