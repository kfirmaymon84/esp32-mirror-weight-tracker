#pragma once
// Scale driver selected at build time, behind one interface (namespace Scale):
//   default        -> OKOK / Chipsea broadcast scale (okok_scale.*): no pairing, no keys
//   -D SCALE_S200  -> Xiaomi Scale S200 over the Mi (mible) protocol (mi_scale.*),
//                     using the MAC + token from secrets.h (personal builds only)
//
// Interface used by main.cpp:
//   setWeightCallback(cb(kg, stable)), setStatusCallback(cb(status)), startTask(),
//   heartbeat(), setLearnedMac(mac), learnedMac()

#if defined(SCALE_S200)

#include <functional>
#include <stdint.h>

#include "config.h"
#include "mi_scale.h"

namespace Scale {

using WeightCallback = std::function<void(float kg, bool stable)>;
using StatusCallback = MiScale::StatusCallback;

inline void setWeightCallback(WeightCallback cb) {
  // The S200 driver also reports the raw CSV line and impedance; the UI needs neither.
  MiScale::setWeightCallback([cb](float kg, bool stable, const char *, int) {
    if (cb) cb(kg, stable);
  });
}
inline void setStatusCallback(StatusCallback cb) { MiScale::setStatusCallback(cb); }
inline void startTask() { MiScale::startTask(); }
inline uint32_t heartbeat() { return MiScale::heartbeat(); }

// The S200 is paired by its fixed MAC + token in secrets.h, so there is nothing to
// learn or forget: report that MAC and ignore resets.
inline void setLearnedMac(const char *) {}
inline const char *learnedMac() { return SCALE_MAC; }
// The S200 is fixed by its MAC + token in secrets.h: no pairing screen.
struct Nearby { char mac[18]; int rssi; float kg; uint32_t seenMs; };
inline int nearby(Nearby *, int, uint32_t) { return 0; }
inline void setPairing(bool) {}
constexpr bool canPair() { return false; }

}  // namespace Scale

#else

#include "okok_scale.h"
namespace Scale = OkokScale;

#endif
