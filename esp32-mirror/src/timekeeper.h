#pragma once
#include <functional>
#include <time.h>

// Provisions WiFi via WiFiManager (captive portal on first setup / when forced),
// syncs time over NTP, then turns WiFi OFF (the RTC keeps running, so timestamps
// stay real while the radio is free for BLE). forcePortal wipes saved creds and
// opens the setup portal. Returns true if the clock got set.
bool timeSyncAtBoot(bool forcePortal);

bool timeIsSet();       // true once NTP has set a plausible time
time_t nowEpoch();      // current epoch seconds (0-ish if never synced)

// On-demand WiFi using the SAVED credentials (shared by NTP + cloud sync).
// Connect, do work, then wifiOff() to free the radio for BLE.
bool wifiConnect(uint32_t timeoutMs);
void wifiOff();

// If the clock isn't set yet and WiFi is up, kick an NTP sync (up to waitMs).
// Lets a later cloud sync backfill the time when boot had no network.
void ntpEnsure(uint32_t waitMs);

// Called when the captive portal opens (so the UI can show join instructions).
void setWifiPortalCallback(std::function<void(const char *apName, const char *ip)> cb);
