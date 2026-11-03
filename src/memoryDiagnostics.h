#pragma once
#include <Arduino.h>

enum class MemoryPoint : uint8_t {
  Baseline, NetworkBegin, NetworkEnd, FeesBegin, FeesEnd,
  HeightBegin, HeightEnd, PriceBegin, PriceEnd, PoolBegin, PoolEnd,
  MailBegin, MailEnd, MailSettled
};
void noteMemory(MemoryPoint point);
void memoryDiagnosticsTick();
void scheduleMemoryMailSettled();
String memoryDiagnosticsJson();
class MemoryProbe {
  MemoryPoint end_;
public:
  MemoryProbe(MemoryPoint begin, MemoryPoint end):end_(end){noteMemory(begin);}
  ~MemoryProbe(){noteMemory(end_);}
  MemoryProbe(const MemoryProbe&)=delete;
  MemoryProbe& operator=(const MemoryProbe&)=delete;
};
