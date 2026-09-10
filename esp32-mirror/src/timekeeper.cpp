#include "timekeeper.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_wifi.h>

#include "config.h"

static std::function<void(const char *, const char *)> portalCb;

void setWifiPortalCallback(std::function<void(const char *, const char *)> cb) {
  portalCb = cb;
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

bool timeSyncAtBoot(bool forcePortal) {
  // Do we already have saved WiFi credentials in NVS?
  WiFi.mode(WIFI_STA);
  wifi_config_t conf = {};
  bool haveCreds = false;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK)
    haveCreds = strlen((const char *)conf.sta.ssid) > 0;

  bool connected = false;
  if (forcePortal || !haveCreds) {
    // First-time setup or a forced re-provision: open the captive portal.
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setAPCallback([](WiFiManager *w) {
      Serial.printf("[wifi] setup portal: join '%s' then browse 192.168.4.1\n",
                    WIFI_AP_NAME);
      if (portalCb) portalCb(WIFI_AP_NAME, "192.168.4.1");
    });
    if (forcePortal) {
      Serial.println("[wifi] forced re-provision (encoder held at boot)");
      wm.resetSettings();
    }
    connected = wm.autoConnect(WIFI_AP_NAME);
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
