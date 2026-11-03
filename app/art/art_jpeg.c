// art_jpeg.c — JPEG decoding with TJpgDec (the copy LVGL ships), shrinking to a small grid on the way out:
// every decoded pixel is added to the cell it lands in, so the memory cost is the target grid, not the picture.
#define LOG_TAG "art"
#include "art/art.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include "src/libs/tjpgd/tjpgd.h"
#include <string.h>
#include <stdlib.h>

#define WORK_BYTES 8192

struct dec {
    const uint8_t *data; size_t len, pos;
    int tw, th; uint16_t W, H;
    uint32_t *sum; uint16_t *cnt;
};

static size_t in_func(JDEC *jd, uint8_t *buf, size_t n)
{
    struct dec *d = jd->device;
    size_t left = d->len - d->pos;
    if (n > left) n = left;
    if (buf) memcpy(buf, d->data + d->pos, n);
    d->pos += n;
    return n;
}

static int out_func(JDEC *jd, void *bitmap, JRECT *r)   // JD_FORMAT 0: 24-bit pixels, which LVGL's build emits as B, G, R
{
    struct dec *d = jd->device;
    const uint8_t *p = bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        int cy = (int)((uint32_t)y * (uint32_t)d->th / d->H);
        for (int x = r->left; x <= r->right; x++, p += 3) {
            if (x >= d->W || y >= d->H) continue;              // the last MCU row/column may overhang the picture
            int cx = (int)((uint32_t)x * (uint32_t)d->tw / d->W);
            size_t c = (size_t)cy * (size_t)d->tw + (size_t)cx;
            d->sum[c * 3] += p[2]; d->sum[c * 3 + 1] += p[1]; d->sum[c * 3 + 2] += p[0];
            d->cnt[c]++;
        }
    }
    return 1;
}

esp_err_t art_decode_jpeg(const char *path, int tw, int th, uint8_t **rgb)
{
    *rgb = NULL;
    size_t n; char *data = file_read_all(path, &n);
    if (!data) return ESP_ERR_NOT_FOUND;
    void *work = malloc(WORK_BYTES);
    struct dec d = { .data = (const uint8_t *)data, .len = n, .tw = tw, .th = th };
    JDEC jd;
    esp_err_t err = ESP_OK;
    JRESULT r = jd_prepare(&jd, in_func, work, WORK_BYTES, &d);
    if (r != JDR_OK) { LOGW("jpeg %s: prepare failed (%d)", path, (int)r); err = ESP_ERR_INVALID_RESPONSE; goto out; }
    d.W = jd.width; d.H = jd.height;
    size_t cells = (size_t)tw * (size_t)th;
    d.sum = mem_alloc_big(cells * 3 * sizeof(uint32_t));
    d.cnt = mem_alloc_big(cells * sizeof(uint16_t));
    *rgb = mem_alloc_big(cells * 3);
    if (!d.sum || !d.cnt || !*rgb) { err = ESP_ERR_NO_MEM; goto out; }
    r = jd_decomp(&jd, out_func, 0);
    if (r != JDR_OK) { LOGW("jpeg %s: decode failed (%d)", path, (int)r); err = ESP_ERR_INVALID_RESPONSE; goto out; }
    for (size_t c = 0; c < cells; c++) {
        uint32_t k = d.cnt[c] ? d.cnt[c] : 1;
        (*rgb)[c * 3] = (uint8_t)(d.sum[c * 3] / k); (*rgb)[c * 3 + 1] = (uint8_t)(d.sum[c * 3 + 1] / k); (*rgb)[c * 3 + 2] = (uint8_t)(d.sum[c * 3 + 2] / k);
    }
    LOGI("jpeg %s: %ux%u → %dx%d", path, jd.width, jd.height, tw, th);
out:
    if (err != ESP_OK && *rgb) { mem_release(*rgb); *rgb = NULL; }
    mem_release(d.sum); mem_release(d.cnt);
    free(work); mem_release(data);
    return err;
}
