#ifndef M5STICK_PLUS2_HW_H
#define M5STICK_PLUS2_HW_H

// M5StickC Plus2 on-board chips on the internal I2C bus (SDA 21, SCL 22):
//   - BM8563 real-time clock (0x51, PCF8563 compatible), kept in UTC
// Only built for -D M5STICK_C_PLUS2 (see m5stickPlus2Hw.cpp).

#include <stdint.h>

bool plus2I2cBegin();                              // safe to call more than once
bool plus2RtcRead(int64_t &utc);                   // false: no chip, or time lost (battery ran flat)
bool plus2RtcWrite(int64_t utc);

// Battery on GPIO38 (1:2 divider). plus2BatteryMillivolts() measures at most
// every 5 s (call it from the monitor task); the cached value can be read
// from any task. 0 = not measured yet.
int plus2BatteryMillivolts();
int plus2BatteryCachedMillivolts();
int plus2BatteryPercent(int mv);

// Releases the power-hold latch (GPIO4). On battery the stick switches off and
// this never returns; on USB power it is still running 1.5 s later, so the
// latch is taken again and the function returns.
void plus2CutPower();

// Monitor task, every animation frame (~100 ms), from the display driver.
//  - plus2BatteryGuardTick: below PLUS2_BATTERY_OFF_mV for 30 s on battery,
//    calls showNotice() (the driver's "Low battery" screen), saves the stats
//    and cuts the power. Does nothing with -D PLUS2_LOW_BATTERY_OFF=0.
//  - plus2SignalTick: LED flash / beep for an accepted 32-bit share, a longer
//    pattern for a block (SHARE_LED / SHARE_BEEP).
void plus2BatteryGuardTick(void (*showNotice)());
void plus2SignalTick();

#endif // M5STICK_PLUS2_HW_H
