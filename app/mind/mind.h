// mind.h — what Gugu says and remembers: prompt blocks (stable first, for the vendor's prefix cache),
// the expression-tag parser, the language policy, per-call dialogue memory, transcript normalisation.
// Memory books, extraction and the life/reflection content arrive in later milestones behind this API.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "provider/provider.h"
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

// ---- language policy ----
lang_t mind_reply_lang(lang_t asr_lang, const char *transcript);
lang_t mind_primary_lang(void);   // the only enabled family language, or LANG_UNKNOWN when several / none
// Fix systematic ASR slips ("姑姑" for "咕咕"); edits in place, returns the same pointer.
char  *mind_normalize_transcript(char *text);

// ---- mood baseline (v1.0: a fixed sentence per time of day) ----
const char *mind_mood_sentence(void);
