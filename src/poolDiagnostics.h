#pragma once
#include <Arduino.h>
enum class PoolEvent : uint8_t {
  WifiLost, WifiRestored, TcpLost, DnsFailed, TcpFailed, TcpConnected,
  SubscribeFailed, Subscribed, AuthAccepted, AuthRefused, Inactivity,
  JobTimeout, ParseFailed, SettingsChanged, PoolSwitched, RetryWait, JobResumed, Count
};
void recordPoolEvent(PoolEvent event, int detail=0);
String poolDiagnosticsJson();
