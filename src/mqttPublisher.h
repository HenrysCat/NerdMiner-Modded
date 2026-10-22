#ifndef MQTT_PUBLISHER_H
#define MQTT_PUBLISHER_H

// MQTT for Home Assistant (or any broker): the /api/status JSON on
// <topic>/state every 30 s, "online"/"offline" on <topic>/status, and Home
// Assistant discovery messages so the sensors appear by themselves.
// Settings on the web settings page; off by default.
void mqttProcess(void);          // Arduino loop, next to webConfigProcess()
void mqttSettingsChanged(void);  // after the MQTT settings were saved
bool mqttConnected(void);

#endif
