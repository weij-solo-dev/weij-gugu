// console_host.c — the same commands on the simulator's stdin.
#define LOG_TAG "console"
#include "console/console.h"
#include "foundation/log.h"
#include "foundation/task.h"
#include <stdio.h>
#include <unistd.h>

int console_dispatch(char *line);

static void console_task(void *arg)
{
    (void)arg;
    char line[256];
    while (fgets(line, sizeof line, stdin)) { console_dispatch(line); printf("gugu> "); fflush(stdout); }
}

esp_err_t console_init(void)
{
    if (!isatty(STDIN_FILENO)) return ESP_OK;     // replay / CI runs have no terminal
    if (!task_spawn("gugu_console", console_task, NULL, 65536, 2, 0)) return ESP_FAIL;
    LOGI("console ready on stdin (type help)");
    return ESP_OK;
}
