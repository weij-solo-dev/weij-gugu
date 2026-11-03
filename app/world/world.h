// world.h — where Gugu is and what time it is there. The trip card (the JSON document the story engine
// generated for the current trip), the trip phase as a pure function of the card and the clock, sunrise
// and sunset at the pigeon's location, and the time of day that drives the scene. Data and time only:
// the words around them live in mind.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef enum { WORLD_TOD_NIGHT = 0, WORLD_TOD_SUNRISE, WORLD_TOD_DAY, WORLD_TOD_DUSK } world_tod_t;
typedef enum { WORLD_TRIP_NONE = 0, WORLD_TRIP_PLANNED, WORLD_TRIP_AWAY, WORLD_TRIP_DONE } world_phase_t;
typedef enum { WORLD_KIND_DELIVER = 0, WORLD_KIND_VACATION, WORLD_KIND_VISIT } world_kind_t;

typedef struct {
    char name[48];              // "青岛"
    char country[32];
    char landmark[64];          // what the scene shows
    double lat, lon;            // decimal degrees, north and east positive
    int tz_offset_min;          // local civil time = UTC + offset
} world_place_t;

#define WORLD_MAX_NIGHTS  3
#define WORLD_MAX_DAYS    (WORLD_MAX_NIGHTS + 1)
#define WORLD_MAX_FACTS   8
#define WORLD_MAX_OBJECTS 8

// One trip, as the model planned it and life dated it. About 6 KB: keep instances static (MEM_BIG_BSS), never on a stack.
typedef struct {
    char id[40];
    world_kind_t kind;
    world_place_t place;
    char reason[200];
    char recipient[64];
    char letter[200];
    char depart_date[11], return_date[11];   // home-local calendar dates, YYYY-MM-DD
    int nights;                              // 1..WORLD_MAX_NIGHTS
    int days_count;                          // nights + 1 when complete
    char days[WORLD_MAX_DAYS][200];          // the plan for each day, departure day first
    int fact_count;
    char facts[WORLD_MAX_FACTS][200];        // the only real-world facts a call may cite
    char friend_name[32], friend_animal[32];
    char scene_prompt[400];                  // English, for the image role (backdrop pipeline)
    char ground[16];                         // sand | grass | pavement | stone | snow | wood: the strip the firmware draws under Gugu
    int object_count;
    char objects[WORLD_MAX_OBJECTS][24];     // prop-catalogue ids (object layer)
    char souvenir[120];
    char postcards[WORLD_MAX_DAYS][400];
    char prev_link[200];                     // how this trip follows from the last one (or that it does not)
    int hook_count;
    char hooks[2][160];                      // threads left for later trips: an invitation, a promise, something unfinished
    char recap[480];                         // written when Gugu is back (life, trip_recap.md)
    char generated_at[24];
} world_trip_t;

typedef struct {
    world_phase_t phase;
    bool away;                      // phase == AWAY: the scene is the destination
    int day_index;                  // days since the departure date (negative before it)
    int nights_left;                // moons still on the blackboard
    world_tod_t tod;                // at Gugu's location (home unless away)
    int local_hour, local_min;      // civil time there
    int sunrise_min, sunset_min;    // there, minutes after local midnight; -1 when the sun does not rise or set
    char place[48];                 // where Gugu is
    time_t now;                     // the clock the state was computed from
} world_state_t;

esp_err_t world_init(const char *data_dir);        // loads trips/current.json if there is one
void      world_state(world_state_t *out);

// The card. Lock returns NULL when there is none (then do not unlock). Hold it briefly: format and let go.
const world_trip_t *world_trip_lock(void);
void      world_trip_unlock(void);
esp_err_t world_trip_set(const world_trip_t *t);   // trips/current.json + trips/<id>.json (atomic), one line in trips/index.jsonl
esp_err_t world_trip_clear(void);
esp_err_t world_trip_set_recap(const char *recap, const char (*open_hooks)[160], int n);   // on the current card; rewrites the files, no index line
esp_err_t world_trip_forget(const char *id);     // delete the archived card; clears the current one if it is that trip
bool      world_trip_from_json(const char *json, size_t n, world_trip_t *out, char *why, size_t why_cap);
char     *world_trip_to_json(const world_trip_t *t);   // caller mem_release()s
void      world_recent_places(char *out, size_t cap);  // "青岛、北京" from the index, newest first, for the planner
const char  *world_kind_name(world_kind_t k);          // "deliver" | "vacation" | "visit"
world_kind_t world_kind_parse(const char *s);
const char  *world_phase_name(world_phase_t p);
const char  *world_tod_name(world_tod_t t);

void world_home(world_place_t *out);                   // settings home_city / home_lat / home_lon; the zone from TZ

// Sun: minutes after local midnight; false when the sun neither rises nor sets that day.
bool        world_sun_times(double lat, double lon, int y, int m, int d, int tz_offset_min, int *rise_min, int *set_min);
world_tod_t world_tod_classify(int now_min, int rise_min, int set_min);

// The clock: the firmware's time plus an offset the console and the tests set, so a trip can be walked in minutes.
time_t  world_now(void);
void    world_clock_shift(int64_t seconds);
void    world_clock_set(time_t now);
void    world_clock_reset(void);
int64_t world_clock_offset(void);
void    world_today(char *out, size_t cap);                  // home-local YYYY-MM-DD
void    world_date_add(const char *ymd, int days, char *out, size_t cap);
int     world_date_diff(const char *from, const char *to);   // whole days
