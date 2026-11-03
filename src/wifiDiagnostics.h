#pragma once
#include <Arduino.h>
void setupWifiDiagnostics();
bool miningNetworkReady();
void noteMiningWifiReconnect();
String wifiDiagnosticsJson();

bool startHardwareComparison();
void stopHardwareComparison();
bool hardwareComparisonActive();
uint32_t hardwareComparisonRemaining();
void noteHardwareComparisonParked(bool parked);
constexpr int32_t hardwareTestTicks(uint32_t until,uint32_t now) {
  return until?(int32_t)(until-now):0;
}
static_assert(hardwareTestTicks(0,1234)==0,"inactive timer");
static_assert(hardwareTestTicks(1000,999)==1 && hardwareTestTicks(1000,1000)==0,"expiry boundary");
static_assert(hardwareTestTicks(1000,1001)<0,"expired timer");
static_assert(hardwareTestTicks(16,0xfffffff0U)==32,"millis wrap");
