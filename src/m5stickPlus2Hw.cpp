// M5StickC Plus2 on-board chips: BM8563 RTC and the power latch. Register
// sequences follow M5Stack's own M5Unified library
// (utility/rtc/PCF8563_Class.inl).

#ifdef M5STICK_C_PLUS2

#include <Arduino.h>
#include <Wire.h>
#include "driver/gpio.h"
#include "m5stickPlus2Hw.h"
#include "timeutil.h"
#include "drivers/devices/device.h"

#define PLUS2_I2C_SDA 21
#define PLUS2_I2C_SCL 22
#define PLUS2_I2C_FREQ 400000
#define RTC_ADDR 0x51

static bool s_i2cStarted = false;

bool plus2I2cBegin()
{
  if (!s_i2cStarted)
    s_i2cStarted = Wire.begin(PLUS2_I2C_SDA, PLUS2_I2C_SCL, PLUS2_I2C_FREQ);
  return s_i2cStarted;
}

static bool writeReg(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(data, len);
  return Wire.endTransmission() == 0;
}

static bool writeReg8(uint8_t addr, uint8_t reg, uint8_t value)
{
  return writeReg(addr, reg, &value, 1);
}

static bool readReg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint16_t)addr, (uint8_t)len) != len)
    return false;
  for (size_t i = 0; i < len; i++)
    data[i] = Wire.read();
  return true;
}

// --------------------------------------------------------------- RTC ----

static bool s_rtcInit = false;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
static bool validBcd(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

static bool rtcInit()
{
  if (s_rtcInit)
    return true;
  if (!plus2I2cBegin())
    return false;
  writeReg8(RTC_ADDR, 0x00, 0x00); // first write can fail on a cold chip (M5Unified does the same)
  s_rtcInit = writeReg8(RTC_ADDR, 0x00, 0x00) && writeReg8(RTC_ADDR, 0x0E, 0x03);
  return s_rtcInit;
}

bool plus2RtcRead(int64_t &utc)
{
  if (!rtcInit())
    return false;
  uint8_t b[7];
  if (!readReg(RTC_ADDR, 0x02, b, sizeof(b)))
    return false;
  if (b[0] & 0x80)
    return false; // VL flag: the clock stopped at some point, time not trustworthy
  const uint8_t sec = b[0] & 0x7F, min = b[1] & 0x7F, hour = b[2] & 0x3F;
  const uint8_t day = b[3] & 0x3F, mon = b[5] & 0x1F, yr = b[6];
  if (!validBcd(sec) || !validBcd(min) || !validBcd(hour) || !validBcd(day) || !validBcd(mon) || !validBcd(yr))
    return false;
  const int year = bcd2bin(yr) + ((b[5] & 0x80) ? 1900 : 2000);
  const unsigned m = bcd2bin(mon), d = bcd2bin(day);
  const unsigned h = bcd2bin(hour), mi = bcd2bin(min), s = bcd2bin(sec);
  if (year < 2024 || m < 1 || m > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59)
    return false; // never set, or nonsense
  utc = tu_daysFromCivil(year, m, d) * 86400 + h * 3600 + mi * 60 + s;
  return true;
}

bool plus2RtcWrite(int64_t utc)
{
  if (!rtcInit())
    return false;
  const int64_t days = tu_daysOf(utc);
  const int64_t secs = utc - days * 86400;
  int y; unsigned m, d;
  tu_civilFromDays(days, y, m, d);
  if (y < 2000 || y > 2099)
    return false;
  uint8_t b[7];
  b[0] = bin2bcd(secs % 60);          // also clears the VL flag
  b[1] = bin2bcd((secs / 60) % 60);
  b[2] = bin2bcd(secs / 3600);
  b[3] = bin2bcd(d);
  b[4] = (uint8_t)tu_weekday(days);
  b[5] = bin2bcd(m);                  // century bit clear: 20xx
  b[6] = bin2bcd(y % 100);
  return writeReg(RTC_ADDR, 0x02, b, sizeof(b));
}

// ----------------------------------------------------------- battery ----
// Calibrated reading from analogReadMilliVolts(), 16 samples averaged,
// smoothed, every 5 s.

static volatile int s_batteryMv = 0;
static uint32_t s_batteryReadAt = 0;

int plus2BatteryMillivolts()
{
  if (s_batteryMv != 0 && millis() - s_batteryReadAt < 5000)
    return s_batteryMv;
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++)
    sum += analogReadMilliVolts(PIN_BATTERY_ADC);
  const int mv = (int)(sum / 16) * 2;
  const int prev = s_batteryMv;
  s_batteryMv = prev ? (prev * 3 + mv) / 4 : mv;
  s_batteryReadAt = millis();
  return s_batteryMv;
}

int plus2BatteryCachedMillivolts()
{
  return s_batteryMv;
}

// Single-cell LiPo, voltage under a light load -> remaining charge
int plus2BatteryPercent(int mv)
{
  static const int16_t curve[][2] = {
      {4150, 100}, {4100, 95}, {4000, 85}, {3900, 72}, {3800, 55},
      {3750, 45},  {3700, 33}, {3650, 22}, {3600, 13}, {3500, 5}, {3300, 0}};
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[0][0]) return 100;
  for (int i = 1; i < n; i++)
    if (mv >= curve[i][0])
      return curve[i][1] + (mv - curve[i][0]) * (curve[i - 1][1] - curve[i][1]) / (curve[i - 1][0] - curve[i][0]);
  return 0;
}

// ------------------------------------------------------------- power ----

void plus2CutPower()
{
  gpio_hold_dis((gpio_num_t)PIN_POWER_HOLD);
  digitalWrite(PIN_POWER_HOLD, LOW);
  delay(1500);
  // Still running: powered by USB. Take the latch again and carry on.
  digitalWrite(PIN_POWER_HOLD, HIGH);
  gpio_hold_en((gpio_num_t)PIN_POWER_HOLD);
}

#endif // M5STICK_C_PLUS2
