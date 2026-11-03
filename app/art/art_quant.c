// art_quant.c — the pixel work: median-cut palettes, the sky test, keying a prop out of its background,
// outlines and connected pieces. Everything here works on small RGB888 or index buffers in PSRAM.
#define LOG_TAG "art"
#include "art_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include <string.h>
#include <stdlib.h>

// ---- median cut ----
typedef struct { uint32_t start, count; } box_t;
static const uint8_t *q_rgb; static int q_chan;
static int q_cmp(const void *a, const void *b)
{
    int va = q_rgb[*(const uint32_t *)a * 3 + q_chan], vb = q_rgb[*(const uint32_t *)b * 3 + q_chan];
    return va - vb;
}

static void box_range(const uint8_t *rgb, const uint32_t *list, const box_t *b, int *chan, int *range)
{
    int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < b->count; i++) {
        const uint8_t *p = rgb + (size_t)list[b->start + i] * 3;
        for (int c = 0; c < 3; c++) { if (p[c] < lo[c]) lo[c] = p[c]; if (p[c] > hi[c]) hi[c] = p[c]; }
    }
    *chan = 0; *range = hi[0] - lo[0];
    for (int c = 1; c < 3; c++) if (hi[c] - lo[c] > *range) { *range = hi[c] - lo[c]; *chan = c; }
}

// Pixels with mask[i] != 0 (or all when mask is NULL) get a palette of at most `ncolors` entries; the rest ART_TRANSPARENT.
int art_quantize(const uint8_t *rgb, const uint8_t *mask, size_t n, int ncolors, art_rgb_t *pal, uint8_t *idx)
{
    uint32_t *list = mem_alloc_big(n * sizeof(uint32_t));
    if (!list) return 0;
    uint32_t m = 0;
    for (size_t i = 0; i < n; i++) { idx[i] = ART_TRANSPARENT; if (!mask || mask[i]) list[m++] = (uint32_t)i; }
    if (m == 0) { mem_release(list); return 0; }
    box_t boxes[ART_MAX_COLORS]; int nb = 1;
    boxes[0] = (box_t){ 0, m };
    while (nb < ncolors) {
        int best = -1, best_range = 0;
        for (int b = 0; b < nb; b++) {
            if (boxes[b].count < 2) continue;
            int chan, range; box_range(rgb, list, &boxes[b], &chan, &range);
            if (range > best_range) { best_range = range; best = b; }
        }
        if (best < 0 || best_range == 0) break;
        int chan, range; box_range(rgb, list, &boxes[best], &chan, &range);
        q_rgb = rgb; q_chan = chan;
        qsort(list + boxes[best].start, boxes[best].count, sizeof(uint32_t), q_cmp);
        uint32_t half = boxes[best].count / 2;
        boxes[nb] = (box_t){ boxes[best].start + half, boxes[best].count - half };
        boxes[best].count = half;
        nb++;
    }
    for (int b = 0; b < nb; b++) {
        uint32_t sr = 0, sg = 0, sb = 0;
        for (uint32_t i = 0; i < boxes[b].count; i++) { const uint8_t *p = rgb + (size_t)list[boxes[b].start + i] * 3; sr += p[0]; sg += p[1]; sb += p[2]; }
        uint32_t k = boxes[b].count ? boxes[b].count : 1;
        pal[b] = (art_rgb_t){ (uint8_t)(sr / k), (uint8_t)(sg / k), (uint8_t)(sb / k) };
        for (uint32_t i = 0; i < boxes[b].count; i++) idx[list[boxes[b].start + i]] = (uint8_t)b;
    }
    mem_release(list);
    return nb;
}

// Palette entries that live in the upper part of the picture: at least 1% of the pixels, 75% of them in the top 42% rows.
uint32_t art_sky_mask(const art_image_t *img)
{
    uint32_t total[ART_MAX_COLORS] = { 0 }, top[ART_MAX_COLORS] = { 0 };
    int top_rows = (int)(img->h * 0.42f);
    for (int y = 0; y < img->h; y++)
        for (int x = 0; x < img->w; x++) {
            uint8_t i = img->idx[(size_t)y * img->w + x];
            if (i >= ART_MAX_COLORS) continue;
            total[i]++; if (y < top_rows) top[i]++;
        }
    uint32_t mask = 0, all = (uint32_t)img->w * img->h;
    for (int i = 0; i < img->ncolors; i++) if (total[i] * 100 >= all && top[i] * 4 >= total[i] * 3) mask |= 1u << i;
    return mask;
}

void art_saturate(uint8_t *rgb, size_t n, float k)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = rgb + i * 3;
        float gray = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000.0f;
        for (int c = 0; c < 3; c++) { float v = gray + (p[c] - gray) * k; p[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
    }
}

// ---- keying a prop out of its flat background ----
// mask[i] = 1 for pixels that stay. Flood fills from the four corners over pixels within `tol` (sum of channel
// differences) of the corner colour. Returns the share of pixels kept.
float art_keyout(const uint8_t *rgb, int w, int h, int tol, uint8_t *mask)
{
    size_t n = (size_t)w * h;
    memset(mask, 1, n);
    const int corners[4] = { 0, w - 1, (h - 1) * w, (h - 1) * w + w - 1 };
    int bg[3] = { 0, 0, 0 };
    for (int k = 0; k < 4; k++) for (int c = 0; c < 3; c++) bg[c] += rgb[(size_t)corners[k] * 3 + c];
    for (int c = 0; c < 3; c++) bg[c] /= 4;
    uint32_t *queue = mem_alloc_big(n * sizeof(uint32_t));
    uint8_t *seen = mem_alloc_big(n);
    if (!queue || !seen) { mem_release(queue); mem_release(seen); return 1.0f; }
    size_t head = 0, tail = 0;
    for (int k = 0; k < 4; k++) if (!seen[corners[k]]) { seen[corners[k]] = 1; queue[tail++] = (uint32_t)corners[k]; }
    while (head < tail) {              // every pixel is queued at most once (seen is set when queued), so tail <= n
        uint32_t i = queue[head++];
        const uint8_t *p = rgb + (size_t)i * 3;
        int diff = abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]);
        if (diff >= tol) continue;     // the object: stays, and the fill does not pass through it
        mask[i] = 0;
        int x = (int)(i % (uint32_t)w), y = (int)(i / (uint32_t)w);
        const uint32_t nb[4] = { i - 1, i + 1, i - (uint32_t)w, i + (uint32_t)w };
        const bool ok[4] = { x > 0, x < w - 1, y > 0, y < h - 1 };
        for (int k = 0; k < 4; k++) if (ok[k] && !seen[nb[k]]) { seen[nb[k]] = 1; queue[tail++] = nb[k]; }
    }
    size_t kept = 0; for (size_t i = 0; i < n; i++) kept += mask[i];
    mem_release(queue); mem_release(seen);
    return (float)kept / (float)n;
}

// 4-connected opaque regions after one pixel of dilation (thin legs and handles still read as one object).
int art_components(const art_image_t *img)
{
    int w = img->w, h = img->h; size_t n = (size_t)w * h;
    uint8_t *m = mem_alloc_big(n), *seen = mem_alloc_big(n); uint32_t *q = mem_alloc_big(n * sizeof(uint32_t));
    if (!m || !seen || !q) { mem_release(m); mem_release(seen); mem_release(q); return 1; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        bool on = false;
        for (int dy = -1; dy <= 1 && !on; dy++) for (int dx = -1; dx <= 1 && !on; dx++) {
            if (dx && dy) continue;
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && yy >= 0 && xx < w && yy < h && img->idx[(size_t)yy * w + xx] != ART_TRANSPARENT) on = true;
        }
        m[(size_t)y * w + x] = on;
    }
    int count = 0;
    for (size_t s = 0; s < n; s++) {
        if (!m[s] || seen[s]) continue;
        count++; size_t head = 0, tail = 0; q[tail++] = (uint32_t)s; seen[s] = 1;
        while (head < tail) {
            uint32_t i = q[head++]; int x = (int)(i % (uint32_t)w), y = (int)(i / (uint32_t)w);
            const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4; k++) {
                int xx = x + nb[k][0], yy = y + nb[k][1];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                size_t j = (size_t)yy * w + xx;
                if (m[j] && !seen[j]) { seen[j] = 1; q[tail++] = (uint32_t)j; }
            }
        }
    }
    mem_release(m); mem_release(seen); mem_release(q);
    return count;
}

// Transparent pixels touching an opaque one become the outline entry.
void art_outline(art_image_t *img, uint8_t line_entry)
{
    int w = img->w, h = img->h;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        size_t i = (size_t)y * w + x;
        if (img->idx[i] != ART_TRANSPARENT) continue;
        #define SOLID(v) ((v) != ART_TRANSPARENT && (v) != line_entry && (v) != 200)   // 200 = marked in this pass: not a neighbour yet
        bool touch = (x > 0 && SOLID(img->idx[i - 1])) || (x < w - 1 && SOLID(img->idx[i + 1])) ||
                     (y > 0 && SOLID(img->idx[i - w])) || (y < h - 1 && SOLID(img->idx[i + w]));
        #undef SOLID
        if (touch) img->idx[i] = 200;     // marked, resolved below so a fresh outline pixel does not spread
    }
    for (size_t i = 0; i < (size_t)w * h; i++) if (img->idx[i] == 200) img->idx[i] = line_entry;
}
