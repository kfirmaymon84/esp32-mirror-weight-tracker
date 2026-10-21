#pragma once

// Per-board hardware profile. Select one with -D BOARD_<name> in platformio.ini.
// The display/touch controller and its SPI pins are TFT_eSPI build flags (they
// must be compile-time for the library); everything else the firmware touches
// lives here so a new board variant is one more block below.

#if defined(BOARD_E32R40T)
// LCDWiki E32R40T: ESP32-32E + 4.0" ST7796S 320x480 + XPT2046 resistive touch
// (touch shares the LCD SPI bus, CS=33). Verified on hardware 2026-10-20.
#define BOARD_NAME        "E32R40T 4.0in"
#define SCR_W             480        // landscape (rotation 1)
#define SCR_H             320
#define PIN_TFT_BL        27         // backlight, HIGH = on
#define PIN_I2C_SDA       32         // I2C header -> VL53L0X ToF
#define PIN_I2C_SCL       25
#define PIN_SPEAKER       26         // speaker amplifier input (LEDC tone)
#define PIN_SPEAKER_EN    4          // amplifier enable, active LOW
#define PIN_LED_R         22         // RGB LED, common anode, active LOW
#define PIN_LED_G         16
#define PIN_LED_B         17
#define PIN_BAT_ADC       34         // battery voltage via divider
#define BAT_DIVIDER       1.983f     // calibrated: pin 2014 mV <-> 3.994 V on a meter
#define TOUCH_CAL_DEFAULT { 278, 3660, 204, 3646, 7 }   // rotation 1
#define TOUCH_CAL_KEY     "cal40"    // NVS key (per board, so variants don't clash)

#elif defined(BOARD_E32R35T)
// LCDWiki E32R35T: ESP32-32E + 3.5" ST7796U 320x480 + XPT2046. The datasheet
// claims the 4" pinout, but this board differs (measured 2026-10-21): RGB LED is
// R=4 G=17 B=16, the speaker amp is always on (no enable pin), the I2C (ToF)
// connector is SDA=21/SCL=22, and there is no battery-voltage sense circuit.
// Same resolution, so the UI is identical.
#define BOARD_NAME        "E32R35T 3.5in"
#define SCR_W             480
#define SCR_H             320
#define PIN_TFT_BL        27
#define PIN_I2C_SDA       21         // VL53L0X found at 0x29 here (not 32/25 as documented)
#define PIN_I2C_SCL       22
#define PIN_SPEAKER       26
#define PIN_SPEAKER_EN    -1         // amplifier always on
#define PIN_LED_R         4          // active LOW
#define PIN_LED_G         17
#define PIN_LED_B         16
#define PIN_BAT_ADC       -1         // no battery gauge on this board (icon hidden)
#define BAT_DIVIDER       2.0f
#define TOUCH_CAL_DEFAULT { 289, 3576, 299, 3482, 3 }   // rotation 1
#define TOUCH_CAL_KEY     "cal35"

#else
#error "No board selected: add -D BOARD_E32R40T / BOARD_E32R35T to build_flags"
#endif
