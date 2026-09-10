# Smart Mirror Weight Tracker — Wiring List

**Board:** ESP32 DevKit v4 (WROOM-32E) · Touch-only build (rotary encoder removed)
**Firmware:** `esp32-mirror`, FW v2 · pins per `src/config.h` + `platformio.ini [env:esp32dev]`
_Last updated: 2026-09-10_

All logic is **3.3 V**. Tie all module grounds to a common GND.

---

## 1. Display + touch — TZT 2.8" ST7789V 240×320 (XPT2046 resistive)

The XPT2046 touch controller shares the SPI bus with the display (same SCK/MOSI/MISO);
only the two chip-selects are unique.

| Module pin | → ESP32 | GPIO | Notes |
|------------|---------|------|-------|
| VCC        | 3V3     | —    | 3.3 V |
| GND        | GND     | —    | common ground |
| CS         | LCD CS  | 5    | display chip-select |
| RESET      | RST     | 17   | |
| DC / RS    | DC      | 16   | data/command |
| SDI (MOSI) | MOSI    | 23   | shared SPI |
| SCK        | SCLK    | 18   | shared SPI |
| LED        | BL      | 32   | backlight (software-controlled) |
| SDO (MISO) | MISO    | 19   | shared SPI |
| T_CLK      | SCLK    | 18   | shared with display SCK |
| T_CS       | Touch CS| 4    | separate chip-select |
| T_DIN      | MOSI    | 23   | shared with display MOSI |
| T_DO       | MISO    | 19   | shared with display MISO |
| T_IRQ      | —       | 14 / NC | **unused** — firmware polls touch; leave unconnected (or park on GPIO14) |

---

## 2. VL53L0X ToF presence sensor (I²C)

| Module pin      | → ESP32 | GPIO |
|-----------------|---------|------|
| VIN / VCC       | 3V3     | —    |
| GND             | GND     | —    |
| SDA             | SDA     | 21   |
| SCL             | SCL     | 22   |
| XSHUT / GPIO1   | —       | NC   |

---

## 3. Passive buzzer (2-pin)

| Buzzer      | → ESP32 | GPIO |
|-------------|---------|------|
| signal (+)  | Buzzer  | 33   |
| (−)         | GND     | —    |

_If yours is a 3-pin module: VCC→3V3, GND→GND, I/O→GPIO33._

---

## Full GPIO map (quick reference)

| GPIO       | Function |
|------------|----------|
| 4          | Touch CS |
| 5          | Display CS |
| 16         | Display DC |
| 17         | Display RST |
| 18         | SPI SCLK (display + touch) |
| 19         | SPI MISO (display + touch) |
| 21         | I²C SDA |
| 22         | I²C SCL |
| 23         | SPI MOSI (display + touch) |
| 32         | Backlight (BL) |
| 33         | Buzzer |
| 14         | (T_IRQ, optional / unused) |
| 25, 26, 27 | **Free** — old rotary encoder, removed |

---

## Notes for the new board

- **Power:** everything on the ESP32's **3V3** rail + a shared GND. Total draw
  (backlight ~40–80 mA + ToF ~20 mA + logic) is well within the DevKit's regulator.
  If powering from USB or 5 V, only the 3V3 pin feeds the modules.
- **Shared SPI bus:** the three touch lines (T_CLK, T_DIN, T_DO) are simply the
  display's SCK/MOSI/MISO — you can bridge them on the board rather than running
  separate wires. Only the two chip-selects (GPIO 5 and 4) are unique.
- **Strapping pin:** GPIO5 (display CS) is an ESP32 strapping pin, but as an SPI CS
  it idles high at boot, so it's fine — no external pull needed.
- **Decoupling:** add a ~100 µF cap across 3V3/GND near the display if the backlight
  causes flicker at power-up.
- **T_IRQ not required:** TFT_eSPI polls the touch controller over SPI, so that wire
  can be omitted entirely.

---

## Firmware references

- Pin defines: `esp32-mirror/src/config.h` (`PIN_TFT_BL`, `PIN_I2C_SDA/SCL`, `PIN_BUZZER`)
- Display + touch flags: `esp32-mirror/platformio.ini`, `[env:esp32dev]` build_flags
  (`TFT_MOSI/SCLK/CS/DC/RST/MISO/BL`, `TOUCH_CS`)
