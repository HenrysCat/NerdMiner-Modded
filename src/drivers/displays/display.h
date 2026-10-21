#ifndef DISPLAY_H
#define DISPLAY_H

#include "displayDriver.h"

extern DisplayDriver *currentDisplayDriver;

void initDisplay();
void alternateScreenState();
void alternateScreenRotation();
void switchToNextScreen();
void resetToFirstScreen();
void drawLoadingScreen();
void drawSetupScreen();
void drawCurrentScreen(unsigned long mElapsed);
void animateCurrentScreen(unsigned long frame);
void doLedStuff(unsigned long frame);

// Backlight control on the boards whose backlight is LEDC channel 0 (M5StickC
// Plus2: GPIO27, set up in tDisplayV1Driver.cpp) and set
// from the LAN settings page; a no-op on the rest.
//  - Screen timeout: off after Settings.ScreenTimeout minutes without a touch
//    or button press (0 = never).
//  - Night dimming: fades between Settings.Brightness and
//    Settings.NightBrightness around sunset and sunrise at the set location.
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796) || defined(TDISPLAY_PULSE) || \
    (defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM)
#define HAS_BACKLIGHT_CONTROL
#include <Arduino.h>
void noteScreenActivity();         // a touch or button press: restart the countdown
bool wakeScreen();                 // backlight back on if it was off; true if it was
void backlightTick();              // monitor task, once a second
int backlightLevel();              // duty to light the screen with right now (day, night or in between)
String nightDimStatus();           // one line for the settings page: sun state, sunrise, sunset
#else
inline void noteScreenActivity() {}
inline bool wakeScreen() { return false; }
inline void backlightTick() {}
#endif

#endif // DISPLAY_H
