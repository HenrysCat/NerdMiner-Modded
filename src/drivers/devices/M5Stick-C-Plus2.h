#ifndef _M5_STICK_C_PLUS2_H
#define _M5_STICK_C_PLUS2_H

// M5StickC Plus2 -- ESP32-PICO-V3-02 (dual core 240 MHz, 8 MB flash, 2 MB PSRAM)
// https://docs.m5stack.com/en/core/M5StickC%20PLUS2
//
//   Display  ST7789V2 135x240 : MOSI 15, SCLK 13, CS 5, DC 14, RST 12, BL 27
//                               (see lib/TFT_eSPI/User_Setups/Setup304_M5StickCPlus2.h)
//   Button A (front, "M5")    : GPIO37  (external pull-up, active low)
//   Button B (right side)     : GPIO39  (external pull-up, active low)
//   Button C (left, power)    : GPIO35  (active low)
//   Power hold latch          : GPIO4   (must stay HIGH or the stick turns off on battery)
//   Red LED + IR emitter      : GPIO19  (active high)
//   Buzzer                    : GPIO2
//   Battery voltage (ADC, /2) : GPIO38
//   Internal I2C (RTC BM8563, IMU MPU6886) : SDA 21, SCL 22
//   Grove port                : G32 / G33
//
// The power-hold latch, the LED and the buzzer are initialised as early as
// possible in boards/variants/m5stick_c_plus2/variant.cpp (initVariant()),
// before setup() runs, and GPIO4 is pad-held so it survives ESP.restart().

// Buttons used by NerdMiner (two button device layout):
//   A (GPIO37): click = screen on/off, double click = rotate 180 deg
//   B (GPIO39): click = next screen, long press 5 s = erase config
#define PIN_BUTTON_1 37
#define PIN_BUTTON_2 39

// Power button (left side). Long press = power off when running on battery.
#define PIN_BUTTON_POWER 35
#define PIN_POWER_HOLD 4

#define LED_PIN 19
#define PIN_BUZZER 2
#define PIN_BATTERY_ADC 38

// Signal when the pool accepts a share: LED flash and/or a short beep. A found
// block plays a longer pattern. Turn either off with -D SHARE_LED=0 or
// -D SHARE_BEEP=0 in platformio.ini.
#ifndef SHARE_LED
#define SHARE_LED 1
#endif
#ifndef SHARE_BEEP
#define SHARE_BEEP 1
#endif

// Backlight (GPIO27) in PWM at 256 Hz like M5GFX does for this board:
// brightness, screen timeout and night dimming from the web settings page.
// -D PLUS2_BACKLIGHT_PWM=0 falls back to a plain on/off backlight.
#ifndef PLUS2_BACKLIGHT_PWM
#define PLUS2_BACKLIGHT_PWM 1
#endif
#define PLUS2_BACKLIGHT_FREQ 256

// On battery, below PLUS2_BATTERY_OFF_mV for 30 s: warning, then power off
// (avoids brown-out reboot loops when the cell is flat). Nothing happens on
// USB power. -D PLUS2_LOW_BATTERY_OFF=0 to turn it off.
#ifndef PLUS2_LOW_BATTERY_OFF
#define PLUS2_LOW_BATTERY_OFF 1
#endif
#ifndef PLUS2_BATTERY_OFF_mV
#define PLUS2_BATTERY_OFF_mV 3400
#endif

// 135x240 ST7789, same layout as the LilyGo T-Display V1
#define V1_DISPLAY

#endif
