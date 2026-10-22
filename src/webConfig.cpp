#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_timer.h>
#include "webConfig.h"
#include "version.h"
#include "wManager.h"
#include "mining.h"
#include "monitor.h"
#include "mqttPublisher.h"
#include "drivers/devices/device.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include "currency.h"
#include "cities.h"
#include "webMonitorPage.h"
#ifdef M5STICK_C_PLUS2
#include "m5stickPlus2Hw.h"
#endif

extern TSettings Settings;

// Basic HTTP auth for the LAN settings page. The password starts as the setup
// AP's (DEFAULT_WIFIPW, "MineYourCoins") and can be changed on the page
// (Settings.WebPassword). Meant as a gate against casual access from others
// on the same network, not strong security (plain HTTP, no rate limiting).
static const char *WEBCFG_USER = "admin";
#define WEBCFG_PASS_MAX 63

static WebServer webCfgServer(80);
static bool webCfgStarted = false;

static bool checkAuth()
{
  // Settings.WebPassword is only written by handleSave, on this same task
  if (!webCfgServer.authenticate(WEBCFG_USER, Settings.WebPassword.c_str()))
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
  page += "<label>Backup pool URL (optional)</label><input type='text' name='bpool' value='" + htmlEscape(Settings.BackupPoolAddress) + "'>";
  page += "<label>Backup pool port</label><input type='number' name='bport' value='" + String(Settings.BackupPoolPort) + "'>";
  page += "<label>Backup pool password (optional)</label><input type='text' name='bpoolpass' value='" + htmlEscape(String(Settings.BackupPoolPassword)) + "'>";
  page += "<small>Mined with the same BTC address after " + String(POOL_FAILOVER_TRIES) +
          " failed attempts in a row on the pool above; back to that pool once it answers again (checked every " +
          String(POOL_FAILBACK_ms / 60000) + " minutes). Now mining on the " + (onBackupPool() ? "<b>backup</b>" : "main") + " pool.</small>";
#ifdef AUTO_DST_EU
  page += "<label>Timezone: UTC offset in WINTER (France/Belgium/Switzerland: 1). Summer time is added automatically.</label><input type='number' name='tz' value='" + String(Settings.Timezone) + "'>";
#else
  page += "<label>Timezone (UTC offset, -12/+12)</label><input type='number' name='tz' value='" + String(Settings.Timezone) + "'>";
#endif
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
#ifdef TDISPLAY_PULSE
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (USB on the left)</label>";
  page += "<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='" + String(Settings.Brightness) + "'>";
#endif
#if defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM
  page += "<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='" + String(Settings.Brightness) + "'>";
#endif
#ifdef HAS_BACKLIGHT_CONTROL
  page += "<label>Screen off after (minutes without a touch or button press, 0 = never)</label><input type='number' name='screenoff' min='0' max='1440' value='" + String(Settings.ScreenTimeout) + "'>";
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
#ifdef HAS_RGB_LED
  {
    static const char *const kLedModes[] = {"Off", "Status (red starting up, blue connecting, blinks the colour below while mining)", "Always on in the colour below"};
    page += F("<label>LED on the back</label><select name='ledmode'>");
    for (int i = LED_MODE_OFF; i <= LED_MODE_STEADY; i++)
    {
      page += "<option value='" + String(i) + "'";
      if (Settings.ledMode == i)
        page += " selected";
      page += ">" + String(kLedModes[i]) + "</option>";
    }
    page += F("</select>");
    char color[8];
    snprintf(color, sizeof(color), "#%06lx", (unsigned long)(Settings.ledColor & 0xFFFFFF));
    page += "<label>LED colour</label><input type='color' name='ledcolor' value='" + String(color) + "'>";
    page += "<label>LED brightness (1-255)</label><input type='number' name='ledbright' min='1' max='255' value='" + String(Settings.ledBrightness) + "'>";
    page += "<label><input type='checkbox' name='ledscreen' " + String(Settings.ledScreenOff ? "checked" : "") + "> LED off while the screen is off</label>";
  }
#endif
  flush();
  page += F("<h3>MQTT (Home Assistant)</h3>");
  page += "<label><input type='checkbox' name='mqtt' " + String(Settings.MqttEnabled ? "checked" : "") +
          "> Publish the miner's state to an MQTT broker</label>";
  page += "<label>Broker address (IP or name)</label><input type='text' name='mqhost' maxlength='64' value='" + htmlEscape(Settings.MqttHost) + "'>";
  page += "<label>Broker port</label><input type='number' name='mqport' min='1' max='65535' value='" + String(Settings.MqttPort) + "'>";
  page += "<label>User (optional)</label><input type='text' name='mquser' maxlength='64' autocomplete='off' value='" + htmlEscape(Settings.MqttUser) + "'>";
  page += "<label>Password (optional)</label><input type='password' name='mqpass' maxlength='64' autocomplete='off' value='" + htmlEscape(Settings.MqttPass) + "'>";
  page += "<label>Topic (empty: nerdminer/ and the end of the MAC address)</label><input type='text' name='mqtopic' maxlength='64' value='" + htmlEscape(Settings.MqttTopic) + "'>";
  page += String("<p><small>") + (mqttConnected() ? "Connected to the broker." : Settings.MqttEnabled ? "Not connected to the broker." : "Off.") +
          " Home Assistant finds the sensors by itself (MQTT discovery).</small></p>";
  page += F("<label>New password for this page (leave empty to keep it)</label>"
            "<input type='password' name='webpass' maxlength='63' autocomplete='new-password'>"
            "<label>New password again</label>"
            "<input type='password' name='webpass2' maxlength='63' autocomplete='new-password'>");
  page += F("<input type='submit' value='Save'>");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  page += F("<p><small>Settings apply straight away. Changing invert or flip restarts the device.</small></p>");
#elif defined(TDISPLAY_PULSE)
  page += F("<p><small>Settings apply straight away. Changing flip restarts the device.</small></p>");
#else
  page += F("<p><small>Settings apply straight away, no restart.</small></p>");
#endif
  page += F("</form><p><small><a href='/'>Live monitor</a> (no login needed) &middot; status as JSON: <a href='/api/status'>/api/status</a></small></p>");
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

  // New page password: checked first, so a typo saves nothing at all.
  const String webPass = webCfgServer.arg("webpass");
  if (!webPass.isEmpty() || !webCfgServer.arg("webpass2").isEmpty())
  {
    const char *problem = webPass != webCfgServer.arg("webpass2") ? "The two new passwords are not the same."
                          : webPass.length() > WEBCFG_PASS_MAX   ? "The new password is longer than 63 characters."
                                                                  : nullptr;
    if (problem)
    {
      webCfgServer.send(400, "text/html",
                        String("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                               "<meta name='viewport' content='width=device-width, initial-scale=1'></head>"
                               "<body style='font-family:sans-serif'><p>") + problem +
                            " Nothing was saved.</p><p><a href='/settings'>Back to settings</a></p></body></html>");
      return;
    }
    next.WebPassword = webPass;
  }

  if (webCfgServer.hasArg("pool"))
    next.PoolAddress = webCfgServer.arg("pool");
  if (webCfgServer.hasArg("port"))
    next.PoolPort = constrain(webCfgServer.arg("port").toInt(), 0, 65535); // like bport
  if (webCfgServer.hasArg("poolpass"))
    strncpy(next.PoolPassword, webCfgServer.arg("poolpass").c_str(), sizeof(next.PoolPassword) - 1);
  if (webCfgServer.hasArg("wallet"))
    strncpy(next.BtcWallet, webCfgServer.arg("wallet").c_str(), sizeof(next.BtcWallet) - 1);
  if (webCfgServer.hasArg("bpool"))
  {
    next.BackupPoolAddress = webCfgServer.arg("bpool");
    next.BackupPoolAddress.trim();
  }
  if (webCfgServer.hasArg("bport"))
    next.BackupPoolPort = constrain(webCfgServer.arg("bport").toInt(), 0, 65535);
  if (webCfgServer.hasArg("bpoolpass"))
    strlcpy(next.BackupPoolPassword, webCfgServer.arg("bpoolpass").c_str(), sizeof(next.BackupPoolPassword));
  next.MqttEnabled = webCfgServer.hasArg("mqtt");
  if (webCfgServer.hasArg("mqhost"))
  {
    next.MqttHost = webCfgServer.arg("mqhost").substring(0, 64);
    next.MqttHost.trim();
  }
  if (webCfgServer.hasArg("mqport"))
    next.MqttPort = constrain(webCfgServer.arg("mqport").toInt(), 1, 65535);
  if (webCfgServer.hasArg("mquser"))
    next.MqttUser = webCfgServer.arg("mquser").substring(0, 64);
  if (webCfgServer.hasArg("mqpass"))
    next.MqttPass = webCfgServer.arg("mqpass").substring(0, 64);
  if (webCfgServer.hasArg("mqtopic"))
  {
    next.MqttTopic = webCfgServer.arg("mqtopic").substring(0, 64);
    next.MqttTopic.trim();
  }
  if (webCfgServer.hasArg("tz"))
    next.Timezone = constrain(webCfgServer.arg("tz").toInt(), -12, 14);
  if (webCfgServer.hasArg("currency"))
    next.Currency = currencyFor(webCfgServer.arg("currency")).code;
  next.saveStats = webCfgServer.hasArg("savestats");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  next.invertColors = webCfgServer.hasArg("invert");
  next.flipDisplay = webCfgServer.hasArg("flip");
  if (webCfgServer.hasArg("brightness"))
    next.Brightness = constrain(webCfgServer.arg("brightness").toInt(), 0, 255);
#endif
#ifdef TDISPLAY_PULSE
  next.flipDisplay = webCfgServer.hasArg("flip");
  if (webCfgServer.hasArg("brightness"))
    next.Brightness = constrain(webCfgServer.arg("brightness").toInt(), 0, 255);
#endif
#if defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM
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
#ifdef HAS_RGB_LED
  if (webCfgServer.hasArg("ledmode"))
    next.ledMode = constrain(webCfgServer.arg("ledmode").toInt(), LED_MODE_OFF, LED_MODE_STEADY);
  const String ledColor = webCfgServer.arg("ledcolor"); // "#rrggbb"
  if (ledColor.length() == 7 && ledColor[0] == '#')
    next.ledColor = strtoul(ledColor.c_str() + 1, NULL, 16) & 0xFFFFFF;
  if (webCfgServer.hasArg("ledbright"))
    next.ledBrightness = constrain(webCfgServer.arg("ledbright").toInt(), 1, 255);
  next.ledScreenOff = webCfgServer.hasArg("ledscreen");
#endif

  saveSettingsToFlash(&next);

  const bool restart = next.invertColors != before.invertColors || next.flipDisplay != before.flipDisplay;
  if (!restart)
  {
    const bool poolChanged = next.PoolAddress != before.PoolAddress || next.PoolPort != before.PoolPort ||
                             strcmp(next.BtcWallet, before.BtcWallet) != 0 ||
                             strcmp(next.PoolPassword, before.PoolPassword) != 0;
    const bool backupChanged = next.BackupPoolAddress != before.BackupPoolAddress ||
                               next.BackupPoolPort != before.BackupPoolPort ||
                               strcmp(next.BackupPoolPassword, before.BackupPoolPassword) != 0;
    if (poolChanged || backupChanged)
      requestPoolChange(next); // stratum task reconnects if the pool in use changed
    const bool passChanged = next.WebPassword != before.WebPassword;
    Settings.WebPassword = next.WebPassword; // this task is the only reader
    if (next.MqttEnabled != before.MqttEnabled || next.MqttHost != before.MqttHost || next.MqttPort != before.MqttPort ||
        next.MqttUser != before.MqttUser || next.MqttPass != before.MqttPass || next.MqttTopic != before.MqttTopic)
    {
      // read by mqttProcess() on this same task (Arduino loop)
      Settings.MqttEnabled = next.MqttEnabled;
      Settings.MqttHost = next.MqttHost;
      Settings.MqttPort = next.MqttPort;
      Settings.MqttUser = next.MqttUser;
      Settings.MqttPass = next.MqttPass;
      Settings.MqttTopic = next.MqttTopic;
      mqttSettingsChanged();
    }
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
#ifdef HAS_RGB_LED
    // rgbLedTick() on the monitor task picks these up on its next pass.
    Settings.ledMode = next.ledMode;
    Settings.ledColor = next.ledColor;
    Settings.ledBrightness = next.ledBrightness;
    Settings.ledScreenOff = next.ledScreenOff;
#endif

    String msg = poolChanged ? F("<p>Saved. Reconnecting to the pool with the new settings...</p>")
                             : F("<p>Saved and applied.</p>");
    if (passChanged)
      msg += F("<p>Password changed: log in again with the new one (user admin).</p>");
    webCfgServer.send(200, "text/html",
                      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                      "<meta http-equiv='refresh' content='3;url=/settings'></head>"
                      "<body style='font-family:sans-serif'>" + msg +
                      "<p><a href='/settings'>Back to settings</a></p></body></html>");
    return;
  }

  // The page polls until the device is back up, then returns to the settings
  // page so the browser isn't left sitting on /save.
  webCfgServer.send(200, "text/html",
                     "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                     "<meta name='viewport' content='width=device-width, initial-scale=1'></head>"
                     "<body style='font-family:sans-serif'>"
                     "<p>Saved. Restarting device...</p>"
                     "<p><a href='/settings'>Back to settings</a></p>"
                     "<script>function p(){fetch('/settings',{cache:'no-store'})"
                     ".then(function(r){if(r.ok||r.status==401)location.replace('/settings');else setTimeout(p,1500)})"
                     ".catch(function(){setTimeout(p,1500)})}setTimeout(p,4000);</script>"
                     "</body></html>");

  saveStat(); // stats mined since the last periodic save
  delay(1000);
  ESP.restart();
}

// Miner state as JSON for Home Assistant, scripts and dashboards. No login:
// it is read-only and holds nothing the screen doesn't show, and no wallet
// address or passwords.
extern uint32_t templates, hashes, Mhashes;
extern uint64_t upTime;
extern volatile uint32_t shares, valids;
extern double best_diff;
extern monitor_data mMonitor;

// The miner's state as /api/status serves it; also the MQTT state message
// (mqttPublisher.cpp). Cheap: cached values only.
void fillMinerStatusJson(JsonDocument &doc)
{
  const MinerStatus m = getMinerStatus();
  doc["version"] = CURRENT_VERSION;
  doc["uptime_s"] = (uint32_t)(esp_timer_get_time() / 1000000ULL); // 64-bit clock: millis() wraps after 49.7 days
  doc["state"] = mMonitor.NerdStatus == NM_hashing ? "mining" : mMonitor.NerdStatus == NM_Connecting ? "connecting" : "starting";
  doc["hashrate_khs"] = round(m.hashrateKHs * 10.0) / 10.0;
  doc["hashrate_avg_khs"] = round(m.avgHashrateKHs * 10.0) / 10.0; // since boot
  doc["total_mhashes"] = Mhashes + hashes / 1000000; // also kept across restarts when stats are saved
  doc["mining_time_s"] = (uint32_t)upTime;
  doc["templates"] = templates;
  doc["best_diff"] = getBestDiff();
  doc["shares_32bit"] = shares;                    // accepted by the pool
  doc["shares_32bit_rejected"] = shares32Rejected; // refused by the pool
  doc["blocks_found"] = valids;
  doc["shares_accepted"] = m.accepted; // since boot
  doc["shares_rejected"] = m.rejected;
  JsonObject pool = doc.createNestedObject("pool");
  {
    // The stratum task swaps a new pool into Settings under this mutex
    // (takePoolChange); this runs on the other core, so copy it under it too.
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    pool["address"] = activePoolAddress();
    pool["port"] = activePoolPort();
  }
  pool["backup"] = onBackupPool();
  pool["subscribed"] = m.subscribed;
  if (m.authorized >= 0)
    pool["authorized"] = m.authorized == 1; // missing: no answer yet
  pool["difficulty"] = m.poolDifficulty;
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();
  doc["free_heap"] = ESP.getFreeHeap();

  // Cached values only: nothing here may start an HTTPS fetch or an NTP
  // request (those run on the monitor task and pause the miner).
  JsonObject clock = doc.createNestedObject("clock");
  int hh, mi, ss, dd, mo, yy;
  if (getLocalClock(hh, mi, ss, dd, mo, yy)) {
    char buf[16];
    clock["synced"] = true;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mi, ss);
    clock["time"] = buf;
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d", dd, mo, yy);
    clock["date"] = buf;
  } else {
    clock["synced"] = false;
  }
  JsonObject btc = doc.createNestedObject("btc");
  btc["price"] = getBTCpriceValue();                         // 0 = not fetched yet
  btc["prefix"] = currencyFor(currencySetting()).prefix;
  btc["block_height"] = getBlockHeightValue();               // 0 = not fetched yet
#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC)
  const int batMv = plus2BatteryCachedMillivolts();
  if (batMv > 0) {
    JsonObject bat = doc.createNestedObject("battery");
    bat["mv"] = batMv;
    bat["percent"] = plus2BatteryPercent(batMv);
  }
#endif
}

static void handleStatus()
{
  noteWebMonitorPoll(); // the live monitor (or a script) is watching
  DynamicJsonDocument doc(1536);
  fillMinerStatusJson(doc);
  String out;
  serializeJson(doc, out);
  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.sendHeader("Cache-Control", "no-store");
  webCfgServer.send(200, "application/json", out);
}

// --------------------------------------------------------------------------
// Live monitor (webMonitorPage.h) and its hashrate history: one sample every
// 30 s, the last hour, kept here in RAM (240 bytes).
// --------------------------------------------------------------------------
#define HISTORY_LEN 120
#define HISTORY_PERIOD_ms 30000
static uint16_t s_history[HISTORY_LEN]; // KH/s x 10
static uint8_t s_historyCount = 0, s_historyHead = 0;
static uint32_t s_historyLast = 0;

static void historyTick()
{
  if (millis() - s_historyLast < HISTORY_PERIOD_ms)
    return;
  s_historyLast = millis();
  uint32_t v = (uint32_t)(getMinerStatus().hashrateKHs * 10.0f + 0.5f);
  if (v > 65535) v = 65535;
  s_history[s_historyHead] = (uint16_t)v;
  s_historyHead = (s_historyHead + 1) % HISTORY_LEN;
  if (s_historyCount < HISTORY_LEN) s_historyCount++;
}

static void handleHistory()
{
  String out;
  out.reserve(16 + s_historyCount * 7);
  out = F("{\"interval_s\":");
  out += HISTORY_PERIOD_ms / 1000;
  out += F(",\"khs\":[");
  for (uint8_t i = 0; i < s_historyCount; i++) {
    const uint16_t v = s_history[(s_historyHead + HISTORY_LEN - s_historyCount + i) % HISTORY_LEN];
    if (i) out += ',';
    out += v / 10;
    out += '.';
    out += v % 10;
  }
  out += F("]}");
  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.sendHeader("Cache-Control", "no-store");
  webCfgServer.send(200, "application/json", out);
}

static void handleMonitor()
{
  noteWebMonitorOpened(); // fetch the BTC price / block height if the screens never did
  webCfgServer.sendHeader("Cache-Control", "no-cache");
  webCfgServer.send_P(200, "text/html; charset=utf-8", MONITOR_HTML);
}

void setup_webConfig(void)
{
  if (webCfgStarted)
    return;

  webCfgServer.on("/", HTTP_GET, handleMonitor);            // live monitor, no login
  webCfgServer.on("/settings", HTTP_GET, handleRoot);        // settings, login
  webCfgServer.on("/save", HTTP_POST, handleSave);
  webCfgServer.on("/api/status", HTTP_GET, handleStatus);
#ifdef TDISPLAY_UI_SCREENSHOT
  // Dev builds: the screens rendered at boot (tDisplayPulseDriver.cpp),
  // run-length encoded; tools/tdisplay_shots.py turns them into PNGs.
  webCfgServer.on("/shot", HTTP_GET, []()
  {
    extern bool tDisplayShot(int i, const char **name, const uint8_t **data, size_t *len);
    const char *name;
    const uint8_t *data;
    size_t len;
    if (!tDisplayShot(webCfgServer.arg("i").toInt(), &name, &data, &len))
    {
      webCfgServer.send(404, "text/plain", "no such shot");
      return;
    }
    webCfgServer.sendHeader("X-Shot-Name", name);
    webCfgServer.send_P(200, "application/octet-stream", (PGM_P)data, len);
  });
#endif
  webCfgServer.on("/api/history", HTTP_GET, handleHistory);
  // Browsers ask for it on every visit; without this it fell to onNotFound,
  // which redirected to "/" and sent the whole 11 KB page again.
  webCfgServer.on("/favicon.ico", HTTP_GET, []() { webCfgServer.send(204); });
  webCfgServer.onNotFound([]()
  {
    webCfgServer.sendHeader("Location", "/");
    webCfgServer.send(302, "text/plain", "");
  });
  webCfgServer.begin();
  webCfgStarted = true;

  Serial.print("[WEBCFG] Monitor at http://");
  Serial.print(WiFi.localIP());
  Serial.println("/  (settings: /settings)");
}

void webConfigProcess(void)
{
  historyTick(); // also before the server is up, so the graph has the full hour
  if (webCfgStarted)
    webCfgServer.handleClient();
}
