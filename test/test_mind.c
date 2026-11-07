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
    // One interaction language. The persona once told the model to follow the child's language as well: a Mandarin
    // Gugu then answered a Cantonese-speaking child half in Cantonese, and the voice read it with Mandarin sounds.
    CHECK(strstr(p.blocks[0].text, "用什么语言，你就用什么语言") == NULL);
    CHECK(strstr(p.blocks[1].text, "香港") != NULL && strstr(p.blocks[1].text, "繁体字") != NULL);   // yue: Hong Kong colloquial, traditional characters
    CHECK(strstr(p.blocks[1].text, "只用粤语回答") != NULL);
    mind_prompt_build(&p, LANG_ZH, NULL, NULL);
    CHECK(strstr(p.blocks[1].text, "只用普通话回答") != NULL && strstr(p.blocks[1].text, "喺") != NULL);   // zh: the Cantonese words to avoid, named
    mind_prompt_build(&p, LANG_EN, NULL, NULL);
    CHECK(strstr(p.blocks[1].text, "simple English") != NULL);
    CHECK(strstr(p.blocks[1].text, "always answer in English") != NULL);
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
    CHECK_EQ_INT(mind_book_text("about_child", buf, sizeof buf), 0);    // mind keeps a copy of each book until it writes it itself
    CHECK(mind_init(&cfg) == ESP_OK);                                  // a write from outside (this test, a wipe) must re-init mind
    CHECK_EQ_INT(mind_book_text("about_child", buf, sizeof buf), 2);
    CHECK(strstr(buf, "豆豆") != NULL);
    CHECK(mind_book_forget("about_child", 0) == ESP_OK);
    CHECK_EQ_INT(mind_book_text("about_child", buf, sizeof buf), 1);
    CHECK(strstr(buf, "怕打雷") == NULL);
    CHECK(mind_book_forget("about_child", 5) == ESP_ERR_NOT_FOUND);
    atomic_file_write(p, "[]", 2); mind_init(&cfg);
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
    // the self-introduction teaches the keys as they work: hold blue to talk, press the yellow gift key for a present, every
    // fifth press a song (the show), ask for a story (it once promised a whistle)
    CHECK(mind_line_hello(LANG_ZH, s, sizeof s) == ESP_OK); CHECK(strstr(s, "你好呀，小满！我是咕咕") != NULL);
    CHECK(strstr(s, "按住蓝色的按键") != NULL); CHECK(strstr(s, "黄色的礼物键") != NULL && strstr(s, "每按五下") != NULL && strstr(s, "唱歌") != NULL && strstr(s, "讲个故事") != NULL); CHECK(strstr(s, "吹哨") == NULL);
    CHECK(mind_line_hello(LANG_YUE, s, sizeof s) == ESP_OK); CHECK(strstr(s, "你好呀，小满！我係咕咕") != NULL);
    CHECK(strstr(s, "撳住藍色個掣") != NULL); CHECK(strstr(s, "黃色嘅禮物掣") != NULL && strstr(s, "唱歌") != NULL && strstr(s, "講個故仔") != NULL); CHECK(strstr(s, "吹哨") == NULL);
    CHECK(mind_line_hello(LANG_EN, s, sizeof s) == ESP_OK); CHECK(strstr(s, "Hi, 小满! I'm Gugu") != NULL);
    CHECK(strstr(s, "hold the blue button") != NULL); CHECK(strstr(s, "yellow gift button") != NULL && strstr(s, "sing you a song") != NULL && strstr(s, "tell me a story") != NULL); CHECK(strstr(s, "whistle") == NULL);
    CHECK(mind_line_sample(LANG_YUE, s, sizeof s) == ESP_OK); CHECK(strstr(s, "我係咕咕") != NULL);   // the voice preview's line
    CHECK(mind_line_sample(LANG_EN, s, sizeof s) == ESP_OK); CHECK(strstr(s, "I'm Gugu") != NULL);
    char a[200], b[200];
    CHECK(mind_line_question(LANG_EN, 0, a, sizeof a) == ESP_OK); CHECK(mind_line_question(LANG_EN, 1, b, sizeof b) == ESP_OK); CHECK(strcmp(a, b) != 0);
    CHECK(mind_line_question(LANG_YUE, 15, a, sizeof a) == ESP_OK); CHECK(mind_line_question(LANG_YUE, 0, b, sizeof b) == ESP_OK); CHECK_EQ_STR(a, b);   // 15 questions: wraps
    MEM_BIG_BSS static char sys[2048];
    CHECK(mind_share_request_build(LANG_YUE, "正在开罗", "金字塔是用大石块盖的。", sys, sizeof sys) == ESP_OK);
    CHECK(strstr(sys, "香港") != NULL && strstr(sys, "繁体字") != NULL);   // the light bulb's lines: Hong Kong Cantonese too
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

// Stories (2026-11-06, the owner: "小朋友喜欢听长故事"). Asking for one starts it in any of the three languages, and so
// does saying yes (or naming a topic) when Gugu just offered one; telling Gugu that mum told a story does not. Each part
// has its own prompt: the one-or-two-sentence rule is lifted, every part but the last ends with a question for the child,
// the child's answer (or the lack of one) goes into the next part, bedtime ends with goodnight. The shelf keeps what was
// told, newest first, the last 30.
TEST(test_mind_story)
{
    mind_config_t cfg = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满", .family_langs = { LANG_ZH, LANG_YUE, LANG_EN } };
    CHECK(mind_init(&cfg) == ESP_OK);
    CHECK_EQ_INT(mind_story_intent("给我讲个恐龙的故事", NULL), STORY_INTENT_START);
    CHECK_EQ_INT(mind_story_intent("咕咕，讲故事！", NULL), STORY_INTENT_START);
    CHECK_EQ_INT(mind_story_intent("我想听小兔子的故事", NULL), STORY_INTENT_START);
    CHECK_EQ_INT(mind_story_intent("再讲一个", "故事讲完啦！"), STORY_INTENT_START);
    CHECK_EQ_INT(mind_story_intent("講個故仔俾我聽", NULL), STORY_INTENT_START);
    CHECK_EQ_INT(mind_story_intent("Can you tell me a story about a whale?", NULL), STORY_INTENT_START);
    CHECK_EQ_INT(mind_story_intent("妈妈昨天给我讲了一个故事", NULL), STORY_INTENT_NONE);      // telling, not asking
    CHECK_EQ_INT(mind_story_intent("这个故事好好听", NULL), STORY_INTENT_NONE);
    CHECK_EQ_INT(mind_story_intent("我今天去公园看到鸭子了", NULL), STORY_INTENT_NONE);
    CHECK_EQ_INT(mind_story_intent("恐龙", "你想听什么故事呀？"), STORY_INTENT_START);         // the topic, after Gugu offered
    CHECK_EQ_INT(mind_story_intent("好呀", "我给你讲个故事好不好？"), STORY_INTENT_START);
    CHECK_EQ_INT(mind_story_intent("不要", "要不要听一个故事？"), STORY_INTENT_NONE);
    CHECK_EQ_INT(mind_story_intent("yes please", "Shall I tell you a story?"), STORY_INTENT_START);
    CHECK_EQ_INT(mind_story_intent("不听了", NULL), STORY_INTENT_STOP);
    CHECK_EQ_INT(mind_story_intent("我不要听了", NULL), STORY_INTENT_STOP);
    CHECK_EQ_INT(mind_story_intent("stop please", NULL), STORY_INTENT_STOP);
    CHECK_EQ_INT(mind_story_intent("然后呢？", NULL), STORY_INTENT_MORE);
    CHECK_EQ_INT(mind_story_intent("繼續講", NULL), STORY_INTENT_MORE);
    CHECK_EQ_INT(mind_story_intent("再讲一遍", NULL), STORY_INTENT_AGAIN);            // the same one again …
    CHECK_EQ_INT(mind_story_intent("刚才那个故事再讲一遍", NULL), STORY_INTENT_AGAIN);
    CHECK_EQ_INT(mind_story_intent("講多次", NULL), STORY_INTENT_AGAIN);
    CHECK_EQ_INT(mind_story_intent("再讲一个", "故事讲完啦！"), STORY_INTENT_START);    // … and another one is a new one

    mind_prompt_t p; memset(&p, 0, sizeof p);
    CHECK(mind_story_prompt_build(&p, LANG_ZH, 1, 3, NULL, false, false) == ESP_OK);
    if (!p.blocks[3].text) return;
    CHECK_EQ_INT(p.count, 4);
    CHECK(strstr(p.blocks[3].text, "今天是") != NULL);                       // today's facts stay, the story's rules follow
    CHECK(strstr(p.blocks[3].text, "【现在：讲故事】") != NULL);
    CHECK(strstr(p.blocks[3].text, "第 1 段（共 3 段）") != NULL);
    CHECK(strstr(p.blocks[3].text, "从前") != NULL && strstr(p.blocks[3].text, "问小满") != NULL);   // opens, ends on a question
    CHECK(strstr(p.blocks[3].text, "350") != NULL);                         // a Mandarin part is about 350 characters
    CHECK(strstr(p.blocks[3].text, "晚安") == NULL);
    CHECK(mind_story_prompt_build(&p, LANG_ZH, 2, 3, "大象来帮忙", false, false) == ESP_OK);
    CHECK(strstr(p.blocks[3].text, "大象来帮忙") != NULL);                    // the child's answer goes into the next part
    CHECK(strstr(p.blocks[3].text, "问小满") != NULL);
    CHECK(mind_story_prompt_build(&p, LANG_EN, 3, 3, NULL, true, false) == ESP_OK);
    CHECK(strstr(p.blocks[3].text, "没有回答") != NULL);                      // no answer came: the model is told so
    CHECK(strstr(p.blocks[3].text, "晚安") != NULL);                          // bedtime: a goodnight ending
    CHECK(strstr(p.blocks[3].text, "220") != NULL);                         // an English part is about 220 words
    CHECK(strstr(p.blocks[3].text, "故事讲完啦") != NULL);
    CHECK(mind_story_prompt_build(&p, LANG_ZH, 1, 3, NULL, false, true) == ESP_OK);   // cut off while written: finish it
    CHECK(strstr(p.blocks[3].text, "不要从头再讲") != NULL && strstr(p.blocks[3].text, "用\"从前\"开头") == NULL);
    CHECK(strstr(p.blocks[3].text, "问小满") != NULL);                       // and still end on the question
    for (int part = 1; part <= 3; part++) {
        mind_story_prompt_build(&p, LANG_YUE, part, 3, part > 1 ? "佢哋去咗海邊" : NULL, part == 3, false);
        CHECK(strchr(p.blocks[3].text, '{') == NULL);                       // every placeholder filled
    }

    char path[200], names[40][40];
    CHECK(mind_story_save(LANG_ZH, "[happy] 从前有一只小恐龙，叫嘟嘟。[curious] 它想去看海。", path, sizeof path) == ESP_OK);
    char *saved = file_read_all(path, NULL);
    CHECK(saved != NULL);
    CHECK(saved && strstr(saved, "\"title\":\"从前有一只小恐龙，叫嘟嘟。\"") != NULL);   // the first sentence names it
    CHECK(saved && strstr(saved, "\"text\":\"从前有一只小恐龙") != NULL);                  // the text without tags …
    CHECK(saved && strstr(saved, "\"tagged\":\"[happy] 从前") != NULL);                     // … and with them, to tell it again
    char *again = mind_story_newest(LANG_ZH);
    CHECK(again && strstr(again, "[curious] 它想去看海") != NULL);
    mem_release(again);
    CHECK(mind_story_newest(LANG_EN) == NULL);                                        // none in English yet
    mem_release(saved);
    for (int i = 0; i < 33; i++) { char t[64]; snprintf(t, sizeof t, "[happy] 第 %d 个故事。", i); mind_story_save(LANG_ZH, t, NULL, 0); }
    int n = mind_story_shelf(names, 40);
    CHECK_EQ_INT(n, 30);                                                      // the newest 30 stay
    char *newest = NULL;
    if (n > 0) { char pth[200]; snprintf(pth, sizeof pth, "%s/stories/%s", store_data_dir(), names[0]); newest = file_read_all(pth, NULL); }
    CHECK(newest && strstr(newest, "第 32 个故事") != NULL);                  // newest first
    mem_release(newest);
}

void test_mind(void)
{
    RUN(test_mind_story);
    RUN(test_mind_lines_and_language);
    RUN(test_mind_prompt_blocks);
    RUN(test_mind_dialog_ring);
    RUN(test_mind_language_and_normalize);
    RUN(test_mind_memory_books);
}
