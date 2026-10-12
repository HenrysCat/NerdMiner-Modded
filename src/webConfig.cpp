#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "webConfig.h"
#include "wManager.h"
#include "mining.h"
#include "monitor.h"
#include "drivers/devices/device.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include "currency.h"
#include "cities.h"

extern TSettings Settings;

// Basic HTTP auth for the LAN settings page. Reuses the same password as
// the device's own setup AP (DEFAULT_WIFIPW, "MineYourCoins" unless changed
// upstream) rather than inventing a separate credential -- this is meant as
// a basic gate against casual access from others on the same network, not
// strong security (plain HTTP, no rate limiting).
static const char *WEBCFG_USER = "admin";
static const char *WEBCFG_PASS = DEFAULT_WIFIPW;

static WebServer webCfgServer(80);
static bool webCfgStarted = false;

static bool checkAuth()
{
  if (!webCfgServer.authenticate(WEBCFG_USER, WEBCFG_PASS))
  {
    webCfgServer.requestAuthentication();
    return false;
  }
  return true;
}

static String htmlEscape(const String &in)
{
  String out = in;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

// The settings as the page should show them: what is in use, or what was
// saved and is still waiting for the stratum or monitor task to apply it.
static TSettings currentSettings()
{
  TSettings s;
  {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    s = Settings;
  }
  readPoolSettings(s);
  s.Currency = currencySetting();
  return s;
}

static void handleRoot()
{
  if (!checkAuth())
    return;

  const TSettings Settings = currentSettings(); // shadows the global on purpose
  String page;
  page.reserve(5200);
  // Sent in parts (chunked), so the whole page with its city list never has
  // to sit in one String.
  bool started = false;
  auto flush = [&]() {
    if (!started)
    {
      webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
      webCfgServer.send(200, "text/html", page);
      started = true;
    }
    else
      webCfgServer.sendContent(page);
    page = "";
  };
  page += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width, initial-scale=1'>"
             "<title>NerdMiner Settings</title>"
             "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em}"
             "label{display:block;margin-top:1em;font-weight:bold}"
             "input[type=text],input[type=number],input[type=password],select{width:100%;padding:.4em;box-sizing:border-box}"
             "input[type=submit]{margin-top:1.5em;padding:.6em 1.2em}</style></head><body>"
             "<h2>NerdMiner Settings</h2>"
             "<form method='POST' action='/save'>");

  page += "<label>Pool URL</label><input type='text' name='pool' value='" + htmlEscape(Settings.PoolAddress) + "'>";
  page += "<label>Pool Port</label><input type='number' name='port' value='" + String(Settings.PoolPort) + "'>";
  page += "<label>Pool Password (optional)</label><input type='text' name='poolpass' value='" + htmlEscape(String(Settings.PoolPassword)) + "'>";
  page += "<label>BTC Address</label><input type='text' name='wallet' value='" + htmlEscape(String(Settings.BtcWallet)) + "'>";
  page += "<label>Timezone (UTC offset, -12/+12)</label><input type='number' name='tz' value='" + String(Settings.Timezone) + "'>";
  page += "<label>BTC price currency</label><select name='currency'>";
  for (int i = 0; i < kCurrencyCount; i++)
  {
    page += "<option value='" + String(kCurrencies[i].code) + "'";
    if (Settings.Currency.equalsIgnoreCase(kCurrencies[i].code))
      page += " selected";
    page += ">" + String(kCurrencies[i].label) + "</option>";
  }
  page += "</select>";
  page += "<label><input type='checkbox' name='savestats' " + String(Settings.saveStats ? "checked" : "") + "> Save mining statistics to flash</label>";
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  page += "<label><input type='checkbox' name='invert' " + String(Settings.invertColors ? "checked" : "") + "> Invert display colors</label>";
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (USB on the left)</label>";
  page += "<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='" + String(Settings.Brightness) + "'>";
#endif
#if defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  page += "<label><input type='checkbox' name='invert' " + String(Settings.invertColors ? "checked" : "") + "> Invert display colors</label>";
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (rotate 180 degrees)</label>";
  page += "<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='" + String(Settings.Brightness) + "'>";
#endif
#ifdef HAS_BACKLIGHT_CONTROL
  page += "<label>Screen off after (minutes without a touch, 0 = never)</label><input type='number' name='screenoff' min='0' max='1440' value='" + String(Settings.ScreenTimeout) + "'>";
  page += "<label><input type='checkbox' name='nightdim' " + String(Settings.nightDim ? "checked" : "") + "> Dim the screen at night</label>";
  page += "<small>Fades from the brightness above to the night brightness around sunset, and back at sunrise. " + nightDimStatus() + "</small>";
  page += "<label>Night brightness (1-255)</label><input type='number' name='nightbright' min='1' max='255' value='" + String(Settings.NightBrightness) + "'>";
  page += "<label>Fade length (minutes)</label><input type='number' name='nightfade' min='1' max='240' value='" + String(Settings.NightFade) + "'>";
  page += F("<small>The fade is centred on sunset and sunrise, half before and half after: 60 minutes starts dimming "
            "30 minutes before sunset and reaches the night brightness 30 minutes after it.</small>");
  // Location for sunrise and sunset: a city fills in the two fields below
  // (also without JavaScript: /save takes the city's coordinates when one is
  // selected); typing in them goes back to "Custom".
  flush();
  page += F("<label>Nearest city</label><select name='city'><option value=''>Custom (latitude and longitude below)</option>");
  for (int i = 0; i < kCityCount; i++)
  {
    page += "<option value='" + String(kCities[i].latitude, 2) + "," + String(kCities[i].longitude, 2) + "'";
    if (fabsf(Settings.Latitude - kCities[i].latitude) < 0.005f && fabsf(Settings.Longitude - kCities[i].longitude) < 0.005f)
      page += " selected";
    page += ">" + String(kCities[i].name) + "</option>";
  }
  page += F("</select>");
  flush();
  page += "<label>Latitude (decimal degrees, south negative)</label><input type='number' name='lat' min='-90' max='90' step='any' value='" + String(Settings.Latitude, 4) + "'>";
  page += "<label>Longitude (decimal degrees, west negative)</label><input type='number' name='lon' min='-180' max='180' step='any' value='" + String(Settings.Longitude, 4) + "'>";
#endif
#ifdef NO_DISPLAY
  page += "<label><input type='checkbox' name='led' " + String(Settings.statusLed ? "checked" : "") + "> Status LED (blinks while mining)</label>";
#endif
  page += F("<input type='submit' value='Save'>");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  page += F("<p><small>Settings apply straight away. Changing invert or flip restarts the device.</small></p>");
#else
  page += F("<p><small>Settings apply straight away, no restart.</small></p>");
#endif
  page += F("</form>");
#ifdef HAS_BACKLIGHT_CONTROL
  page += F("<script>var f=document.forms[0],c=f.city,a=f.lat,o=f.lon;"
            "c.onchange=function(){if(c.value){var p=c.value.split(',');a.value=p[0];o.value=p[1]}};"
            "a.oninput=o.oninput=function(){c.value=''};</script>");
#endif
  page += F("</body></html>");
  flush();
  webCfgServer.sendContent(""); // ends the chunked response
}

static void handleSave()
{
  if (!checkAuth())
    return;

  // The form goes into a copy. Most of it is then applied live, each part by
  // the task that reads it; only invert/flip still need a restart (they are
  // set when the display starts).
  const TSettings before = currentSettings();
  TSettings next = before;

  if (webCfgServer.hasArg("pool"))
    next.PoolAddress = webCfgServer.arg("pool");
  if (webCfgServer.hasArg("port"))
    next.PoolPort = webCfgServer.arg("port").toInt();
  if (webCfgServer.hasArg("poolpass"))
    strncpy(next.PoolPassword, webCfgServer.arg("poolpass").c_str(), sizeof(next.PoolPassword) - 1);
  if (webCfgServer.hasArg("wallet"))
    strncpy(next.BtcWallet, webCfgServer.arg("wallet").c_str(), sizeof(next.BtcWallet) - 1);
  if (webCfgServer.hasArg("tz"))
    next.Timezone = webCfgServer.arg("tz").toInt();
  if (webCfgServer.hasArg("currency"))
    next.Currency = currencyFor(webCfgServer.arg("currency")).code;
  next.saveStats = webCfgServer.hasArg("savestats");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  next.invertColors = webCfgServer.hasArg("invert");
  next.flipDisplay = webCfgServer.hasArg("flip");
  if (webCfgServer.hasArg("brightness"))
    next.Brightness = constrain(webCfgServer.arg("brightness").toInt(), 0, 255);
#endif
#ifdef HAS_BACKLIGHT_CONTROL
  if (webCfgServer.hasArg("screenoff"))
    next.ScreenTimeout = constrain(webCfgServer.arg("screenoff").toInt(), 0, 1440);
  next.nightDim = webCfgServer.hasArg("nightdim");
  if (webCfgServer.hasArg("nightbright"))
    next.NightBrightness = constrain(webCfgServer.arg("nightbright").toInt(), 1, 255);
  if (webCfgServer.hasArg("nightfade"))
    next.NightFade = constrain(webCfgServer.arg("nightfade").toInt(), 1, 240);
  // A selected city is "lat,lon"; Custom is empty and the two fields count.
  const String city = webCfgServer.arg("city");
  const int comma = city.indexOf(',');
  const String lat = comma > 0 ? city.substring(0, comma) : webCfgServer.arg("lat");
  const String lon = comma > 0 ? city.substring(comma + 1) : webCfgServer.arg("lon");
  if (comma > 0 || webCfgServer.hasArg("lat"))
    next.Latitude = constrain(lat.toFloat(), -90.0f, 90.0f);
  if (comma > 0 || webCfgServer.hasArg("lon"))
    next.Longitude = constrain(lon.toFloat(), -180.0f, 180.0f);
#endif
#ifdef NO_DISPLAY
  next.statusLed = webCfgServer.hasArg("led");
#endif

  saveSettingsToFlash(&next);

  const bool restart = next.invertColors != before.invertColors || next.flipDisplay != before.flipDisplay;
  if (!restart)
  {
    const bool poolChanged = next.PoolAddress != before.PoolAddress || next.PoolPort != before.PoolPort ||
                             strcmp(next.BtcWallet, before.BtcWallet) != 0 ||
                             strcmp(next.PoolPassword, before.PoolPassword) != 0;
    if (poolChanged)
      requestPoolChange(next); // stratum task reconnects
    if (!next.Currency.equalsIgnoreCase(before.Currency))
      requestCurrencyChange(currencyFor(next.Currency).code); // monitor task
    Settings.Timezone = next.Timezone; // monitor task moves the clock
    Settings.saveStats = next.saveStats;
#ifdef HAS_BACKLIGHT_CONTROL
    // backlightTick() on the monitor task sets the backlight from these
    // within a second.
    Settings.Brightness = next.Brightness;
    Settings.nightDim = next.nightDim;
    Settings.NightBrightness = next.NightBrightness;
    Settings.NightFade = next.NightFade;
    Settings.Latitude = next.Latitude;
    Settings.Longitude = next.Longitude;
    if (next.ScreenTimeout != before.ScreenTimeout)
    {
      Settings.ScreenTimeout = next.ScreenTimeout;
      wakeScreen(); // back on if it had timed out; the new timeout counts from now
    }
#endif
#ifdef NO_DISPLAY
    Settings.statusLed = next.statusLed;
#endif

    String msg = poolChanged ? F("<p>Saved. Reconnecting to the pool with the new settings...</p>")
                             : F("<p>Saved and applied.</p>");
    webCfgServer.send(200, "text/html",
                      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                      "<meta http-equiv='refresh' content='3;url=/'></head>"
                      "<body style='font-family:sans-serif'>" + msg +
                      "<p><a href='/'>Back to settings</a></p></body></html>");
    return;
  }

  // The page polls until the device is back up, then returns to the settings
  // page so the browser isn't left sitting on /save.
  webCfgServer.send(200, "text/html",
                     "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                     "<meta name='viewport' content='width=device-width, initial-scale=1'></head>"
                     "<body style='font-family:sans-serif'>"
                     "<p>Saved. Restarting device...</p>"
                     "<p><a href='/'>Back to settings</a></p>"
                     "<script>function p(){fetch('/',{cache:'no-store'})"
                     ".then(function(r){if(r.ok||r.status==401)location.replace('/');else setTimeout(p,1500)})"
                     ".catch(function(){setTimeout(p,1500)})}setTimeout(p,4000);</script>"
                     "</body></html>");

  delay(1000);
  ESP.restart();
}

void setup_webConfig(void)
{
  if (webCfgStarted)
    return;

  webCfgServer.on("/", HTTP_GET, handleRoot);
  webCfgServer.on("/save", HTTP_POST, handleSave);
  webCfgServer.onNotFound([]()
  {
    webCfgServer.sendHeader("Location", "/");
    webCfgServer.send(302, "text/plain", "");
  });
  webCfgServer.begin();
  webCfgStarted = true;

  Serial.print("[WEBCFG] Settings page available at http://");
  Serial.println(WiFi.localIP());
}

void webConfigProcess(void)
{
  if (webCfgStarted)
    webCfgServer.handleClient();
}
