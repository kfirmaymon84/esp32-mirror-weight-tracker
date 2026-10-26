#include "timekeeper.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_wifi.h>

#include "cloud.h"
#include "config.h"

static std::function<void(const char *, bool)> portalCb;
static char savedSsid[33] = "";
static char accountMsg[80] = "";             // result of the account fields, for the UI
static bool accountOk = false;

const char *wifiSavedSsid() { return savedSsid; }

void setWifiPortalCallback(std::function<void(const char *, bool)> cb) { portalCb = cb; }

const char *accountSetupMessage(bool *ok) {
  if (ok) *ok = accountOk;
  return accountMsg;
}

// Account fields on the setup page (only when the build has a cloud project).
// The password goes from the phone to the device over the setup network and
// from the device to Google over verified TLS; the device keeps only a token.
struct AccountFields {
  WiFiManagerParameter hint{
      "<hr><p><b>Your account</b><br>Your weigh-ins are saved to it, so they're safe "
      "and follow you to a new device. Leave it empty to keep them on this device only.</p>"};
  WiFiManagerParameter email{"email", "Email", cloudEmail(), 64,
                             "type='email' autocomplete='username'"};
  WiFiManagerParameter pass{"pass", "Password (6 or more characters)", "", 64,
                            "type='password' autocomplete='current-password'"};
  WiFiManagerParameter create{"create", "I'm new: create my account", "1", 2,
                              "type='checkbox'", WFM_LABEL_AFTER};
  WiFiManagerParameter reset{"reset", "Forgot my password: email me a reset link", "1", 2,
                             "type='checkbox'", WFM_LABEL_AFTER};
  void addTo(WiFiManager &wm) {
    wm.addParameter(&hint);
    wm.addParameter(&email);
    wm.addParameter(&pass);
    wm.addParameter(&create);
    wm.addParameter(&reset);
  }
};

// Act on the account fields once WiFi is up. Unchecked boxes come back empty.
static void applyAccount(AccountFields &a) {
  String email = a.email.getValue();
  email.trim();
  email.toLowerCase();
  String pw = a.pass.getValue();
  a.pass.setValue("", 64);                   // don't keep the password around
  accountMsg[0] = 0;
  accountOk = false;
  if (!email.length()) return;               // offline use: nothing to do
  if (a.reset.getValue()[0]) {
    accountOk = cloudSendPasswordReset(email.c_str());
    snprintf(accountMsg, sizeof(accountMsg), accountOk ? "Reset link sent to %s"
                                                       : "Couldn't send the reset email to %s",
             email.c_str());
  } else if (!pw.length()) {
    snprintf(accountMsg, sizeof(accountMsg), "Enter your password too");
  } else {
    CloudAuth r = cloudSignIn(email.c_str(), pw.c_str(), a.create.getValue()[0] != 0);
    accountOk = r == AUTH_OK;
    if (accountOk) snprintf(accountMsg, sizeof(accountMsg), "Signed in as %s", email.c_str());
    else snprintf(accountMsg, sizeof(accountMsg), "%s", cloudAuthText(r));
  }
  Serial.printf("[cloud] setup page: %s\n", accountMsg);
}

bool accountPortal(std::function<void()> tick) {
  if (!cloudAvailable()) return false;
  WiFiManager wm;
  AccountFields acct;
  acct.addTo(wm);
  bool saved = false;
  wm.setSaveParamsCallback([&saved]() { saved = true; });
  std::vector<const char *> menu = {"param", "exit"};      // "Setup" page only
  wm.setMenu(menu);
  wm.setConfigPortalBlocking(false);
  wm.setAPCallback([](WiFiManager *) {
    Serial.printf("[wifi] account portal: join '%s', tap Setup\n", WIFI_AP_NAME);
    if (portalCb) portalCb(WIFI_AP_NAME, true);
  });
  wm.startConfigPortal(WIFI_AP_NAME);
  uint32_t t0 = millis();
  while (!saved && millis() - t0 < 180000) {
    wm.process();
    if (tick) tick();
    delay(5);
  }
  wm.stopConfigPortal();
  if (saved) applyAccount(acct);             // connects with the saved network
  wifiOff();
  return saved;
}

bool timeIsSet() { return time(nullptr) > 1700000000; }  // after ~2023-11

time_t nowEpoch() { return time(nullptr); }

bool wifiConnect(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin();  // reconnect using credentials saved by WiFiManager
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void ntpEnsure(uint32_t waitMs) {
  if (timeIsSet()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.windows.com");
  uint32_t t0 = millis();
  while (!timeIsSet() && millis() - t0 < waitMs) delay(200);
  if (timeIsSet()) Serial.println("[time] clock backfilled via NTP");
}

void timeZoneBegin() {
  setenv("TZ", TZ_INFO, 1);
  tzset();
}

bool timeSyncAtBoot(bool forcePortal) {
  timeZoneBegin();

  // Do we already have saved WiFi credentials in NVS?
  WiFi.mode(WIFI_STA);
  wifi_config_t conf = {};
  bool haveCreds = false;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK)
    haveCreds = strlen((const char *)conf.sta.ssid) > 0;
  strlcpy(savedSsid, (const char *)conf.sta.ssid, sizeof(savedSsid));

  bool connected = false;
  if (forcePortal || !haveCreds) {
    // First-time setup or a forced re-provision: open the captive portal.
    // Besides WiFi, the user can sign in to (or create) their account here;
    // empty = the device runs offline.
    WiFiManager wm;
    AccountFields acct;
    if (cloudAvailable()) acct.addTo(wm);
    wm.setConfigPortalTimeout(180);
    wm.setAPCallback([](WiFiManager *w) {
      Serial.printf("[wifi] setup portal: join '%s' then browse 192.168.4.1\n",
                    WIFI_AP_NAME);
      if (portalCb) portalCb(WIFI_AP_NAME, false);
    });
    if (forcePortal) {
      Serial.println("[wifi] forced re-provision (screen held at boot)");
      wm.resetSettings();
    }
    connected = wm.autoConnect(WIFI_AP_NAME);
    if (connected) {
      strlcpy(savedSsid, WiFi.SSID().c_str(), sizeof(savedSsid));
      if (cloudAvailable()) applyAccount(acct);
    }
  } else {
    // Have creds: just try to connect. If the network is temporarily down we
    // do NOT open the portal — we boot without a clock and retry on next sync.
    Serial.printf("[wifi] connecting to saved '%s'...\n",
                  (const char *)conf.sta.ssid);
    connected = wifiConnect(12000);
  }

  if (!connected) {
    Serial.println("[wifi] no connection — continuing, will retry later");
    wifiOff();
    return false;
  }

  Serial.printf("[time] WiFi ok (%s), syncing NTP...\n",
                WiFi.localIP().toString().c_str());
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.windows.com");

  uint32_t t1 = millis();
  while (!timeIsSet() && millis() - t1 < 8000) delay(200);
  bool ok = timeIsSet();

  wifiOff();  // free the radio for BLE; the RTC keeps time

  if (ok) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    Serial.printf("[time] synced: %s\n", buf);
  } else {
    Serial.println("[time] NTP sync timed out");
  }
  return ok;
}
