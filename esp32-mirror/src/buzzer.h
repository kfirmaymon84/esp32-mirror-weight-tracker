#pragma once

// Tones on a passive buzzer or a speaker amplifier, driven by the ESP32 LEDC
// peripheral (reliable tone output, unlike Arduino tone() which warns "LEDC not
// initialized" on this core). enablePin = amplifier enable (active LOW), or -1.
//
// Volume is set by the square wave's duty cycle: a narrower pulse carries less
// energy at the tone frequency, so it sounds quieter.
enum SoundLevel { SOUND_OFF = 0, SOUND_LOW = 1, SOUND_MID = 2, SOUND_HIGH = 3 };

void buzzerBegin(int pin, int enablePin = -1);
void buzzerSetLevel(int level);               // SoundLevel; OFF = silent
int  buzzerLevel();
const char *buzzerLevelName(int level);       // "Off" / "Low" / "Mid" / "High"
void buzzerTone(int freqHz, int durationMs);  // blocking, short beeps only
void buzzerAccept();                          // stable-reading chirp (rising)
void buzzerReject();                          // rejected-reading tone (low)
void buzzerClick();                           // tiny touch tick
