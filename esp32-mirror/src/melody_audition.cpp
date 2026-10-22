// Boot-melody audition: tap a button to hear each candidate from buzzer.cpp
// (the exact code the firmware will use), pick one, and it becomes the boot sound.
//
// Build/flash:  pio run -e melody -t upload        (4" E32R40T)
//               pio run -e melody35 -t upload      (3.5" E32R35T)
// Serial: '1'..'6' plays a melody, 'a' plays them all.

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "buzzer.h"

TFT_eSPI tft = TFT_eSPI();

#if defined(BRINGUP_35)
static uint16_t calData[5] = { 289, 3576, 299, 3482, 3 };
static const int AMP_EN = -1;           // amp always on
#else
static uint16_t calData[5] = { 278, 3660, 204, 3646, 7 };
static const int AMP_EN = 4;            // amp enable, active LOW
#endif

static const int BX[2] = {20, 250}, BY0 = 58, BW = 210, BH = 58, BGAP = 10;
static const int VX = 140, VY = 266, VW = 200, VH = 44;
static int vol = SOUND_MID;

static void button(int i, bool playing) {
  int x = BX[i % 2], y = BY0 + (i / 2) * (BH + BGAP);
  uint16_t bg = playing ? 0x1AC8 : 0x18E3;
  tft.fillRoundRect(x, y, BW, BH, 12, bg);
  tft.drawRoundRect(x, y, BW, BH, 12, playing ? TFT_GREEN : 0x39C7);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, bg);
  char s[32];
  snprintf(s, sizeof(s), "%d  %s", i + 1, buzzerMelodyName(i));
  tft.drawString(s, x + 16, y + BH / 2, 4);
}

static void volButton() {
  tft.fillRoundRect(VX, VY, VW, VH, 10, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  char s[24];
  snprintf(s, sizeof(s), "Volume: %s", buzzerLevelName(vol));
  tft.drawString(s, VX + VW / 2, VY + VH / 2, 4);
}

static void play(int i) {
  button(i, true);
  Serial.printf("[melody] %d %s (%s)\n", i + 1, buzzerMelodyName(i), buzzerLevelName(vol));
  uint32_t t0 = millis();
  buzzerMelody(i);
  Serial.printf("[melody] took %lu ms (incl. ~52 ms fade in/out)\n", millis() - t0);
  button(i, false);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== boot-melody audition ===");
  buzzerBegin(26, AMP_EN);
  buzzerSetLevel(vol);

  tft.init();
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);
  tft.setRotation(1);
  tft.setTouch(calData);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Pick a boot melody - tap to listen", 20, 16, 4);
  for (int i = 0; i < buzzerMelodyCount(); i++) button(i, false);
  volButton();
}

void loop() {
  static bool wasDown = false;
  uint16_t x, y;
  bool down = tft.getTouch(&x, &y);
  if (down && !wasDown) {
    if (x >= VX && x < VX + VW && y >= VY && y < VY + VH) {
      vol = vol >= SOUND_HIGH ? SOUND_LOW : vol + 1;     // Low -> Mid -> High
      buzzerSetLevel(vol);
      volButton();
    } else {
      for (int i = 0; i < buzzerMelodyCount(); i++) {
        int bx = BX[i % 2], by = BY0 + (i / 2) * (BH + BGAP);
        if (x >= bx && x < bx + BW && y >= by && y < by + BH) { play(i); break; }
      }
    }
  }
  wasDown = down;

  if (Serial.available()) {
    int c = Serial.read();
    if (c >= '1' && c < '1' + buzzerMelodyCount()) play(c - '1');
    else if (c == 'a') for (int i = 0; i < buzzerMelodyCount(); i++) { play(i); delay(900); }
  }
  delay(10);
}
