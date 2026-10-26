#pragma once
#include <functional>
#include <stdint.h>

// Reads weight from an OKOK / Chipsea "body fat scale" (e.g. model P-01) by
// passively listening to its BLE advertisements — no pairing, no app, no keys.
//
// Manufacturer data (15 bytes incl. the 2 company-id bytes):
//   C0 ctr W_hi W_lo M_hi M_lo 0A 11 flag MAC[6]
//   weight = [2:4] big-endian / 100 kg; flag 0x24 = measuring, 0x25 = locked.
//
// Callback: kg = weight, stable = true once per weigh-in for the locked final
// reading (false for live updates while the weight settles).
namespace OkokScale {

using WeightCallback = std::function<void(float kg, bool stable)>;
using StatusCallback = std::function<void(const char *status)>;  // "Ready" / "Reading"

void setWeightCallback(WeightCallback cb);
void setStatusCallback(StatusCallback cb);
void startTask();                  // NimBLE init + continuous passive scan (core 0)
uint32_t heartbeat();              // millis() of the scan task's last tick (watchdog)

// Scale identity: lock onto one scale so a neighbour's scale is ignored.
// "" = accept any OKOK scale; the first locked reading then learns its MAC.
void setLearnedMac(const char *mac);
const char *learnedMac();          // "aa:bb:cc:dd:ee:ff" or "" if none yet

// Pairing: every OKOK scale heard recently (any MAC), strongest signal first.
// A scale only broadcasts while someone stands on it, so the user steps on
// theirs to make it appear. While pairing is on, no weights are reported.
struct Nearby {
  char mac[18];
  int rssi;                        // dBm
  float kg;                        // last weight it broadcast
  uint32_t seenMs;                 // millis() when last heard
};
int nearby(Nearby *out, int max, uint32_t maxAgeMs);
void setPairing(bool on);

}  // namespace OkokScale
