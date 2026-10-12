#ifndef SUN_TIMES_H
#define SUN_TIMES_H

#include <time.h>

// How far into the night it is at a place: 0 in daylight, 1 after dark, and
// in between across a window of fadeMinutes centred on sunset and on sunrise
// (a 40 minute fade starts 20 minutes before sunset and ends 20 after).
// Latitude and longitude in decimal degrees, north and east positive.
// sunriseUtcMinute / sunsetUtcMinute (may be NULL) get today's crossings as
// minutes after 00:00 UTC, or -1 where the sun doesn't cross (polar day/night).
float sunNightFraction(time_t utc, double latitude, double longitude, int fadeMinutes,
                       int *sunriseUtcMinute, int *sunsetUtcMinute);

#endif // SUN_TIMES_H
