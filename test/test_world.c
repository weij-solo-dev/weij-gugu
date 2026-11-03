// test_world.c — sun times against published tables, the time-of-day bands, the trip card on and off
// JSON, and the trip phase as the clock walks through a trip.
#include "minitest.h"
#include "world/world.h"
#include "store/store.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"

static void expect_sun(const char *name, double lat, double lon, int y, int m, int d, int tz, int rise_ref, int set_ref)
{
    int r, s;
    CHECK(world_sun_times(lat, lon, y, m, d, tz, &r, &s));
    printf("  %-10s %04d-%02d-%02d  sunrise %02d:%02d (ref %02d:%02d)  sunset %02d:%02d (ref %02d:%02d)\n", name, y, m, d,
           r / 60, r % 60, rise_ref / 60, rise_ref % 60, s / 60, s % 60, set_ref / 60, set_ref % 60);
    CHECK(abs(r - rise_ref) <= 3);
    CHECK(abs(s - set_ref) <= 3);
}

TEST(test_world_sun_times)
{
    // References: api.sunrise-sunset.org (UTC, fetched 2026-11-03), converted to the local zone and rounded.
    expect_sun("Guangzhou", 23.1291, 113.2644, 2026, 11, 3, 480, 6 * 60 + 33, 17 * 60 + 48);
    expect_sun("Beijing", 39.9042, 116.4074, 2026, 6, 21, 480, 4 * 60 + 46, 19 * 60 + 46);
    expect_sun("Cairo", 30.0444, 31.2357, 2026, 11, 5, 120, 6 * 60 + 12, 17 * 60 + 5);
    expect_sun("Oslo", 59.9139, 10.7522, 2026, 12, 21, 60, 9 * 60 + 18, 15 * 60 + 12);
    int r, s;
    CHECK(!world_sun_times(78.22, 15.63, 2026, 12, 21, 60, &r, &s));   // Longyearbyen in December: polar night
    CHECK(r == -1 && s == -1);
}

TEST(test_world_tod_bands)
{
    int rise = 393, set = 1067;   // Guangzhou, 3 Nov
    CHECK_EQ_INT(world_tod_classify(350, rise, set), WORLD_TOD_NIGHT);
    CHECK_EQ_INT(world_tod_classify(370, rise, set), WORLD_TOD_SUNRISE);
    CHECK_EQ_INT(world_tod_classify(432, rise, set), WORLD_TOD_SUNRISE);
    CHECK_EQ_INT(world_tod_classify(433, rise, set), WORLD_TOD_DAY);
    CHECK_EQ_INT(world_tod_classify(1026, rise, set), WORLD_TOD_DAY);
    CHECK_EQ_INT(world_tod_classify(1027, rise, set), WORLD_TOD_DUSK);
    CHECK_EQ_INT(world_tod_classify(1096, rise, set), WORLD_TOD_DUSK);
    CHECK_EQ_INT(world_tod_classify(1097, rise, set), WORLD_TOD_NIGHT);
    CHECK_EQ_INT(world_tod_classify(720, -1, -1), WORLD_TOD_NIGHT);
}

static const char *CARD =
    "{\"kind\":\"deliver\",\"place\":{\"name\":\"北京\",\"country\":\"中国\",\"landmark\":\"天坛\",\"lat\":39.9042,\"lon\":116.4074,\"tz_offset_min\":480},"
    "\"reason\":\"海鸥阿白托我给它住在北京的表哥送一封信。\",\"recipient\":\"喜鹊小花\",\"letter\":\"阿白说海边的虾条很好吃，请表哥来玩。\","
    "\"nights\":2,\"days\":[\"搭高铁车顶出发，傍晚到北京。\",\"去天坛找喜鹊小花，把信送到。\",\"逛胡同，买纪念品，飞回家。\"],"
    "\"facts\":[\"北京有很多弯弯曲曲的小巷子，叫胡同。\",\"天坛是一座圆圆的蓝屋顶房子。\",\"北京的冬天会下雪。\",\"糖葫芦是用山楂做的，酸酸甜甜。\",\"长城像一条长长的龙趴在山上。\"],"
    "\"friend\":{\"name\":\"小花\",\"animal\":\"喜鹊\"},\"ground\":\"stone\",\"prev_link\":\"这是第一趟。\",\"hooks\":[\"下次去看小花的表妹\"],\"scene_prompt\":\"Temple of Heaven, round blue-tiled roof, daytime, flat colours, no people\","
    "\"objects\":[\"tree\",\"lantern\",\"bench\"],\"souvenir\":\"一颗小糖葫芦\",\"postcards\":[\"今天我坐在高铁车顶上……\",\"信送到啦！……\",\"我要回家啦……\"]}";

MEM_BIG_BSS static world_trip_t s_card;

TEST(test_world_card_json)
{
    char why[96] = "";
    CHECK(world_trip_from_json(CARD, strlen(CARD), &s_card, why, sizeof why));
    CHECK_EQ_STR(s_card.place.name, "北京");
    CHECK_EQ_INT(s_card.kind, WORLD_KIND_DELIVER);
    CHECK_EQ_INT(s_card.nights, 2);
    CHECK_EQ_INT(s_card.days_count, 3);
    CHECK_EQ_INT(s_card.fact_count, 5);
    CHECK_EQ_INT(s_card.object_count, 3);
    CHECK_EQ_STR(s_card.friend_animal, "喜鹊");
    CHECK_EQ_STR(s_card.ground, "stone");
    CHECK_EQ_STR(s_card.prev_link, "这是第一趟。");
    CHECK_EQ_INT(s_card.hook_count, 1);
    CHECK_EQ_INT(s_card.place.tz_offset_min, 480);
    CHECK(s_card.place.lat > 39.9 && s_card.place.lat < 39.91);
    snprintf(s_card.id, sizeof s_card.id, "t20261103-010203");
    char *txt = world_trip_to_json(&s_card);
    CHECK(txt != NULL);
    MEM_BIG_BSS static world_trip_t again;
    CHECK(world_trip_from_json(txt, strlen(txt), &again, why, sizeof why));
    CHECK_EQ_STR(again.id, "t20261103-010203");
    CHECK_EQ_STR(again.ground, "stone");
    CHECK_EQ_STR(again.hooks[0], "下次去看小花的表妹");
    CHECK_EQ_STR(again.facts[4], s_card.facts[4]);
    CHECK_EQ_STR(again.postcards[1], "信送到啦！……");
    mem_release(txt);
    // rejections name the field
    CHECK(!world_trip_from_json("not json", 8, &again, why, sizeof why)); CHECK(strstr(why, "JSON") != NULL);
    const char *no_lat = "{\"kind\":\"visit\",\"place\":{\"name\":\"x\",\"tz_offset_min\":0},\"reason\":\"r\",\"nights\":1,\"facts\":[\"a\",\"b\",\"c\"]}";
    CHECK(!world_trip_from_json(no_lat, strlen(no_lat), &again, why, sizeof why)); CHECK(strstr(why, "lat") != NULL);
    const char *few = "{\"kind\":\"visit\",\"place\":{\"name\":\"x\",\"lat\":1,\"lon\":2,\"tz_offset_min\":0},\"reason\":\"r\",\"nights\":1,\"facts\":[\"a\"]}";
    CHECK(!world_trip_from_json(few, strlen(few), &again, why, sizeof why)); CHECK(strstr(why, "facts") != NULL);
    const char *long_trip = "{\"kind\":\"visit\",\"place\":{\"name\":\"x\",\"lat\":1,\"lon\":2,\"tz_offset_min\":0},\"reason\":\"r\",\"nights\":5,\"facts\":[\"a\",\"b\",\"c\"]}";
    CHECK(!world_trip_from_json(long_trip, strlen(long_trip), &again, why, sizeof why)); CHECK(strstr(why, "nights") != NULL);
}

static time_t at(const char *ymd, int h, int min)
{
    struct tm tm = { 0 }; int y, m, d; sscanf(ymd, "%d-%d-%d", &y, &m, &d);
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = min; tm.tm_isdst = -1;
    return mktime(&tm);
}

TEST(test_world_trip_phase)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");   // TZ = CST-8 from here on
    world_init(TEST_DATA);
    world_trip_clear();
    world_state_t st; world_state(&st);
    CHECK_EQ_INT(st.phase, WORLD_TRIP_NONE); CHECK(!st.away); CHECK_EQ_STR(st.place, "广州");
    char why[96];
    CHECK(world_trip_from_json(CARD, strlen(CARD), &s_card, why, sizeof why));
    snprintf(s_card.id, sizeof s_card.id, "t-test"); snprintf(s_card.depart_date, 11, "2026-11-10"); snprintf(s_card.return_date, 11, "2026-11-12");
    CHECK(world_trip_set(&s_card) == ESP_OK);
    char path[300]; snprintf(path, sizeof path, "%s/trips/current.json", TEST_DATA); CHECK(file_exists(path));
    snprintf(path, sizeof path, "%s/trips/t-test.json", TEST_DATA); CHECK(file_exists(path));
    world_clock_set(at("2026-11-09", 20, 0)); world_state(&st);
    CHECK_EQ_INT(st.phase, WORLD_TRIP_PLANNED); CHECK_EQ_INT(st.day_index, -1); CHECK_EQ_INT(st.nights_left, 2); CHECK(!st.away); CHECK_EQ_INT(st.tod, WORLD_TOD_NIGHT);
    world_clock_set(at("2026-11-10", 7, 59)); world_state(&st); CHECK_EQ_INT(st.phase, WORLD_TRIP_PLANNED);
    world_clock_set(at("2026-11-10", 8, 0)); world_state(&st);
    CHECK_EQ_INT(st.phase, WORLD_TRIP_AWAY); CHECK(st.away); CHECK_EQ_INT(st.day_index, 0); CHECK_EQ_INT(st.nights_left, 2); CHECK_EQ_STR(st.place, "北京");
    CHECK_EQ_INT(st.local_hour, 8); CHECK_EQ_INT(st.tod, WORLD_TOD_DAY);
    printf("  Beijing 10 Nov: sun %02d:%02d-%02d:%02d\n", st.sunrise_min / 60, st.sunrise_min % 60, st.sunset_min / 60, st.sunset_min % 60);
    world_clock_set(at("2026-11-11", 18, 0)); world_state(&st);
    CHECK_EQ_INT(st.day_index, 1); CHECK_EQ_INT(st.nights_left, 1); CHECK_EQ_INT(st.tod, WORLD_TOD_NIGHT);
    world_clock_set(at("2026-11-11", 16, 50)); world_state(&st); CHECK_EQ_INT(st.tod, WORLD_TOD_DUSK);
    world_clock_set(at("2026-11-12", 16, 59)); world_state(&st); CHECK_EQ_INT(st.phase, WORLD_TRIP_AWAY); CHECK_EQ_INT(st.day_index, 2); CHECK_EQ_INT(st.nights_left, 0);
    world_clock_set(at("2026-11-12", 17, 0)); world_state(&st); CHECK_EQ_INT(st.phase, WORLD_TRIP_DONE); CHECK(!st.away); CHECK_EQ_STR(st.place, "广州");
    // a card far away shifts the clock of the scene: Cairo at 15:00 Beijing time is 09:00
    snprintf(s_card.place.name, sizeof s_card.place.name, "开罗"); s_card.place.lat = 30.0444; s_card.place.lon = 31.2357; s_card.place.tz_offset_min = 120;
    CHECK(world_trip_set(&s_card) == ESP_OK);
    world_clock_set(at("2026-11-11", 15, 0)); world_state(&st);
    CHECK(st.away); CHECK_EQ_INT(st.local_hour, 9); CHECK_EQ_INT(st.tod, WORLD_TOD_DAY);
    world_clock_set(at("2026-11-11", 23, 30)); world_state(&st); CHECK_EQ_INT(st.local_hour, 17); CHECK_EQ_INT(st.tod, WORLD_TOD_DUSK);
    // the index remembers both cards, newest first
    char recent[128]; world_recent_places(recent, sizeof recent);
    CHECK(strstr(recent, "开罗") == recent); CHECK(strstr(recent, "北京") != NULL); CHECK(strstr(recent, "（中国）") != NULL);
    world_clock_reset();
    CHECK(world_trip_clear() == ESP_OK);
    snprintf(path, sizeof path, "%s/trips/current.json", TEST_DATA); CHECK(!file_exists(path));
    world_state(&st); CHECK_EQ_INT(st.phase, WORLD_TRIP_NONE);
    // dates
    char d[11]; world_date_add("2026-11-30", 1, d, sizeof d); CHECK_EQ_STR(d, "2026-12-01");
    world_date_add("2026-01-01", -1, d, sizeof d); CHECK_EQ_STR(d, "2025-12-31");
    CHECK_EQ_INT(world_date_diff("2026-11-03", "2026-11-10"), 7);
    CHECK_EQ_INT(world_date_diff("2026-11-10", "2026-11-03"), -7);
    CHECK_EQ_INT(world_clock_offset(), 0);
}

TEST(test_world_recap_and_forget)
{
    char why[96];
    CHECK(world_trip_from_json(CARD, strlen(CARD), &s_card, why, sizeof why));
    snprintf(s_card.id, sizeof s_card.id, "t-recap"); snprintf(s_card.depart_date, 11, "2026-11-10"); snprintf(s_card.return_date, 11, "2026-11-12");
    CHECK(world_trip_set(&s_card) == ESP_OK);
    char hooks[2][160] = { "春天再去看玉兰花", "" };
    CHECK(world_trip_set_recap("这趟去北京给小花送了信。", hooks, 1) == ESP_OK);
    const world_trip_t *t = world_trip_lock();
    CHECK(t && !strcmp(t->recap, "这趟去北京给小花送了信。") && t->hook_count == 1 && !strcmp(t->hooks[0], "春天再去看玉兰花"));
    if (t) world_trip_unlock();
    // the recap survives a reload of the files
    world_init(TEST_DATA);
    t = world_trip_lock(); CHECK(t && strstr(t->recap, "小花") != NULL); if (t) world_trip_unlock();
    char path[300]; snprintf(path, sizeof path, "%s/trips/t-recap.json", TEST_DATA); CHECK(file_exists(path));
    CHECK(world_trip_forget("t-recap") == ESP_OK);          // the current trip: archive gone, card cleared
    CHECK(!file_exists(path));
    world_state_t st; world_state(&st); CHECK_EQ_INT(st.phase, WORLD_TRIP_NONE);
    CHECK(world_trip_forget("../etc") == ESP_ERR_INVALID_ARG);
}

void test_world(void)
{
    RUN(test_world_sun_times);
    RUN(test_world_tod_bands);
    RUN(test_world_card_json);
    RUN(test_world_trip_phase);
    RUN(test_world_recap_and_forget);
}
