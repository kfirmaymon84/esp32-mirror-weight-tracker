// Pin probe for the 3.5" E32R35T board (several pins differ from its datasheet).
//
// Findings so far (user-verified 2026-10-21): RGB LED red = GPIO4, green = GPIO17,
// blue = GPIO16 (active LOW); the speaker amp is always on (no enable pin); no
// battery-sense circuit.
//
// This stage: the user reports the ToF (VL53L0X) is on the IO21/IO22 connector,
// not the datasheet's SDA32/SCL25. Scan I2C with both pin orders (and the
// datasheet pair for comparison), then show live distance from the one that works.
// Build/flash:  pio run -e probe35 -t upload

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <VL53L0X.h>
#include <Wire.h>

TFT_eSPI tft = TFT_eSPI();
static VL53L0X tof;
static bool tofOk = false;
static int okSda = -1, okScl = -1;

static int scan(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl);
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[i2c] SDA=%d SCL=%d: device at 0x%02X%s\n", sda, scl, a,
                    a == 0x29 ? " (VL53L0X)" : "");
      found++;
    }
  }
  Serial.printf("[i2c] SDA=%d SCL=%d: %d device(s)\n", sda, scl, found);
  return found;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== E32R35T ToF/I2C probe ===");
  pinMode(4, OUTPUT);  digitalWrite(4, HIGH);     // RGB LED off (R4 G17 B16)
  pinMode(16, OUTPUT); digitalWrite(16, HIGH);
  pinMode(17, OUTPUT); digitalWrite(17, HIGH);

  tft.init();
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("E32R35T ToF probe", 10, 10, 4);

  const int pairs[][2] = {{21, 22}, {22, 21}, {32, 25}};
  int y = 60;
  for (auto &p : pairs) {
    int n = scan(p[0], p[1]);
    char s[48];
    snprintf(s, sizeof(s), "SDA=%d SCL=%d : %d device(s)", p[0], p[1], n);
    tft.setTextColor(n ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(s, 10, y, 4);
    y += 34;
    if (n && okSda < 0) { okSda = p[0]; okScl = p[1]; }
  }
  if (okSda >= 0) {
    Wire.end();
    Wire.begin(okSda, okScl);
    tof.setTimeout(500);
    tofOk = tof.init();
    if (tofOk) tof.startContinuous();
  }
  Serial.printf("[tof] %s (SDA=%d SCL=%d)\n", tofOk ? "init OK" : "not found", okSda, okScl);
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t < 200) return;
  t = millis();
  char s[48];
  uint16_t col = TFT_RED;
  if (!tofOk) {
    snprintf(s, sizeof(s), "ToF: not found");
  } else {
    int mm = tof.readRangeContinuousMillimeters();
    if (tof.timeoutOccurred()) snprintf(s, sizeof(s), "ToF: timeout");
    else if (mm >= 8000) { snprintf(s, sizeof(s), "ToF: out of range"); col = TFT_ORANGE; }
    else {
      snprintf(s, sizeof(s), "ToF: %4d mm  %s", mm, mm < 900 ? "PRESENT" : "clear");
      col = mm < 900 ? TFT_GREEN : TFT_ORANGE;
    }
    Serial.printf("[tof] %s\n", s);
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextPadding(460);
  tft.drawString(s, 10, 220, 4);
  tft.setTextPadding(0);
}
