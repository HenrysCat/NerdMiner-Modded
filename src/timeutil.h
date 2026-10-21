#ifndef NERDMINER_TIMEUTIL_H
#define NERDMINER_TIMEUTIL_H

// Small calendar helpers with no dependency on the C library's time zone
// handling (plain integer maths, unit-testable on a PC).

#include <stdint.h>

// Days since 1970-01-01 for a civil date (proleptic Gregorian calendar).
// Algorithm by Howard Hinnant, "chrono-compatible low-level date algorithms".
static inline int64_t tu_daysFromCivil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// Inverse of tu_daysFromCivil.
static inline void tu_civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d)
{
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}

// Floor division of a timestamp into days (works for negative times too).
static inline int64_t tu_daysOf(int64_t t)
{
  return t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
}

// Day of the week, 0 = Sunday (1970-01-01 was a Thursday).
static inline int tu_weekday(int64_t days)
{
  return (int)(((days % 7) + 7 + 4) % 7);
}

// Day number of the last Sunday of a month.
static inline int64_t tu_lastSunday(int y, unsigned m)
{
  const int64_t lastDay = (m == 12 ? tu_daysFromCivil(y + 1, 1, 1) : tu_daysFromCivil(y, m + 1, 1)) - 1;
  return lastDay - tu_weekday(lastDay);
}

// European Union summer time: from the last Sunday of March, 01:00 UTC, to
// the last Sunday of October, 01:00 UTC (the same instant in every EU zone).
static inline bool tu_euSummerTime(int64_t utc)
{
  int y; unsigned m, d;
  tu_civilFromDays(tu_daysOf(utc), y, m, d);
  const int64_t start = tu_lastSunday(y, 3) * 86400 + 3600;
  const int64_t end = tu_lastSunday(y, 10) * 86400 + 3600;
  return utc >= start && utc < end;
}

#endif // NERDMINER_TIMEUTIL_H
