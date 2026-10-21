// Bring-up for the 4" LCDWiki E32R40T board (ESP32-32E + ST7796S 320x480 +
// XPT2046 resistive touch on the shared LCD SPI bus). Board is on COM3.
//
// Config lives in platformio.ini [env:disp4].
// Build/flash:  pio run -e disp4 -t upload  then  pio device monitor -e disp4
//
// Confirmed so far: display, colors, orientation, touch (calibrated below).
// This stage adds an interactive peripheral test panel:
//   BEEP  - plays a chime on the onboard speaker amp (DAC pin IO26, amp enable
//           IO4 active-low). Needs a small speaker plugged into "SPEAKER".
//   RGB   - cycles the onboard RGB LED: off/red/green/blue/white
//           (R=IO22 G=IO16 B=IO17, common anode = active LOW)
//   CAL   - re-runs touch calibration and prints the new calData
//   battery voltage on IO34 (ADC) refreshed every second; the divider ratio is
//   undocumented, so both the pin voltage and the x2 estimate are shown.
//   ToF   - VL53L0X on the I2C header (SDA=IO32, SCL=IO25): I2C scan at boot,
//           live distance in mm + presence (< 900 mm) on screen.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <VL53L0X.h>
#include <Wire.h>

TFT_eSPI tft = TFT_eSPI();
static VL53L0X tof;
static bool tofOk = false;
static const int PIN_I2C_SDA = 32, PIN_I2C_SCL = 25;
static const int PRESENCE_MM = 900;

static const int PIN_AUDIO_DAC = 26;
static const int PIN_AUDIO_EN  = 4;    // active low
static const int PIN_LED_R = 22, PIN_LED_G = 16, PIN_LED_B = 17;  // active low
static const int PIN_BAT_ADC = 34;

static const int AUDIO_CH = 0;         // LEDC channel for the speaker tone

// Touch calibration captured 2026-10-20 for rotation 1 (user-confirmed accurate).
// Same sketch serves both LCDWiki boards (identical pinout); -D BRINGUP_35 = 3.5".
#if defined(BRINGUP_35)
#define BOARD_TAG "E32R35T 3.5in"
static uint16_t calData[5] = { 300, 3600, 300, 3600, 7 };  // placeholder: calibrated at boot
static const bool CAL_AT_BOOT = true;
#else
#define BOARD_TAG "E32R40T 4.0in"
static uint16_t calData[5] = { 278, 3660, 204, 3646, 7 };  // user-confirmed 2026-10-20
static const bool CAL_AT_BOOT = false;
#endif

// Button row geometry (landscape 480x320).
static const int BTN_Y = 250, BTN_H = 56, BTN_W = 140;
static const int BTN_X[3] = { 10, 170, 330 };
static const char *BTN_TXT[3] = { "BEEP", "RGB", "CAL" };

static int ledState = 0;   // 0 off, 1 red, 2 green, 3 blue, 4 white

// ── RGB LED ─────────────────────────────────────────────────────────────────
static void ledSet(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}
static const char *ledName() {
  static const char *n[5] = { "off", "RED", "GREEN", "BLUE", "WHITE" };
  return n[ledState];
}
static void ledApply() {
  switch (ledState) {
    case 0: ledSet(false, false, false); break;
    case 1: ledSet(true, false, false); break;
    case 2: ledSet(false, true, false); break;
    case 3: ledSet(false, false, true); break;
    case 4: ledSet(true, true, true); break;
  }
}

// ── Speaker ─────────────────────────────────────────────────────────────────
static void tone(int hz, int ms) {
  ledcWriteTone(AUDIO_CH, hz);
  delay(ms);
  ledcWriteTone(AUDIO_CH, 0);
}
static void chime() {
  tone(1047, 120);   // C6
  tone(1319, 120);   // E6
  tone(1568, 200);   // G6
}

// ── Battery ─────────────────────────────────────────────────────────────────
// Divider ratio calibrated 2026-10-20: pin 2014 mV <-> 3.994 V on a meter.
static const float BAT_RATIO = 1.983f;

static uint32_t batPinMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
  return sum / 16;
}

// Single-cell LiPo resting-voltage -> % (linear between points).
static int batPercent(float v) {
  static const float V[] = {3.27f, 3.61f, 3.69f, 3.73f, 3.77f, 3.80f, 3.84f,
                            3.87f, 3.91f, 3.95f, 4.02f, 4.08f, 4.15f, 4.20f};
  static const int   P[] = {0,     5,     10,    20,    30,    40,    50,
                            60,    65,    70,    80,    85,    95,    100};
  const int n = sizeof(V) / sizeof(V[0]);
  if (v <= V[0]) return 0;
  if (v >= V[n - 1]) return 100;
  for (int i = 1; i < n; i++)
    if (v < V[i]) return P[i - 1] + (int)((v - V[i - 1]) * (P[i] - P[i - 1]) / (V[i] - V[i - 1]));
  return 100;
}

// Battery icon (x,y = top-left), fill colored by level.
static void drawBatIcon(int x, int y, int pct) {
  const int w = 56, h = 26;
  tft.fillRect(x, y, w + 6, h, TFT_BLACK);
  tft.drawRoundRect(x, y, w, h, 3, TFT_WHITE);
  tft.fillRect(x + w, y + 8, 5, h - 16, TFT_WHITE);           // terminal nub
  uint16_t c = pct > 50 ? TFT_GREEN : pct > 20 ? TFT_YELLOW : TFT_RED;
  int fw = (w - 6) * pct / 100;
  tft.fillRect(x + 3, y + 3, fw, h - 6, c);
}

// ── Screen ──────────────────────────────────────────────────────────────────
static void drawButtons() {
  for (int i = 0; i < 3; i++) {
    tft.fillRoundRect(BTN_X[i], BTN_Y, BTN_W, BTN_H, 8, TFT_NAVY);
    tft.drawRoundRect(BTN_X[i], BTN_Y, BTN_W, BTN_H, 8, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString(BTN_TXT[i], BTN_X[i] + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
  }
  tft.setTextDatum(TL_DATUM);
}

static void drawStatus(const char *msg) {
  tft.fillRect(10, 200, 460, 22, TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(msg, 10, 202, 2);
}

static void drawBattery() {
  uint32_t mv = batPinMv();
  float v = mv * BAT_RATIO / 1000.0f;
  int pct = batPercent(v);
  char s[64];
  snprintf(s, sizeof(s), "Battery %.2f V  %3d%%   (pin %u mV)   LED: %s   ",
           v, pct, (unsigned)mv, ledName());
  tft.fillRect(10, 170, 460, 22, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(s, 10, 172, 2);
  drawBatIcon(404, 74, pct);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  char p[8];
  snprintf(p, sizeof(p), "%d%% ", pct);
  tft.fillRect(340, 78, 60, 20, TFT_BLACK);
  tft.drawString(p, 398, 80, 2);
  tft.setTextDatum(TL_DATUM);
  Serial.printf("[bat] pin=%u mV  V=%.3f  %d%%\n", (unsigned)mv, v, pct);
}

// ToF line (y=148): distance + presence, or a clear "not found" message.
static void drawTof() {
  char s[64];
  uint16_t col = TFT_ORANGE;
  if (!tofOk) {
    snprintf(s, sizeof(s), "ToF: NOT FOUND on I2C (SDA32/SCL25)            ");
    col = TFT_RED;
  } else {
    int mm = tof.readRangeContinuousMillimeters();
    if (tof.timeoutOccurred()) {
      snprintf(s, sizeof(s), "ToF: timeout                                   ");
      col = TFT_RED;
    } else if (mm >= 8000) {
      snprintf(s, sizeof(s), "ToF: out of range (nothing in front)           ");
    } else {
      bool present = mm < PRESENCE_MM;
      snprintf(s, sizeof(s), "ToF: %4d mm   %s          ", mm,
               present ? "PRESENT" : "clear");
      col = present ? TFT_GREEN : TFT_ORANGE;
    }
  }
  tft.fillRect(10, 148, 460, 20, TFT_BLACK);
  tft.setTextColor(col, TFT_BLACK);
  tft.drawString(s, 10, 150, 2);
}

static void i2cScan() {
  Serial.println("[i2c] scanning SDA=32 SCL=25 ...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[i2c] device at 0x%02X%s\n", a, a == 0x29 ? "  (VL53L0X)" : "");
      found++;
    }
  }
  Serial.printf("[i2c] %d device(s) found\n", found);
}

static void draw() {
  int w = tft.width(), h = tft.height();
  tft.fillScreen(TFT_BLACK);

  int bw = w / 4, bh = 60;
  tft.fillRect(0 * bw, 0, bw, bh, TFT_RED);
  tft.fillRect(1 * bw, 0, bw, bh, TFT_GREEN);
  tft.fillRect(2 * bw, 0, bw, bh, TFT_BLUE);
  tft.fillRect(3 * bw, 0, bw, bh, TFT_WHITE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(BOARD_TAG " test", 10, 72, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Tap a button. Plug a speaker into SPEAKER for BEEP.", 10, 110, 2);
  tft.drawString("Touch anywhere else draws a dot.", 10, 128, 2);
  drawButtons();
  drawBattery();
}

static void runCalibration() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Touch calibration: tap each arrow TIP", 20, 140, 4);
  delay(1200);
  tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 20);
  tft.setTouch(calData);
  Serial.print("[touch] calData = { ");
  for (int i = 0; i < 5; i++) Serial.printf("%u%s", calData[i], i < 4 ? ", " : " ");
  Serial.println("};");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== " BOARD_TAG " peripheral bring-up ===");

  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  ledApply();                              // all off

  pinMode(PIN_AUDIO_EN, OUTPUT);
  digitalWrite(PIN_AUDIO_EN, LOW);         // enable the speaker amplifier
  ledcSetup(AUDIO_CH, 2000, 8);
  ledcAttachPin(PIN_AUDIO_DAC, AUDIO_CH);
  ledcWriteTone(AUDIO_CH, 0);

  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);   // full ~0-3.1 V range

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  i2cScan();
  tof.setTimeout(500);
  tofOk = tof.init();
  if (tofOk) tof.startContinuous();
  Serial.printf("[tof] init %s\n", tofOk ? "OK" : "FAILED");

  tft.init();
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);                  // backlight on
  tft.setRotation(1);                      // landscape 480x320
  tft.setTouch(calData);
  if (CAL_AT_BOOT) runCalibration();       // new panel: calibrate before anything else
  draw();
  chime();                                 // boot chime (if a speaker is fitted)
  Serial.println("Ready. Tap BEEP / RGB / CAL.");
}

void loop() {
  static uint32_t tBat = 0, tTouch = 0, tTof = 0;
  static bool wasDown = false;

  if (millis() - tBat > 1000) {
    tBat = millis();
    drawBattery();
  }
  if (millis() - tTof > 200) {
    tTof = millis();
    drawTof();
  }

  uint16_t x, y;
  bool down = tft.getTouch(&x, &y);
  if (down && !wasDown && millis() - tTouch > 200) {     // new tap
    tTouch = millis();
    int hit = -1;
    for (int i = 0; i < 3; i++)
      if (x >= BTN_X[i] && x < BTN_X[i] + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H) hit = i;

    if (hit == 0) {
      drawStatus("BEEP: playing chime on IO26 (amp enabled on IO4)");
      Serial.println("[audio] chime");
      chime();
    } else if (hit == 1) {
      ledState = (ledState + 1) % 5;
      ledApply();
      char s[48];
      snprintf(s, sizeof(s), "RGB LED -> %s (look at the back)", ledName());
      drawStatus(s);
      Serial.printf("[led] %s\n", ledName());
    } else if (hit == 2) {
      runCalibration();
      draw();
      drawStatus("Calibrated - new calData printed on serial");
    } else {
      tft.fillCircle(x, y, 4, TFT_YELLOW);
    }
  } else if (down) {
    // dragging outside the buttons keeps drawing
    if (!(y >= BTN_Y && y < BTN_Y + BTN_H)) tft.fillCircle(x, y, 3, TFT_YELLOW);
  }
  wasDown = down;
  delay(10);
}
