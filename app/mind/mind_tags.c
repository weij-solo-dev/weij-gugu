#include "mind_tags.h"
#include "foundation/mem.h"
#include <string.h>

static const char *const NAMES[TAG_COUNT] = {
    "", "neutral", "happy", "laughing", "surprised", "curious", "shy", "sad", "sleepy", "angry", "scared", "proud", "dizzy",
    "coo", "puff", "whistle", "wave", "nod", "peck", "jump",
};

const char *mind_tag_name(mind_tag_t t) { return t < TAG_COUNT ? NAMES[t] : ""; }
bool mind_tag_is_emotion(mind_tag_t t) { return t >= TAG_NEUTRAL && t <= TAG_DIZZY; }

mind_tag_t mind_tag_parse(const char *name, size_t n)
{
    for (int t = 1; t < TAG_COUNT; t++) {
        if (strlen(NAMES[t]) == n && !strncasecmp(NAMES[t], name, n)) return (mind_tag_t)t;
    }
    return TAG_NONE;
}

void tag_parser_init(tag_parser_t *p, const tag_sink_t *sink, void *user)
{
    memset(p, 0, sizeof *p);
    p->sink = *sink; p->user = user;
}

#define SEG_SOFT_CHARS 30   // past this many characters, a comma is also a boundary

static void emit_text(tag_parser_t *p, const char *s, size_t n)
{
    if (n && p->sink.on_text) p->sink.on_text(s, n, p->user);
    p->seg_bytes += n;
}

static void boundary(tag_parser_t *p)
{
    p->cut_pending = false;
    if (p->seg_bytes == 0) return;
    if (p->sink.on_segment_end) p->sink.on_segment_end(p->user);
    p->seg_bytes = 0; p->seg_chars = 0;
}

// A closing quote or bracket: ” ’ 」 』 ） " ' )
static bool closes(const unsigned char *c, size_t n)
{
    if (n == 1) return c[0] == '"' || c[0] == '\'' || c[0] == ')';
    return n == 3 && ((c[0] == 0xE2 && c[1] == 0x80 && (c[2] == 0x9D || c[2] == 0x99)) || (c[0] == 0xE3 && c[1] == 0x80 && (c[2] == 0x8D || c[2] == 0x8F))
                      || (c[0] == 0xEF && c[1] == 0xBC && c[2] == 0x89));
}

// One complete UTF-8 character (1–4 bytes) outside a tag. A sentence's end cuts the segment at the next character
// unless that one belongs to it: a closing quote or bracket, or another end mark ("！？", "……") (2026-11-06: a story's
// "我想回家！”" left a segment of nothing but ” for the voice).
static void put_char(tag_parser_t *p, const unsigned char *c, size_t n)
{
    bool hard = false, soft = false;
    if (n == 1) {
        hard = c[0] == '.' || c[0] == '!' || c[0] == '?' || c[0] == '\n' || c[0] == ';';
        soft = c[0] == ',';
    } else if (n == 3) {
        // 。 E3 80 82 · ！ EF BC 81 · ？ EF BC 9F · ； EF BC 9B · … E2 80 A6 · ， EF BC 8C · 、 E3 80 81
        hard = (c[0] == 0xE3 && c[1] == 0x80 && c[2] == 0x82) || (c[0] == 0xEF && c[1] == 0xBC && (c[2] == 0x81 || c[2] == 0x9F || c[2] == 0x9B))
            || (c[0] == 0xE2 && c[1] == 0x80 && c[2] == 0xA6);
        soft = (c[0] == 0xEF && c[1] == 0xBC && c[2] == 0x8C) || (c[0] == 0xE3 && c[1] == 0x80 && c[2] == 0x81);
    }
    if (c[0] == '\n') {                     // newlines end a segment but are not voiced
        boundary(p);
        return;
    }
    if (p->cut_pending) {
        if (hard || closes(c, n)) {           // still the same sentence
            emit_text(p, (const char *)c, n); p->seg_chars++;
            if (!hard) boundary(p);
            return;
        }
        boundary(p);
    }
    emit_text(p, (const char *)c, n);
    p->seg_chars++;
    if (hard) p->cut_pending = true;
    else if (soft && p->seg_chars >= SEG_SOFT_CHARS) boundary(p);
}

static void feed_impl(tag_parser_t *p, const char *utf8, size_t n, bool allow_tags);

static void flush_pending_as_text(tag_parser_t *p)
{
    // The '[' did not start a known tag: replay the collected bytes as ordinary text (tags disabled,
    // so the leading '[' cannot re-open a tag and loop forever).
    char buf[40]; size_t n = p->pending_len;
    memcpy(buf, p->pending, n);
    p->in_tag = false; p->pending_len = 0;
    feed_impl(p, buf, n, false);
}

static void feed_impl(tag_parser_t *p, const char *utf8, size_t n, bool allow_tags)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)utf8[i];
        if (p->in_tag) {
            if (b == ']') {
                mind_tag_t t = mind_tag_parse(p->pending + 1, p->pending_len - 1);
                p->in_tag = false; p->pending_len = 0;
                if (t != TAG_NONE && p->sink.on_tag) p->sink.on_tag(t, p->user);
                // unknown tags vanish silently (the model invented one)
                continue;
            }
            bool tagchar = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '_';
            if (!tagchar || p->pending_len >= sizeof p->pending - 1) {
                flush_pending_as_text(p);
                i--;                          // re-process this byte as text
                continue;
            }
            p->pending[p->pending_len++] = (char)b;
            continue;
        }
        if (p->utf8_need) {
            if ((b & 0xC0) != 0x80) {          // not a continuation byte: drop the broken sequence
                p->utf8_hold_len = p->utf8_need = 0;
                i--; continue;
            }
            p->utf8_hold[p->utf8_hold_len++] = b;
            if (p->utf8_hold_len == p->utf8_need) {
                put_char(p, p->utf8_hold, p->utf8_hold_len);
                p->utf8_hold_len = p->utf8_need = 0;
            }
            continue;
        }
        if (b == '[' && allow_tags) {
            if (p->cut_pending) boundary(p);  // the sentence closes before a tag: the tag belongs to the next one
            p->in_tag = true; p->pending[0] = '['; p->pending_len = 1;
            continue;
        }
        size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        if (len == 1) { put_char(p, &b, 1); continue; }
        p->utf8_hold[0] = b; p->utf8_hold_len = 1; p->utf8_need = len;
    }
}

void tag_parser_feed(tag_parser_t *p, const char *utf8, size_t n)
{
    feed_impl(p, utf8, n, true);
}

void tag_parser_finish(tag_parser_t *p)
{
    if (p->in_tag) flush_pending_as_text(p);
    if (p->utf8_hold_len) { emit_text(p, (const char *)p->utf8_hold, p->utf8_hold_len); p->utf8_hold_len = p->utf8_need = 0; }
    boundary(p);
}

// ---- tag_strip ----
struct strip_ctx { strbuf_t out; mind_tag_t *tags; size_t cap, n; };
static void strip_text(const char *s, size_t n, void *u) { strbuf_append(&((struct strip_ctx *)u)->out, s, n); }
static void strip_tag(mind_tag_t t, void *u) { struct strip_ctx *c = u; if (c->tags && c->n < c->cap) c->tags[c->n] = t; c->n++; }

char *tag_strip(const char *utf8, mind_tag_t *tags_out, size_t tags_cap, size_t *tags_n)
{
    struct strip_ctx c = { .tags = tags_out, .cap = tags_cap };
    strbuf_init(&c.out);
    tag_sink_t sink = { .on_text = strip_text, .on_tag = strip_tag, .on_segment_end = NULL };
    tag_parser_t p; tag_parser_init(&p, &sink, &c);
    tag_parser_feed(&p, utf8, strlen(utf8));
    tag_parser_finish(&p);
    if (tags_n) *tags_n = c.n;
    if (!c.out.s) strbuf_puts(&c.out, "");
    return strbuf_detach(&c.out);
}
