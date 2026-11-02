#include "minitest.h"
#include "mind/mind_tags.h"
#include "foundation/mem.h"

static char seg_log[512]; static int seg_n; static mind_tag_t tag_log[16]; static int tag_n; static char text_acc[256];
static void t_text(const char *s, size_t n, void *u) { strncat(text_acc, s, n); }
static void t_tag(mind_tag_t t, void *u) { if (tag_n < 16) tag_log[tag_n] = t; tag_n++; }
static void t_seg(void *u) { seg_n++; strcat(seg_log, text_acc); strcat(seg_log, "|"); text_acc[0] = 0; }
static void reset(void) { seg_log[0] = 0; seg_n = 0; tag_n = 0; text_acc[0] = 0; }

static void feed_bytewise(tag_parser_t *p, const char *s) { for (size_t i = 0; s[i]; i++) tag_parser_feed(p, s + i, 1); }

TEST(test_tags_basic_stream)
{
    reset();
    tag_sink_t sink = { t_text, t_tag, t_seg }; tag_parser_t p; tag_parser_init(&p, &sink, NULL);
    feed_bytewise(&p, "[happy] 你去公园啦！[surprised] 有鸭子？[coo] 我最喜欢鸭子了。");
    tag_parser_finish(&p);
    CHECK_EQ_INT(tag_n, 3);
    CHECK(tag_log[0] == TAG_HAPPY && tag_log[1] == TAG_SURPRISED && tag_log[2] == TAG_COO);
    CHECK_EQ_INT(seg_n, 3);
    CHECK_EQ_STR(seg_log, " 你去公园啦！| 有鸭子？| 我最喜欢鸭子了。|");
}

TEST(test_tags_unknown_and_brackets)
{
    reset();
    tag_sink_t sink = { t_text, t_tag, t_seg }; tag_parser_t p; tag_parser_init(&p, &sink, NULL);
    feed_bytewise(&p, "[angry]不行哦。[1+1] 等于二。[ 嗯");
    tag_parser_finish(&p);
    CHECK_EQ_INT(tag_n, 0);                                  // unknown tag dropped, not voiced
    CHECK_EQ_STR(seg_log, "不行哦。|[1+1] 等于二。|[ 嗯|");   // non-tag brackets are ordinary text
}

TEST(test_tags_split_tag_across_chunks)
{
    reset();
    tag_sink_t sink = { t_text, t_tag, t_seg }; tag_parser_t p; tag_parser_init(&p, &sink, NULL);
    const char *a = "[sle", *b = "epy] 我困", *c = "了。";
    tag_parser_feed(&p, a, strlen(a)); tag_parser_feed(&p, b, strlen(b) - 1); tag_parser_feed(&p, c + 0, 0);
    tag_parser_feed(&p, b + strlen(b) - 1, 1); tag_parser_feed(&p, c, strlen(c));   // 困 split across three feeds
    tag_parser_finish(&p);
    CHECK_EQ_INT(tag_n, 1); CHECK(tag_log[0] == TAG_SLEEPY);
    CHECK_EQ_STR(seg_log, " 我困了。|");
}

TEST(test_tags_soft_boundary_and_newline)
{
    reset();
    tag_sink_t sink = { t_text, t_tag, t_seg }; tag_parser_t p; tag_parser_init(&p, &sink, NULL);
    // 31 characters then a Chinese comma: the comma becomes a boundary; a newline is a boundary too
    feed_bytewise(&p, "一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十一，然后呢\n再见");
    tag_parser_finish(&p);
    CHECK_EQ_INT(seg_n, 3);
}

TEST(test_tags_strip_and_english)
{
    size_t n; mind_tag_t tags[4];
    char *s = tag_strip("[curious] Really? [wave] Bye-bye!", tags, 4, &n);
    CHECK_EQ_STR(s, " Really?  Bye-bye!");
    CHECK_EQ_INT(n, 2); CHECK(tags[0] == TAG_CURIOUS && tags[1] == TAG_WAVE);
    mem_release(s);
    CHECK_EQ_STR(mind_tag_name(TAG_PUFF), "puff");
    CHECK(mind_tag_parse("HAPPY", 5) == TAG_HAPPY);
    CHECK(mind_tag_is_emotion(TAG_SAD) && !mind_tag_is_emotion(TAG_NOD));
}

void test_tags(void)
{
    RUN(test_tags_basic_stream);
    RUN(test_tags_unknown_and_brackets);
    RUN(test_tags_split_tag_across_chunks);
    RUN(test_tags_soft_boundary_and_newline);
    RUN(test_tags_strip_and_english);
}
