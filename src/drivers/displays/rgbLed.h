#ifndef _RGB_LED_H_
#define _RGB_LED_H_

// RGB LED on the back of the CYD boards, set from the LAN settings page
// (Settings.ledMode, ledColor, ledBrightness, ledScreenOff).
void rgbLedInit(void);
void rgbLedTick(void); // monitor task, every doLedStuff call

#endif // _RGB_LED_H_
