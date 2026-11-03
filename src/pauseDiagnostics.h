#pragma once
#include <Arduino.h>
class HTTPClient;
enum class BackgroundFetch : uint8_t { Network, Fees, Height, Price, Pool };
int measuredBackgroundGet(HTTPClient &http, BackgroundFetch kind);
String backgroundPauseJson();
void noteHardwareChunk(uint32_t nonces);
void sampleRateDiagnostics(float displayedKHs, uint32_t monitorAgeMs, bool subscribed, int authorized);
String rateDiagnosticsJson();
