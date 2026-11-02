#include "engine_internal.h"
#include <stdlib.h>

esp_err_t engine_start_call(engine_t *e, lang_t l, const char *where) { return e->ops->start_call(e, l, where); }
esp_err_t engine_on_audio(engine_t *e, const int16_t *pcm, size_t n) { return e->ops->on_audio(e, pcm, n); }
esp_err_t engine_on_release(engine_t *e, const char *events) { return e->ops->on_release(e, events); }
void engine_abort(engine_t *e) { e->ops->abort(e); }
void engine_end_call(engine_t *e) { e->ops->end_call(e); }
bool engine_busy(engine_t *e) { return e->ops->busy(e); }
void engine_destroy(engine_t *e) { if (e) { e->ops->destroy(e); free(e); } }
