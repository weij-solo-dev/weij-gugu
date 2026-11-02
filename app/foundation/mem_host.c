#include "foundation/mem.h"
#include <stdlib.h>
#include <string.h>

void *mem_alloc_big(size_t bytes)
{
    size_t rounded = (bytes + 63) & ~(size_t)63;
    void *p = NULL;
    if (posix_memalign(&p, 64, rounded) != 0) return NULL;
    memset(p, 0, rounded);
    return p;
}

void *mem_alloc_dma(size_t bytes) { return calloc(1, bytes); }
void mem_release(void *p) { free(p); }
size_t mem_avail_internal(void) { return 300 * 1024; }     // the host has no budget; report a constant
size_t mem_min_avail_internal(void) { return 300 * 1024; }
size_t mem_avail_big(void) { return 8u * 1024 * 1024; }

#include <stdio.h>
void mem_print_stats(void) { printf("host build: memory is unbounded here; measure on the device\n"); }

