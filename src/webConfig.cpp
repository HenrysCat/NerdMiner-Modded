#include "wifiDiagnostics.h"
#include "poolDiagnostics.h"
#include "pauseDiagnostics.h"
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
#include "mailNotifier.h"
#include "mailPolicy.h"
#include "memoryDiagnostics.h"
#include "drivers/devices/device.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include "currency.h"
#include "cities.h"
#include "webMonitorPage.h"
#include "hashrateHistory.h"
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

// A form posted from another site still carries the Basic auth the browser
// has cached for this page, so /save only takes requests whose Origin (or,
// without one, Referer) is this device. Browsers send Origin with every POST;
// a request with neither header (curl, scripts) is let through.
static bool sameOrigin()
{
  const String self = "http://" + webCfgServer.hostHeader();
  const String origin = webCfgServer.header("Origin");
  if (origin.length())
    return origin.equalsIgnoreCase(self);
  const String referer = webCfgServer.header("Referer");
  if (referer.length())
    return referer.equalsIgnoreCase(self) || referer.startsWith(self + "/");
  return true;
}

static String htmlEscape(const String &in)
{
  String out = in;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("'", "&#39;"); // the page puts every value between single quotes
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

  webCfgServer.sendHeader("Cache-Control", "no-store");
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
             "<h2>NerdMiner Settings</h2>");
  if (Settings.WebPassword == DEFAULT_WEBPASSWORD)
    page += F("<p style='background:#fff3cd;color:#664d03;padding:.6em;border-radius:4px'>"
              "This page still uses the default password, the same on every NerdMiner: "
              "set your own at the bottom of the page.</p>");
  page += F("<form method='POST' action='/save'>");

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
#ifdef M5STICK_C_PLUS2
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (rotate 180 degrees)</label>";
#else
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (USB on the left)</label>";
#endif
  page += "<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='" + String(Settings.Brightness) + "'>";
#endif
#if defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM && !defined(TDISPLAY_PULSE)
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
#ifndef NO_MQTT
  page += F("<h3>MQTT (Home Assistant)</h3>");
  page += "<label><input type='checkbox' name='mqtt' " + String(Settings.MqttEnabled ? "checked" : "") +
          "> Publish the miner's state to an MQTT broker</label>";
  page += "<label>Broker address (IP or name)</label><input type='text' name='mqhost' maxlength='64' value='" + htmlEscape(Settings.MqttHost) + "'>";
  page += "<label>Broker port</label><input type='number' name='mqport' min='1' max='65535' value='" + String(Settings.MqttPort) + "'>";
  page += "<label>User (optional)</label><input type='text' name='mquser' maxlength='64' autocomplete='off' value='" + htmlEscape(Settings.MqttUser) + "'>";
  // The saved password is never sent back to the browser: empty keeps it.
  page += String("<label>Password (optional)</label><input type='password' name='mqpass' maxlength='64' autocomplete='new-password' placeholder='") +
          (Settings.MqttPass.length() ? "saved, leave empty to keep it" : "none") + "'>";
  if (Settings.MqttPass.length())
    page += F("<label><input type='checkbox' name='mqpassclear'> Remove the saved password</label>");
  page += "<label>Topic (empty: nerdminer/ and the end of the MAC address)</label><input type='text' name='mqtopic' maxlength='64' value='" + htmlEscape(Settings.MqttTopic) + "'>";
  page += String("<p><small>") + (mqttConnected() ? "Connected to the broker." : Settings.MqttEnabled ? "Not connected to the broker." : "Off.") +
          " Home Assistant finds the sensors by itself (MQTT discovery). Plain MQTT only (usually port 1883), no TLS.</small></p>";
#endif
  flush();
  page += F("<h3>E-Mail (Gmail)</h3>");
  page += "<label><input type='checkbox' name='mailenabled' " + String(Settings.MailEnabled ? "checked" : "") + "> E-Mail aktivieren</label>";
  page += "<label>Gmail-Absender</label><input type='text' name='mailfrom' maxlength='100' value='" + htmlEscape(Settings.MailFrom) + "'>";
  page += "<label>Empf&auml;nger</label><input type='text' name='mailto' maxlength='100' value='" + htmlEscape(Settings.MailTo) + "'>";
  page += F("<label>Google-App-Passwort (leer lassen = behalten)</label><input type='password' name='mailpass' maxlength='32' autocomplete='new-password'>"
            "<label><input type='checkbox' name='mailpassclear'> Gespeichertes App-Passwort entfernen</label>"
            "<p><small>Bericht t&auml;glich 20:00 Europe/Berlin (Sommer-/Winterzeit automatisch). Warnung nach 2 Minuten Pool-Unterbrechung, Entwarnung nach 30 Sekunden stabiler Verbindung. "
            "Maximal 3 Versandversuche, mindestens 15 Minuten zwischen Fehlversuchen. WLAN und Internet werden ben&ouml;tigt. Ein Versand kann das Mining kurz pausieren.</small></p>"
            "<p><small>Nur ein Google-App-Passwort verwenden. Es wird auf dem Miner gespeichert; die Eingabe erfolgt &uuml;ber diese lokale HTTP-Seite. Nur im vertrauensw&uuml;rdigen Heimnetz eingeben.</small></p>");
  page += "<p><small>Status: " + htmlEscape(mailStatusText()) + "</small></p>";
  flush();
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
  page += F("</form><form method='POST' action='/mail-test'><button type='submit'>Testmail senden (zuerst Einstellungen speichern)</button></form><p><small><a href='/'>Live monitor</a> (no login needed) &middot; status as JSON: <a href='/api/status'>/api/status</a></small></p>");
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
  if (!sameOrigin())
  {
    webCfgServer.send(403, "text/plain", "Refused: the form was not sent from this miner's own settings page.");
    return;
  }

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
  {
    next.PoolAddress = webCfgServer.arg("pool");
    next.PoolAddress.trim(); // a pasted address often ends with a space or a newline
  }
  if (webCfgServer.hasArg("port"))
    next.PoolPort = constrain(webCfgServer.arg("port").toInt(), 0, 65535); // like bport
  if (webCfgServer.hasArg("poolpass"))
    strlcpy(next.PoolPassword, webCfgServer.arg("poolpass").c_str(), sizeof(next.PoolPassword));
  if (webCfgServer.hasArg("wallet"))
  {
    String wallet = webCfgServer.arg("wallet");
    wallet.trim();
    strlcpy(next.BtcWallet, wallet.c_str(), sizeof(next.BtcWallet));
  }
  if (webCfgServer.hasArg("bpool"))
  {
    next.BackupPoolAddress = webCfgServer.arg("bpool");
    next.BackupPoolAddress.trim();
  }
  if (webCfgServer.hasArg("bport"))
    next.BackupPoolPort = constrain(webCfgServer.arg("bport").toInt(), 0, 65535);
  if (webCfgServer.hasArg("bpoolpass"))
    strlcpy(next.BackupPoolPassword, webCfgServer.arg("bpoolpass").c_str(), sizeof(next.BackupPoolPassword));
#ifndef NO_MQTT
  next.MqttEnabled = webCfgServer.hasArg("mqtt");
  if (webCfgServer.hasArg("mqport"))
    next.MqttPort = constrain(webCfgServer.arg("mqport").toInt(), 1, 65535);
  if (webCfgServer.hasArg("mqhost")) // after the port: "host:1884" overrides it
    next.MqttHost = mqttCleanHost(webCfgServer.arg("mqhost").substring(0, 64), next.MqttPort);
  if (webCfgServer.hasArg("mquser"))
    next.MqttUser = webCfgServer.arg("mquser").substring(0, 64);
  if (webCfgServer.hasArg("mqpassclear"))
    next.MqttPass = "";
  else if (webCfgServer.arg("mqpass").length()) // empty: keep the saved one
    next.MqttPass = webCfgServer.arg("mqpass").substring(0, 64);
  if (webCfgServer.hasArg("mqtopic"))
  {
    next.MqttTopic = webCfgServer.arg("mqtopic").substring(0, 64);
    next.MqttTopic.trim();
  }
#endif
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
#if defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM && !defined(TDISPLAY_PULSE)
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

  next.MailEnabled = webCfgServer.hasArg("mailenabled");
  if (webCfgServer.hasArg("mailfrom")) { next.MailFrom=webCfgServer.arg("mailfrom");next.MailFrom.trim(); }
  if (webCfgServer.hasArg("mailto")) { next.MailTo=webCfgServer.arg("mailto");next.MailTo.trim(); }
  String mailPassword=webCfgServer.arg("mailpass");mailPassword.replace(" ","");
  if (webCfgServer.hasArg("mailpassclear")) next.MailPass="";
  else if (mailPassword.length()) next.MailPass=mailPassword;
  // Disabled mail must not prevent saving unrelated settings with empty defaults.
  if (!mailSettingsAllowed(next.MailEnabled, mailAddressValid(next.MailFrom),
      mailAddressValid(next.MailTo), mailPasswordValid(next.MailPass))) {
    webCfgServer.send(400,"text/plain; charset=utf-8","E-Mail nicht gespeichert: Adressen und 16-stelliges Google-App-Passwort pruefen. Abschalten ist auch ohne Passwort moeglich.");
    return;
  }
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
    if (next.MailEnabled!=before.MailEnabled || next.MailFrom!=before.MailFrom || next.MailTo!=before.MailTo || next.MailPass!=before.MailPass) {
      Settings.MailEnabled=next.MailEnabled;Settings.MailFrom=next.MailFrom;
      Settings.MailTo=next.MailTo;Settings.MailPass=next.MailPass;
      mailSettingsChanged();
    }
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
// address or passwords. No CORS header either: Home Assistant and scripts
// don't need one, and with it any web page open on a computer of the network
// could read the SSID, the local IP and the pool.
extern uint32_t templates;
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
  doc["total_mhashes"] = (uint32_t)(getTotalHashes() / 1000000); // also kept across restarts when stats are saved
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
  webCfgServer.sendHeader("Cache-Control", "no-store");
  webCfgServer.send(200, "application/json", out);
}

// Phase 2: bounded histories, collected even with no browser connected.
// 1176 bytes of sample arrays, no flash writes. Reset on reboot.
static HashrateHistory<120, 30> s_hour;
static HashrateHistory<180, 120> s_sixHours;
static HashrateHistory<288, 300> s_day;
static uint64_t s_lastHistoryTick = 0;

static void historyTick()
{
  const uint64_t now = esp_timer_get_time() / 1000000ULL;
  if (now - s_lastHistoryTick < 30) return;
  s_lastHistoryTick = now;
  const MinerStatus snapshot = getMinerStatus();
  float rate = snapshot.hashrateKHs;
  sampleRateDiagnostics(rate, rateSampleAgeMs(), snapshot.subscribed, snapshot.authorized);
  // Also rejects NaN, negative and infinite rates before integer conversion.
  if (!(rate >= 0.0f)) rate = 0.0f;
  if (rate > 6553.4f) rate = 6553.4f;
  const uint16_t value = (uint16_t)(rate * 10.0f + 0.5f);
  s_hour.sample(now, value);
  s_sixHours.sample(now, value);
  s_day.sample(now, value);
}

template <size_t N, uint32_t Period>
static void appendHistory(String &out, const HashrateHistory<N, Period> &h)
{
  out += '[';
  for (size_t i = 0; i < h.count; ++i) {
    if (i) out += ',';
    const uint16_t v = h.at(i);
    if (v == 65535) { out += F("null"); continue; }
    out += v / 10; out += '.'; out += v % 10;
  }
  out += ']';
}

template <size_t N,uint32_t Period>
static void appendMailStats(String &body,const char *label,const HashrateHistory<N,Period> &h) {
  uint32_t sum=0;uint16_t lo=65535,hi=0;size_t count=0;
  for(size_t i=0;i<h.count;++i) {
    const uint16_t v=h.at(i);if(v==65535)continue;
    sum+=v;if(v<lo)lo=v;if(v>hi)hi=v;++count;
  }
  body+=label;
  if(!count){body+=": noch keine Messwerte\n";return;}
  body+=": Min "+String(lo/10.0f,1)+", Max "+String(hi/10.0f,1)+", Mittel "+String(sum/(10.0f*count),1)+" KH/s ("+String((unsigned)count)+" Werte)\n";
}
void mailAppendHistoryStats(String &body) {
  body+="Verlaufsstatistik (verfuegbare Messwerte, Luecken ausgeschlossen):\n";
  appendMailStats(body,"1 h / 30-s-Stichproben",s_hour);
  appendMailStats(body,"6 h / 2-min-Mittelwerte",s_sixHours);
  appendMailStats(body,"24 h / 5-min-Mittelwerte",s_day);
}
static void handleHardwareComparison() {
  if(!checkAuth())return;
  if(webCfgServer.method()==HTTP_POST) {
    if(!sameOrigin()){webCfgServer.send(403,"text/plain","Refused");return;}
    const String action=webCfgServer.arg("action");
    if(action=="start")startHardwareComparison();
    else if(action=="stop")stopHardwareComparison();
    else {webCfgServer.send(400,"text/plain","Unknown action");return;}
    webCfgServer.sendHeader("Location","/hardware-test");
    webCfgServer.send(303,"text/plain","");return;
  }
  webCfgServer.sendHeader("Cache-Control","no-store");
  String page=F("<!doctype html><html lang='de'><meta charset='utf-8'><meta name='viewport' content='width=device-width'><title>Hardware-Vergleich</title><body style='font-family:Arial;max-width:600px;margin:24px'><h1>Hardware-Mining vergleichen</h1><p>Dieser Test pausiert nur den Hardware-Miner fuer 12 Minuten. Danach startet er automatisch wieder. WLAN, Poolverbindung und Software-Miner bleiben aktiv.</p><p>Die Hashrate sinkt waehrend des Tests deutlich. Ein Mausklick startet den Test.</p><p>Restzeit: ");
  page+=String(hardwareComparisonRemaining());page+=F(" Sekunden</p><form method='post'><button name='action' value='start'>12-Minuten-Test starten</button> <button name='action' value='stop'>Test beenden</button></form><p><a href='/hardware-test'>Status aktualisieren</a> | <a href='/'>Dashboard</a></p></body></html>");
  webCfgServer.send(200,"text/html; charset=utf-8",page);
}
static void handleMailTest() {
  if(!checkAuth())return;
  if(!sameOrigin()){webCfgServer.send(403,"text/plain","Refused");return;}
  const bool queued=mailQueueTest();
  webCfgServer.send(queued?202:409,"text/html; charset=utf-8",String("<html><body><p>")+
    (queued?"Testmail vorgemerkt. Versandstatus unter Einstellungen pruefen.":"Mailversand nicht bereit oder Testmail bereits vorgemerkt. Einstellungen speichern und Status pruefen.")+"</p><a href='/settings'>Einstellungen</a></body></html>");
}
static void handleHistory()
{
  const String range = webCfgServer.arg("range");
  if (range.length() && range != "1h" && range != "6h" && range != "24h") {
    webCfgServer.send(400, "application/json", "{\"error\":\"range must be 1h, 6h or 24h\"}");
    return;
  }
  const uint32_t period = range == "24h" ? 300 : range == "6h" ? 120 : 30;
  String out;
  if (!out.reserve(3072)) {
    webCfgServer.send(503, "application/json", "{\"error\":\"insufficient memory\"}");
    return;
  }
  out = F("{\"interval_s\":"); out += period;
  out += F(",\"age_s\":");
  out += (uint32_t)(esp_timer_get_time() / 1000000ULL % period);
  out += F(",\"khs\":");
  if (period == 300) appendHistory(out, s_day);
  else if (period == 120) appendHistory(out, s_sixHours);
  else appendHistory(out, s_hour);
  out += F(",\"khs_1h\":"); appendHistory(out, s_hour);
  out += '}';
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

  static const char *kHeaders[] = {"Origin", "Referer"}; // read by sameOrigin()
  webCfgServer.collectHeaders(kHeaders, 2);
  webCfgServer.on("/", HTTP_GET, handleMonitor);            // live monitor, no login
  webCfgServer.on("/settings", HTTP_GET, handleRoot);        // settings, login
  webCfgServer.on("/save", HTTP_POST, handleSave);
  webCfgServer.on("/mail-test", HTTP_POST, handleMailTest);
  webCfgServer.on("/api/status", HTTP_GET, handleStatus);
  webCfgServer.on("/hardware-test", HTTP_GET, handleHardwareComparison);
  webCfgServer.on("/hardware-test", HTTP_POST, handleHardwareComparison);
  webCfgServer.on("/api/wifi-diagnostics", HTTP_GET, []() {
    webCfgServer.sendHeader("Cache-Control", "no-store");
    webCfgServer.send(200, "application/json", wifiDiagnosticsJson());
  });
  webCfgServer.on("/api/pool-diagnostics", HTTP_GET, []() {
    webCfgServer.sendHeader("Cache-Control", "no-store");
    webCfgServer.send(200, "application/json", poolDiagnosticsJson());
  });
  webCfgServer.on("/api/rate-diagnostics", HTTP_GET, []() {
    webCfgServer.sendHeader("Cache-Control", "no-store");
    webCfgServer.send(200, "application/json", rateDiagnosticsJson());
  });
  webCfgServer.on("/api/memory-diagnostics", HTTP_GET, []() {
    webCfgServer.sendHeader("Cache-Control", "no-store");
    webCfgServer.send(200, "application/json", memoryDiagnosticsJson());
  });
  webCfgServer.on("/api/pauses", HTTP_GET, []() {
    webCfgServer.sendHeader("Cache-Control", "no-store");
    webCfgServer.send(200, "application/json", backgroundPauseJson());
  });
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
  historyTick(); // also before the server is up, so history grows without an open browser
  if (webCfgStarted)
    webCfgServer.handleClient();
}
