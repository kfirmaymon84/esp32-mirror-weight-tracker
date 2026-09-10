#include "buzzer.h"

#include <Arduino.h>

static const int CH = 4;      // LEDC channel (avoid 0 which tone()/others grab)
static const int RES = 10;    // duty resolution bits
static bool muted = false;

void buzzerBegin(int pin) {
  ledcSetup(CH, 2000, RES);
  ledcAttachPin(pin, CH);
  ledcWrite(CH, 0);
}

void buzzerSetMute(bool m) { muted = m; }

void buzzerTone(int freqHz, int durationMs) {
  if (muted) { delay(durationMs); return; }
  if (freqHz <= 0) {
    ledcWrite(CH, 0);
    delay(durationMs);
    return;
  }
  ledcWriteTone(CH, freqHz);
  ledcWrite(CH, 1 << (RES - 1));  // 50% duty
  delay(durationMs);
  ledcWrite(CH, 0);
}

void buzzerAccept() {
  buzzerTone(2200, 80);
  buzzerTone(2800, 120);
}

void buzzerReject() {
  buzzerTone(700, 250);
}

void buzzerClick() {
  buzzerTone(3200, 12);
}
