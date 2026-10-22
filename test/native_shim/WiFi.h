// Host stand-in for the WiFi client used by src/stratum.cpp: what the code
// sends is kept in `sent`, the lines it reads come from `toRead`.
#pragma once
#include "Arduino.h"
#include <deque>

class WiFiClient
{
public:
  std::string sent;
  std::deque<std::string> toRead;
  size_t print(const char *s) { sent += s; return strlen(s); }
  size_t print(const String &s) { return print(s.c_str()); }
  String readStringUntil(char)
  {
    if (toRead.empty()) return String();
    std::string l = toRead.front();
    toRead.pop_front();
    return String(l);
  }
  int available() { return (int)toRead.size(); }
  bool connected() { return true; }
  void stop() {}
};
