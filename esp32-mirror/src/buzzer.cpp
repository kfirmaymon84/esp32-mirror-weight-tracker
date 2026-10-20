#include "buzzer.h"

#include <Arduino.h>

static const int CH = 4;      // LEDC channel (avoid 0 which tone()/others grab)
static const int RES = 10;    // duty resolution bits (0..1023)
static int level = SOUND_MID;

// Duty per level (of 1024). Fundamental amplitude ~ sin(pi * duty):
//   Low 2% ≈ -24 dB, Mid 8% ≈ -12 dB, High 50% = full.
static const int DUTY[4] = { 0, 20, 82, 512 };

void buzzerBegin(int pin, int enablePin) {
  if (enablePin >= 0) {             // speaker amplifier: enable is active LOW
    pinMode(enablePin, OUTPUT);
    digitalWrite(enablePin, LOW);
  }
  ledcSetup(CH, 2000, RES);
  ledcAttachPin(pin, CH);
  ledcWrite(CH, 0);
}

void buzzerSetLevel(int l) { level = (l < SOUND_OFF || l > SOUND_HIGH) ? SOUND_MID : l; }
int buzzerLevel() { return level; }
const char *buzzerLevelName(int l) {
  static const char *n[4] = { "Off", "Low", "Mid", "High" };
  return (l >= 0 && l <= 3) ? n[l] : "?";
}

void buzzerTone(int freqHz, int durationMs) {
  if (level == SOUND_OFF) { delay(durationMs); return; }
  if (freqHz <= 0) {
    ledcWrite(CH, 0);
    delay(durationMs);
    return;
  }
  ledcWriteTone(CH, freqHz);
  ledcWrite(CH, DUTY[level]);     // duty sets the volume
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
