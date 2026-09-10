#pragma once
#include <functional>
#include <stdint.h>

// Reads weight from a Xiaomi Scale S200 over the Mi (mible) BLE protocol.
// Port of the proven gatt_mi.py reference.
//
// Callback: kg = weight, stable = true for a locked final reading (false for
// live/intermediate updates), csv = the raw CSV line for stable readings (may
// be nullptr), impedance = raw impedance from a stable reading (0 if unknown).
namespace MiScale {

using WeightCallback =
    std::function<void(float kg, bool stable, const char *csv, int impedance)>;
using StatusCallback = std::function<void(const char *status)>;

void begin();                       // NimBLE init + scan setup
void setWeightCallback(WeightCallback cb);
void setStatusCallback(StatusCallback cb);
void loop();                        // drive scan/connect/session once
void startTask();                   // run begin()+loop() forever on core 0
uint32_t heartbeat();               // millis() of the BLE task's last tick (liveness)

}  // namespace MiScale
