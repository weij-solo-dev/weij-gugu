#pragma once
#include "engine/engine.h"
#include "foundation/task.h"

typedef struct engine_ops {
    esp_err_t (*start_call)(engine_t *e, lang_t lang_hint, const char *where);
    esp_err_t (*on_audio)(engine_t *e, const int16_t *pcm, size_t samples);
    esp_err_t (*on_release)(engine_t *e, const char *events);
    void      (*abort)(engine_t *e);
    void      (*end_call)(engine_t *e);
    bool      (*busy)(engine_t *e);
    void      (*destroy)(engine_t *e);
} engine_ops_t;

struct engine {
    const engine_ops_t *ops;
    engine_listener_t cb;
    void *user;
    void *impl;
};

static inline void engine_emit(engine_t *e, const engine_event_t *ev) { if (e->cb) e->cb(ev, e->user); }
