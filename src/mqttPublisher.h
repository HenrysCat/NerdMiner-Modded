#ifndef MQTT_PUBLISHER_H
#define MQTT_PUBLISHER_H

#include <Arduino.h>

// MQTT for Home Assistant (or any broker): the /api/status JSON on
// <topic>/state every 30 s, "online"/"offline" on <topic>/status, and Home
// Assistant discovery messages so the sensors appear by themselves.
// Settings on the web settings page; off by default.
void mqttProcess(void);          // Arduino loop, next to webConfigProcess()
void mqttSettingsChanged(void);  // after the MQTT settings were saved
bool mqttConnected(void);
// The broker as typed on the page, cleaned: "mqtt://host:1884/" gives "host"
// and sets port to 1884 (port unchanged when none is given).
String mqttCleanHost(const String &in, int &port);

#endif
