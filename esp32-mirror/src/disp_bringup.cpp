// Bring-up for the new ST7789V 240x320 SPI TFT + XPT2046 touch.
// Verifies: display renders (color bars + text, right colors/orientation) and
// touch responds (draws where you tap + prints raw/mapped coords on serial).
//
// Build/flash:  pio run -e disp -t upload  then  pio device monitor
//
// If colors look INVERTED (e.g. background white, red looks cyan), tell me and
// I'll add -D TFT_INVERSION_ON (common on some ST7789 panels).

#include <Arduino.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

static void header() {
  int w = tft.width();
  tft.fillRect(0, 0, w / 4, 30, TFT_RED);
  tft.fillRect(w / 4, 0, w / 4, 30, TFT_GREEN);
  tft.fillRect(w / 2, 0, w / 4, 30, TFT_BLUE);
  tft.fillRect(3 * w / 4, 0, w / 4, 30, TFT_WHITE);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(4);
  tft.setCursor(6, 40);
  tft.print("ST7789 OK");
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(6, 74);
  tft.printf("%d x %d", tft.width(), tft.height());
  tft.setCursor(6, 96);
  tft.print("Tap the screen...");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ST7789 + XPT2046 bring-up ===");

  tft.init();
  tft.setRotation(0);  // portrait 240x320
  tft.fillScreen(TFT_BLACK);

  // Recalibrate: tap each corner arrow tip PRECISELY (use a nail/stylus).
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 10);
  tft.println("Calibration:");
  tft.setCursor(10, 30);
  tft.println("tap each arrow TIP");
  uint16_t calData[5];
  tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 20);
  tft.setTouch(calData);
  Serial.print("[touch] calData = { ");
  for (int i = 0; i < 5; i++) Serial.printf("%u%s", calData[i], i < 4 ? ", " : " ");
  Serial.println("};");

  tft.fillScreen(TFT_BLACK);
  header();
  Serial.printf("TFT %dx%d ready\n", tft.width(), tft.height());
}

uint32_t lastReport = 0;

void loop() {
  // Pressure (raw Z) — proves the XPT2046 responds even before calibration.
  uint16_t z = tft.getTouchRawZ();

  uint16_t x, y;
  if (tft.getTouch(&x, &y)) {           // mapped to screen coords
    tft.fillCircle(x, y, 4, TFT_YELLOW);
    tft.fillRect(0, 120, tft.width(), 22, TFT_BLACK);
    tft.setTextFont(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(6, 122);
    tft.printf("touch x=%d y=%d", x, y);
  }

  if (z > 300 && millis() - lastReport > 120) {
    lastReport = millis();
    uint16_t rx, ry;
    tft.getTouchRaw(&rx, &ry);
    Serial.printf("touch z=%u  raw(%u,%u)  mapped(%u,%u)\n", z, rx, ry, x, y);
  }
  delay(15);
}
