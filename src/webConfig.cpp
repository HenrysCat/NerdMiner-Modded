#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "webConfig.h"
#include "wManager.h"
#include "drivers/devices/device.h"
#include "drivers/storage/storage.h"
#include "currency.h"

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

static void handleRoot()
{
  if (!checkAuth())
    return;

  String page;
  page.reserve(3600);
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
#ifdef NO_DISPLAY
  page += "<label><input type='checkbox' name='led' " + String(Settings.statusLed ? "checked" : "") + "> Status LED (blinks while mining)</label>";
#endif
  page += F("<input type='submit' value='Save &amp; Restart'>"
            "</form></body></html>");

  webCfgServer.send(200, "text/html", page);
}

static void handleSave()
{
  if (!checkAuth())
    return;

  if (webCfgServer.hasArg("pool"))
    Settings.PoolAddress = webCfgServer.arg("pool");
  if (webCfgServer.hasArg("port"))
    Settings.PoolPort = webCfgServer.arg("port").toInt();
  if (webCfgServer.hasArg("poolpass"))
    strncpy(Settings.PoolPassword, webCfgServer.arg("poolpass").c_str(), sizeof(Settings.PoolPassword) - 1);
  if (webCfgServer.hasArg("wallet"))
    strncpy(Settings.BtcWallet, webCfgServer.arg("wallet").c_str(), sizeof(Settings.BtcWallet) - 1);
  if (webCfgServer.hasArg("tz"))
    Settings.Timezone = webCfgServer.arg("tz").toInt();
  if (webCfgServer.hasArg("currency"))
    Settings.Currency = currencyFor(webCfgServer.arg("currency")).code;
  Settings.saveStats = webCfgServer.hasArg("savestats");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  Settings.invertColors = webCfgServer.hasArg("invert");
  Settings.flipDisplay = webCfgServer.hasArg("flip");
  if (webCfgServer.hasArg("brightness"))
    Settings.Brightness = webCfgServer.arg("brightness").toInt();
#endif
#if defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  Settings.invertColors = webCfgServer.hasArg("invert");
  Settings.flipDisplay = webCfgServer.hasArg("flip");
  if (webCfgServer.hasArg("brightness"))
    Settings.Brightness = webCfgServer.arg("brightness").toInt();
#endif
#ifdef NO_DISPLAY
  Settings.statusLed = webCfgServer.hasArg("led");
#endif

  saveSettingsToFlash();

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
