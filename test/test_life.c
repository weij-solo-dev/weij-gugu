// test_life.c — the planner and the journey against the scripted vendor: a card with chatter around the
// JSON becomes a dated, saved trip the call prompt cites; the diary and the recap write the journey; the
// next card is asked with the journey in hand; the parent can make Gugu forget a trip.
#include "minitest.h"
#include "life/life.h"
#include "world/world.h"
#include "mind/mind.h"
#include "store/store.h"
#include "provider/provider.h"
#include "foundation/atomic_file.h"
#include "art/art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"
void provider_fake_script(const char *asr_text, const char *chat_reply);   // provider_fake.c
void provider_fake_script_queue(const char *chat_reply);
void provider_fake_script_clear(void);
void provider_fake_image_queue(const char *path);
void provider_fake_image_clear(void);

static const char *CARD1 =
    "好的，这是我的趟卡：\n```json\n"
    "{\"kind\":\"deliver\",\"place\":{\"name\":\"青岛\",\"country\":\"中国\",\"landmark\":\"栈桥\",\"lat\":36.0671,\"lon\":120.3826,\"tz_offset_min\":480},"
    "\"reason\":\"奶奶的老邻居托我给青岛的海鸥阿白送一封信。\",\"prev_link\":\"这是第一趟。\",\"hooks\":[\"阿白请我下次去北京看它的表哥喜鹊小花\"],"
    "\"recipient\":\"海鸥阿白\",\"letter\":\"问阿白今年的虾条还香不香。\","
    "\"nights\":1,\"days\":[\"搭高铁车顶去青岛，傍晚到栈桥。\",\"把信交给阿白，吹吹海风，飞回家。\"],"
    "\"facts\":[\"青岛在海边，海水是蓝绿色的。\",\"栈桥是一条伸进海里的长长的桥。\",\"海鸥喜欢跟着船飞。\",\"青岛的房子很多是红屋顶。\"],"
    "\"friend\":{\"name\":\"阿白\",\"animal\":\"海鸥\"},\"ground\":\"sand\",\"scene_prompt\":\"Zhanqiao pier, daytime, flat colours, no people\","
    "\"objects\":[\"seagull\",\"boat\",\"bench\"],\"souvenir\":\"一枚小贝壳\",\"postcards\":[\"我到青岛啦，海风咸咸的。\",\"信送到啦，阿白请我吃了虾条。\",\"我要回家啦。\"]}"
    "\n```\n祝我一路顺风！";
static const char *RECAP1 = "{\"recap\":\"我去青岛给海鸥阿白送了奶奶老邻居的信。第一天海风咸咸的，第二天阿白请我吃虾条，还说下次带我去北京看它的表哥。我带回了一枚小贝壳。\",\"open_hooks\":[\"去北京看阿白的表哥喜鹊小花\"]}";
static const char *CARD2 =
    "{\"kind\":\"visit\",\"place\":{\"name\":\"北京\",\"country\":\"中国\",\"landmark\":\"天坛\",\"lat\":39.9042,\"lon\":116.4074,\"tz_offset_min\":480},"
    "\"reason\":\"去北京看阿白的表哥喜鹊小花。\",\"prev_link\":\"上次在青岛，阿白邀请我去看它的表哥小花，这趟就去北京。\",\"hooks\":[\"小花说春天带我去看玉兰花\"],"
    "\"recipient\":\"喜鹊小花\",\"letter\":\"\",\"nights\":2,\"days\":[\"搭高铁车顶去北京。\",\"去天坛找小花。\",\"飞回家。\"],"
    "\"facts\":[\"天坛是一座圆圆的蓝屋顶房子。\",\"北京的胡同弯弯曲曲。\",\"糖葫芦酸酸甜甜。\"],"
    "\"friend\":{\"name\":\"小花\",\"animal\":\"喜鹊\"},\"ground\":\"stone\",\"scene_prompt\":\"Temple of Heaven, daytime, flat colours, no people\","
    "\"objects\":[\"tree\",\"lantern\"],\"souvenir\":\"一颗小糖葫芦\",\"postcards\":[\"到北京啦。\",\"见到小花啦。\",\"回家啦。\"]}";

static time_t at(const char *ymd, int h, int min)
{
    struct tm tm = { 0 }; int y, m, d; sscanf(ymd, "%d-%d-%d", &y, &m, &d);
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = min; tm.tm_isdst = -1;
    return mktime(&tm);
}

static void fresh(void)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    world_init(TEST_DATA);
    mind_config_t cfg = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满", .child_age_months = 42, .family_langs = { LANG_ZH, LANG_YUE, LANG_EN } };
    mind_init(&cfg);
    provider_init(GUGU_REPO_ROOT "/content/providers");
    provider_assign(PROVIDER_ROLE_CHAT, "fake");
    provider_fake_script_clear();
    char p[300]; snprintf(p, sizeof p, "%s/memory/journey.jsonl", TEST_DATA); remove(p);
    world_trip_clear();
    world_clock_reset();
    mind_dialog_reset();
}

TEST(test_life_plan_trip_fake)
{
    fresh();
    provider_fake_script(NULL, CARD1);
    char why[200];
    life_plan_t p = { .kind_hint = "deliver", .nights = 2 };
    CHECK(life_plan_trip(&p, why, sizeof why) == ESP_OK);
    printf("  planned: %s\n", why);
    char today[11], tomorrow[11]; world_today(today, sizeof today); world_date_add(today, 1, tomorrow, sizeof tomorrow);
    const world_trip_t *t = world_trip_lock();
    CHECK(t != NULL);
    if (t) {
        CHECK_EQ_STR(t->place.name, "青岛");
        CHECK_EQ_INT(t->nights, 2);                        // the caller's nights win over the model's 1
        CHECK_EQ_STR(t->depart_date, tomorrow);
        CHECK_EQ_INT(world_date_diff(t->depart_date, t->return_date), 2);
        CHECK(t->id[0] == 't' && strlen(t->id) == 16);
        CHECK(t->generated_at[0] != '\0');
        CHECK_EQ_INT(t->days_count, 3);                    // padded to nights + 1
        CHECK_EQ_STR(t->prev_link, "这是第一趟。");
        CHECK_EQ_INT(t->hook_count, 1);
        CHECK(mind_journey_has(JOURNEY_PLANNED, t->id, NULL));
        world_trip_unlock();
    }
    world_state_t st; world_state(&st);
    CHECK_EQ_INT(st.phase, WORLD_TRIP_PLANNED);
    char path[300]; snprintf(path, sizeof path, "%s/trips/current.json", TEST_DATA); CHECK(file_exists(path));

    // the call prompt cites the card and nothing else
    mind_prompt_t pr;
    CHECK(mind_prompt_build(&pr, LANG_ZH, NULL, NULL) == ESP_OK);
    CHECK(strstr(pr.blocks[2].text, "青岛") != NULL);
    CHECK(strstr(pr.blocks[2].text, "只能讲这些") != NULL);
    CHECK(strstr(pr.blocks[2].text, "栈桥是一条伸进海里的长长的桥") != NULL);
    CHECK(strstr(pr.blocks[2].text, "明天要出发") != NULL);
    CHECK(strstr(pr.blocks[2].text, "你记着的约定：阿白请我") != NULL);
    CHECK(strstr(pr.blocks[2].text, "【旅程志】") != NULL);
    CHECK(strstr(pr.blocks[2].text, "咕，我不记得了") != NULL);
    CHECK(strstr(pr.blocks[3].text, "在家里") != NULL);
    // ... and once the clock is inside the trip, block C puts Gugu there on the walkie-talkie
    world_clock_shift(36 * 3600);
    CHECK(mind_prompt_build(&pr, LANG_ZH, NULL, NULL) == ESP_OK);
    CHECK(strstr(pr.blocks[2].text, "你正在青岛") != NULL);
    CHECK(strstr(pr.blocks[3].text, "对讲机") != NULL);
    CHECK(strstr(pr.blocks[3].text, "在青岛") != NULL);
    world_clock_reset();

    // the planner's own request carries the rules, the schema, the hints and the journey
    mind_trip_request_t req;
    CHECK(mind_trip_request_build(&req, "vacation", "海南", "2026-12-01", 3, "青岛", "【旅程志】\n- 上次去了青岛。\n") == ESP_OK);
    CHECK_EQ_INT(req.count, 2);
    CHECK(strstr(req.blocks[1].text, "tz_offset_min") != NULL);
    CHECK(strstr(req.blocks[1].text, "prev_link") != NULL);
    CHECK(strstr(req.blocks[1].text, "上次去了青岛") != NULL);
    CHECK(strstr(req.blocks[1].text, "{home_city}") == NULL);
    CHECK(strstr(req.user, "海南") != NULL && strstr(req.user, "2026-12-01") != NULL && strstr(req.user, "住 3 晚") != NULL && strstr(req.user, "青岛") != NULL);

    // a reply without a card is rejected and the saved card stays
    provider_fake_script(NULL, "咕，我今天不想出门。");
    CHECK(life_plan_trip(&p, why, sizeof why) != ESP_OK);
    printf("  rejected: %s\n", why);
    CHECK(strstr(why, "JSON") != NULL);
    provider_fake_script(NULL, "{\"kind\":\"visit\",\"place\":{\"name\":\"x\"},\"reason\":\"r\",\"nights\":1,\"facts\":[\"a\",\"b\",\"c\"]}");
    CHECK(life_plan_trip(&p, why, sizeof why) != ESP_OK);
    printf("  rejected: %s\n", why);
    CHECK(strstr(why, "lat") != NULL);
    t = world_trip_lock(); CHECK(t && !strcmp(t->place.name, "青岛")); if (t) world_trip_unlock();

    // the tick: a pending trip means nothing to plan, and the date is remembered
    store_setting_set_str("life_tick_date", "");
    CHECK(life_tick() == ESP_OK);
    CHECK_EQ_STR(store_setting_str("life_tick_date", ""), today);
    world_trip_clear();
}

TEST(test_life_story_chain)
{
    fresh();
    char why[200], id1[40], depart[11], ret[11], next[11];
    // 1. the first trip
    provider_fake_script_queue(CARD1);
    life_plan_t p = { .kind_hint = "deliver" };
    CHECK(life_plan_trip(&p, why, sizeof why) == ESP_OK);
    const world_trip_t *t = world_trip_lock();
    CHECK(t != NULL); if (!t) return;
    snprintf(id1, sizeof id1, "%s", t->id); snprintf(depart, sizeof depart, "%s", t->depart_date); snprintf(ret, sizeof ret, "%s", t->return_date);
    CHECK_EQ_INT(t->nights, 1);
    world_trip_unlock();
    // 2. the first day away: the diary takes the postcard
    world_clock_set(at(depart, 10, 0));
    CHECK(life_diary_now(why, sizeof why) == ESP_OK); printf("  diary day 1: %s\n", why);
    CHECK(strstr(why, "1 day entry") != NULL);
    CHECK(mind_journey_has(JOURNEY_DAY, id1, depart));
    static char buf[4096];
    mind_journey_recent(buf, sizeof buf, 3);
    CHECK(strstr(buf, "这趟第 1 天") != NULL && strstr(buf, "海风咸咸的") != NULL);
    // 3. the next day: one more entry, and a second run writes nothing
    world_clock_set(at(ret, 10, 0));
    CHECK(life_diary_now(why, sizeof why) == ESP_OK); CHECK(strstr(why, "1 day entry") != NULL);
    CHECK(life_diary_now(why, sizeof why) == ESP_OK); CHECK(strstr(why, "0 day entries") != NULL);
    // the recap refuses while Gugu is still away
    CHECK(life_recap_now(why, sizeof why) == ESP_OK); CHECK(strstr(why, "not back yet") != NULL);
    // 4. home: the recap, from the model, lands on the card and in the journey with its open hook
    world_clock_set(at(ret, 18, 0));
    provider_fake_script_queue(RECAP1);
    CHECK(life_recap_now(why, sizeof why) == ESP_OK); printf("  recap: %s\n", why);
    CHECK(strstr(why, "1 open hook") != NULL);
    t = world_trip_lock(); CHECK(t && strstr(t->recap, "虾条") != NULL && t->hook_count == 1 && strstr(t->hooks[0], "小花") != NULL); if (t) world_trip_unlock();
    CHECK(mind_journey_has(JOURNEY_RETURNED, id1, NULL));
    mind_journey_recent(buf, sizeof buf, 3);
    CHECK(strstr(buf, "去青岛送信：我去青岛") != NULL);
    CHECK(strstr(buf, "还没兑现的线头：去北京看阿白的表哥") != NULL);
    CHECK(life_recap_now(why, sizeof why) == ESP_OK); CHECK(strstr(why, "already written") != NULL);
    // 5. the planner's request carries the journey
    mind_trip_request_t req;
    CHECK(mind_trip_request_build(&req, NULL, NULL, "2026-12-01", 0, "青岛", buf) == ESP_OK);
    CHECK(strstr(req.blocks[1].text, "表哥") != NULL);
    // 6. the tick the day after the return: diary and recap are done, the next trip is planned on the journey
    world_date_add(ret, 1, next, sizeof next);
    world_clock_set(at(next, 3, 5));
    store_setting_set_str("life_tick_date", "");
    provider_fake_script_queue(CARD2);
    CHECK(life_tick() == ESP_OK);
    t = world_trip_lock(); CHECK(t != NULL);
    if (t) { CHECK_EQ_STR(t->place.name, "北京"); CHECK(strstr(t->prev_link, "阿白") != NULL); CHECK(mind_journey_has(JOURNEY_PLANNED, t->id, NULL)); world_trip_unlock(); }
    world_state_t st; world_state(&st); CHECK_EQ_INT(st.phase, WORLD_TRIP_PLANNED);
    // 7. the call prompt: the journey, the hook, and what the child's words point at
    mind_dialog_reset();
    mind_prompt_t pr;
    CHECK(mind_prompt_build(&pr, LANG_ZH, NULL, NULL) == ESP_OK);
    CHECK(strstr(pr.blocks[2].text, "【旅程志】") != NULL && strstr(pr.blocks[2].text, "去青岛送信") != NULL);
    CHECK(strstr(pr.blocks[2].text, "这趟和上一趟的关系：上次在青岛") != NULL);
    CHECK(strstr(pr.blocks[2].text, "【你想起来的事】") == NULL);
    mind_dialog_add("user", "你上次去青岛见到阿白了吗");
    CHECK(mind_prompt_build(&pr, LANG_ZH, NULL, NULL) == ESP_OK);
    CHECK(strstr(pr.blocks[2].text, "【你想起来的事】") != NULL && strstr(pr.blocks[2].text, "阿白请我吃虾条") != NULL);
    printf("  block B is %u bytes\n", (unsigned)strlen(pr.blocks[2].text));
    // 8. the parent makes Gugu forget the first trip
    CHECK(mind_journey_forget(id1) == ESP_OK);
    CHECK(!mind_journey_has(JOURNEY_RETURNED, id1, NULL));
    CHECK(world_trip_forget(id1) == ESP_OK);
    char path[300]; snprintf(path, sizeof path, "%s/trips/%s.json", TEST_DATA, id1); CHECK(!file_exists(path));
    t = world_trip_lock(); CHECK(t && !strcmp(t->place.name, "北京")); if (t) world_trip_unlock();   // the current trip is untouched
    mind_dialog_reset();
    world_trip_clear();
    world_clock_reset();
    provider_fake_script_clear();
    provider_fake_script(NULL, "[happy] 哇，看到鸭子啦！[curious] 鸭子在游泳吗？[coo] 我最喜欢看鸭子了。");   // leave the fake as other tests expect it
}

TEST(test_life_art_fake)
{
    fresh();
    provider_assign(PROVIDER_ROLE_IMAGE, "fake");
    provider_fake_image_clear();
    provider_fake_script_queue(CARD1);                      // objects: seagull, boat, bench; friend: 海鸥
    life_plan_t p = { .kind_hint = "deliver" };
    char why[200];
    CHECK(life_plan_trip(&p, why, sizeof why) == ESP_OK);
    char id[40]; const world_trip_t *t = world_trip_lock(); snprintf(id, sizeof id, "%s", t ? t->id : ""); if (t) world_trip_unlock();
    char dir[300]; snprintf(dir, sizeof dir, "%s/art/%s", TEST_DATA, id);
    // backdrop from the Giza fixture; the seagull prop from the lamp picture (a clean object), the boat from the
    // backdrop picture (no flat background: fails the gate), the bench from the camel, the friend from the seagull
    provider_fake_image_queue(GUGU_REPO_ROOT "/test/fixtures/art/backdrop_giza.jpg");
    provider_fake_image_queue(GUGU_REPO_ROOT "/test/fixtures/art/prop_lamp.jpg");
    provider_fake_image_queue(GUGU_REPO_ROOT "/test/fixtures/art/backdrop_giza.jpg");
    provider_fake_image_queue(GUGU_REPO_ROOT "/test/fixtures/art/prop_camel.jpg");
    provider_fake_image_queue(GUGU_REPO_ROOT "/test/fixtures/art/prop_seagull.jpg");
    CHECK(life_art_now(why, sizeof why) == ESP_OK); printf("  art: %s\n", why);
    char path[400];
    snprintf(path, sizeof path, "%s/backdrop.art", dir); CHECK(file_exists(path));
    snprintf(path, sizeof path, "%s/seagull.art", dir); CHECK(file_exists(path));
    snprintf(path, sizeof path, "%s/boat.fail", dir); CHECK(file_exists(path));
    snprintf(path, sizeof path, "%s/bench.art", dir); CHECK(file_exists(path));
    snprintf(path, sizeof path, "%s/backdrop.jpg", dir); CHECK(!file_exists(path));   // the JPEG is gone once imported
    art_image_t img;
    snprintf(path, sizeof path, "%s/backdrop.art", dir);
    CHECK(art_load(path, &img) == ESP_OK); CHECK_EQ_INT(img.w, 116);
    // the sand strip of the card's ground sits under the pigeon
    art_rgb_t c = img.pal[img.idx[(ART_GROUND_TOP + 6) * 116 + 50]]; CHECK(c.r > 200 && c.r > c.b + 40);
    art_free(&img);
    // a second run makes nothing new
    CHECK(life_art_now(why, sizeof why) == ESP_OK); printf("  again: %s\n", why);
    CHECK(strstr(why, "0 made") != NULL && strstr(why, "already there") != NULL);
    // the ledger has a line per picture
    char lp[300]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    snprintf(lp, sizeof lp, "%s/ledger/%04d-%02d.jsonl", TEST_DATA, tm.tm_year + 1900, tm.tm_mon + 1);
    CHECK(file_exists(lp));
    world_trip_clear(); provider_fake_image_clear();
}

static bool s_chk_ok; static int s_chk_kind; static char s_chk_why[120]; static int s_chk_calls;
static void chk_done(bool ok, int kind, const char *why, void *user) { (void)user; s_chk_ok = ok; s_chk_kind = kind; snprintf(s_chk_why, sizeof s_chk_why, "%s", why); s_chk_calls++; }

TEST(test_life_service_check)
{
    fresh();
    s_chk_calls = 0;
    life_check_service(chk_done, NULL);                          // tests run it at once (no worker)
    CHECK_EQ_INT(s_chk_calls, 1); CHECK(s_chk_ok); CHECK(strstr(s_chk_why, "answered") != NULL);
    provider_fake_script_queue("!error 401");
    life_check_service(chk_done, NULL);
    CHECK_EQ_INT(s_chk_calls, 2); CHECK(!s_chk_ok); CHECK_EQ_INT(s_chk_kind, PERR_AUTH); CHECK(strstr(s_chk_why, "http 401") != NULL);
    provider_fake_script_queue("!error 429");
    life_check_service(chk_done, NULL);
    CHECK(!s_chk_ok); CHECK_EQ_INT(s_chk_kind, PERR_QUOTA);
    life_check_service(chk_done, NULL);                          // the queue is used up: answers again
    CHECK(s_chk_ok);
}

void test_life(void)
{
    RUN(test_life_service_check);
    RUN(test_life_plan_trip_fake);
    RUN(test_life_story_chain);
    RUN(test_life_art_fake);
}
