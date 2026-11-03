// art.c — the façade: importing backdrops and props, the ground strip, time-of-day palettes, the file
// format, rendering to RGB565, the prop catalogue.
#define LOG_TAG "art"
#include "art_internal.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>

static const art_rgb_t LINE = { 0x2B, 0x36, 0x42 };   // pixelgen's outline colour

// ---- ground strips: body, speckle, edge ----
static const struct { const char *name; art_rgb_t body, speck, edge; } GROUND[ART_GROUND_COUNT] = {
    { "pavement", { 178, 172, 164 }, { 160, 154, 146 }, { 112, 106, 100 } },
    { "sand",     { 232, 206, 150 }, { 214, 186, 128 }, { 166, 138, 86 } },
    { "grass",    { 122, 170, 92 },  { 104, 150, 78 },  { 70, 104, 52 } },
    { "stone",    { 160, 150, 136 }, { 142, 132, 118 }, { 96, 88, 78 } },
    { "snow",     { 236, 240, 246 }, { 218, 224, 234 }, { 168, 178, 196 } },
    { "wood",     { 190, 140, 90 },  { 168, 120, 74 },  { 112, 76, 44 } },
};

art_ground_t art_ground_parse(const char *name)
{
    for (int g = 0; name && g < ART_GROUND_COUNT; g++) if (!strcmp(name, GROUND[g].name)) return (art_ground_t)g;
    return ART_GROUND_PAVEMENT;
}
const char *art_ground_name(art_ground_t g) { return g < ART_GROUND_COUNT ? GROUND[g].name : "pavement"; }

static uint8_t nearest_entry(const art_image_t *img, art_rgb_t c)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < img->ncolors; i++) {
        int d = abs(img->pal[i].r - c.r) + abs(img->pal[i].g - c.g) + abs(img->pal[i].b - c.b);
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

static uint8_t add_entry(art_image_t *img, art_rgb_t c)
{
    if (img->ncolors < ART_MAX_COLORS) { img->pal[img->ncolors] = c; return img->ncolors++; }
    return nearest_entry(img, c);
}

static void draw_ground(art_image_t *img, art_ground_t g)
{
    if (g >= ART_GROUND_COUNT) g = ART_GROUND_PAVEMENT;
    uint8_t body = add_entry(img, GROUND[g].body), speck = add_entry(img, GROUND[g].speck), edge = add_entry(img, GROUND[g].edge);
    uint32_t seed = 3;
    for (int y = ART_GROUND_TOP; y < img->h; y++)
        for (int x = 0; x < img->w; x++) {
            uint8_t v = body;
            if (y == ART_GROUND_TOP) v = edge;
            else if (g == ART_GROUND_WOOD && (y - ART_GROUND_TOP) % 4 == 0) v = edge;
            else { seed = seed * 1103515245u + 12345u; if (((seed >> 16) & 0xFF) < 10) v = speck; }
            img->idx[(size_t)y * img->w + x] = v;
        }
}

// ---- imports ----
esp_err_t art_import_backdrop(const char *jpeg_path, art_ground_t ground, art_image_t *out)
{
    memset(out, 0, sizeof *out);
    uint8_t *rgb;
    esp_err_t err = art_decode_jpeg(jpeg_path, ART_GRID, ART_GRID, &rgb);
    if (err != ESP_OK) return err;
    size_t n = (size_t)ART_GRID * ART_GRID;
    art_saturate(rgb, n, 1.15f);
    out->w = out->h = ART_GRID;
    out->idx = mem_alloc_big(n);
    if (!out->idx) { mem_release(rgb); return ESP_ERR_NO_MEM; }
    out->ncolors = (uint8_t)art_quantize(rgb, NULL, n, 20, out->pal, out->idx);
    mem_release(rgb);
    out->sky_mask = art_sky_mask(out);
    draw_ground(out, ground);
    LOGI("backdrop %s: %d colours, sky mask %08lx, ground %s", jpeg_path, out->ncolors, (unsigned long)out->sky_mask, art_ground_name(ground));
    return ESP_OK;
}

bool art_prop_passes(const art_prop_stats_t *s) { return s->coverage >= 0.02f && s->components <= 3 && s->fill >= 0.15f && s->fill <= 0.90f; }

#define PROP_SRC 256   // the keying happens on a 256-grid shrink of the picture

esp_err_t art_import_prop(const char *jpeg_path, int longer, art_image_t *out, art_prop_stats_t *stats)
{
    memset(out, 0, sizeof *out); memset(stats, 0, sizeof *stats);
    if (longer < 4) longer = 4;
    if (longer > 64) longer = 64;
    uint8_t *rgb;
    esp_err_t err = art_decode_jpeg(jpeg_path, PROP_SRC, PROP_SRC, &rgb);
    if (err != ESP_OK) return err;
    uint8_t *mask = mem_alloc_big((size_t)PROP_SRC * PROP_SRC);
    if (!mask) { mem_release(rgb); return ESP_ERR_NO_MEM; }
    stats->coverage = art_keyout(rgb, PROP_SRC, PROP_SRC, 48, mask);
    // the bounding box of what stayed
    int x0 = PROP_SRC, y0 = PROP_SRC, x1 = -1, y1 = -1;
    for (int y = 0; y < PROP_SRC; y++) for (int x = 0; x < PROP_SRC; x++) if (mask[y * PROP_SRC + x]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x1 < 0) { mem_release(rgb); mem_release(mask); return ESP_ERR_NOT_FOUND; }
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    float scale = (float)longer / (float)(bw > bh ? bw : bh);
    int w = (int)lroundf(bw * scale), h = (int)lroundf(bh * scale);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    // area average of the kept pixels into each cell; a cell is opaque when at least half of it was kept
    size_t n = (size_t)w * h;
    uint8_t *cell = mem_alloc_big(n * 3), *opaque = mem_alloc_big(n);
    out->idx = mem_alloc_big(n);
    if (!cell || !opaque || !out->idx) { mem_release(rgb); mem_release(mask); mem_release(cell); mem_release(opaque); mem_release(out->idx); out->idx = NULL; return ESP_ERR_NO_MEM; }
    for (int cy = 0; cy < h; cy++) for (int cx = 0; cx < w; cx++) {
        int sx0 = x0 + (int)((float)cx * bw / w), sx1 = x0 + (int)((float)(cx + 1) * bw / w), sy0 = y0 + (int)((float)cy * bh / h), sy1 = y0 + (int)((float)(cy + 1) * bh / h);
        if (sx1 <= sx0) sx1 = sx0 + 1;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        uint32_t sr = 0, sg = 0, sb = 0, kept = 0, total = 0;
        for (int y = sy0; y < sy1 && y <= y1; y++) for (int x = sx0; x < sx1 && x <= x1; x++) {
            total++;
            if (!mask[y * PROP_SRC + x]) continue;
            const uint8_t *p = rgb + ((size_t)y * PROP_SRC + x) * 3;
            sr += p[0]; sg += p[1]; sb += p[2]; kept++;
        }
        size_t i = (size_t)cy * w + cx;
        opaque[i] = total && kept * 2 >= total;
        if (kept) { cell[i * 3] = (uint8_t)(sr / kept); cell[i * 3 + 1] = (uint8_t)(sg / kept); cell[i * 3 + 2] = (uint8_t)(sb / kept); }
    }
    mem_release(rgb); mem_release(mask);
    uint8_t *small = out->idx;                       // quantise on the tight grid, then pad by one pixel so the outline fits
    int nc = art_quantize(cell, opaque, n, 10, out->pal, small);
    mem_release(cell);
    size_t op = 0; for (size_t i = 0; i < n; i++) op += opaque[i];
    mem_release(opaque);
    out->w = (uint16_t)(w + 2); out->h = (uint16_t)(h + 2); out->alpha = true; out->ncolors = (uint8_t)nc;
    out->idx = mem_alloc_big((size_t)out->w * out->h);
    if (!out->idx) { mem_release(small); return ESP_ERR_NO_MEM; }
    memset(out->idx, ART_TRANSPARENT, (size_t)out->w * out->h);
    for (int y = 0; y < h; y++) memcpy(out->idx + (size_t)(y + 1) * out->w + 1, small + (size_t)y * w, (size_t)w);
    mem_release(small);
    stats->w = out->w; stats->h = out->h; stats->fill = n ? (float)op / (float)n : 0;
    stats->components = art_components(out);
    uint8_t line = add_entry(out, LINE);
    art_outline(out, line);
    stats->passed = art_prop_passes(stats);
    LOGI("prop %s: %dx%d, %d colours, %d piece%s, coverage %.2f, fill %.2f → %s", jpeg_path, w, h, out->ncolors, stats->components,
         stats->components == 1 ? "" : "s", (double)stats->coverage, (double)stats->fill, stats->passed ? "ok" : "FAILED the gate");
    return ESP_OK;
}

// ---- time of day ----
static art_rgb_t hsv(float h, float s, float v)
{
    h = fmodf(h, 1.0f); if (h < 0) h += 1.0f; if (v > 1) v = 1;
    float i = floorf(h * 6), f = h * 6 - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch ((int)i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                          case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; break; }
    return (art_rgb_t){ (uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255) };
}

void art_palette_tod(const art_image_t *img, art_tod_t tod, art_rgb_t out[ART_MAX_COLORS])
{
    memcpy(out, img->pal, sizeof img->pal);
    if (tod == ART_TOD_DAY) return;
    const art_rgb_t tint = tod == ART_TOD_NIGHT ? (art_rgb_t){ 18, 26, 64 } : tod == ART_TOD_DUSK ? (art_rgb_t){ 255, 140, 60 } : (art_rgb_t){ 255, 205, 175 };
    const float k = tod == ART_TOD_NIGHT ? 0.62f : tod == ART_TOD_DUSK ? 0.30f : 0.28f;
    const float dim = tod == ART_TOD_NIGHT ? 0.50f : tod == ART_TOD_DUSK ? 0.92f : 1.0f;   // night keeps outlines visible (0.42 in the eval was too dark)
    for (int i = 0; i < img->ncolors; i++) {
        art_rgb_t c = img->pal[i];
        if (img->sky_mask & (1u << i)) {
            float v = (c.r * 299 + c.g * 587 + c.b * 114) / 255000.0f;
            if (tod == ART_TOD_NIGHT) out[i] = hsv(0.64f, 0.70f, 0.10f + 0.22f * v);
            else if (tod == ART_TOD_DUSK) out[i] = hsv(0.04f + 0.06f * v, 0.72f, 0.60f + 0.38f * v);
            else out[i] = hsv(0.93f + 0.05f * v, 0.28f, 0.80f + 0.20f * v);
        } else {
            out[i].r = (uint8_t)(((1 - k) * c.r + k * tint.r) * dim);
            out[i].g = (uint8_t)(((1 - k) * c.g + k * tint.g) * dim);
            out[i].b = (uint8_t)(((1 - k) * c.b + k * tint.b) * dim);
        }
    }
}

// ---- rendering ----
void art_render_rgb565(const art_image_t *img, const art_rgb_t *pal, int scale, uint16_t *rgb565, uint8_t *alpha)
{
    int W = img->w * scale;
    for (int y = 0; y < img->h; y++) {
        uint16_t *row = rgb565 + (size_t)y * scale * W;
        uint8_t *arow = alpha ? alpha + (size_t)y * scale * W : NULL;
        for (int x = 0; x < img->w; x++) {
            uint8_t i = img->idx[(size_t)y * img->w + x];
            uint16_t v = 0; uint8_t a = 0;
            if (i != ART_TRANSPARENT && i < ART_MAX_COLORS) { art_rgb_t c = pal[i]; v = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3)); a = 255; }
            for (int k = 0; k < scale; k++) { row[x * scale + k] = v; if (arow) arow[x * scale + k] = a; }
        }
        for (int k = 1; k < scale; k++) { memcpy(row + (size_t)k * W, row, (size_t)W * 2); if (arow) memcpy(arow + (size_t)k * W, arow, (size_t)W); }
    }
}

// ---- files ----
#define ART_MAGIC "GART"
typedef struct __attribute__((packed)) { char magic[4]; uint8_t version, ncolors, flags, pad; uint16_t w, h; uint32_t sky_mask; } art_header_t;

static void mkdir_parent(const char *path)
{
    char tmp[200]; snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

esp_err_t art_save(const art_image_t *img, const char *path)
{
    size_t n = (size_t)img->w * img->h, pal_bytes = (size_t)img->ncolors * 3, total = sizeof(art_header_t) + pal_bytes + n;
    uint8_t *buf = mem_alloc_big(total);
    if (!buf) return ESP_ERR_NO_MEM;
    art_header_t h; memset(&h, 0, sizeof h);
    memcpy(h.magic, ART_MAGIC, 4); h.version = 1; h.ncolors = img->ncolors; h.flags = img->alpha ? 1 : 0; h.w = img->w; h.h = img->h; h.sky_mask = img->sky_mask;
    memcpy(buf, &h, sizeof h);
    for (int i = 0; i < img->ncolors; i++) { buf[sizeof h + i * 3] = img->pal[i].r; buf[sizeof h + i * 3 + 1] = img->pal[i].g; buf[sizeof h + i * 3 + 2] = img->pal[i].b; }
    memcpy(buf + sizeof h + pal_bytes, img->idx, n);
    mkdir_parent(path);
    bool ok = atomic_file_write(path, buf, total);
    mem_release(buf);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t art_load(const char *path, art_image_t *out)
{
    memset(out, 0, sizeof *out);
    size_t n; char *buf = file_read_all(path, &n);
    if (!buf) return ESP_ERR_NOT_FOUND;
    art_header_t h;
    if (n < sizeof h) { mem_release(buf); return ESP_ERR_INVALID_SIZE; }
    memcpy(&h, buf, sizeof h);
    size_t pixels = (size_t)h.w * h.h, pal_bytes = (size_t)h.ncolors * 3;
    if (memcmp(h.magic, ART_MAGIC, 4) || h.version != 1 || h.ncolors > ART_MAX_COLORS || n < sizeof h + pal_bytes + pixels) { mem_release(buf); return ESP_ERR_INVALID_RESPONSE; }
    out->w = h.w; out->h = h.h; out->ncolors = h.ncolors; out->alpha = h.flags & 1; out->sky_mask = h.sky_mask;
    for (int i = 0; i < h.ncolors; i++) out->pal[i] = (art_rgb_t){ (uint8_t)buf[sizeof h + i * 3], (uint8_t)buf[sizeof h + i * 3 + 1], (uint8_t)buf[sizeof h + i * 3 + 2] };
    out->idx = mem_alloc_big(pixels);
    if (!out->idx) { mem_release(buf); return ESP_ERR_NO_MEM; }
    memcpy(out->idx, buf + sizeof h + pal_bytes, pixels);
    mem_release(buf);
    return ESP_OK;
}

void art_free(art_image_t *img) { if (img->idx) mem_release(img->idx); img->idx = NULL; }

// ---- the catalogue ----
// The style suffixes the planner appends to every image prompt ("_styles" in catalogue.json): "backdrop" | "prop".
size_t art_catalogue_style(const char *content_dir, const char *which, char *dst, size_t cap)
{
    dst[0] = '\0';
    char p[200]; snprintf(p, sizeof p, "%s/props/catalogue.json", content_dir);
    size_t n; char *txt = file_read_all(p, &n);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    mem_release(txt);
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "_styles"), which));
    if (s) snprintf(dst, cap, "%s", s);
    cJSON_Delete(j);
    return strlen(dst);
}

bool art_catalogue_lookup(const char *content_dir, const char *id, art_catalogue_t *out)
{
    memset(out, 0, sizeof *out);
    char p[200]; snprintf(p, sizeof p, "%s/props/catalogue.json", content_dir);
    size_t n; char *txt = file_read_all(p, &n);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    mem_release(txt);
    const cJSON *e = cJSON_GetObjectItem(j, id);
    bool known = cJSON_IsObject(e);
    if (known) {
        const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(e, "subject")), *bg = cJSON_GetStringValue(cJSON_GetObjectItem(e, "bg")), *k = cJSON_GetStringValue(cJSON_GetObjectItem(e, "kind"));
        snprintf(out->subject, sizeof out->subject, "%s", s ? s : id);
        const cJSON *size = cJSON_GetObjectItem(e, "size");
        out->size = cJSON_IsNumber(size) ? (int)size->valuedouble : 24;
        snprintf(out->bg, sizeof out->bg, "%s", bg ? bg : "white");
        snprintf(out->kind, sizeof out->kind, "%s", k ? k : "static");
        out->builtin = cJSON_IsTrue(cJSON_GetObjectItem(e, "builtin"));
    } else {   // the model may name something outside the list: ask for it by name, medium size, on white
        snprintf(out->subject, sizeof out->subject, "a %s", id);
        for (char *c = out->subject; *c; c++) if (*c == '_') *c = ' ';
        out->size = 24; snprintf(out->bg, sizeof out->bg, "white"); snprintf(out->kind, sizeof out->kind, "static");
    }
    cJSON_Delete(j);
    return known;
}
