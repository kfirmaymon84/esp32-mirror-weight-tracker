#include "okok_scale.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <math.h>
#include <string.h>

namespace {

OkokScale::WeightCallback s_weightCb;
OkokScale::StatusCallback s_statusCb;
volatile uint32_t s_heartbeat = 0;
char s_mac[18] = "";               // learned scale MAC (lowercase), "" = any

// Weigh-in state, shared by the NimBLE host task (onResult) and the scan task
// (idle timeout) — guarded by s_lock.
SemaphoreHandle_t s_lock = nullptr;
bool s_inSession = false;          // scale is broadcasting a weigh-in
bool s_emitted = false;            // final reading already reported for this stand
float s_emittedKg = 0;
uint32_t s_lastPktMs = 0;
uint32_t s_lastLiveMs = 0;
float s_lastLiveKg = -1;

const uint32_t SESSION_IDLE_MS = 5000;  // no packet this long -> weigh-in over
const float STEP_OFF_FRACTION = 0.5f;   // live weight below half -> stepped off
const float MIN_KG = 1.0f;              // ignore an empty / near-empty platform
const uint32_t LIVE_MIN_GAP_MS = 150;   // throttle live updates

void status(const char *s) {
  if (s_statusCb) s_statusCb(s);
}

// One decoded packet from our scale (called with s_lock held).
void onPacket(float kg, bool stable, const char *mac) {
  uint32_t now = millis();
  s_lastPktMs = now;
  if (!s_inSession) {
    s_inSession = true;
    s_emitted = false;
    s_lastLiveKg = -1;
    status("Reading");
  }

  // Stepped off and back on while the scale stayed awake -> a new stand.
  if (s_emitted && kg < s_emittedKg * STEP_OFF_FRACTION) s_emitted = false;

  if (stable) {
    if (!s_emitted && kg >= MIN_KG) {
      s_emitted = true;
      s_emittedKg = kg;
      if (!s_mac[0]) {                 // first lock -> remember this scale
        strncpy(s_mac, mac, sizeof(s_mac) - 1);
        s_mac[sizeof(s_mac) - 1] = 0;
        Serial.printf("[okok] learned scale %s\n", s_mac);
      }
      Serial.printf("[okok] FINAL %.2f kg\n", kg);
      if (s_weightCb) s_weightCb(kg, true);
    }
    return;
  }

  // Live frame. After the lock the scale keeps broadcasting the same stand;
  // ignore those until a new stand starts.
  if (s_emitted || kg < MIN_KG) return;
  if (now - s_lastLiveMs < LIVE_MIN_GAP_MS) return;
  if (fabsf(kg - s_lastLiveKg) < 0.05f && now - s_lastLiveMs < 1000) return;
  s_lastLiveMs = now;
  s_lastLiveKg = kg;
  if (s_weightCb) s_weightCb(kg, false);
}

class AdvCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *dev) override {
    if (!dev->haveManufacturerData()) return;
    std::string m = dev->getManufacturerData();     // includes company-id bytes
    if (m.size() != 15) return;
    const uint8_t *b = (const uint8_t *)m.data();
    if (b[0] != 0xC0 || b[6] != 0x0A || b[7] != 0x11) return;

    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             b[9], b[10], b[11], b[12], b[13], b[14]);
    if (s_mac[0] && strcmp(mac, s_mac) != 0) return;   // someone else's scale

    float kg = ((b[2] << 8) | b[3]) / 100.0f;
    bool stable = (b[8] == 0x25);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    onPacket(kg, stable, mac);
    xSemaphoreGive(s_lock);
  }
};

void scanTask(void *) {
  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new AdvCallbacks(), true);  // want duplicates
  scan->setActiveScan(false);       // passive: the weight is in the advert itself
  scan->setInterval(100);
  scan->setWindow(80);              // leave radio time for WiFi coexistence
  scan->setDuplicateFilter(false);  // payload changes every packet — see them all
  scan->setMaxResults(0);           // callbacks only, don't accumulate results
  scan->start(0, nullptr, false);   // 0 = scan forever
  Serial.println("[okok] passive scan running");
  status("Ready");

  for (;;) {
    s_heartbeat = millis();
    if (!scan->isScanning()) {
      Serial.println("[okok] scan stopped -> restarting");
      scan->start(0, nullptr, false);
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (s_inSession && millis() - s_lastPktMs > SESSION_IDLE_MS) {
        s_inSession = false;
        s_emitted = false;
        s_lastLiveKg = -1;
        status("Ready");
      }
      xSemaphoreGive(s_lock);
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace

namespace OkokScale {

void setWeightCallback(WeightCallback cb) { s_weightCb = cb; }
void setStatusCallback(StatusCallback cb) { s_statusCb = cb; }
uint32_t heartbeat() { return s_heartbeat; }

void setLearnedMac(const char *mac) {
  strncpy(s_mac, mac ? mac : "", sizeof(s_mac) - 1);
  s_mac[sizeof(s_mac) - 1] = 0;
}
const char *learnedMac() { return s_mac; }

void startTask() {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(scanTask, "okok", 6144, nullptr, 2, nullptr, 0);
}

}  // namespace OkokScale
