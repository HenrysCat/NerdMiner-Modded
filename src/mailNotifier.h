#ifndef NM_MAIL_NOTIFIER_H
#define NM_MAIL_NOTIFIER_H
#include <Arduino.h>
void mailProcess(); // Arduino loop, one tick per second
void mailSettingsChanged(); // same task, after settings are saved
bool mailQueueTest(); // same task, authenticated POST only
String mailStatusText(); // no addresses, credentials or server reply text
bool mailAddressValid(const String &address);
bool mailPasswordValid(const String &password);
#endif
