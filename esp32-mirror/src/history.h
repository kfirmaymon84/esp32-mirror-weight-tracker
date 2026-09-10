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
Reading historyAt(int i);            // 0 = oldest kept
Reading historyLast();               // most recent (valid only if count>0)

// Average of the most recent `n` readings, optionally skipping the very last one
// (use skipLast=true to get the baseline *before* the newest reading).
float historyAvg(int n, bool skipLast);

// Classify newest vs the trailing average that preceded it.
Trend historyTrend();
const char *trendText(Trend t);
