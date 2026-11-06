#include "engine_internal.h"
#include <stdlib.h>

esp_err_t engine_start_call(engine_t *e, lang_t l, const char *where) { return e->ops->start_call(e, l, where); }
esp_err_t engine_on_audio(engine_t *e, const int16_t *pcm, size_t n) { return e->ops->on_audio(e, pcm, n); }
esp_err_t engine_on_release(engine_t *e, const char *events) { return e->ops->on_release(e, events); }
void engine_abort(engine_t *e) { e->ops->abort(e); }
void engine_end_call(engine_t *e) { e->ops->end_call(e); }
bool engine_busy(engine_t *e) { return e->ops->busy(e); }
void engine_destroy(engine_t *e) { if (e) { e->ops->destroy(e); free(e); } }
esp_err_t engine_story_start(engine_t *e, const char *request, bool bedtime) { return e->ops->story_start ? e->ops->story_start(e, request, bedtime) : ESP_ERR_NOT_SUPPORTED; }
esp_err_t engine_story_next(engine_t *e) { return e->ops->story_next ? e->ops->story_next(e) : ESP_ERR_NOT_SUPPORTED; }
void engine_story_pause(engine_t *e, int seg) { if (e->ops->story_pause) e->ops->story_pause(e, seg); else e->ops->abort(e); }
bool engine_story_on(engine_t *e) { return e->ops->story_on && e->ops->story_on(e); }
void engine_set_backlog_probe(engine_t *e, uint32_t (*queued_ms)(void)) { if (e->ops->set_backlog_probe) e->ops->set_backlog_probe(e, queued_ms); }
