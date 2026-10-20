#include "battery.h"

#include <Arduino.h>

#include "config.h"

static float s_volts = -1;   // exponential moving average

void batteryBegin() {
  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);   // full ~0-3.1 V range
}

float batteryVolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
  float v = (sum / 16.0f) * BAT_DIVIDER / 1000.0f;
  s_volts = (s_volts < 0) ? v : s_volts * 0.7f + v * 0.3f;
  return s_volts;
}

int batteryPercent(float v) {
  static const float V[] = {3.27f, 3.61f, 3.69f, 3.73f, 3.77f, 3.80f, 3.84f,
                            3.87f, 3.91f, 3.95f, 4.02f, 4.08f, 4.15f, 4.20f};
  static const int P[] = {0, 5, 10, 20, 30, 40, 50, 60, 65, 70, 80, 85, 95, 100};
  const int n = sizeof(V) / sizeof(V[0]);
  if (v <= V[0]) return 0;
  if (v >= V[n - 1]) return 100;
  for (int i = 1; i < n; i++)
    if (v < V[i])
      return P[i - 1] + (int)((v - V[i - 1]) * (P[i] - P[i - 1]) / (V[i] - V[i - 1]));
  return 100;
}
