// mind_internal.h — private to the mind module.
#pragma once
#include "mind/mind.h"

esp_err_t mind_prompt_load(const char *content_dir);
void mind_fill(char *dst, size_t cap, const char *tpl, const char *const *pairs);
const char *mind_block_a(void);           // persona + seed canon + tags, placeholders filled (mind_prompt.c)
const char *mind_child_name(void);
// mind_trip.c
extern char *mind_trip_tpl;               // content/prompts/trip_card.md
void mind_trip_block(char *dst, size_t cap);        // the trip part of block B; empty without a card
void mind_where_sentence(char *dst, size_t cap);    // block C's "where", from the world
extern char *mind_recap_tpl;              // content/prompts/trip_recap.md
// mind_memory.c
esp_err_t mind_memory_init(const char *data_dir);
