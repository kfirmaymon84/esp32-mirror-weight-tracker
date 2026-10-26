#pragma once
#include <functional>
#include <time.h>

// Provisions WiFi via WiFiManager (captive portal on first setup / when forced),
// syncs time over NTP, then turns WiFi OFF (the RTC keeps running, so timestamps
// stay real while the radio is free for BLE). forcePortal wipes saved creds and
// opens the setup portal. Returns true if the clock got set.
bool timeSyncAtBoot(bool forcePortal);
// Local time zone (calendar days), set before anything uses local time: the
// one chosen on the phone page (timezones.h), else TZ_DEFAULT.
void timeZoneBegin();
const char *timeZoneCity();    // e.g. "London", for the Settings tile

bool timeIsSet();       // true once NTP has set a plausible time
time_t nowEpoch();      // current epoch seconds (0-ish if never synced)

// On-demand WiFi using the SAVED credentials (shared by NTP + cloud sync).
// Connect, do work, then wifiOff() to free the radio for BLE.
bool wifiConnect(uint32_t timeoutMs);
void wifiOff();
const char *wifiSavedSsid();   // network saved by the setup portal ("" if none)

// If the clock isn't set yet and WiFi is up, kick an NTP sync (up to waitMs).
// Lets a later cloud sync backfill the time when boot had no network.
void ntpEnsure(uint32_t waitMs);

// The phone page the setup network serves: first setup (Wi-Fi + account + time
// zone), or opened from Settings to sign in or to change the time zone.
enum PortalKind { PORTAL_SETUP, PORTAL_ACCOUNT, PORTAL_TIMEZONE };

// Called when the captive portal opens (so the UI can show join instructions).
void setWifiPortalCallback(std::function<void(const char *apName, PortalKind kind)> cb);

// From Settings: the phone page with the account fields (PORTAL_ACCOUNT) and/or
// the time zone; Wi-Fi stays as it is. Blocks up to 3 min; tick() is called
// while waiting and returns true to cancel. True if the page was saved; the
// account outcome is in accountSetupMessage(), tzChanged says if the zone changed.
bool phonePortal(PortalKind kind, std::function<bool()> tick, bool *tzChanged = nullptr);
// What the account fields did last ("Signed in as ...", "Wrong email or
// password", ...); "" if they were left empty. ok = it succeeded.
const char *accountSetupMessage(bool *ok);
