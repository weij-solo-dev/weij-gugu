#include "foundation/mem.h"
#include "esp_heap_caps.h"
#include <string.h>

void *mem_alloc_big(size_t bytes)
{
    size_t rounded = (bytes + 63) & ~(size_t)63;
    void *p = heap_caps_aligned_alloc(64, rounded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memset(p, 0, rounded);
    return p;
}

void *mem_alloc_dma(size_t bytes)
{
    return heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

void mem_release(void *p) { heap_caps_free(p); }
size_t mem_avail_internal(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
size_t mem_min_avail_internal(void) { return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL); }
size_t mem_avail_big(void) { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

#include <stdio.h>
void mem_print_stats(void)
{
    static const struct { const char *name; uint32_t caps; } regions[] = {
        { "internal", MALLOC_CAP_INTERNAL }, { "internal+dma", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA }, { "psram", MALLOC_CAP_SPIRAM },
    };
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        multi_heap_info_t info;
        heap_caps_get_info(&info, regions[i].caps);
        printf("%-13s free %7u B  largest %7u B  min %7u B  allocated %7u B in %u blocks\n", regions[i].name,
               (unsigned)info.total_free_bytes, (unsigned)info.largest_free_block, (unsigned)info.minimum_free_bytes,
               (unsigned)info.total_allocated_bytes, (unsigned)info.allocated_blocks);
    }
}

