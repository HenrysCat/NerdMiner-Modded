#include "poolDiagnostics.h"
#include <esp_timer.h>
#include <mutex>
namespace {
constexpr unsigned capacity=32, kinds=(unsigned)PoolEvent::Count;
struct Entry { uint64_t ms; PoolEvent event; int detail; };
Entry entries[capacity]{};
unsigned head=0,count=0;
uint32_t totals[kinds]{};
std::mutex mutex;
const char *name(PoolEvent event) {
  static const char *names[]={"wifi_lost","wifi_restored","tcp_lost","dns_failed",
    "tcp_failed","tcp_connected","subscribe_failed","subscribed","auth_accepted",
    "auth_refused","inactivity_restart","job_timeout","notify_parse_failed",
    "settings_changed","pool_switched","retry_wait","job_resumed"};
  return names[(unsigned)event];
}
}
void recordPoolEvent(PoolEvent event,int detail) {
  std::lock_guard<std::mutex> lock(mutex);
  entries[head]={(uint64_t)(esp_timer_get_time()/1000ULL),event,detail};
  head=(head+1)%capacity;if(count<capacity)++count;
  ++totals[(unsigned)event];
}
String poolDiagnosticsJson() {
  Entry snapshot[capacity];uint32_t counts[kinds];unsigned n;
  {
    std::lock_guard<std::mutex> lock(mutex);n=count;
    for(unsigned i=0;i<n;++i)snapshot[i]=entries[(head+capacity-count+i)%capacity];
    for(unsigned i=0;i<kinds;++i)counts[i]=totals[i];
  }
  const uint64_t now=esp_timer_get_time()/1000ULL;
  String json;json.reserve(4000);
  json="{\"uptime_s\":"+String((uint32_t)(now/1000))+",\"capacity\":32,\"totals\":{";
  for(unsigned i=0;i<kinds;++i){if(i)json+=',';json+='"';json+=name((PoolEvent)i);json+="\":"+String(counts[i]);}
  json+="},\"recent\":[";
  for(unsigned i=0;i<n;++i){
    if(i)json+=',';
    json+="{\"age_ms\":"+String((uint32_t)(now-snapshot[i].ms))+",\"event\":\""+String(name(snapshot[i].event))+"\",\"detail\":"+String(snapshot[i].detail)+"}";
  }
  json+="]}";return json;
}
