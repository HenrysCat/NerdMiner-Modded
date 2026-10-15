#include "../devices/device.h"

#ifdef HAS_RGB_LED

#include <Arduino.h>
#include "rgbLed.h"
#include "monitor.h"
#include "../storage/storage.h"

extern TSettings Settings;
extern monitor_data mMonitor;

// Common anode: each colour pin is active low. Driven by PWM so any colour
// and brightness works. LEDC channels 2-4 (timers 1 and 2); the backlight
// has channel 0 on timer 0, and ledcSetup on channel 1 would retime it.
static const uint8_t kPins[3] = {LED_PIN, LED_PIN_G, LED_PIN_B};
static const uint8_t kFirstChannel = 2;

static uint32_t s_shown = 0xFFFFFFFF; // duties on the pins now, packed; none yet
static uint32_t s_lastBlink = 0;
static bool s_blinkOn = false;

// Rough gamma (square) so the brightness setting and mixed colours look
// even; any lit component stays at least 1/255.
static uint32_t level(uint32_t component, uint32_t brightness)
{
  const uint32_t v = component * brightness / 255;
  return (v * v + 254) / 255;
}

static void show(uint32_t rgb)
{
  const uint32_t b = constrain(Settings.ledBrightness, 1, 255);
  const uint32_t r = level((rgb >> 16) & 0xFF, b);
  const uint32_t g = level((rgb >> 8) & 0xFF, b);
  const uint32_t bl = level(rgb & 0xFF, b);
  const uint32_t packed = (r << 16) | (g << 8) | bl;
  if (packed == s_shown)
    return; // only touch the peripheral when something changes
  s_shown = packed;
  ledcWrite(kFirstChannel + 0, 255 - r); // 255 = pin held high = off
  ledcWrite(kFirstChannel + 1, 255 - g);
  ledcWrite(kFirstChannel + 2, 255 - bl);
}

void rgbLedInit(void)
{
  // Off is written after attaching: ledcAttachPin takes the duty it reads back
  // from a channel that has not run yet (0 = pin low = all three lit), so a
  // write before it is lost.
  for (int i = 0; i < 3; i++)
  {
    ledcSetup(kFirstChannel + i, 5000, 8);
    ledcAttachPin(kPins[i], kFirstChannel + i);
    ledcWrite(kFirstChannel + i, 255);
  }
  s_shown = 0xFFFFFFFF; // first rgbLedTick writes whatever it wants, off included
}

void rgbLedTick(void)
{
  uint32_t rgb = 0;
  switch (Settings.ledMode)
  {
  case LED_MODE_OFF:
    break;

  case LED_MODE_STEADY:
    rgb = Settings.ledColor;
    break;

  default: // LED_MODE_STATUS
    switch (mMonitor.NerdStatus)
    {
    case NM_waitingConfig:
      rgb = 0xFF0000; // red, steady: starting up or setup portal
      break;
    case NM_Connecting:
      rgb = 0x0000FF; // blue: connecting
      break;
    case NM_hashing:
      if (millis() - s_lastBlink >= 500)
      {
        s_lastBlink = millis();
        s_blinkOn = !s_blinkOn;
      }
      rgb = s_blinkOn ? Settings.ledColor : 0;
      break;
    }
    break;
  }

  if (Settings.ledScreenOff && ledcRead(0) == 0)
    rgb = 0; // screen switched or timed out off (backlight is LEDC channel 0)
  show(rgb);
}

#endif // HAS_RGB_LED
