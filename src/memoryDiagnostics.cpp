#include "memoryDiagnostics.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <mutex>

namespace {
struct Sample { uint64_t at; uint32_t free, largest, minimum; MemoryPoint point; };
constexpr unsigned capacity=32;
Sample samples[capacity]{};
unsigned head=0,count=0;
uint32_t total=0;
uint64_t previousBaseline=0, mailSettledAt=0;
std::mutex sampleMutex;
Sample measure(MemoryPoint point) {
  return {uint64_t(esp_timer_get_time()/1000ULL),
    uint32_t(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
    uint32_t(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
    uint32_t(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),point};
}
const char *name(MemoryPoint point) {
  static const char *names[]={"baseline","network_begin","network_end","fees_begin","fees_end",
    "height_begin","height_end","price_begin","price_end","pool_begin","pool_end",
    "mail_begin","mail_end","mail_settled"};
  return names[unsigned(point)];
}
}
void noteMemory(MemoryPoint point) {
  const Sample s=measure(point);
  std::lock_guard<std::mutex> lock(sampleMutex);
  samples[head]=s;head=(head+1)%capacity;
  if(count<capacity)++count;
  ++total;
}
// Called only by the Arduino loop, after publishing a completed mail result.
void scheduleMemoryMailSettled() {
  mailSettledAt=esp_timer_get_time()/1000ULL+30000;
}
void memoryDiagnosticsTick() {
  const uint64_t now=esp_timer_get_time()/1000ULL;
  if(mailSettledAt && now>=mailSettledAt){
    mailSettledAt=0;noteMemory(MemoryPoint::MailSettled);
  }
  if(previousBaseline && now-previousBaseline<60000)return;
  previousBaseline=now;noteMemory(MemoryPoint::Baseline);
}
String memoryDiagnosticsJson() {
  // Measure before allocating the JSON response. No strings/secrets are retained.
  const Sample current=measure(MemoryPoint::Baseline);
  Sample snapshot[capacity];unsigned size;uint32_t seen;
  {
    std::lock_guard<std::mutex> lock(sampleMutex);
    size=count;seen=total;
    for(unsigned i=0;i<size;++i)snapshot[i]=samples[(head+capacity-count+i)%capacity];
  }
  String json;json.reserve(6500);
  json="{\"uptime_s\":"+String(uint32_t(current.at/1000))+",\"capacity\":32,\"sample_count\":"+String(seen);
  json+=",\"ring_bytes\":"+String(sizeof(samples))+",\"current\":";
  auto append=[&](const Sample &s){
    json+="{\"point\":\""+String(name(s.point))+"\",\"age_ms\":"+String(uint32_t(current.at>s.at?current.at-s.at:0));
    json+=",\"free_bytes\":"+String(s.free)+",\"largest_block_bytes\":"+String(s.largest);
    json+=",\"minimum_free_bytes\":"+String(s.minimum)+"}";
  };
  append(current);json+=",\"recent\":[";
  for(unsigned i=0;i<size;++i){if(i)json+=',';append(snapshot[i]);}
  json+="]}";return json;
}
