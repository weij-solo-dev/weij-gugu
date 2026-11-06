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
EMOTIONS = ['neutral', 'happy', 'laughing', 'surprised', 'curious', 'shy', 'sad', 'sleepy', 'angry', 'scared', 'proud', 'dizzy', 'puff', 'wave', 'urge', 'urge_you', 'peek', 'eyeroll']   # ui_emotion_t order
# Pose frames (2026-11-04): not expressions but frames ui_anim.c strings together for idle life and the food. The order
# is the UI_PF_* enum written into ui_sprites.h; check_faces.py checks the same list.
POSES = ['look_l', 'look_r', 'read_l', 'read_r', 'chew_a', 'chew_b', 'preen_a', 'preen_b', 'stretch',
         'walk_l_a', 'walk_l_b', 'walk_r_a', 'walk_r_b', 'kick_l', 'kick_r', 'sly_l', 'sly_r',
         'reach_l', 'reach_r', 'hold_l', 'hold_r', 'lift_l', 'lift_r', 'sneak_l_a', 'sneak_l_b', 'sneak_r_a', 'sneak_r_b',
         'bite_l', 'bite_r', 'hide_l', 'hide_r']

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

def pad(bb, canvas):
    """One more pixel all round, inside the canvas. LVGL draws a ×4 patch a screen pixel short of where the pre-scaled base
    puts the same pixels, so a patch that ends where the pixels change left a one-pixel sliver of the base showing (the
    surprised face's tall O beside its smaller talking mouth, 2026-11-06); the extra pixel is the same in both."""
    x0, y0, x1, y1 = bb
    return (max(0, x0 - 1), max(0, y0 - 1), min(canvas.w - 1, x1 + 1), min(canvas.h - 1, y1 + 1))

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
    images.append(('scn_stage_disco', pg.stage_disco(), False))   # the gift key's show (app/ui/ui_stage.c)
    images.append(('scn_stage_night', pg.stage_night(), False))
    for f in (0, 1): images.append((f'spr_stage_ball_{f}', pg.stage_ball(f), True))
    for col, lean in (('pink', -1), ('cyan', 1), ('yel', -1), ('purp', 1)): images.append((f'spr_stage_beam_{col}', pg.stage_beam(col, lean), True))
    images.append(('spr_stage_mic', pg.stage_mic(), True))
    for col in ('yel', 'pink', 'cyan'): images.append((f'spr_stage_note_{col}', pg.stage_note(col), True))
    # v1.1: built-in props (fallbacks behind generated ones), the window pane at four times of day, accessories
    for pid, fn in pg.PROPS.items():
        images.append((f'spr_prop_{pid}', fn(), True))
    for fid, fn in pg.FOODS.items():                                # the yellow key's food
        images.append((f'spr_food_{fid}', fn(), True))
    for tod in ('night', 'sunrise', 'day', 'dusk'):
        images.append((f'spr_win_{tod}', pg.window(tod), True))
    images.append(('spr_acc_sun_hat', pg.acc_sun_hat(), True))
    images.append(('spr_acc_camera', pg.acc_camera(), True))
    for icon in ('setup', 'wifi', 'service', 'hello', 'idea'): images.append((f'spr_bubble_{icon}', pg.bubble(icon), True))
    for pz in POSES:
        images.append((f'spr_pose_{pz}', pg.gugu(pz), True))
    images.append(('spr_news', pg.newspaper(), True))
    images.append(('spr_news_turn', pg.newspaper(turn=True), True))
    images.append(('spr_book', pg.storybook(), True))
    images.append(('spr_book_turn', pg.storybook(turn=True), True))
    pose_glasses, pose_hat_y, pose_camera_y, pose_hand, pose_grip = [], [], [], [], []
    for pz in POSES:                                              # the trip's accessories follow a pose's head too; the held item its hand
        pg.gugu(pz); g = pg.LAST_GEOM
        pose_hand.append(g['hand'] or (-1, -1))
        if g.get('grip'):                                         # the hand's tip, drawn over the held item: transparent elsewhere
            xs, ys = [x for x, _ in g['grip']], [y for _, y in g['grip']]
            gx0, gy0 = min(xs) - 1, min(ys) - 1                   # a clear pixel all round: see pad()
            gc = pg.Canvas(max(xs) - gx0 + 2, max(ys) - gy0 + 2)
            for (x, y), col in g['grip'].items(): gc.put(x - gx0, y - gy0, col)
            images.append((f'spr_grip_{pz}', gc, True)); pose_grip.append((f'&spr_grip_{pz}', gx0, gy0))
        else:
            pose_grip.append(('NULL', 0, 0))
        gc, gx, gy = pg.acc_sunglasses(g['eye_boxes'], g['cx'], g['rx'])
        images.append((f'spr_acc_sunglasses_pose_{pz}', gc, True)); pose_glasses.append((f'&spr_acc_sunglasses_pose_{pz}', gx, gy))
        pose_hat_y.append(int(round(g['head_top'])) - 7); pose_camera_y.append(int(round(g['cy'])) + 1)
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
        bb = bb and pad(bb, closed)
        if bb and covers(closed, opened, bb):
            images.append((f'spr_beak_{e}', crop(opened, bb), True))
            beak.append((f'&spr_beak_{e}', bb[0], bb[1]))
        else:
            beak.append(('NULL', 0, 0))
    neutral, blink = pg.gugu('neutral'), pg.gugu('neutral', eyes_closed=True)
    bb_eyes = diff_bbox(neutral, blink)
    bb_eyes = bb_eyes and pad(bb_eyes, neutral)
    blink_ok = bb_eyes is not None and covers(neutral, blink, bb_eyes)
    if blink_ok:
        images.append(('spr_eyes_blink', crop(blink, bb_eyes), True))
    blink_fits = [blink_ok and same_region(pg.gugu(e), neutral, bb_eyes) for e in EMOTIONS]

    c_parts = ['// Generated by assets/gen/export_lvgl.py — do not edit.', '#include "ui/ui_sprites.h"', '']
    h_parts = ['// Generated by assets/gen/export_lvgl.py — do not edit.', '#pragma once', '#include <stdbool.h>', '#include <stdint.h>', '#include "lvgl.h"', '',
               '#define UI_SPRITE_SCALE 4   // base pixel -> screen pixels (nearest neighbour)',
               f'#define UI_GUGU_FEET_ROW {pg.FEET_ROW + 1}   // the sprite row of the feet\'s outline, the same in every frame: it sits on the ground line', '']
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
    h_parts.append(f'#define UI_FOOD_COUNT {len(pg.FOODS)}')
    h_parts.append('extern const ui_prop_sprite_t ui_food_sprites[UI_FOOD_COUNT];   // the yellow key\'s food, by id: ' + ', '.join(pg.FOODS))
    h_parts.append('extern const lv_image_dsc_t *const ui_window_sprites[4];      // night, sunrise, day, dusk')
    h_parts.append('extern const ui_patch_t ui_sunglasses_patch[UI_EMOTION_COUNT];   // dark lenses over that face\'s eyes')
    h_parts.append('extern const int16_t ui_hat_y[UI_EMOTION_COUNT];               // base y of the sun hat for that face (x = 12)')
    h_parts.append('extern const int16_t ui_camera_y[UI_EMOTION_COUNT];            // base y of the camera on the chest (x = 18)')
    c_parts.append('const ui_prop_sprite_t ui_prop_sprites[] = {')
    c_parts.append('    ' + ', '.join(f'{{ "{pid}", &spr_prop_{pid} }}' for pid in pg.PROPS) + ',')
    c_parts.append('};')
    c_parts.append(f'const int ui_prop_sprite_count = {len(pg.PROPS)};')
    c_parts.append('const ui_prop_sprite_t ui_food_sprites[UI_FOOD_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'{{ "{fid}", &spr_food_{fid} }}' for fid in pg.FOODS) + ',')
    c_parts.append('};')
    c_parts.append('const lv_image_dsc_t *const ui_window_sprites[4] = { &spr_win_night, &spr_win_sunrise, &spr_win_day, &spr_win_dusk };')
    c_parts.append('const ui_patch_t ui_sunglasses_patch[UI_EMOTION_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'{{ {img}, {x}, {y} }}' for img, x, y in glasses) + ',')
    c_parts.append('};')
    c_parts.append('const int16_t ui_hat_y[UI_EMOTION_COUNT] = { ' + ', '.join(str(v) for v in hat_y) + ' };')
    c_parts.append('const int16_t ui_camera_y[UI_EMOTION_COUNT] = { ' + ', '.join(str(v) for v in camera_y) + ' };')
    h_parts.append('')
    h_parts.append('// Pose frames: not expressions, frames ui_anim.c strings together (idle life, the food). Order: ' + ', '.join(POSES))
    h_parts.append('enum { ' + ', '.join(f'UI_PF_{pz.upper()}' for pz in POSES) + ', UI_POSE_FRAME_COUNT };')
    h_parts.append('extern const lv_image_dsc_t *const ui_pose_sprites[UI_POSE_FRAME_COUNT];')
    h_parts.append('extern const ui_patch_t ui_pose_sunglasses_patch[UI_POSE_FRAME_COUNT];')
    h_parts.append('extern const int16_t ui_pose_hat_y[UI_POSE_FRAME_COUNT];')
    h_parts.append('extern const int16_t ui_pose_camera_y[UI_POSE_FRAME_COUNT];')
    h_parts.append('extern const int8_t ui_pose_hand[UI_POSE_FRAME_COUNT][2];   // the wing-hand (base x, y in the sprite) that holds the item; -1 none')
    h_parts.append('extern const ui_patch_t ui_pose_grip[UI_POSE_FRAME_COUNT];   // the hand\'s tip, drawn over the held item; img NULL none')
    h_parts.append(f'#define UI_STAGE_FLOOR {pg.STAGE_FLOOR}   // the show\'s floor: its first base row (the tiles cover it)')
    h_parts.append('#define UI_NEWS_X 9    // the newspaper\'s base offset over the pigeon sprite (held in front of the belly)')
    h_parts.append('#define UI_NEWS_Y 26')
    c_parts.append('const lv_image_dsc_t *const ui_pose_sprites[UI_POSE_FRAME_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'&spr_pose_{pz}' for pz in POSES) + ',')
    c_parts.append('};')
    c_parts.append('const ui_patch_t ui_pose_sunglasses_patch[UI_POSE_FRAME_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'{{ {img}, {x}, {y} }}' for img, x, y in pose_glasses) + ',')
    c_parts.append('};')
    c_parts.append('const int16_t ui_pose_hat_y[UI_POSE_FRAME_COUNT] = { ' + ', '.join(str(v) for v in pose_hat_y) + ' };')
    c_parts.append('const int16_t ui_pose_camera_y[UI_POSE_FRAME_COUNT] = { ' + ', '.join(str(v) for v in pose_camera_y) + ' };')
    c_parts.append('const int8_t ui_pose_hand[UI_POSE_FRAME_COUNT][2] = { ' + ', '.join(f'{{ {x}, {y} }}' for x, y in pose_hand) + ' };')
    c_parts.append('const ui_patch_t ui_pose_grip[UI_POSE_FRAME_COUNT] = {')
    c_parts.append('    ' + ', '.join(f'{{ {img}, {x}, {y} }}' for img, x, y in pose_grip) + ',')
    c_parts.append('};')
    print('beak patches:', ', '.join(f'{e}={"ok" if b[0] != "NULL" else "-"}' for e, b in zip(EMOTIONS, beak)))
    print('blink patch:', 'ok' if blink_ok else 'none', 'bbox', bb_eyes, 'fits:', ', '.join(e for e, f in zip(EMOTIONS, blink_fits) if f))
    os.makedirs(OUT_DIR, exist_ok=True)
    open(os.path.join(OUT_DIR, 'ui_sprites.c'), 'w').write('\n'.join(c_parts) + '\n')
    open(os.path.join(OUT_DIR, 'ui_sprites.h'), 'w').write('\n'.join(h_parts) + '\n')
    print(f'exported {len(images)} images, {total} bytes of pixel data')

if __name__ == '__main__':
    main()
