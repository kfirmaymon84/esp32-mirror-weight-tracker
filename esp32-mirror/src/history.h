#pragma once
#include <stdint.h>
#include <time.h>

// Append-only weight log on LittleFS, mirrored in a RAM ring for fast trend/graph.

struct Reading {
  uint32_t ts;   // epoch seconds (0 if clock wasn't set)
  float kg;
};

enum Trend { TREND_NONE, TREND_UP, TREND_DOWN, TREND_STEADY };

void historyBegin();                 // mount FS + load recent readings
void historyReload();                // re-read the log file into RAM (after a cloud pull)
void historyAdd(uint32_t ts, float kg);

int historyCount();
// Once the clock is set: give the latest reading, if it was taken unclocked
// earlier in this boot, its real time (now minus the time since it was taken).
void historyDateUnclocked(uint32_t now);
Reading historyAt(int i);            // 0 = oldest kept
Reading historyLast();               // most recent (valid only if count>0)

// Average of the most recent `n` readings, optionally skipping the very last one
// (use skipLast=true to get the baseline *before* the newest reading).
float historyAvg(int n, bool skipLast);

// Classify newest vs the trailing average that preceded it.
Trend historyTrend();
const char *trendText(Trend t);

// ── Trend weight (EWMA) ─────────────────────────────────────────────────────
// Time-aware exponential moving average of the readings (Hacker's Diet style:
// each day pulls the trend TREND_ALPHA of the way toward that day's weight), so
// day-to-day water noise is smoothed out. Recomputed whenever readings change.
float historyTrendAt(int i);         // trend value at reading i (0 = oldest)
float historyTrendLast();            // current trend weight (NAN if no readings)
// Trend as it stood `days` days before `now` (the last reading at or before
// that moment). NAN if there is no reading that old.
float historyTrendDaysAgo(int days, uint32_t now);

// ── Calendar days (local time) ──────────────────────────────────────────────
long historyDay(uint32_t ts);        // local calendar day number (-1 if unclocked)
// Consecutive days with a weigh-in, ending today — or yesterday, so a streak
// isn't shown as broken before today's weigh-in has happened.
int historyStreak(uint32_t now);
// Bit i set = weighed in on day (today - 6 + i); bit 6 = today.
uint8_t historyWeekMask(uint32_t now);
// Mean of the readings from the `days` calendar days before today (today itself
// excluded). NAN if there are none.
float historyAvgPrevDays(int days, uint32_t now);

// ── Demo data (dev: serial 'f' / 'r') ───────────────────────────────────────
// Back the real log up to /history.real.csv, then write ~4 months of made-up
// daily readings ending at the real ones (local only, never sent to the sheet).
// Returns the number of demo readings added (0 if the clock isn't set).
int historySeedDemo(uint32_t now);
bool historyRestoreReal();           // put the backed-up real log back
