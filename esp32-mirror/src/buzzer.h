#pragma once

// Passive buzzer driven by the ESP32 LEDC peripheral (reliable tone output,
// unlike Arduino tone() which warns "LEDC not initialized" on this core).
void buzzerBegin(int pin);
void buzzerSetMute(bool mute);                 // silence all tones when true
void buzzerTone(int freqHz, int durationMs);  // blocking, short beeps only
void buzzerAccept();                          // stable-reading chirp (rising)
void buzzerReject();                          // rejected-reading tone (low)
void buzzerClick();                           // tiny encoder tick
