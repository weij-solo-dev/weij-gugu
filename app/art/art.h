// art.h — pictures that arrive as JPEGs become pixel art the scenes can use: a backdrop on the 116 grid
// with a small palette (and that palette shifted to the four times of day), a prop keyed out of its flat
// background into a sprite with transparency and a dark outline. Pure image work on PSRAM buffers plus one
// tiny file format under data/art/. The decoder is LVGL's bundled TJpgDec; the shrink happens while
// decoding, so a 1024×1024 JPEG never exists in memory at full size.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ART_GRID        116
#define ART_MAX_COLORS  32
#define ART_TRANSPARENT 255
#define ART_GROUND_TOP  98      // the strip under Gugu's feet starts on this row (the pigeon at (34, 46) is 54 rows tall)

typedef struct { uint8_t r, g, b; } art_rgb_t;
typedef enum { ART_TOD_NIGHT = 0, ART_TOD_SUNRISE, ART_TOD_DAY, ART_TOD_DUSK } art_tod_t;   // the same order as world_tod_t
typedef enum { ART_GROUND_PAVEMENT = 0, ART_GROUND_SAND, ART_GROUND_GRASS, ART_GROUND_STONE, ART_GROUND_SNOW, ART_GROUND_WOOD, ART_GROUND_COUNT } art_ground_t;

typedef struct {
    uint16_t w, h;
    uint8_t ncolors;
    bool alpha;              // ART_TRANSPARENT indices present (props)
    uint32_t sky_mask;       // bit i set: palette entry i is sky (backdrops); the time-of-day remap treats those apart
    art_rgb_t pal[ART_MAX_COLORS];
    uint8_t *idx;            // w*h palette indices, mem_alloc_big
} art_image_t;

typedef struct { int w, h, components; float coverage, fill; bool passed; } art_prop_stats_t;

// JPEG → RGB888 tw×th, area-averaged while decoding. Caller mem_release()s *rgb.
esp_err_t art_decode_jpeg(const char *path, int tw, int th, uint8_t **rgb);
// A scene JPEG → the 116 grid, at most 20 colours, the sky entries found, the ground strip drawn from row ART_GROUND_TOP.
esp_err_t art_import_backdrop(const char *jpeg_path, art_ground_t ground, art_image_t *out);
// A single object on a flat background → a sprite whose longer side is `longer` base pixels: keyed out from the
// corners, cropped, at most 10 colours, a 1-px dark outline. `stats` tells whether it passed the quality gate.
esp_err_t art_import_prop(const char *jpeg_path, int longer, art_image_t *out, art_prop_stats_t *stats);
art_ground_t art_ground_parse(const char *name);     // "sand" … ; anything else is pavement
const char  *art_ground_name(art_ground_t g);

esp_err_t art_save(const art_image_t *img, const char *path);   // creates the directory; tmp + rename
esp_err_t art_load(const char *path, art_image_t *out);         // idx in PSRAM; art_free() it
void      art_free(art_image_t *img);

// The palette at a time of day: sky entries become that sky (brightness order kept), the rest is tinted and dimmed.
void art_palette_tod(const art_image_t *img, art_tod_t tod, art_rgb_t out[ART_MAX_COLORS]);
// Nearest-neighbour ×scale into RGB565 little-endian, plus an alpha plane when the image has transparency (may be NULL otherwise).
void art_render_rgb565(const art_image_t *img, const art_rgb_t *pal, int scale, uint16_t *rgb565, uint8_t *alpha);

// The prop catalogue (content/props/catalogue.json): what to ask the image model for, how big to make it, what it is.
typedef struct { char subject[96]; int size; char bg[12]; char kind[12]; bool builtin; } art_catalogue_t;   // kind: static | living | light
bool art_catalogue_lookup(const char *content_dir, const char *id, art_catalogue_t *out);   // an unknown id gets a guess from the id itself
size_t art_catalogue_style(const char *content_dir, const char *which, char *dst, size_t cap);   // "backdrop" | "prop": the prompt suffix; {bg} in the prop one

// Quality gate shared by the import and the tests.
bool art_prop_passes(const art_prop_stats_t *s);
