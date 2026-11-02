// foundation/mem.h — where big buffers live (CLAUDE.md rule 9). On the device "big" means PSRAM and
// "dma" means internal DMA-capable RAM; on the host both are plain aligned allocations.
#pragma once
#include <stddef.h>

// Place a large static array in PSRAM .bss on the device (nothing on the host).
#ifdef GUGU_HOST
#define MEM_BIG_BSS
#else
#include "esp_attr.h"
#define MEM_BIG_BSS EXT_RAM_BSS_ATTR
#endif

void  *mem_alloc_big(size_t bytes);     // 64-byte aligned, zeroed; NULL on failure
void  *mem_alloc_dma(size_t bytes);     // internal, DMA-capable; zeroed
void   mem_release(void *p);
void   mem_print_stats(void);          // heap totals, largest free block, low-water marks (device: per region)
size_t mem_avail_internal(void);
size_t mem_min_avail_internal(void);
size_t mem_avail_big(void);
