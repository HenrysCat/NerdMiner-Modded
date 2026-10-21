#ifndef _LILYGO_V1_TDISPLAY_H
#define _LILYGO_V1_TDISPLAY_H

#define PIN_BUTTON_1 0
#define PIN_BUTTON_2 35

// Pulse theme (tDisplayPulseDriver.cpp); TDISPLAY_THEME_CLASSIC (env
// TTGO-T-Display-Classic) keeps the original bitmap screens.
#ifdef TDISPLAY_THEME_CLASSIC
#define V1_DISPLAY
#else
#define TDISPLAY_PULSE
// POOL screen: stats from the pool actually in use (hmpool, heliospool,
// ckpool...), not always public-pool.io
#define SCREEN_WORKERS_ENABLE (1)
#endif

#endif
