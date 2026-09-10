// Hardware bring-up self-test for the Smart Mirror.
// Exercises every peripheral at once and reports on TFT + serial:
//   - Buzzer: startup beep, plus a click on each encoder detent
//   - TFT:    color bars + live text (proves SPI wiring + tab/colors)
//   - VL53L0X: live distance in mm (proves I2C wiring)
//   - Encoder: rotation counter + button state (proves the 3 encoder pins)
//
// Build/flash:  pio run -e bringup -t upload  then  pio device monitor

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <VL53L0X.h>
#include <Wire.h>

// ── Pins (match the wiring plan) ────────────────────────────────────────────
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define ENC_CLK 25
#define ENC_DT 26
#define ENC_SW 27
#define PIN_BUZZER 33
#define PIN_TFT_BL 32

TFT_eSPI tft = TFT_eSPI();
VL53L0X tof;
bool tofOk = false;

volatile long encPos = 0;
int lastClk;
bool lastSw = false;

void beep(int freq, int ms) { tone(PIN_BUZZER, freq, ms); }

void drawStatic() {
  tft.fillScreen(TFT_BLACK);
  // color bars across the top to verify color order + no offset
  int w = tft.width();
  tft.fillRect(0, 0, w / 4, 12, TFT_RED);
  tft.fillRect(w / 4, 0, w / 4, 12, TFT_GREEN);
  tft.fillRect(w / 2, 0, w / 4, 12, TFT_BLUE);
  tft.fillRect(3 * w / 4, 0, w / 4, 12, TFT_WHITE);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(2, 16);
  tft.print("Mirror bring-up");
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== HARDWARE BRING-UP ===");

  // Backlight on explicitly (belt & suspenders alongside TFT_BL).
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

  pinMode(PIN_BUZZER, OUTPUT);

  // TFT
  tft.init();
  tft.setRotation(1);  // landscape 160x128
  drawStatic();
  Serial.printf("TFT init done (%dx%d)\n", tft.width(), tft.height());

  // I2C + ToF
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  tof.setTimeout(500);
  tofOk = tof.init();
  if (tofOk) {
    tof.startContinuous();
    Serial.println("VL53L0X: OK");
  } else {
    Serial.println("VL53L0X: NOT FOUND (check SDA=21 SCL=22, 3V3, GND)");
  }

  // Encoder
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  lastClk = digitalRead(ENC_CLK);

  beep(2000, 150);
  Serial.println("Setup done. Rotate encoder, press button, wave hand at ToF.");
}

uint32_t lastDraw = 0;

void loop() {
  // ── Encoder rotation (poll) ──
  int clk = digitalRead(ENC_CLK);
  if (clk != lastClk && clk == LOW) {
    if (digitalRead(ENC_DT) != clk)
      encPos++;
    else
      encPos--;
    beep(3000, 20);
    Serial.printf("Encoder: %ld\n", encPos);
  }
  lastClk = clk;

  // ── Encoder button ──
  bool sw = digitalRead(ENC_SW) == LOW;
  if (sw && !lastSw) {
    beep(1500, 80);
    Serial.println("Button: PRESSED");
  }
  lastSw = sw;

  // ── Periodic display + ToF refresh (~5 Hz) ──
  if (millis() - lastDraw > 200) {
    lastDraw = millis();

    int mm = -1;
    if (tofOk) {
      mm = tof.readRangeContinuousMillimeters();
      if (tof.timeoutOccurred()) mm = -2;
    }

    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    tft.setCursor(2, 40);
    tft.printf("ToF: %s      ", tofOk ? (mm >= 0 ? String(mm).c_str() : "timeout")
                                      : "NO SENSOR");
    if (tofOk && mm >= 0) {
      tft.setCursor(2, 40);
      tft.printf("ToF: %d mm   ", mm);
    }

    tft.setCursor(2, 62);
    tft.printf("Enc: %ld    ", encPos);

    tft.setCursor(2, 84);
    tft.setTextColor(sw ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
    tft.printf("Btn: %s   ", sw ? "DOWN" : "up  ");

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(2, 106);
    tft.printf("uptime %lus  ", millis() / 1000);

    if (tofOk && mm >= 0)
      Serial.printf("ToF=%dmm Enc=%ld Btn=%d\n", mm, encPos, sw);
  }
}
