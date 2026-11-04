#include "foundation/task.h"
#include <stdlib.h>
#include <stdio.h>
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
//
// The trampoline also keeps the running tasks in a table, for the stacks' low-water marks (`mem`). A task enters it
// itself and leaves it before it is deleted, both under the lock the report holds, so a report never reads a task
// that is gone.
struct trampoline { task_fn_t fn; void *arg; uint32_t stack; };
#define TASK_TABLE 32
static struct { TaskHandle_t h; uint32_t stack; } s_tasks[TASK_TABLE];
static SemaphoreHandle_t s_tasks_lock;

static void table_set(TaskHandle_t h, uint32_t stack, bool add)
{
    xSemaphoreTake(s_tasks_lock, portMAX_DELAY);
    for (int i = 0; i < TASK_TABLE; i++) {
        if (add ? s_tasks[i].h == NULL : s_tasks[i].h == h) { s_tasks[i].h = add ? h : NULL; s_tasks[i].stack = stack; break; }
    }
    xSemaphoreGive(s_tasks_lock);
}

static void task_trampoline(void *p)
{
    struct trampoline t = *(struct trampoline *)p;
    free(p);
    table_set(xTaskGetCurrentTaskHandle(), t.stack, true);
    t.fn(t.arg);
    table_set(xTaskGetCurrentTaskHandle(), 0, false);
    vTaskDelete(NULL);
}

void task_print_stacks(void)
{
    if (!s_tasks_lock) return;
    struct { char name[16]; uint32_t stack, low; } rows[TASK_TABLE]; int n = 0; char sz[12];
    xSemaphoreTake(s_tasks_lock, portMAX_DELAY);
    for (int i = 0; i < TASK_TABLE; i++) {
        if (!s_tasks[i].h) continue;
        snprintf(rows[n].name, sizeof rows[n].name, "%s", pcTaskGetName(s_tasks[i].h));
        rows[n].stack = s_tasks[i].stack; rows[n].low = (uint32_t)uxTaskGetStackHighWaterMark(s_tasks[i].h);   // bytes on ESP-IDF
        n++;
    }
    xSemaphoreGive(s_tasks_lock);
    static const char *const SYSTEM[] = { "main", "esp_timer", "sys_evt", "tiT", "wifi" };   // IDF's own, never deleted
    for (size_t k = 0; k < sizeof SYSTEM / sizeof SYSTEM[0] && n < TASK_TABLE; k++) {
        TaskHandle_t h = xTaskGetHandle(SYSTEM[k]);
        if (!h) continue;
        snprintf(rows[n].name, sizeof rows[n].name, "%s", SYSTEM[k]); rows[n].stack = 0; rows[n].low = (uint32_t)uxTaskGetStackHighWaterMark(h); n++;
    }
    for (int i = 0; i < n; i++) printf("task %-16s stack %5s B, least free %5u B%s\n", rows[i].name, rows[i].stack ? (snprintf(sz, sizeof sz, "%u", (unsigned)rows[i].stack), sz) : "idf", (unsigned)rows[i].low, rows[i].low < 768 ? "  <-- tight" : "");
}

bool task_spawn_internal(const char *name, task_fn_t fn, void *arg, uint32_t stack_bytes, int priority, int core)
{
    if (!s_tasks_lock) s_tasks_lock = xSemaphoreCreateMutex();   // the first spawn is from app_main, before any other
    struct trampoline *t = malloc(sizeof *t);
    if (!t) return false;
    t->fn = fn; t->arg = arg; t->stack = stack_bytes;
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
void mutex_free(mutex_t m) { if (m) vSemaphoreDelete((SemaphoreHandle_t)m); }

queue_t queue_create(size_t item_size, size_t depth) { return (queue_t)xQueueCreate(depth, item_size); }
bool queue_send(queue_t q, const void *item, uint32_t timeout_ms) { return xQueueSend((QueueHandle_t)q, item, to_ticks(timeout_ms)) == pdTRUE; }
bool queue_recv(queue_t q, void *item, uint32_t timeout_ms) { return xQueueReceive((QueueHandle_t)q, item, to_ticks(timeout_ms)) == pdTRUE; }
size_t queue_count(queue_t q) { return uxQueueMessagesWaiting((QueueHandle_t)q); }
void queue_reset(queue_t q) { xQueueReset((QueueHandle_t)q); }
void queue_free(queue_t q) { if (q) vQueueDelete((QueueHandle_t)q); }

signal_t signal_create(void) { return (signal_t)xSemaphoreCreateBinary(); }
void signal_set(signal_t s) { xSemaphoreGive((SemaphoreHandle_t)s); }
void IRAM_ATTR signal_set_from_isr(signal_t s)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)s, &woken);
    if (woken) portYIELD_FROM_ISR();
}
bool signal_wait(signal_t s, uint32_t timeout_ms) { return xSemaphoreTake((SemaphoreHandle_t)s, to_ticks(timeout_ms)) == pdTRUE; }
void signal_free(signal_t s) { if (s) vSemaphoreDelete((SemaphoreHandle_t)s); }
