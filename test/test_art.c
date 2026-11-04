// test_art.c — the generated-art pipeline on fixtures: a backdrop shrinks to the grid with a small palette
// and a sky, props key out of their background into outlined sprites, palettes shift with the time of day,
// and the file format round-trips.
#include "minitest.h"
#include "art/art.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FIX GUGU_REPO_ROOT "/test/fixtures/art/"
#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"

static int lum(art_rgb_t c) { return (c.r * 299 + c.g * 587 + c.b * 114) / 1000; }

TEST(test_art_backdrop_import)
{
    uint8_t *rgb;
    CHECK(art_decode_jpeg(FIX "backdrop_giza.jpg", 116, 116, &rgb) == ESP_OK);
    if (!rgb) return;
    int sky_b = rgb[(5 * 116 + 58) * 3 + 2], sky_r = rgb[(5 * 116 + 58) * 3];
    int sand_b = rgb[(100 * 116 + 58) * 3 + 2], sand_r = rgb[(100 * 116 + 58) * 3];
    printf("  sky pixel r %d b %d, ground pixel r %d b %d\n", sky_r, sky_b, sand_r, sand_b);
    CHECK(sky_b > sky_r);          // blue sky at the top
    CHECK(sand_r > sand_b);        // sand at the bottom
    mem_release(rgb);

    art_image_t bg;
    CHECK(art_import_backdrop(FIX "backdrop_giza.jpg", ART_GROUND_SAND, &bg) == ESP_OK);
    CHECK_EQ_INT(bg.w, 116); CHECK_EQ_INT(bg.h, 116);
    CHECK(bg.ncolors >= 12 && bg.ncolors <= 23);   // 20 from the picture plus up to 3 for the ground
    CHECK(bg.sky_mask != 0);
    uint8_t top = bg.idx[5 * 116 + 58];
    CHECK((bg.sky_mask >> top) & 1u);              // the pixel at the top is a sky entry
    uint8_t ground = bg.idx[110 * 116 + 10];
    CHECK(!((bg.sky_mask >> ground) & 1u));
    CHECK(bg.idx[ART_GROUND_TOP * 116 + 3] != bg.idx[(ART_GROUND_TOP + 5) * 116 + 3] || bg.ncolors == ART_MAX_COLORS);   // edge line vs body
    printf("  backdrop: %d colours, sky mask %08lx\n", bg.ncolors, (unsigned long)bg.sky_mask);
    // the four times of day
    art_rgb_t day[ART_MAX_COLORS], night[ART_MAX_COLORS], dusk[ART_MAX_COLORS], sunrise[ART_MAX_COLORS];
    art_palette_tod(&bg, ART_TOD_DAY, day); art_palette_tod(&bg, ART_TOD_NIGHT, night); art_palette_tod(&bg, ART_TOD_DUSK, dusk); art_palette_tod(&bg, ART_TOD_SUNRISE, sunrise);
    CHECK(memcmp(day, bg.pal, sizeof day) == 0);
    CHECK(lum(night[top]) < lum(day[top]) / 3);        // the night sky is dark
    CHECK(night[top].b > night[top].r);                 // and blue
    CHECK(dusk[top].r > dusk[top].b);                   // dusk sky is warm
    CHECK(lum(night[ground]) < lum(day[ground]));       // the ground dims at night
    CHECK(lum(night[ground]) > 25);                     // but stays visible
    printf("  sky day (%d,%d,%d) night (%d,%d,%d) dusk (%d,%d,%d) sunrise (%d,%d,%d)\n", day[top].r, day[top].g, day[top].b, night[top].r, night[top].g, night[top].b,
           dusk[top].r, dusk[top].g, dusk[top].b, sunrise[top].r, sunrise[top].g, sunrise[top].b);
    // file round trip
    CHECK(art_save(&bg, TEST_DATA "/art/t-test/backdrop.art") == ESP_OK);
    art_image_t back;
    CHECK(art_load(TEST_DATA "/art/t-test/backdrop.art", &back) == ESP_OK);
    CHECK_EQ_INT(back.w, bg.w); CHECK_EQ_INT(back.ncolors, bg.ncolors); CHECK(back.sky_mask == bg.sky_mask); CHECK(!back.alpha);
    CHECK(memcmp(back.idx, bg.idx, 116 * 116) == 0); CHECK(memcmp(back.pal, bg.pal, sizeof bg.pal) == 0);
    // rendering ×4 into RGB565
    uint16_t *px = mem_alloc_big(464 * 464 * 2);
    art_render_rgb565(&back, day, 4, px, NULL);
    art_rgb_t c = day[top];
    uint16_t expect = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    CHECK_EQ_INT(px[(5 * 4 + 2) * 464 + 58 * 4 + 1], expect);
    mem_release(px);
    art_free(&back);
    // A backdrop saved before the ground line moved up (2026-11-04: row 98 → ART_GROUND_TOP) is moved on load, so the
    // trip under way keeps its picture: the same image cut for the old line, saved as a version-1 file, loads as bg.
    {
        art_image_t old = bg; old.idx = mem_alloc_big(116 * 116);
        int d = 98 - ART_GROUND_TOP;
        for (int y = 0; y < 116; y++) memcpy(old.idx + y * 116, bg.idx + (y < d ? 0 : y - d) * 116, 116);
        CHECK(art_save(&old, TEST_DATA "/art/t-test/old.art") == ESP_OK);
        size_t n; char *f = file_read_all(TEST_DATA "/art/t-test/old.art", &n);
        CHECK(f && n > 8 && f[4] == 2 && (uint8_t)f[7] == ART_GROUND_TOP);   // new files carry their line (version 2)
        if (f) { f[4] = 1; f[7] = 0; atomic_file_write(TEST_DATA "/art/t-test/old.art", f, n); mem_release(f); }   // an old file did not
        art_image_t moved;
        CHECK(art_load(TEST_DATA "/art/t-test/old.art", &moved) == ESP_OK);
        CHECK(memcmp(moved.idx, bg.idx, (size_t)(116 - d) * 116) == 0);       // everything above the new bottom rows is in place
        CHECK(moved.idx[ART_GROUND_TOP * 116 + 3] == bg.idx[ART_GROUND_TOP * 116 + 3]);   // the strip's edge on the new line
        CHECK(memcmp(moved.idx + (116 - d) * 116, moved.idx + (116 - d - 8) * 116, (size_t)d * 116) == 0);   // the freed rows repeat the strip
        art_free(&moved); art_free(&old);
    }
    art_free(&bg);
    CHECK(art_load(TEST_DATA "/art/t-test/missing.art", &back) == ESP_ERR_NOT_FOUND);
}

static void prop_case(const char *file, int longer, art_prop_stats_t *st, art_image_t *img)
{
    CHECK(art_import_prop(file, longer, img, st) == ESP_OK);
    printf("  %s → %dx%d, %d colours, %d piece(s), coverage %.3f, fill %.2f, %s\n", strrchr(file, '/') + 1, st->w, st->h, img->ncolors, st->components,
           (double)st->coverage, (double)st->fill, st->passed ? "passed" : "FAILED");
}

TEST(test_art_prop_import)
{
    art_prop_stats_t st; art_image_t lamp, camel, gull;
    prop_case(FIX "prop_lamp.jpg", 40, &st, &lamp);
    CHECK_EQ_INT(st.h, 42); CHECK(st.w < 22); CHECK(st.passed); CHECK(lamp.alpha);   // 40 tall plus the one-pixel frame the outline sits in
    // an outline: the entry of the line colour exists and surrounds opaque pixels
    int line = -1; for (int i = 0; i < lamp.ncolors; i++) if (lamp.pal[i].r == 0x2B && lamp.pal[i].g == 0x36 && lamp.pal[i].b == 0x42) line = i;
    CHECK(line >= 0);
    int outline_px = 0, opaque_px = 0, transparent_px = 0;
    for (int i = 0; i < lamp.w * lamp.h; i++) { if (lamp.idx[i] == line) outline_px++; else if (lamp.idx[i] == ART_TRANSPARENT) transparent_px++; else opaque_px++; }
    CHECK(outline_px > 10 && opaque_px > 50 && transparent_px > 50);
    // the corners are transparent (the keying worked)
    CHECK_EQ_INT(lamp.idx[0], ART_TRANSPARENT); CHECK_EQ_INT(lamp.idx[lamp.w - 1], ART_TRANSPARENT);
    prop_case(FIX "prop_camel.jpg", 26, &st, &camel);
    CHECK_EQ_INT(st.w, 28); CHECK(st.passed);
    prop_case(FIX "prop_seagull.jpg", 18, &st, &gull);   // white on white: keep whatever the gate says, but it must not crash or pass with nothing
    CHECK(st.h == 20 || st.w == 20);
    // save / load keeps the alpha
    CHECK(art_save(&camel, TEST_DATA "/art/t-test/camel.art") == ESP_OK);
    art_image_t back; CHECK(art_load(TEST_DATA "/art/t-test/camel.art", &back) == ESP_OK);
    CHECK(back.alpha); CHECK_EQ_INT(back.w, camel.w); CHECK(memcmp(back.idx, camel.idx, (size_t)camel.w * camel.h) == 0);
    uint16_t *px = mem_alloc_big((size_t)back.w * 4 * back.h * 4 * 2); uint8_t *al = mem_alloc_big((size_t)back.w * 4 * back.h * 4);
    art_render_rgb565(&back, back.pal, 4, px, al);
    CHECK_EQ_INT(al[0], 0);      // a transparent corner
    int opaque_count = 0; for (size_t i = 0; i < (size_t)back.w * 4 * back.h * 4; i++) opaque_count += al[i] == 255;
    CHECK(opaque_count > 1000);
    mem_release(px); mem_release(al);
    art_free(&lamp); art_free(&camel); art_free(&gull); art_free(&back);
}

TEST(test_art_catalogue_and_ground)
{
    art_catalogue_t e;
    CHECK(art_catalogue_lookup(GUGU_REPO_ROOT "/content", "lamp", &e));
    CHECK_EQ_INT(e.size, 40); CHECK_EQ_STR(e.kind, "light"); CHECK(strstr(e.subject, "lamp") != NULL);
    CHECK(art_catalogue_lookup(GUGU_REPO_ROOT "/content", "seagull", &e)); CHECK_EQ_STR(e.bg, "green"); CHECK_EQ_STR(e.kind, "living");
    CHECK(art_catalogue_lookup(GUGU_REPO_ROOT "/content", "suitcase", &e)); CHECK(e.builtin);
    CHECK(!art_catalogue_lookup(GUGU_REPO_ROOT "/content", "bamboo_raft", &e));   // invented by the model: still usable
    CHECK_EQ_STR(e.subject, "a bamboo raft"); CHECK_EQ_INT(e.size, 24);
    CHECK_EQ_INT(art_ground_parse("sand"), ART_GROUND_SAND); CHECK_EQ_INT(art_ground_parse("wood"), ART_GROUND_WOOD); CHECK_EQ_INT(art_ground_parse("lava"), ART_GROUND_PAVEMENT);
    CHECK_EQ_STR(art_ground_name(ART_GROUND_GRASS), "grass");
    char style[300]; CHECK(art_catalogue_style(GUGU_REPO_ROOT "/content", "prop", style, sizeof style) > 0); CHECK(strstr(style, "{bg}") != NULL);
    CHECK(art_catalogue_style(GUGU_REPO_ROOT "/content", "backdrop", style, sizeof style) > 0); CHECK(strstr(style, "no animals") != NULL);
}

void test_art(void)
{
    RUN(test_art_backdrop_import);
    RUN(test_art_prop_import);
    RUN(test_art_catalogue_and_ground);
}
