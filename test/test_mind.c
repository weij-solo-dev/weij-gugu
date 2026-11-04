#include "minitest.h"
#include "mind/mind.h"
#include "world/world.h"
#include "store/store.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"

TEST(test_mind_prompt_blocks)
{
    mind_config_t cfg = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满", .child_age_months = 42, .family_langs = { LANG_ZH, LANG_YUE, LANG_EN } };
    CHECK(mind_init(&cfg) == ESP_OK);
    mind_prompt_t p;
    CHECK(mind_prompt_build(&p, LANG_YUE, "在家里，刚被叫到洞口聊天", "小满刚摸了你一下。") == ESP_OK);
    CHECK_EQ_INT(p.count, 4);
    CHECK(strstr(p.blocks[0].text, "小满家窗台") != NULL);          // placeholder filled
    CHECK(strstr(p.blocks[0].text, "{child_name}") == NULL);
    CHECK(strstr(p.blocks[0].text, "[laughing]") != NULL);           // tag vocabulary present
    CHECK(strstr(p.blocks[1].text, "粤语") != NULL && strstr(p.blocks[1].text, "喺屋企") != NULL);
    CHECK(p.blocks[0].cache && p.blocks[1].cache && p.blocks[2].cache && !p.blocks[3].cache);
    CHECK(strstr(p.blocks[3].text, "3 岁半") != NULL);
    CHECK(strstr(p.blocks[3].text, "刚摸了你一下") != NULL);
    mind_prompt_build(&p, LANG_EN, NULL, NULL);
    CHECK(strstr(p.blocks[1].text, "simple English") != NULL);
}

TEST(test_mind_dialog_ring)
{
    mind_dialog_reset();
    for (int i = 0; i < 8; i++) { char b[16]; snprintf(b, 16, "u%d", i); mind_dialog_add("user", b); snprintf(b, 16, "a%d", i); mind_dialog_add("assistant", b); }
    chat_message_t m[16];
    size_t n = mind_dialog_messages(m, 16);
    CHECK_EQ_INT(n, 12);
    CHECK_EQ_STR(m[0].text, "u2"); CHECK_EQ_STR(m[11].text, "a7"); CHECK_EQ_STR(m[11].role, "assistant");
}

TEST(test_mind_language_and_normalize)
{
    CHECK(mind_reply_lang(LANG_YUE, "係呀") == LANG_YUE);
    CHECK(mind_reply_lang(LANG_UNKNOWN, "hello there") == LANG_EN);
    CHECK(mind_reply_lang(LANG_UNKNOWN, "你好") == LANG_ZH);
    CHECK(mind_reply_lang(LANG_UNKNOWN, "…") == LANG_ZH);            // falls back to the last language
    char t[] = "你好呀我是姑姑今天我去啦公园";
    CHECK_EQ_STR(mind_normalize_transcript(t), "你好呀我是咕咕今天我去啦公园");
}

static const char *MCARD =
    "{\"id\":\"t-m1\",\"kind\":\"deliver\",\"place\":{\"name\":\"北京\",\"country\":\"中国\",\"lat\":39.9,\"lon\":116.4,\"tz_offset_min\":480},"
    "\"reason\":\"给小花送信\",\"depart_date\":\"2026-11-01\",\"return_date\":\"2026-11-03\",\"nights\":2,\"facts\":[\"a\",\"b\",\"c\"],\"friend\":{\"name\":\"小花\",\"animal\":\"喜鹊\"}}";
MEM_BIG_BSS static world_trip_t s_mcard;

TEST(test_mind_memory_books)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    mind_config_t cfg = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满", .child_age_months = 42, .family_langs = { LANG_ZH, LANG_YUE, LANG_EN } };
    CHECK(mind_init(&cfg) == ESP_OK);
    char p[300]; snprintf(p, sizeof p, "%s/memory/journey.jsonl", TEST_DATA); remove(p);
    char why[96];
    CHECK(world_trip_from_json(MCARD, strlen(MCARD), &s_mcard, why, sizeof why));
    static char buf[4096];
    CHECK_EQ_INT(mind_journey_recent(buf, sizeof buf, 3), 0);
    CHECK(strstr(buf, "还没出过门") != NULL);
    CHECK(mind_journey_add(JOURNEY_PLANNED, &s_mcard, "2026-10-31", 0, "给小花送信。这是第一趟。", s_mcard.hooks, 0) == ESP_OK);
    CHECK(mind_journey_add(JOURNEY_DAY, &s_mcard, "2026-11-01", 1, "到北京了，见到小花。", NULL, 0) == ESP_OK);
    char hooks[2][160] = { "小花约我春天再来看玉兰花", "" };
    CHECK(mind_journey_add(JOURNEY_RETURNED, &s_mcard, "2026-11-03", 0, "这趟去北京给小花送信，见到了天坛。", hooks, 1) == ESP_OK);
    CHECK_EQ_INT(mind_journey_recent(buf, sizeof buf, 3), 1);
    CHECK(strstr(buf, "11月1日到11月3日去北京送信：这趟去北京") != NULL);
    CHECK(strstr(buf, "还没兑现的线头：小花约我春天再来看玉兰花") != NULL);
    CHECK(strstr(buf, "第 1 天") == NULL);                 // no current card: the day lines belong to a trip in progress only
    CHECK_EQ_INT(mind_journey_days_text("t-m1", buf, sizeof buf), 1);
    CHECK(strstr(buf, "第 1 天（2026-11-01）：到北京了") != NULL);
    CHECK_EQ_INT(mind_journey_match("你还记得小花吗", buf, sizeof buf, 3), 1);
    CHECK(strstr(buf, "【你想起来的事】") != NULL && strstr(buf, "北京") != NULL);
    CHECK_EQ_INT(mind_journey_match("今天天气真好", buf, sizeof buf, 3), 0);
    CHECK(mind_journey_has(JOURNEY_RETURNED, "t-m1", NULL));
    CHECK(mind_journey_has(JOURNEY_DAY, "t-m1", "2026-11-01"));
    CHECK(!mind_journey_has(JOURNEY_DAY, "t-m1", "2026-11-02"));
    // the two lists: empty files exist, entries read as lines, forgetting by index
    CHECK_EQ_INT(mind_book_text("about_child", buf, sizeof buf), 0);
    snprintf(p, sizeof p, "%s/memory/about_child.json", TEST_DATA);
    const char *two = "[{\"text\":\"小满怕打雷\",\"date\":\"2026-11-02\"},{\"text\":\"小满有个好朋友叫豆豆\",\"date\":\"2026-11-02\"}]";
    CHECK(atomic_file_write(p, two, strlen(two)));
    CHECK_EQ_INT(mind_book_text("about_child", buf, sizeof buf), 2);
    CHECK(strstr(buf, "豆豆") != NULL);
    CHECK(mind_book_forget("about_child", 0) == ESP_OK);
    CHECK_EQ_INT(mind_book_text("about_child", buf, sizeof buf), 1);
    CHECK(strstr(buf, "怕打雷") == NULL);
    CHECK(mind_book_forget("about_child", 5) == ESP_ERR_NOT_FOUND);
    atomic_file_write(p, "[]", 2);
    // forgetting a trip removes every entry of it
    CHECK(mind_journey_forget("t-m1") == ESP_OK);
    CHECK(!mind_journey_has(JOURNEY_RETURNED, "t-m1", NULL));
    CHECK_EQ_INT(mind_journey_recent(buf, sizeof buf, 3), 0);
    // UTF-8 safe cuts
    char s[] = "你好世界"; mind_utf8_cut(s, 7); CHECK_EQ_STR(s, "你好");
    char e[] = "hello"; mind_utf8_cut(e, 10); CHECK_EQ_STR(e, "hello");
}

TEST(test_mind_lines_and_language)
{
    mind_config_t cfg = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满", .child_age_months = 42, .family_langs = { LANG_ZH, LANG_YUE, LANG_EN } };
    CHECK(mind_init(&cfg) == ESP_OK);
    char s[400];
    // the self-introduction teaches the keys as they work: hold blue to talk, press yellow to feed (it once promised a whistle)
    CHECK(mind_line_hello(LANG_ZH, s, sizeof s) == ESP_OK); CHECK(strstr(s, "你好呀，小满！我是咕咕") != NULL);
    CHECK(strstr(s, "按住蓝色的按键") != NULL); CHECK(strstr(s, "黄色的按键，就能喂我") != NULL); CHECK(strstr(s, "吹哨") == NULL);
    CHECK(mind_line_hello(LANG_YUE, s, sizeof s) == ESP_OK); CHECK(strstr(s, "你好呀，小满！我係咕咕") != NULL);
    CHECK(strstr(s, "撳住藍色個掣") != NULL); CHECK(strstr(s, "黃色個掣，就可以餵我") != NULL); CHECK(strstr(s, "吹哨") == NULL);
    CHECK(mind_line_hello(LANG_EN, s, sizeof s) == ESP_OK); CHECK(strstr(s, "Hi, 小满! I'm Gugu") != NULL);
    CHECK(strstr(s, "hold the blue button") != NULL); CHECK(strstr(s, "yellow button to feed me") != NULL); CHECK(strstr(s, "whistle") == NULL);
    char a[200], b[200];
    CHECK(mind_line_question(LANG_EN, 0, a, sizeof a) == ESP_OK); CHECK(mind_line_question(LANG_EN, 1, b, sizeof b) == ESP_OK); CHECK(strcmp(a, b) != 0);
    CHECK(mind_line_question(LANG_YUE, 15, a, sizeof a) == ESP_OK); CHECK(mind_line_question(LANG_YUE, 0, b, sizeof b) == ESP_OK); CHECK_EQ_STR(a, b);   // 15 questions: wraps
    MEM_BIG_BSS static char sys[2048];
    CHECK(mind_share_request_build(LANG_EN, "正在开罗", "金字塔是用大石块盖的。", sys, sizeof sys) == ESP_OK);
    CHECK(strstr(sys, "English") != NULL && strstr(sys, "正在开罗") != NULL && strstr(sys, "金字塔是用大石块盖的。") != NULL && strstr(sys, "小满") != NULL);
    char t[] = " [happy] 你知道吗？[wave]金字塔很高！ ";
    mind_strip_tags(t); CHECK_EQ_STR(t, "你知道吗？金字塔很高！");
    mind_set_language(LANG_EN);                                  // one interaction language: Gugu answers in it whatever it hears
    CHECK_EQ_INT(mind_reply_lang(LANG_ZH, "你好"), LANG_EN);
    CHECK_EQ_INT(mind_reply_lang(LANG_UNKNOWN, "你好"), LANG_EN);
    mind_set_language(LANG_YUE);
    CHECK_EQ_INT(mind_reply_lang(LANG_EN, "hello"), LANG_YUE);
    mind_set_asked("你最喜欢什么颜色？", 60000);                 // the question just asked is in block C while fresh
    mind_prompt_t pr; mind_prompt_build(&pr, LANG_ZH, "在家里", NULL);
    CHECK(strstr(pr.blocks[3].text, "你最喜欢什么颜色？") != NULL);
    mind_set_asked("", 0);
    mind_prompt_build(&pr, LANG_ZH, "在家里", NULL);
    CHECK(strstr(pr.blocks[3].text, "你最喜欢什么颜色？") == NULL);
    CHECK(mind_init(&cfg) == ESP_OK);                            // back to the three family languages for the other tests
}

void test_mind(void)
{
    RUN(test_mind_lines_and_language);
    RUN(test_mind_prompt_blocks);
    RUN(test_mind_dialog_ring);
    RUN(test_mind_language_and_normalize);
    RUN(test_mind_memory_books);
}
