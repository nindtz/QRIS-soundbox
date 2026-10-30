/*
 * battery.h - battery level from the board's TG28 power-management chip.
 *
 * ASSUMPTION (not confirmed by Waveshare docs): the TG28 is register-compatible with the
 * X-Powers AXP2101 at I2C address 0x34. Only these registers are used:
 *   0x00 bit3      battery present
 *   0x01 bits 6:5  01 = charging
 *   0x30 bit0      battery-voltage ADC enable (set once)
 *   0x34/0x35      battery voltage, 14 bit, mV
 *   0xA4           fuel-gauge percent (0..100)
 * If nothing answers at 0x34, batteryInit() prints an I2C scan and the battery icon is hidden.
 *
 * Shares the I2C bus with the ES8311 codec, so call batteryInit() AFTER audioInit()
 * (which has already started Wire on the right pins).
 */
#pragma once
#include <Arduino.h>
#include <Wire.h>

#define PMU_ADDR 0x34

struct BatteryInfo {
  bool    present;      // false: no PMU found or no battery attached -> icon hidden
  bool    charging;
  uint8_t percent;      // 0..100
};

static bool pmuFound = false;

static bool pmuRead(uint8_t reg, uint8_t* v, uint8_t n = 1) {
  Wire.beginTransmission(PMU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)PMU_ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) v[i] = Wire.read();
  return true;
}

static void i2cScan() {
  Serial.print("I2C devices:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println();
}

static void batteryInit() {
  uint8_t id = 0;
  pmuFound = pmuRead(0x03, &id);
  if (!pmuFound) {
    Serial.println("Battery: no PMU at 0x34, battery icon disabled");
    i2cScan();
    return;
  }
  Serial.printf("Battery: PMU found, chip id 0x%02X (AXP2101 = 0x4A)\n", id);
  uint8_t adc;
  if (pmuRead(0x30, &adc) && !(adc & 0x01)) {          // enable battery-voltage ADC if off
    Wire.beginTransmission(PMU_ADDR);
    Wire.write(0x30);
    Wire.write(adc | 0x01);
    Wire.endTransmission();
  }
}

// Rough Li-ion curve, used only if the fuel-gauge register looks wrong
static uint8_t percentFromMv(int mv) {
  static const int pts[][2] = {{4150, 100}, {4000, 85}, {3900, 72}, {3800, 55}, {3700, 35},
                               {3600, 15}, {3500, 5}, {3300, 0}};
  if (mv >= pts[0][0]) return 100;
  for (int i = 1; i < 8; i++)
    if (mv >= pts[i][0])
      return pts[i][1] + (mv - pts[i][0]) * (pts[i - 1][1] - pts[i][1]) / (pts[i - 1][0] - pts[i][0]);
  return 0;
}

static BatteryInfo batteryRead() {
  BatteryInfo b{false, false, 0};
  if (!pmuFound) return b;
  uint8_t s0, s1, pct, v[2];
  if (!pmuRead(0x00, &s0) || !pmuRead(0x01, &s1)) return b;
  b.present  = s0 & 0x08;
  b.charging = ((s1 >> 5) & 0x03) == 0x01;
  if (!b.present) return b;
  if (pmuRead(0xA4, &pct) && pct > 0 && pct <= 100) {
    b.percent = pct;
  } else if (pmuRead(0x34, v, 2)) {
    b.percent = percentFromMv(((v[0] & 0x3F) << 8) | v[1]);
  }
  return b;
}
