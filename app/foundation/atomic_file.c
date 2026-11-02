#include "foundation/atomic_file.h"
#include "foundation/mem.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

bool atomic_file_write(const char *path, const void *data, size_t n)
{
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = (fflush(f) == 0) && ok;
    fsync(fileno(f));
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return false; }
    remove(path);                     // LittleFS rename does not overwrite on every port
    return rename(tmp, path) == 0;
}

char *file_read_all(const char *path, size_t *n_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = mem_alloc_big((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (n_out) *n_out = got;
    return buf;
}

bool file_append(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "ab");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}
