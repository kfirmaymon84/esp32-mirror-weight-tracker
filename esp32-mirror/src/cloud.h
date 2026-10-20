#pragma once
#include <stdint.h>

// Google Sheet sync via a Google Apps Script web app.
// Readings are queued to LittleFS so nothing is lost when offline; the queue is
// flushed (WiFi up → POST → WiFi off) at a safe time when BLE is idle.
//
// The sheet is a PER-DEVICE setting (NVS), entered by each user on the WiFi
// setup page — nothing is compiled in, so a unit never reaches someone else's
// sheet. Empty URL = cloud off: the device works fully offline (readings are
// still queued locally and flush if a sheet is added later).

void cloudBegin();                         // load the sheet config from NVS
void cloudSetConfig(const char *url, const char *token);  // trim + persist
bool cloudConfigured();                    // a sheet URL is set
const char *cloudUrl();
const char *cloudToken();
void cloudQueue(uint32_t ts, float kg);   // add a reading to the pending queue
bool cloudHasPending();                    // any unsynced readings?
int  cloudPendingCount();
// Bring WiFi up, POST all pending readings, WiFi off. Returns #synced.
int  cloudFlush();

// Pull the sheet down (sheet = source of truth): GET the CSV, overwrite the
// local history log, reload it. Returns #readings loaded, or -1 on failure.
int  cloudPull();
