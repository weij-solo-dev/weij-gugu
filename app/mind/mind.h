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
const char *mind_dialog_last_assistant(void);                  // what Gugu said last, or NULL

// ---- language policy ----
lang_t mind_reply_lang(lang_t asr_lang, const char *transcript);
void   mind_set_language(lang_t lang);   // the interaction language from the pairing page: Gugu answers in it whatever it hears
lang_t mind_primary_lang(void);   // the only enabled family language, or LANG_UNKNOWN when several / none
// Fix systematic ASR slips ("姑姑" for "咕咕"); edits in place, returns the same pointer.
char  *mind_normalize_transcript(char *text);

// ---- what Gugu says on its own, from the bubble (mind_lines.c; content/prompts/hello.json, questions.json, share.md) ----
esp_err_t mind_line_hello(lang_t lang, char *out, size_t cap);                     // the self-introduction, the child's name filled in
esp_err_t mind_line_sample(lang_t lang, char *out, size_t cap);                    // the pairing page's voice preview (content/prompts/voice_sample.json)
esp_err_t mind_line_question(lang_t lang, unsigned pick, char *out, size_t cap);   // one of the questions for the child (pick % count)
// The system prompt that retells one fact of the trip as a sentence Gugu tells the child in `lang`; where_phrase says
// where Gugu is in relation to the place ("正在开罗", "过几天要去开罗", "刚从开罗回来").
esp_err_t mind_share_request_build(lang_t lang, const char *where_phrase, const char *fact, char *system, size_t system_cap);
void      mind_strip_tags(char *text);                                              // drop [tags] the model added anyway, trim
// Gugu just asked the child `question` from the bubble: block C says so for the next ttl_ms, so the answer makes sense.
void      mind_set_asked(const char *question, uint32_t ttl_ms);

// ---- stories (mind_story.c; content/prompts/story.md): told in parts, kept on a shelf under data/stories/ ----
typedef enum { STORY_INTENT_NONE = 0, STORY_INTENT_START, STORY_INTENT_STOP, STORY_INTENT_MORE, STORY_INTENT_AGAIN } story_intent_t;
// What the child's words mean for a story: asking for one (or saying yes to the one Gugu just offered in last_reply),
// asking to stop, asking for more, asking for the last one again ("再讲一遍", not "再讲一个"). The caller decides what applies (stop and more only while a story is on).
story_intent_t mind_story_intent(const char *said, const char *last_reply);
// The prompt for one part of `parts`: block C carries today and the story's rules; child_said is the answer to the
// last part's question (NULL: none came); bedtime ends the story with goodnight; go_on: the part was cut off while the
// model wrote it (the child broke in), so it is finished from where the dialogue's last part stops.
esp_err_t mind_story_prompt_build(mind_prompt_t *out, lang_t lang, int part, int parts, const char *child_said, bool bedtime, bool go_on);
esp_err_t mind_story_save(lang_t lang, const char *text, char *path_out, size_t cap);   // the text with its tags and without; keeps the newest 30
char     *mind_story_newest(lang_t lang);   // the newest story in this language, with its tags (mem_release), or NULL
int       mind_story_shelf(char (*names)[40], int cap);                                 // file names, newest first

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
