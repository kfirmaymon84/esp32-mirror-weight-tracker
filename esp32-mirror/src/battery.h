#pragma once

// LiPo battery gauge from the board's ADC divider (PIN_BAT_ADC / BAT_DIVIDER in
// board.h). There is no charge-status pin, so on USB the reading is the charging
// voltage and reads high; with no battery fitted it reads the charger's ~4.2 V.
void batteryBegin();
float batteryVolts();              // smoothed cell voltage
int batteryPercent(float volts);   // 0-100 from a single-cell LiPo curve
