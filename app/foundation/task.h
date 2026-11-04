// foundation/task.h — the few concurrency primitives shared code may use: a task, a recursive mutex,
// a fixed-item queue and a binary signal. FreeRTOS on the device, pthreads on the host.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*task_fn_t)(void *arg);
// core: 0 or 1 on the device (ignored on host). Returns false if the task could not be created.
// All stacks are internal RAM on the device (PSRAM stacks assert in the flash driver on any file access);
// task_spawn_internal is the same thing, kept for call sites that want to state the requirement.
bool task_spawn(const char *name, task_fn_t fn, void *arg, uint32_t stack_bytes, int priority, int core);
bool task_spawn_internal(const char *name, task_fn_t fn, void *arg, uint32_t stack_bytes, int priority, int core);
// Every task spawned here that is still running: its stack size and the least it has ever had free (console `mem`).
void task_print_stacks(void);

typedef struct gugu_mutex *mutex_t;
mutex_t mutex_create(void);                 // recursive
void    mutex_lock(mutex_t m);
void    mutex_unlock(mutex_t m);
void    mutex_free(mutex_t m);           // NULL is fine; nobody may hold or wait on it any more

typedef struct gugu_queue *queue_t;
queue_t queue_create(size_t item_size, size_t depth);
bool    queue_send(queue_t q, const void *item, uint32_t timeout_ms);
bool    queue_recv(queue_t q, void *item, uint32_t timeout_ms);
size_t  queue_count(queue_t q);
void    queue_reset(queue_t q);
void    queue_free(queue_t q);

typedef struct gugu_signal *signal_t;       // binary: set() wakes one waiter, state is latched until taken
signal_t signal_create(void);
void     signal_set(signal_t s);
void     signal_set_from_isr(signal_t s);   // the one primitive an interrupt may call (device: IRAM-safe)
bool     signal_wait(signal_t s, uint32_t timeout_ms);
// Every per-session primitive must be freed with its session: on the device each is ~88 B of internal RAM, and the
// voice sessions that forgot leaked 176 B per call, check and bubble line (2026-11-04).
void     signal_free(signal_t s);

#define TASK_WAIT_FOREVER 0xFFFFFFFFu
