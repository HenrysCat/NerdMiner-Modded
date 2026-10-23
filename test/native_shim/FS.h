// In-memory stand-in for the Arduino FS / SPIFFS API (pio test -e native),
// with the faults the config save has to survive: a short write (full
// filesystem) and a power cut at a chosen step.
#pragma once
#include <map>
#include <memory>
#include <string>
#include "Arduino.h"

namespace fakefs
{
struct Disk
{
  std::map<std::string, std::string> files;
  long writeBudget = -1;     // bytes that can still be written, -1 = no limit
  int stepsBeforeCut = -1;   // remove/rename calls left before the power goes, -1 = never
  bool powerCut = false;     // set when it went: every later call fails
  bool step()                // false once the power is gone
  {
    if (powerCut)
      return false;
    if (stepsBeforeCut == 0)
    {
      powerCut = true;
      return false;
    }
    if (stepsBeforeCut > 0)
      stepsBeforeCut--;
    return true;
  }
};
inline Disk &disk()
{
  static Disk d;
  return d;
}
} // namespace fakefs

class File
{
public:
  File() {}
  File(const std::string &path, bool write) : path_(path), write_(write), ok_(true)
  {
    if (write)
      fakefs::disk().files[path] = ""; // "w" empties the file at once, like SPIFFS
  }
  explicit operator bool() const { return ok_; }
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t write(const uint8_t *p, size_t n)
  {
    fakefs::Disk &d = fakefs::disk();
    if (!ok_ || !write_ || d.powerCut)
      return 0;
    if (d.writeBudget >= 0 && (long)n > d.writeBudget)
      n = (size_t)d.writeBudget;
    if (d.writeBudget >= 0)
      d.writeBudget -= (long)n;
    d.files[path_].append((const char *)p, n);
    return n;
  }
  int read()
  {
    const std::string &f = fakefs::disk().files[path_];
    return pos_ < f.size() ? (unsigned char)f[pos_++] : -1;
  }
  size_t readBytes(char *buf, size_t n)
  {
    size_t i = 0;
    for (int c; i < n && (c = read()) >= 0; ++i)
      buf[i] = (char)c;
    return i;
  }
  void close() { ok_ = false; }

private:
  std::string path_;
  bool write_ = false;
  bool ok_ = false;
  size_t pos_ = 0;
};

class FakeSPIFFS
{
public:
  bool begin(bool = false) { return true; }
  void end() {}
  bool exists(const char *p) { return fakefs::disk().files.count(p) != 0; }
  File open(const char *p, const char *mode)
  {
    const bool w = mode[0] == 'w';
    if (fakefs::disk().powerCut || (!w && !exists(p)))
      return File();
    return File(p, w);
  }
  bool remove(const char *p)
  {
    if (!fakefs::disk().step())
      return false;
    return fakefs::disk().files.erase(p) != 0;
  }
  bool rename(const char *from, const char *to)
  {
    fakefs::Disk &d = fakefs::disk();
    if (!d.step() || !exists(from) || exists(to)) // SPIFFS won't rename over a file
      return false;
    d.files[to] = d.files[from];
    d.files.erase(from);
    return true;
  }
};
extern FakeSPIFFS SPIFFS;
