// foundation/ringbuf.h — single-producer / single-consumer byte ring, lock-free for one writer and one
// reader (the audio path: capture ISR/task writes, the engine reads). Capacity is fixed at init.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef struct {
    uint8_t *buf;
    size_t cap;
    atomic_size_t head;   // write index
    atomic_size_t tail;   // read index
} ringbuf_t;

void   ringbuf_init(ringbuf_t *r, void *storage, size_t capacity);
size_t ringbuf_used(const ringbuf_t *r);
size_t ringbuf_free(const ringbuf_t *r);
size_t ringbuf_write(ringbuf_t *r, const void *data, size_t n);   // returns bytes written (may be < n)
size_t ringbuf_read(ringbuf_t *r, void *out, size_t n);           // returns bytes read
size_t ringbuf_peek(const ringbuf_t *r, void *out, size_t n);
void   ringbuf_clear(ringbuf_t *r);
