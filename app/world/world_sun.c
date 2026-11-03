// world_sun.c — sunrise and sunset from the NOAA solar calculator equations (the spreadsheet version:
// Meeus' low-precision solar position, 0.833° of refraction). Within a minute or two of published
// tables, which is all a time-of-day classifier needs.
#define LOG_TAG "world"
#include "world/world.h"
#include <math.h>

#define RAD(x) ((x) * M_PI / 180.0)
#define DEG(x) ((x) * 180.0 / M_PI)

static double julian_day(int y, int m, int d)   // at 0h UT
{
    int a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
    long jdn = d + (153 * mm + 2) / 5 + 365L * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
    return (double)jdn - 0.5;
}

// Equation of time (minutes) and declination (degrees) for a Julian century t since J2000.
static void solar(double t, double *eqtime, double *decl)
{
    double L0 = fmod(280.46646 + t * (36000.76983 + 0.0003032 * t), 360.0);
    if (L0 < 0) L0 += 360;
    double M = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    double C = sin(RAD(M)) * (1.914602 - t * (0.004817 + 0.000014 * t)) + sin(RAD(2 * M)) * (0.019993 - 0.000101 * t) + sin(RAD(3 * M)) * 0.000289;
    double true_long = L0 + C;
    double omega = 125.04 - 1934.136 * t;
    double lambda = true_long - 0.00569 - 0.00478 * sin(RAD(omega));
    double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    double eps = eps0 + 0.00256 * cos(RAD(omega));
    *decl = DEG(asin(sin(RAD(eps)) * sin(RAD(lambda))));
    double y = tan(RAD(eps) / 2); y *= y;
    double et = y * sin(2 * RAD(L0)) - 2 * e * sin(RAD(M)) + 4 * e * y * sin(RAD(M)) * cos(2 * RAD(L0))
              - 0.5 * y * y * sin(4 * RAD(L0)) - 1.25 * e * e * sin(2 * RAD(M));
    *eqtime = 4 * DEG(et);
}

// Hour angle of sunrise (degrees), or NAN when the sun does not cross the horizon that day.
static double hour_angle(double lat, double decl)
{
    double x = cos(RAD(90.833)) / (cos(RAD(lat)) * cos(RAD(decl))) - tan(RAD(lat)) * tan(RAD(decl));
    if (x > 1 || x < -1) return NAN;
    return DEG(acos(x));
}

// One event in minutes UTC from 0h UT of the day; two passes (noon, then the event itself) as NOAA does.
static double event_utc(double jd, double lat, double lon, bool rise)
{
    double t = (jd - 2451545.0) / 36525.0, eq, decl;
    solar(t, &eq, &decl);
    double ha = hour_angle(lat, decl);
    if (isnan(ha)) return NAN;
    double minutes = 720 - 4 * (lon + (rise ? ha : -ha)) - eq;
    t = (jd + minutes / 1440.0 - 2451545.0) / 36525.0;
    solar(t, &eq, &decl);
    ha = hour_angle(lat, decl);
    if (isnan(ha)) return NAN;
    return 720 - 4 * (lon + (rise ? ha : -ha)) - eq;
}

bool world_sun_times(double lat, double lon, int y, int m, int d, int tz_offset_min, int *rise_min, int *set_min)
{
    double jd = julian_day(y, m, d);
    double r = event_utc(jd, lat, lon, true), s = event_utc(jd, lat, lon, false);
    if (isnan(r) || isnan(s)) { *rise_min = *set_min = -1; return false; }
    int rl = (int)lround(r) + tz_offset_min, sl = (int)lround(s) + tz_offset_min;
    *rise_min = ((rl % 1440) + 1440) % 1440;
    *set_min = ((sl % 1440) + 1440) % 1440;
    return true;
}

// Sunrise: half an hour before to forty minutes after the sun comes up; dusk: forty minutes before to
// half an hour after it goes down. The scene palettes (backdrop pipeline) are keyed on these four.
world_tod_t world_tod_classify(int now_min, int rise_min, int set_min)
{
    if (rise_min < 0 || set_min < 0) return WORLD_TOD_NIGHT;
    if (now_min >= rise_min - 30 && now_min < rise_min + 40) return WORLD_TOD_SUNRISE;
    if (now_min >= rise_min + 40 && now_min < set_min - 40) return WORLD_TOD_DAY;
    if (now_min >= set_min - 40 && now_min < set_min + 30) return WORLD_TOD_DUSK;
    return WORLD_TOD_NIGHT;
}
