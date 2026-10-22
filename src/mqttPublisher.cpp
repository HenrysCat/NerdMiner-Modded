// MQTT publisher, see mqttPublisher.h. Uses ESP-IDF's MQTT client (esp-mqtt,
// already in the Arduino core): no extra library. The client runs in its own
// task and reconnects by itself; this file only starts/stops it and publishes.
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <mqtt_client.h>
#include "mqttPublisher.h"
#include "webConfig.h"
#include "version.h"
#include "drivers/storage/storage.h"

extern TSettings Settings;

#define MQTT_PUBLISH_ms (30 * 1000UL)
#define MQTT_DISCOVERY_PREFIX "homeassistant"

static esp_mqtt_client_handle_t s_client = nullptr;
static volatile bool s_connected = false;
static volatile bool s_announce = false; // send discovery + "online" (set on connect)
static bool s_restart = true;            // (re)read the settings and (re)start
static uint32_t s_lastPublish = 0;
// The client keeps pointers into these while it runs
static String s_uri, s_user, s_pass, s_clientId, s_base, s_availTopic, s_node;

void mqttSettingsChanged(void) { s_restart = true; }
bool mqttConnected(void) { return s_client && s_connected; }

static void onMqttEvent(void *, esp_event_base_t, int32_t id, void *)
{
  if (id == MQTT_EVENT_CONNECTED)
  {
    s_connected = true;
    s_announce = true;
    Serial.println("[MQTT] connected");
  }
  else if (id == MQTT_EVENT_DISCONNECTED)
  {
    if (s_connected)
      Serial.println("[MQTT] disconnected");
    s_connected = false;
  }
}

static void stopClient()
{
  if (!s_client)
    return;
  if (s_connected)
  {
    // A clean disconnect doesn't trigger the last will: say "offline" before
    // going, or Home Assistant keeps showing the miner as available (with its
    // last values) after MQTT is switched off or moved to another broker.
    esp_mqtt_client_publish(s_client, s_availTopic.c_str(), "offline", 0, 1, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  esp_mqtt_client_stop(s_client);
  esp_mqtt_client_destroy(s_client);
  s_client = nullptr;
  s_connected = false;
}

static String nodeId()
{
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char id[24];
  snprintf(id, sizeof(id), "nerdminer_%02x%02x%02x", mac[3], mac[4], mac[5]);
  return String(id);
}

static void startClient()
{
  s_node = nodeId();
  s_base = Settings.MqttTopic;
  s_base.trim();
  while (s_base.endsWith("/"))
    s_base.remove(s_base.length() - 1);
  if (s_base.isEmpty())
    s_base = "nerdminer/" + s_node.substring(10); // nerdminer/<last 3 MAC bytes>
  s_availTopic = s_base + "/status";
  s_uri = "mqtt://" + Settings.MqttHost + ":" + String(Settings.MqttPort);
  s_user = Settings.MqttUser;
  s_pass = Settings.MqttPass;
  s_clientId = s_node;

  esp_mqtt_client_config_t cfg = {};
  cfg.uri = s_uri.c_str();
  cfg.client_id = s_clientId.c_str();
  if (s_user.length())
    cfg.username = s_user.c_str();
  if (s_pass.length())
    cfg.password = s_pass.c_str();
  cfg.lwt_topic = s_availTopic.c_str(); // the broker says "offline" for us if we vanish
  cfg.lwt_msg = "offline";
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.keepalive = 60;
  s_client = esp_mqtt_client_init(&cfg);
  if (!s_client)
  {
    Serial.println("[MQTT] client init failed");
    return;
  }
  esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, onMqttEvent, nullptr);
  esp_mqtt_client_start(s_client);
  Serial.printf("[MQTT] connecting to %s, topic %s\n", s_uri.c_str(), s_base.c_str());
}

static void publish(const String &topic, const String &payload, bool retain)
{
  esp_mqtt_client_publish(s_client, topic.c_str(), payload.c_str(), payload.length(), 1, retain ? 1 : 0);
}

// One Home Assistant sensor reading a field of the state message.
static void announceSensor(const char *key, const char *name, const char *tpl, const char *unit,
                           const char *devClass, const char *stateClass, const char *icon)
{
  DynamicJsonDocument d(1024);
  d["name"] = name;
  d["uniq_id"] = s_node + "_" + key;
  d["obj_id"] = s_node + "_" + key;
  d["stat_t"] = s_base + "/state";
  d["val_tpl"] = tpl;
  d["avty_t"] = s_availTopic;
  if (unit) d["unit_of_meas"] = unit;
  if (devClass) d["dev_cla"] = devClass;
  if (stateClass) d["stat_cla"] = stateClass;
  if (icon) d["ic"] = icon;
  JsonObject dev = d.createNestedObject("dev");
  dev.createNestedArray("ids").add(s_node);
  dev["name"] = "NerdMiner " + s_node.substring(10);
  dev["mf"] = "NerdMiner Modded";
  dev["mdl"] = ESP.getChipModel();
  dev["sw"] = CURRENT_VERSION;
  dev["cu"] = "http://" + WiFi.localIP().toString() + "/";
  String out;
  serializeJson(d, out);
  publish(String(MQTT_DISCOVERY_PREFIX "/sensor/") + s_node + "/" + key + "/config", out, true);
}

static void announce()
{
  publish(s_availTopic, "online", true);
  announceSensor("hashrate", "Hashrate", "{{ value_json.hashrate_khs }}", "kH/s", nullptr, "measurement", "mdi:pickaxe");
  announceSensor("hashrate_avg", "Average hashrate", "{{ value_json.hashrate_avg_khs }}", "kH/s", nullptr, "measurement", "mdi:pickaxe");
  announceSensor("best_diff", "Best difficulty", "{{ value_json.best_diff }}", nullptr, nullptr, "measurement", "mdi:trophy");
  announceSensor("shares_32bit", "32-bit shares", "{{ value_json.shares_32bit }}", nullptr, nullptr, "total_increasing", "mdi:check-decagram");
  announceSensor("shares_accepted", "Pool shares accepted", "{{ value_json.shares_accepted }}", nullptr, nullptr, "total_increasing", "mdi:check");
  announceSensor("shares_rejected", "Pool shares rejected", "{{ value_json.shares_rejected }}", nullptr, nullptr, "total_increasing", "mdi:close");
  announceSensor("blocks_found", "Blocks found", "{{ value_json.blocks_found }}", nullptr, nullptr, "total_increasing", "mdi:cube-outline");
  announceSensor("templates", "Block templates", "{{ value_json.templates }}", nullptr, nullptr, "total_increasing", "mdi:file-document-outline");
  announceSensor("mining_time", "Mining time", "{{ value_json.mining_time_s }}", "s", "duration", nullptr, nullptr);
  announceSensor("pool_difficulty", "Pool difficulty", "{{ value_json.pool.difficulty }}", nullptr, nullptr, "measurement", "mdi:tune");
  announceSensor("pool", "Pool", "{{ value_json.pool.address }}", nullptr, nullptr, nullptr, "mdi:server-network");
  announceSensor("rssi", "Wi-Fi signal", "{{ value_json.wifi.rssi }}", "dBm", "signal_strength", "measurement", nullptr);
  announceSensor("uptime", "Uptime", "{{ value_json.uptime_s }}", "s", "duration", nullptr, nullptr);
#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC)
  announceSensor("battery", "Battery", "{{ value_json.battery.percent }}", "%", "battery", "measurement", nullptr);
  announceSensor("battery_voltage", "Battery voltage", "{{ (value_json.battery.mv / 1000) | round(2) }}", "V", "voltage", "measurement", nullptr);
#endif
}

static void publishState()
{
  DynamicJsonDocument doc(1536);
  fillMinerStatusJson(doc);
  String out;
  serializeJson(doc, out);
  publish(s_base + "/state", out, false);
}

void mqttProcess(void)
{
  if (s_restart)
  {
    s_restart = false;
    stopClient();
    if (Settings.MqttEnabled && Settings.MqttHost.length() && WiFi.status() == WL_CONNECTED)
      startClient();
    else if (Settings.MqttEnabled && Settings.MqttHost.length())
      s_restart = true; // try again once the Wi-Fi is up
  }
  if (!mqttConnected())
    return;
  if (s_announce)
  {
    s_announce = false;
    announce();
    s_lastPublish = millis() - MQTT_PUBLISH_ms; // state straight away
  }
  if (millis() - s_lastPublish >= MQTT_PUBLISH_ms)
  {
    s_lastPublish = millis();
    publishState();
  }
}
