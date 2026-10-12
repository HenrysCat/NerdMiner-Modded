#include "sunTimes.h"

#include <math.h>

// Sunrise and sunset from the NOAA general solar position equations, good to
// a minute or two -- plenty for fading a backlight.

static const double kPi = 3.14159265358979323846;
static const double kRad = kPi / 180.0;
static const double kDeg = 180.0 / kPi;
static const double kSunriseZenithDeg = 90.833; // sun's upper edge on the horizon, with refraction

static double wrapMinutes(double minutes)
{
  while (minutes < 0.0) minutes += 1440.0;
  while (minutes >= 1440.0) minutes -= 1440.0;
  return minutes;
}

// Shortest signed distance from b to a in minutes, in [-720, 720).
// Positive means a is past b.
static double signedMinuteDiff(double a, double b)
{
  double diff = a - b;
  while (diff < -720.0) diff += 1440.0;
  while (diff >= 720.0) diff -= 1440.0;
  return diff;
}

float sunNightFraction(time_t utc, double latitude, double longitude, int fadeMinutes,
                       int *sunriseUtcMinute, int *sunsetUtcMinute)
{
  if (sunriseUtcMinute) *sunriseUtcMinute = -1;
  if (sunsetUtcMinute) *sunsetUtcMinute = -1;

  struct tm t;
  gmtime_r(&utc, &t);

  // At the poles themselves cos(latitude) is zero
  if (latitude > 89.9) latitude = 89.9;
  if (latitude < -89.9) latitude = -89.9;

  const double gamma = (2.0 * kPi / 365.0) * t.tm_yday; // fractional year at 12:00 UTC
  const double eqTime = 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma) -
                                  0.014615 * cos(2.0 * gamma) - 0.040849 * sin(2.0 * gamma));
  const double decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma) -
                      0.006758 * cos(2.0 * gamma) + 0.000907 * sin(2.0 * gamma) -
                      0.002697 * cos(3.0 * gamma) + 0.00148 * sin(3.0 * gamma);
  const double latRad = latitude * kRad;
  const double solarNoon = 720.0 - 4.0 * longitude - eqTime;

  const double hourAngleArg =
      cos(kSunriseZenithDeg * kRad) / (cos(latRad) * cos(decl)) - tan(latRad) * tan(decl);
  if (hourAngleArg <= -1.0)
    return 0.0f; // polar day
  if (hourAngleArg >= 1.0)
    return 1.0f; // polar night

  const double hourAngleDeg = acos(hourAngleArg) * kDeg;
  const double sunrise = wrapMinutes(solarNoon - 4.0 * hourAngleDeg);
  const double sunset = wrapMinutes(solarNoon + 4.0 * hourAngleDeg);
  if (sunriseUtcMinute) *sunriseUtcMinute = (int)(sunrise + 0.5) % 1440;
  if (sunsetUtcMinute) *sunsetUtcMinute = (int)(sunset + 0.5) % 1440;

  const double now = t.tm_hour * 60.0 + t.tm_min + t.tm_sec / 60.0;
  // Centred on the crossing, so the fade begins before the sun is down and
  // finishes after it, the way the light outside goes.
  const double half = fadeMinutes > 0 ? fadeMinutes / 2.0 : 0.5;

  const double pastSunset = signedMinuteDiff(now, sunset);
  if (fabs(pastSunset) <= half)
    return (float)(0.5 + pastSunset / (2.0 * half));
  const double pastSunrise = signedMinuteDiff(now, sunrise);
  if (fabs(pastSunrise) <= half)
    return (float)(0.5 - pastSunrise / (2.0 * half));
  // Where a long window and a short summer night make the two fades overlap,
  // the sunset arm above wins and the value never reaches full dark.

  // Outside the fades: daylight runs from sunrise for the length of the day
  // (which may cross midnight UTC).
  return wrapMinutes(now - sunrise) < wrapMinutes(sunset - sunrise) ? 0.0f : 1.0f;
}
