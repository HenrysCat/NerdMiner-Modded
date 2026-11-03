#include "wifiDiagnostics.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <mutex>
#include <atomic>
namespace {
enum Kind { Disconnected, Connected, GotIP, LostIP, StatusMismatch, StatusRecovered, ReconnectRequested, TestStarted, TestEnded, TestCancelled, TestParked, KindCount };
constexpr unsigned capacity=32;
struct Entry { uint64_t ms; Kind kind; int detail; };
Entry events[capacity]{};
uint32_t totals[KindCount]{};
unsigned head=0,count=0;
std::mutex mutex;
bool mismatch=false; // only the Stratum task reads/writes this flag
std::atomic<bool> sleepDisableOK{false};
std::atomic<uint32_t> pauseUntil{0};
std::atomic<bool> hardwareParked{false};
const char *name(Kind kind) {
  static const char *names[]={"sta_disconnected","sta_connected","got_ip","lost_ip",
    "cached_status_mismatch","cached_status_recovered","mining_reconnect_requested","hardware_test_started","hardware_test_ended","hardware_test_cancelled","hardware_test_parked"};
  return names[kind];
}
void record(Kind kind,int detail=0) {
  std::lock_guard<std::mutex> lock(mutex);
  events[head]={(uint64_t)(esp_timer_get_time()/1000ULL),kind,detail};
  head=(head+1)%capacity;if(count<capacity)++count;++totals[kind];
}
bool associatedNow() {
  wifi_ap_record_t ap{};
  return esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
}
void onWifiEvent(arduino_event_id_t event,arduino_event_info_t info) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:record(Disconnected,info.wifi_sta_disconnected.reason);break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:record(Connected);break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:record(GotIP);break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:record(LostIP);break;
    default:break;
  }
}
}
void setupWifiDiagnostics() {
  // Called once, on the Stratum task. Core 0 dispatches the SDK WiFi events.
  WiFi.onEvent(WiFiEventFuncCb(onWifiEvent));
  // A/B test: keep the radio awake for WPA group-key renewal.
  // This is a latency test, not a confirmed fix for reason 16.
  sleepDisableOK.store(WiFi.setSleep(false));
}
bool miningNetworkReady() {
  const int cached=WiFi.status();
  if(cached==WL_CONNECTED) {
    if(mismatch){record(StatusRecovered,cached);mismatch=false;}
    return true;
  }
  // WiFi.status is event-driven. Confirm a negative status before tearing
  // down a live pool socket or issuing a reconnect that disconnects STA.
  const bool associated=associatedNow();
  const bool hasIP=(uint32_t)WiFi.localIP()!=0;
  if(associated && hasIP) {
    if(!mismatch)record(StatusMismatch,cached);
    mismatch=true;return true;
  }
  mismatch=false;return false;
}
void noteMiningWifiReconnect(){record(ReconnectRequested);}
uint32_t hardwareComparisonRemaining() {
  const uint32_t until=pauseUntil.load();
  if(!until)return 0;
  const int32_t delta=hardwareTestTicks(until,millis());
  return delta>0?((uint32_t)delta+999)/1000:0;
}
bool hardwareComparisonActive() {
  uint32_t until=pauseUntil.load();
  if(!until)return false;
  if(hardwareTestTicks(until,millis())>0)return true;
  if(pauseUntil.compare_exchange_strong(until,0))record(TestEnded);
  return false;
}
bool startHardwareComparison() {
  if(hardwareComparisonActive())return false;
  uint32_t deadline=millis()+12UL*60*1000;
  if(!deadline)deadline=1;
  pauseUntil.store(deadline);record(TestStarted,720);return true;
}
void stopHardwareComparison() {
  if(pauseUntil.exchange(0))record(TestCancelled);
}
void noteHardwareComparisonParked(bool parked) {
  hardwareParked.store(parked);
  if(parked)record(TestParked);
}

String wifiDiagnosticsJson() {
  Entry snapshot[capacity];uint32_t counts[KindCount];unsigned n;
  {
    std::lock_guard<std::mutex> lock(mutex);n=count;
    for(unsigned i=0;i<n;++i)snapshot[i]=events[(head+capacity-count+i)%capacity];
    for(unsigned i=0;i<KindCount;++i)counts[i]=totals[i];
  }
  const uint64_t now=esp_timer_get_time()/1000ULL;
  const int cached=WiFi.status();const bool associated=associatedNow(), hasIP=(uint32_t)WiFi.localIP()!=0;
  wifi_ps_type_t powerSave=WIFI_PS_MIN_MODEM;
  const bool powerSaveKnown=esp_wifi_get_ps(&powerSave)==ESP_OK;
  String json;json.reserve(4000);
  json="{\"uptime_s\":"+String((uint32_t)(now/1000))+",\"capacity\":32,\"current\":{\"cached_status\":"+String(cached)+",\"associated\":"+String(associated?"true":"false")+",\"has_ip\":"+String(hasIP?"true":"false")+"},\"totals\":{";
  for(unsigned i=0;i<KindCount;++i){if(i)json+=',';json+='"';json+=name((Kind)i);json+="\":"+String(counts[i]);}
  json+="},\"recent\":[";
  for(unsigned i=0;i<n;++i){
    if(i)json+=',';
    json+="{\"age_ms\":"+String((uint32_t)(now-snapshot[i].ms))+",\"event\":\""+String(name(snapshot[i].kind))+"\",\"detail\":"+String(snapshot[i].detail)+"}";
  }
  json+="],\"sleep_disable_ok\":"+String(sleepDisableOK.load()?"true":"false")+",\"power_save_mode\":";
  json+=powerSaveKnown?String((int)powerSave):String("null");
  json+=",\"hardware_test_remaining_s\":"+String(hardwareComparisonRemaining());
  json+=",\"hardware_test_hw_parked\":"+String(hardwareParked.load()?"true":"false");
  json+="}";return json;
}
