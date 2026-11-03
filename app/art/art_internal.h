// art_internal.h — private to the art module.
#pragma once
#include "art/art.h"

int      art_quantize(const uint8_t *rgb, const uint8_t *mask, size_t n, int ncolors, art_rgb_t *pal, uint8_t *idx);
uint32_t art_sky_mask(const art_image_t *img);
void     art_saturate(uint8_t *rgb, size_t n, float k);
float    art_keyout(const uint8_t *rgb, int w, int h, int tol, uint8_t *mask);
int      art_components(const art_image_t *img);
void     art_outline(art_image_t *img, uint8_t line_entry);
