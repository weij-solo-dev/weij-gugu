#!/usr/bin/env python3
"""Export the generated pixel art as LVGL 9 image descriptors (C source).

Sprites (with transparency) become LV_COLOR_FORMAT_RGB565A8: w*h RGB565 little-endian pixels
followed by w*h alpha bytes. Scenes become LV_COLOR_FORMAT_RGB565. Output: app/ui/ui_sprites.c/.h.
Generated — do not edit by hand (CLAUDE.md, content work).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pixelgen as pg

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'app', 'ui')
EMOTIONS = ['neutral', 'happy', 'laughing', 'surprised', 'curious', 'shy', 'sad', 'sleepy', 'angry', 'scared', 'proud', 'dizzy', 'puff', 'wave', 'urge', 'urge_you', 'peek']   # ui_emotion_t order

def rgb565(c):
    r, g, b = c
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def emit_image(name, canvas, with_alpha):
    w, h = canvas.w, canvas.h
    px = bytearray(); alpha = bytearray()
    for y in range(h):
        for x in range(w):
            c = canvas.px[y][x]
            v = rgb565(c) if c is not None else 0
            px += bytes((v & 0xFF, v >> 8))
            alpha.append(0xFF if c is not None else 0x00)
    data = bytes(px) + (bytes(alpha) if with_alpha else b'')
    lines = [f'static const uint8_t {name}_map[] = {{']
    for i in range(0, len(data), 24):
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i+24]) + ',')
    lines.append('};')
    cf = 'LV_COLOR_FORMAT_RGB565A8' if with_alpha else 'LV_COLOR_FORMAT_RGB565'
    lines.append(f'''const lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = {cf},
    .header.flags = 0,
    .header.w = {w},
    .header.h = {h},
    .header.stride = {w * 2},
    .data_size = {len(data)},
    .data = {name}_map,
}};
''')
    return '\n'.join(lines)

def crop(canvas, bb):
    x0, y0, x1, y1 = bb
    c = pg.Canvas(x1 - x0 + 1, y1 - y0 + 1)
    for y in range(c.h):
        for x in range(c.w):
            c.px[y][x] = canvas.px[y0 + y][x0 + x]
    return c

def diff_bbox(a, b):
    xs = [x for y in range(a.h) for x in range(a.w) if a.px[y][x] != b.px[y][x]]
    ys = [y for y in range(a.h) for x in range(a.w) if a.px[y][x] != b.px[y][x]]
    return (min(xs), min(ys), max(xs), max(ys)) if xs else None

def covers(base, over, bb):
    """True if every opaque base pixel inside bb is opaque in over: the overlay can stand in for the base there."""
    x0, y0, x1, y1 = bb
    return all(over.px[y][x] is not None for y in range(y0, y1 + 1) for x in range(x0, x1 + 1) if base.px[y][x] is not None)

def same_region(a, b, bb):
    x0, y0, x1, y1 = bb
    return all(a.px[y][x] == b.px[y][x] for y in range(y0, y1 + 1) for x in range(x0, x1 + 1))

def main():
    import check_faces                                    # every lens holds one mark: refuse to export a face that breaks it
    bad = check_faces.problems()
    if bad: sys.exit('faces not exported:\n  ' + '\n  '.join(bad))
    images = []
    for e in EMOTIONS:
        images.append((f'spr_gugu_{e}', pg.gugu(e), True))
    for e in EMOTIONS:                                            # open-beak frames for speaking
        images.append((f'spr_gugu_{e}_open', pg.gugu(e, beak_open=True), True))
    images.append(('spr_gugu_blink', pg.gugu('neutral', eyes_closed=True), True))
    case = pg.Canvas(13, 24)
    pg.draw_case(case, 1, 8, color='red', handle_up=True)
    images.append(('spr_case_up', case, True))
    case2 = pg.Canvas(13, 24)
    pg.draw_case(case2, 1, 8, color='red', handle_up=False)
    images.append(('spr_case_down', case2, True))
    images.append(('scn_home_day', pg.room('home', night=False, moons=0), False))
    images.append(('scn_home_night', pg.room('home', night=True, moons=0), False))
    images.append(('scn_away_day', pg.room('away', night=False, moons=2), False))
    images.append(('scn_away_night', pg.room('away', night=True, moons=2), False))
    images.append(('scn_home_off', pg.room('home', night=False, moons=0, lamp_on=False), False))   # the lamp switched off
    # v1.1: built-in props (fallbacks behind generated ones), the window pane at four times of day, accessories
    for pid, fn in pg.PROPS.items():
        images.append((f'spr_prop_{pid}', fn(), True))
    for tod in ('night', 'sunrise', 'day', 'dusk'):
        images.append((f'spr_win_{tod}', pg.window(tod), True))
    images.append(('spr_acc_sun_hat', pg.acc_sun_hat(), True))
    images.append(('spr_acc_camera', pg.acc_camera(), True))
    for icon in ('setup', 'wifi', 'service', 'hello', 'idea'): images.append((f'spr_bubble_{icon}', pg.bubble(icon), True))
    glasses, hat_y, camera_y = [], [], []
    for e in EMOTIONS:
        pg.gugu(e); g = pg.LAST_GEOM
        gc, gx, gy = pg.acc_sunglasses(g['eye_boxes'], g['cx'], g['rx'])
        images.append((f'spr_acc_sunglasses_{e}', gc, True)); glasses.append((f'&spr_acc_sunglasses_{e}', gx, gy))
        hat_y.append(int(round(g['head_top'])) - 7); camera_y.append(int(round(g['cy'])) + 1)

    # Overlay patches: the pixels that change when the beak opens or the eyes close, cropped to their bounding
    # box, so ui_anim.c repaints a patch instead of the whole pigeon. A patch is only usable when it covers every
    # opaque base pixel in its box; otherwise the table holds NULL and the UI swaps the full sprite instead.
    beak = []
    for e in EMOTIONS:
        closed, opened = pg.gugu(e), pg.gugu(e, beak_open=True)
        bb = diff_bbox(closed, opened)
        if bb and covers(closed, opened, bb):
            images.append((f'spr_beak_{e}', crop(opened, bb), True))
            beak.append((f'&spr_beak_{e}', bb[0], bb[1]))
        else:
            beak.append(('NULL', 0, 0))
    neutral, blink = pg.gugu('neutral'), pg.gugu('neutral', eyes_closed=True)
    bb_eyes = diff_bbox(neutral, blink)
    blink_ok = bb_eyes is not None and covers(neutral, blink, bb_eyes)
    if blink_ok:
        images.append(('spr_eyes_blink', crop(blink, bb_eyes), True))
    blink_fits = [blink_ok and same_region(pg.gugu(e), neutral, bb_eyes) for e in EMOTIONS]

    c_parts = ['// Generated by assets/gen/export_lvgl.py — do not edit.', '#include "ui/ui_sprites.h"', '']
    h_parts = ['// Generated by assets/gen/export_lvgl.py — do not edit.', '#pragma once', '#include <stdbool.h>', '#include <stdint.h>', '#include "lvgl.h"', '',
               '#define UI_SPRITE_SCALE 4   // base pixel -> screen pixels (nearest neighbour)', '']
    total = 0
    for name, canvas, alpha in images:
        c_parts.append(emit_image(name, canvas, alpha))
        h_parts.append(f'extern const lv_image_dsc_t {name};')
        total += canvas.w * canvas.h * (3 if alpha else 2)
    h_parts.append('')
    h_parts.append('// Emotion sprites indexed by ui_emotion_t order: ' + ', '.join(EMOTIONS))
    h_parts.append(f'#define UI_EMOTION_COUNT {len(EMOTIONS)}')
    h_parts.append('extern const lv_image_dsc_t *const ui_emotion_sprites[UI_EMOTION_COUNT];')
    c_parts.append('const lv_image_dsc_t *const ui_emotion_sprites[UI_EMOTION_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'&spr_gugu_{e}' for e in EMOTIONS) + ',')
    c_parts.append('};')
    h_parts.append('extern const lv_image_dsc_t *const ui_emotion_sprites_open[UI_EMOTION_COUNT];   // same emotions, beak open')
    c_parts.append('const lv_image_dsc_t *const ui_emotion_sprites_open[UI_EMOTION_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'&spr_gugu_{e}_open' for e in EMOTIONS) + ',')
    c_parts.append('};')
    h_parts.append('')
    h_parts.append('// Overlay patches drawn over the base sprite at base-pixel offset (x, y); img NULL = no patch for that face.')
    h_parts.append('typedef struct { const lv_image_dsc_t *img; int16_t x, y; } ui_patch_t;')
    h_parts.append('extern const ui_patch_t ui_beak_patch[UI_EMOTION_COUNT];   // beak-open pixels per emotion')
    h_parts.append('extern const ui_patch_t ui_blink_patch;                     // eyes-closed pixels of the neutral face')
    h_parts.append('extern const bool ui_blink_fits[UI_EMOTION_COUNT];          // that face equals neutral under the blink patch')
    c_parts.append('const ui_patch_t ui_beak_patch[UI_EMOTION_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'{{ {img}, {x}, {y} }}' for img, x, y in beak) + ',')
    c_parts.append('};')
    if blink_ok:
        c_parts.append(f'const ui_patch_t ui_blink_patch = {{ &spr_eyes_blink, {bb_eyes[0]}, {bb_eyes[1]} }};')
    else:
        c_parts.append('const ui_patch_t ui_blink_patch = { NULL, 0, 0 };')
    c_parts.append('const bool ui_blink_fits[UI_EMOTION_COUNT] = { ' + ', '.join('true' if f else 'false' for f in blink_fits) + ' };')
    h_parts.append('')
    h_parts.append('// v1.1 object layer: built-in props by catalogue id, the window panes (art_tod_t order), accessory anchors per face.')
    h_parts.append('typedef struct { const char *id; const lv_image_dsc_t *img; } ui_prop_sprite_t;')
    h_parts.append('extern const ui_prop_sprite_t ui_prop_sprites[];')
    h_parts.append('extern const int ui_prop_sprite_count;')
    h_parts.append('extern const lv_image_dsc_t *const ui_window_sprites[4];      // night, sunrise, day, dusk')
    h_parts.append('extern const ui_patch_t ui_sunglasses_patch[UI_EMOTION_COUNT];   // dark lenses over that face\'s eyes')
    h_parts.append('extern const int16_t ui_hat_y[UI_EMOTION_COUNT];               // base y of the sun hat for that face (x = 12)')
    h_parts.append('extern const int16_t ui_camera_y[UI_EMOTION_COUNT];            // base y of the camera on the chest (x = 18)')
    c_parts.append('const ui_prop_sprite_t ui_prop_sprites[] = {')
    c_parts.append('    ' + ', '.join(f'{{ "{pid}", &spr_prop_{pid} }}' for pid in pg.PROPS) + ',')
    c_parts.append('};')
    c_parts.append(f'const int ui_prop_sprite_count = {len(pg.PROPS)};')
    c_parts.append('const lv_image_dsc_t *const ui_window_sprites[4] = { &spr_win_night, &spr_win_sunrise, &spr_win_day, &spr_win_dusk };')
    c_parts.append('const ui_patch_t ui_sunglasses_patch[UI_EMOTION_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'{{ {img}, {x}, {y} }}' for img, x, y in glasses) + ',')
    c_parts.append('};')
    c_parts.append('const int16_t ui_hat_y[UI_EMOTION_COUNT] = { ' + ', '.join(str(v) for v in hat_y) + ' };')
    c_parts.append('const int16_t ui_camera_y[UI_EMOTION_COUNT] = { ' + ', '.join(str(v) for v in camera_y) + ' };')
    print('beak patches:', ', '.join(f'{e}={"ok" if b[0] != "NULL" else "-"}' for e, b in zip(EMOTIONS, beak)))
    print('blink patch:', 'ok' if blink_ok else 'none', 'bbox', bb_eyes, 'fits:', ', '.join(e for e, f in zip(EMOTIONS, blink_fits) if f))
    os.makedirs(OUT_DIR, exist_ok=True)
    open(os.path.join(OUT_DIR, 'ui_sprites.c'), 'w').write('\n'.join(c_parts) + '\n')
    open(os.path.join(OUT_DIR, 'ui_sprites.h'), 'w').write('\n'.join(h_parts) + '\n')
    print(f'exported {len(images)} images, {total} bytes of pixel data')

if __name__ == '__main__':
    main()
