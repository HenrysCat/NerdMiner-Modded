// Minimal Arduino API for the host unit tests (pio test -e native): just what
// src/stratum.cpp, src/utils.cpp and src/ShaTests/nerdSHA256plus.cpp use.
// Not a full Arduino core.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <string>
#include <cstdarg>

#define IRAM_ATTR
#define PROGMEM
#define DRAM_ATTR
#define F(s) (s)
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
typedef uint8_t byte;

#define portTICK_PERIOD_MS 1
inline void vTaskDelay(uint32_t) {}
inline void delay(uint32_t) {}
unsigned long millis();
template <class T, class L, class H>
inline T constrain(T x, L lo, H hi) { return x < lo ? lo : (x > hi ? hi : x); }

class String
{
public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  String(char c) : s_(1, c) {}
  explicit String(int v, int base = 10) { num((long long)v, base); }
  explicit String(unsigned v, int base = 10) { num((unsigned long long)v, base); }
  explicit String(long v, int base = 10) { num((long long)v, base); }
  explicit String(unsigned long v, int base = 10) { num((unsigned long long)v, base); }
  explicit String(long long v, int base = 10) { num(v, base); }
  explicit String(unsigned long long v, int base = 10) { num(v, base); }
  explicit String(double v, unsigned decimals = 2)
  {
    char b[64];
    snprintf(b, sizeof(b), "%.*f", (int)decimals, v);
    s_ = b;
  }

  const char *c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); } // size_t: ArduinoJson takes it as a string type
  bool isEmpty() const { return s_.empty(); }
  void reserve(unsigned n) { s_.reserve(n); }
  char operator[](unsigned i) const { return i < s_.size() ? s_[i] : 0; }
  char &operator[](unsigned i) { return s_[i]; }
  String substring(unsigned from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
  String substring(unsigned from, unsigned to) const
  {
    if (from > to) { unsigned t = from; from = to; to = t; }
    if (from >= s_.size()) return String();
    return String(s_.substr(from, to - from));
  }
  void trim()
  {
    size_t a = 0, b = s_.size();
    while (a < b && isspace((unsigned char)s_[a])) a++;
    while (b > a && isspace((unsigned char)s_[b - 1])) b--;
    s_ = s_.substr(a, b - a);
  }
  int indexOf(const char *x) const { size_t p = s_.find(x); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(char c) const { size_t p = s_.find(c); return p == std::string::npos ? -1 : (int)p; }
  bool endsWith(const char *x) const { size_t n = strlen(x); return s_.size() >= n && s_.compare(s_.size() - n, n, x) == 0; }
  bool startsWith(const char *x) const { return s_.compare(0, strlen(x), x) == 0; }
  void remove(unsigned index) { if (index < s_.size()) s_.erase(index); }
  void remove(unsigned index, unsigned count) { if (index < s_.size()) s_.erase(index, count); }
  long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
  bool equalsIgnoreCase(const String &o) const { return strcasecmp(s_.c_str(), o.s_.c_str()) == 0; }
  bool concat(const char *s) { s_ += s ? s : ""; return true; }
  bool concat(const String &s) { s_ += s.s_; return true; }
  bool concat(char c) { s_ += c; return true; }

  String &operator+=(const String &o) { s_ += o.s_; return *this; }
  String &operator+=(const char *o) { s_ += o ? o : ""; return *this; }
  String &operator+=(char c) { s_ += c; return *this; }
  friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
  friend String operator+(const String &a, const char *b) { return String(a.s_ + (b ? b : "")); }
  friend String operator+(const char *a, const String &b) { return String(std::string(a ? a : "") + b.s_); }
  bool operator==(const String &o) const { return s_ == o.s_; }
  bool operator==(const char *o) const { return s_ == (o ? o : ""); }
  bool operator!=(const String &o) const { return s_ != o.s_; }
  bool operator!=(const char *o) const { return !(*this == o); }

private:
  void num(long long v, int base)
  {
    if (v < 0) { num((unsigned long long)(-v), base); s_ = "-" + s_; return; }
    num((unsigned long long)v, base);
  }
  void num(unsigned long long v, int base)
  {
    char b[72];
    int i = 71;
    b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    s_ = b + i;
  }
  std::string s_;
};

// Serial: quiet by default; NERD_TEST_VERBOSE=1 in the environment prints it.
class HostSerial
{
public:
  bool on() const { static int v = getenv("NERD_TEST_VERBOSE") ? 1 : 0; return v; }
  void begin(unsigned long) {}
  // no format check: the firmware's formats are written for the 32-bit ESP32
  int printf(const char *fmt, ...)
  {
    if (!on()) return 0;
    va_list a; va_start(a, fmt); int n = vfprintf(stderr, fmt, a); va_end(a); return n;
  }
  void print(const String &s) { if (on()) fputs(s.c_str(), stderr); }
  void print(const char *s) { if (on()) fputs(s, stderr); }
  void print(char c) { if (on()) fputc(c, stderr); }
  void print(long v) { if (on()) fprintf(stderr, "%ld", v); }
  void print(int v) { print((long)v); }
  void print(unsigned long v) { if (on()) fprintf(stderr, "%lu", v); }
  void print(unsigned v) { print((unsigned long)v); }
  void print(double v, int d = 2) { if (on()) fprintf(stderr, "%.*f", d, v); }
  template <class T> void println(const T &v) { print(v); print('\n'); }
  void println(double v, int d) { print(v, d); print('\n'); }
  void println() { print('\n'); }
};
extern HostSerial Serial;
