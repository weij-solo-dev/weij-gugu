#include "minitest.h"
#include "mind/mind.h"
#include <string.h>

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

void test_mind(void)
{
    RUN(test_mind_prompt_blocks);
    RUN(test_mind_dialog_ring);
    RUN(test_mind_language_and_normalize);
}
