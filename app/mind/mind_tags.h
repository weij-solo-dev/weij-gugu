// mind_tags.h — private to mind: the closed expression-tag vocabulary and the streaming parser that
// strips tags from model output, reports them, and cuts the clean text into TTS segments.
// The vocabulary exists in exactly three places that change together: here, ui_anim.c, and
// content/prompts/tags.md (CLAUDE.md rule 7; test_tags diffs them).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "foundation/strbuf.h"

typedef enum {
    TAG_NONE = 0,
    // emotions (12)
    TAG_NEUTRAL, TAG_HAPPY, TAG_LAUGHING, TAG_SURPRISED, TAG_CURIOUS, TAG_SHY, TAG_SAD, TAG_SLEEPY,
    TAG_ANGRY, TAG_SCARED, TAG_PROUD, TAG_DIZZY,
    // actions / sounds (7)
    TAG_COO, TAG_PUFF, TAG_WHISTLE, TAG_WAVE, TAG_NOD, TAG_PECK, TAG_JUMP,
    TAG_COUNT
} mind_tag_t;

const char *mind_tag_name(mind_tag_t t);                 // "happy" …
mind_tag_t   mind_tag_parse(const char *name, size_t n); // TAG_NONE if unknown
bool         mind_tag_is_emotion(mind_tag_t t);

typedef struct {
    void (*on_text)(const char *utf8, size_t n, void *user);      // clean text, tags removed
    void (*on_tag)(mind_tag_t tag, void *user);                   // in stream order, before the text that follows it
    void (*on_segment_end)(void *user);                           // a sentence boundary in the clean text
} tag_sink_t;

typedef struct {
    tag_sink_t sink;
    void *user;
    char pending[40];      // bytes collected since an unmatched '['
    size_t pending_len;
    bool in_tag;
    size_t seg_bytes;      // clean bytes emitted since the last boundary
    size_t seg_chars;      // UTF-8 characters since the last boundary
    unsigned char utf8_hold[4];   // partial multi-byte sequence split across feeds
    size_t utf8_hold_len, utf8_need;
} tag_parser_t;

void tag_parser_init(tag_parser_t *p, const tag_sink_t *sink, void *user);
void tag_parser_feed(tag_parser_t *p, const char *utf8, size_t n);
void tag_parser_finish(tag_parser_t *p);   // flush a pending '[' as text and close the last segment

// Convenience for tests and the host replay: strip tags, return clean text (caller mem_release()s) and the tags seen.
char *tag_strip(const char *utf8, mind_tag_t *tags_out, size_t tags_cap, size_t *tags_n);
