#include "pauseDiagnostics.h"
#include "mining.h"
#include <HTTPClient.h>
#include <esp_timer.h>
#include <mutex>
#include <atomic>
#include <math.h>

namespace {
constexpr unsigned capacity=32;
struct Record { uint64_t endedMs; uint32_t waitMs, heldMs; int code; BackgroundFetch kind; };
Record records[capacity]{};
unsigned head=0, count=0;
uint32_t calls[5]{}, failures[5]{};
uint64_t heldTotal[5]{};
std::mutex stateMutex;
bool active=false, waiting=false;
BackgroundFetch activeKind=BackgroundFetch::Network;
uint64_t activeSince=0;
uint64_t nowMs(){return esp_timer_get_time()/1000ULL;}
const char *name(BackgroundFetch k) {
  static const char *names[]={"network","fees","height","price","pool"};
  return names[(unsigned)k];
}
}

int measuredBackgroundGet(HTTPClient &http, BackgroundFetch kind) {
  const uint64_t requested=nowMs();
  {
    std::lock_guard<std::mutex> state(stateMutex);
    active=true;waiting=true;activeKind=kind;activeSince=requested;
  }
  uint64_t acquired=0, ended=0;
  int code=-10000;
  try {
    HW_SHA_REQUEST();
    std::lock_guard<std::mutex> sha(g_hwShaMutex);
    acquired=nowMs();
    {
      std::lock_guard<std::mutex> state(stateMutex);
      waiting=false;activeSince=acquired;
    }
    code=http.GET();
    ended=nowMs();
  } catch (...) {
    ended=nowMs();
    // Preserve the caller's exception handling while still recording the pause.
    std::lock_guard<std::mutex> state(stateMutex);
    if(!acquired)acquired=ended;
    records[head]={ended,(uint32_t)(acquired-requested),(uint32_t)(ended-acquired),code,kind};
    head=(head+1)%capacity;if(count<capacity)++count;
    ++calls[(unsigned)kind];++failures[(unsigned)kind];heldTotal[(unsigned)kind]+=ended-acquired;
    active=false;
    throw;
  }
  {
    std::lock_guard<std::mutex> state(stateMutex);
    records[head]={ended,(uint32_t)(acquired-requested),(uint32_t)(ended-acquired),code,kind};
    head=(head+1)%capacity;if(count<capacity)++count;
    ++calls[(unsigned)kind];if(code<200 || code>=300)++failures[(unsigned)kind];
    heldTotal[(unsigned)kind]+=ended-acquired;active=false;
  }
  return code;
}

String backgroundPauseJson() {
  Record snapshot[capacity];
  unsigned size;uint32_t totals[5], failed[5];uint64_t held[5];
  bool running,wait;BackgroundFetch kind;uint64_t since;
  {
    std::lock_guard<std::mutex> state(stateMutex);
    size=count;
    for(unsigned i=0;i<size;++i)snapshot[i]=records[(head+capacity-count+i)%capacity];
    for(unsigned i=0;i<5;++i){totals[i]=calls[i];failed[i]=failures[i];held[i]=heldTotal[i];}
    running=active;wait=waiting;kind=activeKind;since=activeSince;
  }
  // Allocate the response only on demand, after releasing the state mutex.
  String json;json.reserve(5000);const uint64_t now=nowMs();
  json="{\"uptime_s\":"+String((uint32_t)(now/1000))+",\"capacity\":32,\"active\":";
  if(!running)json+="null";
  else json+="{\"kind\":\""+String(name(kind))+"\",\"phase\":\""+String(wait?"waiting_sha":"http_get")+"\",\"age_ms\":"+String((uint32_t)(now-since))+"}";
  json+=",\"totals\":[";
  char number[32];
  for(unsigned i=0;i<5;++i){
    if(i)json+=',';
    snprintf(number,sizeof(number),"%llu",(unsigned long long)held[i]);
    json+="{\"kind\":\""+String(name((BackgroundFetch)i))+"\",\"calls\":"+String(totals[i])+",\"failures\":"+String(failed[i])+",\"held_ms\":"+number+"}";
  }
  json+="],\"recent\":[";
  for(unsigned i=0;i<size;++i){
    if(i)json+=',';
    const Record &r=snapshot[i];
    json+="{\"kind\":\""+String(name(r.kind))+"\",\"age_ms\":"+String((uint32_t)(now-r.endedMs))+",\"wait_ms\":"+String(r.waitMs)+",\"held_ms\":"+String(r.heldMs)+",\"http_code\":"+String(r.code)+"}";
  }
  json+="]}";return json;
}

// Completed HW work only, independent of the monitor and result consumer.
// Unsigned counter subtraction tolerates wrapping within each <=60 s window.
namespace {
std::atomic<uint32_t> hwCompleted{0}, hwChunks{0};
struct RateRecord {
  uint64_t atMs;
  float displayedKHs;
  uint32_t elapsedMs, nonces, chunks, monitorAgeMs;
  int8_t authorized;
  bool subscribed, validWindow;
};
constexpr unsigned anomalyCapacity=16;
RateRecord anomalies[anomalyCapacity]{}, latestRate{};
unsigned anomalyHead=0, anomalyCount=0;
uint32_t anomalyTotal=0, sampledCount=0;
uint32_t previousNonces=0, previousChunks=0;
uint64_t previousAt=0;
std::mutex rateMutex;
}

void noteHardwareChunk(uint32_t nonces) {
  // Once per completed nonce chunk, never per hash.
  hwCompleted.fetch_add(nonces,std::memory_order_relaxed);
  hwChunks.fetch_add(1,std::memory_order_relaxed);
}

void sampleRateDiagnostics(float displayedKHs, uint32_t monitorAgeMs, bool subscribed, int authorized) {
  const uint64_t now=nowMs();
  const uint32_t nonces=hwCompleted.load(std::memory_order_relaxed);
  const uint32_t chunks=hwChunks.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> state(rateMutex);
  const uint64_t elapsed=now-previousAt;
  const bool valid=previousAt!=0 && elapsed>0 && elapsed<=60000;
  latestRate={now,displayedKHs,(uint32_t)elapsed,nonces-previousNonces,
    chunks-previousChunks,monitorAgeMs,(int8_t)authorized,subscribed,valid};
  previousNonces=nonces;previousChunks=chunks;previousAt=now;++sampledCount;
  if(!isfinite(displayedKHs) || displayedKHs<=0 || monitorAgeMs>5000) {
    anomalies[anomalyHead]=latestRate;
    anomalyHead=(anomalyHead+1)%anomalyCapacity;
    if(anomalyCount<anomalyCapacity)++anomalyCount;
    ++anomalyTotal;
  }
}

String rateDiagnosticsJson() {
  RateRecord snapshot[anomalyCapacity], last;
  unsigned count;uint32_t total,samples;
  {
    std::lock_guard<std::mutex> state(rateMutex);
    count=anomalyCount;total=anomalyTotal;samples=sampledCount;last=latestRate;
    for(unsigned i=0;i<count;++i)snapshot[i]=anomalies[(anomalyHead+anomalyCapacity-count+i)%anomalyCapacity];
  }
  String json;json.reserve(6000);const uint64_t now=nowMs();
  auto append=[&](const RateRecord &r) {
    json+="{\"age_ms\":"+String((uint32_t)(now-r.atMs))+",\"displayed_khs\":";
    json+=isfinite(r.displayedKHs)?String(r.displayedKHs,1):String("null");
    json+=",\"window_ms\":"+String(r.elapsedMs)+",\"hw_completed_khs\":";
    json+=r.validWindow?String((double)r.nonces/r.elapsedMs,1):String("null");
    json+=",\"hw_chunks\":"+String(r.chunks)+",\"monitor_age_ms\":"+String(r.monitorAgeMs);
    json+=",\"subscribed\":"+String(r.subscribed?"true":"false")+",\"authorized\":"+String(r.authorized)+"}";
  };
  json="{\"uptime_s\":"+String((uint32_t)(now/1000))+",\"capacity\":16,\"sampled\":"+String(samples)+",\"anomaly_total\":"+String(total)+",\"latest\":";
  if(samples)append(last);else json+="null";
  json+=",\"anomalies\":[";
  for(unsigned i=0;i<count;++i){if(i)json+=',';append(snapshot[i]);}
  json+="]}";return json;
}
