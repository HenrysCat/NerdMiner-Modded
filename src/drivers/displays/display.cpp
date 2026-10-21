#include "display.h"

#ifdef NO_DISPLAY
DisplayDriver *currentDisplayDriver = &noDisplayDriver;
#endif

#ifdef M5STACK_DISPLAY
DisplayDriver *currentDisplayDriver = &m5stackDisplayDriver;
#endif

#ifdef WT32_DISPLAY
DisplayDriver *currentDisplayDriver = &wt32DisplayDriver;
#endif

#ifdef LED_DISPLAY
DisplayDriver *currentDisplayDriver = &ledDisplayDriver;
#endif

#ifdef OLED_042_DISPLAY
DisplayDriver *currentDisplayDriver = &oled042DisplayDriver;
#endif

#ifdef T_DISPLAY
DisplayDriver *currentDisplayDriver = &tDisplayDriver;
#endif

#ifdef AMOLED_DISPLAY
DisplayDriver *currentDisplayDriver = &amoledDisplayDriver;
#endif

#ifdef DONGLE_DISPLAY
DisplayDriver *currentDisplayDriver = &dongleDisplayDriver;
#endif

#ifdef ESP32_2432S028R
DisplayDriver *currentDisplayDriver = &esp32_2432S028RDriver;
#endif

#ifdef ESP32_2432S028_2USB
DisplayDriver *currentDisplayDriver = &esp32_2432S028RDriver;
#endif

#ifdef ES3C35P_DISPLAY
DisplayDriver *currentDisplayDriver = &es3c35pDriver;
#endif

#ifdef ESP32_4IN_ST7796_DISPLAY
DisplayDriver *currentDisplayDriver = &esp32_4inDriver;
#endif

#ifdef T_QT_DISPLAY
DisplayDriver *currentDisplayDriver = &t_qtDisplayDriver;
#endif

#ifdef V1_DISPLAY
DisplayDriver *currentDisplayDriver = &tDisplayV1Driver;
#endif

#ifdef TDISPLAY_PULSE
DisplayDriver *currentDisplayDriver = &tDisplayPulseDriver;
#endif

#ifdef M5STICKC_DISPLAY
DisplayDriver *currentDisplayDriver = &m5stickCDriver;
#endif

#ifdef M5STICKCPLUS_DISPLAY
DisplayDriver *currentDisplayDriver = &m5stickCPlusDriver;
#endif

#ifdef T_HMI_DISPLAY
DisplayDriver *currentDisplayDriver = &t_hmiDisplayDriver;
#endif

#ifdef ST7735S_DISPLAY
DisplayDriver *currentDisplayDriver = &sp_kcDisplayDriver;
#endif

#ifdef OLED_SSD1306_128X64_DISPLAY
DisplayDriver *currentDisplayDriver = &ssd1306DisplayDriver;
#endif


// Initialize the display
void initDisplay()
{
  currentDisplayDriver->initDisplay();
}

#ifdef HAS_BACKLIGHT_CONTROL
#include <Arduino.h>
#include "../storage/storage.h"
#include "../../monitor.h"
#include "../../sunTimes.h"
extern TSettings Settings;

static volatile uint32_t s_lastScreenActivity = 0; // millis(); 0 = boot
static volatile int s_backlightLevel = -1;         // duty the lit screen is at; -1 = not worked out yet

static bool nightDimLocationSet()
{
  return Settings.Latitude != 0.0f || Settings.Longitude != 0.0f;
}

// Blends the day and night levels by how far through the sunset (or sunrise)
// fade the sun is, so the backlight follows the light outside instead of
// stepping at one particular minute. Before NTP, without a location or with
// night dimming off, this is simply the day level.
static int targetBacklightLevel(bool sync)
{
  const int day = Settings.Brightness;
  time_t utc;
  if (!Settings.nightDim || !nightDimLocationSet() || !getUtcTime(utc, sync))
    return day;
  const float night = sunNightFraction(utc, Settings.Latitude, Settings.Longitude, Settings.NightFade, NULL, NULL);
  return (int)(day + (Settings.NightBrightness - day) * night + 0.5f);
}

int backlightLevel()
{
  const int level = s_backlightLevel;
  return level >= 0 ? level : Settings.Brightness;
}

void noteScreenActivity()
{
  s_lastScreenActivity = millis();
}

bool wakeScreen()
{
  noteScreenActivity();
  if (ledcRead(0) > 0)
    return false;
  ledcWrite(0, backlightLevel());
  return true;
}

void backlightTick()
{
  // Brightness (also when changed on the LAN settings page) and night
  // dimming. A screen that has been switched off stays off; one that is
  // only dark because the level was 0 follows the new level.
  const int level = targetBacklightLevel(true);
  if (level != s_backlightLevel)
  {
    const bool lit = ledcRead(0) > 0 || s_backlightLevel == 0;
    s_backlightLevel = level;
    if (lit)
      ledcWrite(0, level);
  }

  const int minutes = Settings.ScreenTimeout;
  if (minutes <= 0 || ledcRead(0) == 0)
    return;
  if (millis() - s_lastScreenActivity >= (uint32_t)minutes * 60000UL)
  {
    Serial.printf("Screen off after %d min without a touch\n", minutes);
    ledcWrite(0, 0);
  }
}

String nightDimStatus()
{
  if (!nightDimLocationSet())
    return F("Pick a city or enter your latitude and longitude to use night dimming.");
  time_t utc;
  if (!getUtcTime(utc))
    return F("Waiting for the clock to be set from the network.");
  int sunrise, sunset;
  const float night = sunNightFraction(utc, Settings.Latitude, Settings.Longitude, Settings.NightFade, &sunrise, &sunset);
  String s = night <= 0.0f ? F("Sun is up") : night >= 1.0f ? F("Sun is down") : F("Fading");
  if (sunrise < 0)
    return s + F(" all day at this location.");
  // In the timezone set above
  const int offset = Settings.Timezone * 60 + (int)(currentDstOffset() / 60) + 1440; // + summer time (AUTO_DST_EU)
  char times[48];
  snprintf(times, sizeof(times), ": sunrise %02d:%02d, sunset %02d:%02d.",
           (sunrise + offset) % 1440 / 60, (sunrise + offset) % 60,
           (sunset + offset) % 1440 / 60, (sunset + offset) % 60);
  return s + times + F(" Backlight now ") + String(backlightLevel()) + F(".");
}
#endif

// Alternate screen state
void alternateScreenState()
{
  noteScreenActivity();
  currentDisplayDriver->alternateScreenState();
}

// Alternate screen rotation
void alternateScreenRotation()
{
  if (wakeScreen())
    return; // dark: the press only wakes the screen
  currentDisplayDriver->alternateScreenRotation();
}

// Draw the loading screen
void drawLoadingScreen()
{
  currentDisplayDriver->loadingScreen();
}

// Draw the setup screen
void drawSetupScreen()
{
  currentDisplayDriver->setupScreen();
}

// Reset the current cyclic screen to the first one
void resetToFirstScreen()
{
  currentDisplayDriver->current_cyclic_screen = 0;
}

// Switches to the next cyclic screen without drawing it
void switchToNextScreen()
{
  if (wakeScreen())
    return; // dark: the press only wakes the screen
  currentDisplayDriver->current_cyclic_screen = (currentDisplayDriver->current_cyclic_screen + 1) % currentDisplayDriver->num_cyclic_screens;
}

// Draw the current cyclic screen
void drawCurrentScreen(unsigned long mElapsed)
{
  currentDisplayDriver->cyclic_screens[currentDisplayDriver->current_cyclic_screen](mElapsed);
}

// Animate the current cyclic screen
void animateCurrentScreen(unsigned long frame)
{
  currentDisplayDriver->animateCurrentScreen(frame);
}

// Do LED stuff
void doLedStuff(unsigned long frame)
{
  currentDisplayDriver->doLedStuff(frame);
}
