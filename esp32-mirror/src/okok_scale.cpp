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
float s_lastFinalKg = 0;           // last reported final weight + when (echo filter)
uint32_t s_lastFinalMs = 0;

// Pairing: every OKOK scale heard (any MAC), so the user can pick theirs.
const int NEARBY_MAX = 8;
OkokScale::Nearby s_nearby[NEARBY_MAX];
int s_nNearby = 0;
volatile bool s_pairing = false;    // picker open: report no weights

void noteNearby(const char *mac, int rssi, float kg) {   // s_lock held
  int slot = -1, oldest = 0;
  for (int i = 0; i < s_nNearby; i++) {
    if (strcmp(s_nearby[i].mac, mac) == 0) { slot = i; break; }
    if (s_nearby[i].seenMs < s_nearby[oldest].seenMs) oldest = i;
  }
  if (slot < 0) slot = s_nNearby < NEARBY_MAX ? s_nNearby++ : oldest;
  OkokScale::Nearby &n = s_nearby[slot];
  strncpy(n.mac, mac, sizeof(n.mac) - 1);
  n.mac[sizeof(n.mac) - 1] = 0;
  n.rssi = rssi;
  n.kg = kg;
  n.seenMs = millis();
}

const uint32_t SESSION_IDLE_MS = 5000;  // no packet this long -> weigh-in over
const uint32_t ECHO_MS = 30000;         // locked value re-broadcast after a stand
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
    // The scale keeps showing (and broadcasting) its locked value for a while
    // after a stand: that echo opens a "new" session with a stable packet of the
    // same weight. It's the same stand, not a new weigh-in.
    if (stable && s_lastFinalMs && now - s_lastFinalMs < ECHO_MS &&
        fabsf(kg - s_lastFinalKg) < 0.15f) {
      s_emitted = true;
      s_emittedKg = kg;
    }
  }

  // Stepped off and back on while the scale stayed awake -> a new stand.
  if (s_emitted && kg < s_emittedKg * STEP_OFF_FRACTION) s_emitted = false;

  if (stable) {
    if (!s_emitted && kg >= MIN_KG) {
      s_emitted = true;
      s_emittedKg = kg;
      s_lastFinalKg = kg;
      s_lastFinalMs = now;
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
    float kg = ((b[2] << 8) | b[3]) / 100.0f;
    bool stable = (b[8] == 0x25);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    noteNearby(mac, dev->getRSSI(), kg);
    // Weights only from our scale (or any, until one is learned), never while pairing.
    if (!s_pairing && (!s_mac[0] || strcmp(mac, s_mac) == 0)) onPacket(kg, stable, mac);
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

int nearby(Nearby *out, int max, uint32_t maxAgeMs) {
  Nearby fresh[NEARBY_MAX];
  int n = 0;
  if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
  uint32_t now = millis();
  for (int i = 0; i < s_nNearby; i++)
    if (now - s_nearby[i].seenMs <= maxAgeMs) fresh[n++] = s_nearby[i];
  xSemaphoreGive(s_lock);
  for (int i = 1; i < n; i++)                       // strongest signal first
    for (int j = i; j > 0 && fresh[j - 1].rssi < fresh[j].rssi; j--) {
      Nearby t = fresh[j]; fresh[j] = fresh[j - 1]; fresh[j - 1] = t;
    }
  if (n > max) n = max;
  for (int i = 0; i < n; i++) out[i] = fresh[i];
  return n;
}

void setPairing(bool on) {
  s_pairing = on;
  if (!on || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
  s_nNearby = 0;                    // a fresh list for each pairing session
  s_inSession = false;              // and drop any half-finished weigh-in
  s_emitted = false;
  xSemaphoreGive(s_lock);
}

void startTask() {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(scanTask, "okok", 6144, nullptr, 2, nullptr, 0);
}

}  // namespace OkokScale
