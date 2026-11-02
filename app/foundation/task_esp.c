#include "foundation/task.h"
#include <stdlib.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static TickType_t to_ticks(uint32_t ms)
{
    return ms == TASK_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

// A FreeRTOS task function must never return: on Xtensa the task starts with a zero return address, so a
// plain `return` executes RETW with A0 = 0 and the chip panics with IllegalInstruction. The host build
// (pthreads) is happy with a returning function, and short-lived tasks such as the connection pre-warms
// are written that way, so every task runs through this trampoline, which deletes the task on return.
struct trampoline { task_fn_t fn; void *arg; };
static void task_trampoline(void *p)
{
    struct trampoline t = *(struct trampoline *)p;
    free(p);
    t.fn(t.arg);
    vTaskDelete(NULL);
}

bool task_spawn_internal(const char *name, task_fn_t fn, void *arg, uint32_t stack_bytes, int priority, int core)
{
    struct trampoline *t = malloc(sizeof *t);
    if (!t) return false;
    t->fn = fn; t->arg = arg;
    BaseType_t ok = xTaskCreatePinnedToCore(task_trampoline, name, stack_bytes, t, priority, NULL, core == 1 ? 1 : 0);
    if (ok != pdPASS) free(t);
    return ok == pdPASS;
}

// Every task keeps an internal-RAM stack: a task whose stack is in PSRAM asserts inside the flash
// driver the moment it reads or writes a LittleFS file (cache_utils.c:141), and almost every task here
// touches files (sound effects, call logs, prompts). PSRAM stacks would only suit pure-compute tasks.
bool task_spawn(const char *name, task_fn_t fn, void *arg, uint32_t stack_bytes, int priority, int core)
{
    return task_spawn_internal(name, fn, arg, stack_bytes, priority, core);
}

mutex_t mutex_create(void) { return (mutex_t)xSemaphoreCreateRecursiveMutex(); }
void mutex_lock(mutex_t m) { xSemaphoreTakeRecursive((SemaphoreHandle_t)m, portMAX_DELAY); }
void mutex_unlock(mutex_t m) { xSemaphoreGiveRecursive((SemaphoreHandle_t)m); }

queue_t queue_create(size_t item_size, size_t depth) { return (queue_t)xQueueCreate(depth, item_size); }
bool queue_send(queue_t q, const void *item, uint32_t timeout_ms) { return xQueueSend((QueueHandle_t)q, item, to_ticks(timeout_ms)) == pdTRUE; }
bool queue_recv(queue_t q, void *item, uint32_t timeout_ms) { return xQueueReceive((QueueHandle_t)q, item, to_ticks(timeout_ms)) == pdTRUE; }
size_t queue_count(queue_t q) { return uxQueueMessagesWaiting((QueueHandle_t)q); }
void queue_reset(queue_t q) { xQueueReset((QueueHandle_t)q); }

signal_t signal_create(void) { return (signal_t)xSemaphoreCreateBinary(); }
void signal_set(signal_t s) { xSemaphoreGive((SemaphoreHandle_t)s); }
void IRAM_ATTR signal_set_from_isr(signal_t s)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)s, &woken);
    if (woken) portYIELD_FROM_ISR();
}
bool signal_wait(signal_t s, uint32_t timeout_ms) { return xSemaphoreTake((SemaphoreHandle_t)s, to_ticks(timeout_ms)) == pdTRUE; }
