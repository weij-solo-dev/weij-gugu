// mind.h — what Gugu says and remembers: prompt blocks (stable first, for the vendor's prefix cache),
// the expression-tag parser, the language policy, per-call dialogue memory, transcript normalisation.
// Memory books, extraction and the life/reflection content arrive in later milestones behind this API.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "provider/provider.h"
#include "world/world.h"
#include "mind_tags.h"

typedef struct {
    const char *content_dir;        // directory holding prompts/ (host: <repo>/content, device: /content)
    const char *child_name;         // NULL → "小朋友"
    int child_age_months;           // 0 = unknown
    lang_t family_langs[3];         // languages the family enabled; LANG_UNKNOWN entries ignored
} mind_config_t;

esp_err_t mind_init(const mind_config_t *cfg);
void mind_set_child(const char *name, int age_months);

// ---- prompt ----
#define MIND_MAX_BLOCKS 4
typedef struct {
    chat_block_t blocks[MIND_MAX_BLOCKS];   // A persona+canon, A2 language examples, B memory, C today
    size_t count;
} mind_prompt_t;
// `where` and `events` are plain sentences for block C ("在家里，刚被叫到洞口聊天", "孩子刚摸了你一下").
esp_err_t mind_prompt_build(mind_prompt_t *out, lang_t reply_lang, const char *where, const char *events);

// ---- dialogue memory for the current call (block D) ----
void   mind_dialog_reset(void);
void   mind_dialog_add(const char *role, const char *text);
size_t mind_dialog_messages(chat_message_t *out, size_t cap);   // oldest first
const char *mind_dialog_last_user(void);                       // what the child just said, or NULL

// ---- language policy ----
lang_t mind_reply_lang(lang_t asr_lang, const char *transcript);
lang_t mind_primary_lang(void);   // the only enabled family language, or LANG_UNKNOWN when several / none
// Fix systematic ASR slips ("姑姑" for "咕咕"); edits in place, returns the same pointer.
char  *mind_normalize_transcript(char *text);

// ---- mood baseline (v1.0: a fixed sentence per time of day) ----
const char *mind_mood_sentence(void);

// ---- the trip card: life asks the model for the next trip with this request; world keeps the answer ----
typedef struct {
    chat_block_t blocks[2];   // persona + canon; the planning task and the card's schema (content/prompts/trip_card.md)
    size_t count;
    const char *user;         // the request for this particular trip: date, home, hints, places already visited
} mind_trip_request_t;
esp_err_t mind_trip_request_build(mind_trip_request_t *out, const char *kind_hint, const char *place_hint,
                                  const char *depart_date, int nights_hint, const char *recent_places, const char *journey);
// The recap of the trip just finished (content/prompts/trip_recap.md); `days_text` is the journey's day entries for it.
esp_err_t mind_recap_request_build(mind_trip_request_t *out, const char *days_text);

// ---- the three memory books (mind_memory.c): the journey log and the two lists the extraction will fill ----
typedef enum { JOURNEY_PLANNED = 0, JOURNEY_DAY, JOURNEY_RETURNED } journey_kind_t;
esp_err_t mind_journey_add(journey_kind_t k, const world_trip_t *t, const char *date, int day, const char *text, const char (*hooks)[160], int n_hooks);
size_t    mind_journey_recent(char *dst, size_t cap, int trips);          // the last `trips` recaps, this trip's days so far, open hooks
size_t    mind_journey_days_text(const char *trip_id, char *dst, size_t cap);
size_t    mind_journey_match(const char *utterance, char *dst, size_t cap, int max);   // entries whose place or friend the child named
bool      mind_journey_has(journey_kind_t k, const char *trip_id, const char *date);   // date may be NULL
esp_err_t mind_journey_forget(const char *trip_id);
size_t    mind_book_text(const char *book, char *dst, size_t cap);       // "about_child" | "canon_grown", one line per entry
esp_err_t mind_book_forget(const char *book, int index);
size_t    mind_calls_digest(const char *date, char *dst, size_t cap);    // what the child and Gugu said that day, short
void      mind_utf8_cut(char *s, size_t max_bytes);
