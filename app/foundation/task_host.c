#include "foundation/task.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

struct spawn { task_fn_t fn; void *arg; };
static void *trampoline(void *p)
{
    struct spawn s = *(struct spawn *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

bool task_spawn_internal(const char *name, task_fn_t fn, void *arg, uint32_t stack_bytes, int priority, int core);
bool task_spawn_internal(const char *name, task_fn_t fn, void *arg, uint32_t stack_bytes, int priority, int core)
{
    return task_spawn(name, fn, arg, stack_bytes, priority, core);
}

bool task_spawn(const char *name, task_fn_t fn, void *arg, uint32_t stack_bytes, int priority, int core)
{
    (void)name; (void)priority; (void)core;
    struct spawn *s = malloc(sizeof *s);
    if (!s) return false;
    s->fn = fn; s->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stack = stack_bytes < 256 * 1024 ? 256 * 1024 : stack_bytes;   // host threads are cheap; be generous
    pthread_attr_setstacksize(&attr, stack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = pthread_create(&t, &attr, trampoline, s);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(s); return false; }
    return true;
}

struct gugu_mutex { pthread_mutex_t m; };
mutex_t mutex_create(void)
{
    struct gugu_mutex *m = malloc(sizeof *m);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a); pthread_mutexattr_destroy(&a);
    return m;
}
void mutex_lock(mutex_t m) { pthread_mutex_lock(&m->m); }
void mutex_unlock(mutex_t m) { pthread_mutex_unlock(&m->m); }

static void deadline(struct timespec *ts, uint32_t ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

struct gugu_queue { pthread_mutex_t m; pthread_cond_t not_empty, not_full; char *buf; size_t item, depth, head, count; };
queue_t queue_create(size_t item_size, size_t depth)
{
    struct gugu_queue *q = calloc(1, sizeof *q);
    q->buf = malloc(item_size * depth); q->item = item_size; q->depth = depth;
    pthread_mutex_init(&q->m, NULL); pthread_cond_init(&q->not_empty, NULL); pthread_cond_init(&q->not_full, NULL);
    return q;
}
bool queue_send(queue_t q, const void *item, uint32_t timeout_ms)
{
    struct timespec ts; deadline(&ts, timeout_ms == TASK_WAIT_FOREVER ? 3600000u : timeout_ms);
    pthread_mutex_lock(&q->m);
    while (q->count == q->depth) {
        if (pthread_cond_timedwait(&q->not_full, &q->m, &ts) == ETIMEDOUT && timeout_ms != TASK_WAIT_FOREVER) { pthread_mutex_unlock(&q->m); return false; }
    }
    memcpy(q->buf + ((q->head + q->count) % q->depth) * q->item, item, q->item);
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->m);
    return true;
}
bool queue_recv(queue_t q, void *item, uint32_t timeout_ms)
{
    struct timespec ts; deadline(&ts, timeout_ms == TASK_WAIT_FOREVER ? 3600000u : timeout_ms);
    pthread_mutex_lock(&q->m);
    while (q->count == 0) {
        if (pthread_cond_timedwait(&q->not_empty, &q->m, &ts) == ETIMEDOUT && timeout_ms != TASK_WAIT_FOREVER) { pthread_mutex_unlock(&q->m); return false; }
    }
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->depth; q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->m);
    return true;
}
size_t queue_count(queue_t q) { pthread_mutex_lock(&q->m); size_t n = q->count; pthread_mutex_unlock(&q->m); return n; }
void queue_reset(queue_t q) { pthread_mutex_lock(&q->m); q->count = 0; q->head = 0; pthread_cond_broadcast(&q->not_full); pthread_mutex_unlock(&q->m); }

struct gugu_signal { pthread_mutex_t m; pthread_cond_t c; bool set; };
signal_t signal_create(void) { struct gugu_signal *s = calloc(1, sizeof *s); pthread_mutex_init(&s->m, NULL); pthread_cond_init(&s->c, NULL); return s; }
void signal_set(signal_t s) { pthread_mutex_lock(&s->m); s->set = true; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m); }
void signal_set_from_isr(signal_t s) { signal_set(s); }
bool signal_wait(signal_t s, uint32_t timeout_ms)
{
    struct timespec ts; deadline(&ts, timeout_ms == TASK_WAIT_FOREVER ? 3600000u : timeout_ms);
    pthread_mutex_lock(&s->m);
    while (!s->set) {
        if (pthread_cond_timedwait(&s->c, &s->m, &ts) == ETIMEDOUT && timeout_ms != TASK_WAIT_FOREVER) { pthread_mutex_unlock(&s->m); return false; }
    }
    s->set = false;
    pthread_mutex_unlock(&s->m);
    return true;
}

void task_print_stacks(void) { }                         // host threads have generous stacks: nothing to watch
