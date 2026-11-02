// mind_internal.h — private to the mind module.
#pragma once
#include "mind/mind.h"

esp_err_t mind_prompt_load(const char *content_dir);
void mind_fill(char *dst, size_t cap, const char *tpl, const char *const *pairs);
