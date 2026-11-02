#include "foundation/ringbuf.h"
#include <string.h>

void ringbuf_init(ringbuf_t *r, void *storage, size_t capacity)
{
    r->buf = storage; r->cap = capacity;
    atomic_store(&r->head, 0); atomic_store(&r->tail, 0);
}

size_t ringbuf_used(const ringbuf_t *r)
{
    size_t h = atomic_load(&r->head), t = atomic_load(&r->tail);
    return h >= t ? h - t : r->cap - t + h;
}

size_t ringbuf_free(const ringbuf_t *r) { return r->cap - 1 - ringbuf_used(r); }

size_t ringbuf_write(ringbuf_t *r, const void *data, size_t n)
{
    size_t space = ringbuf_free(r);
    if (n > space) n = space;
    size_t h = atomic_load(&r->head);
    size_t first = r->cap - h;
    if (first > n) first = n;
    memcpy(r->buf + h, data, first);
    if (n > first) memcpy(r->buf, (const uint8_t *)data + first, n - first);
    atomic_store(&r->head, (h + n) % r->cap);
    return n;
}

size_t ringbuf_peek(const ringbuf_t *r, void *out, size_t n)
{
    size_t used = ringbuf_used(r);
    if (n > used) n = used;
    size_t t = atomic_load(&r->tail);
    size_t first = r->cap - t;
    if (first > n) first = n;
    memcpy(out, r->buf + t, first);
    if (n > first) memcpy((uint8_t *)out + first, r->buf, n - first);
    return n;
}

size_t ringbuf_read(ringbuf_t *r, void *out, size_t n)
{
    n = ringbuf_peek(r, out, n);
    atomic_store(&r->tail, (atomic_load(&r->tail) + n) % r->cap);
    return n;
}

void ringbuf_clear(ringbuf_t *r) { atomic_store(&r->tail, atomic_load(&r->head)); }
