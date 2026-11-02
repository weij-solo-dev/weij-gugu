// foundation/atomic_file.h — write-whole-file-then-rename, so a power cut leaves the old file intact
// (CLAUDE.md rule 13). Works on LittleFS and on the host filesystem.
#pragma once
#include <stddef.h>
#include <stdbool.h>

bool atomic_file_write(const char *path, const void *data, size_t n);
// Reads a whole file into a mem_alloc_big buffer (NUL-terminated); caller mem_release()s. NULL if missing.
char *file_read_all(const char *path, size_t *n_out);
bool file_append(const char *path, const void *data, size_t n);   // for JSONL logs
bool file_exists(const char *path);
