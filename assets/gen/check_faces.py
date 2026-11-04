#!/usr/bin/env python3
"""check_faces.py — every lens holds one mark and nothing else (2026-11-03, owner: "眼镜内线条太多太复杂"), every frame
stands on the same row (pixelgen FEET_ROW: the ground line Gugu's feet meet, at home and in the away scenes' strip), and
every wing used as a hand grows out of the body (2026-11-05, owner: "就是感觉翅膀（手）跟身体是分离的"): from the belly,
through feathers alone, without crossing an outline, one reaches the end of each such wing (the pixel nearest its tip
that still shows feathers: a tip may go under the glasses).

For each face (and the blink frame), the pixels inside each round lens may only be the lens white, the pupil colour and
the pupil's glint; a closed-eye mark (an arch, a line, an arrow, an ×) must not touch the frame, diagonals included.
export_lvgl.py runs this first and refuses to export when anything is found. Run it alone with: python3 assets/gen/check_faces.py
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import pixelgen as pg

EMOTIONS = ['neutral', 'happy', 'laughing', 'surprised', 'curious', 'shy', 'sad', 'sleepy', 'angry', 'scared', 'proud', 'dizzy', 'puff', 'wave', 'urge', 'urge_you', 'peek', 'eyeroll']
POSES = ['look_l', 'look_r', 'read_l', 'read_r', 'chew_a', 'chew_b', 'preen_a', 'preen_b', 'stretch',
         'walk_l_a', 'walk_l_b', 'walk_r_a', 'walk_r_b', 'kick_l', 'kick_r', 'sly_l', 'sly_r',
         'reach_l', 'reach_r', 'hold_l', 'hold_r', 'lift_l', 'lift_r', 'sneak_l_a', 'sneak_l_b', 'sneak_r_a', 'sneak_r_b',
         'bite_l', 'bite_r', 'hide_l', 'hide_r']   # export_lvgl.py's pose frames
CLOSED = {'happy', 'laughing', 'sleepy', 'proud', 'dizzy', 'puff', 'blink', 'chew_a', 'chew_b', 'preen_a', 'preen_b', 'stretch', 'bite_l', 'bite_r'}   # marks that must keep clear of the frame


def reached(g, P):
    """The feathers one reaches from the belly without crossing an outline (4-connected)."""
    feathers = {P['body'], P['shade'], P['hi'], P['belly']}
    todo = [(x, y) for y in range(g.h) for x in range(g.w) if g.get(x, y) == P['belly']]
    seen = set(todo)
    while todo:
        x, y = todo.pop()
        for q in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if q not in seen and g.get(*q) in feathers: seen.add(q); todo.append(q)
    return seen


def visible_end(g, P, pts):
    """Walking a wing's spine back from its tip to its shoulder, the first pixel that shows feathers."""
    feathers = {P['body'], P['shade'], P['hi'], P['belly']}
    path = list(reversed(pts))
    for (ax, ay), (bx, by) in zip(path, path[1:]):
        n = max(1, int(((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5 * 2))
        for i in range(n + 1):
            q = (int(ax + (bx - ax) * i / n), int(ay + (by - ay) * i / n))
            if g.get(*q) in feathers: return q
    return None


def problems():
    P = pg.PAL
    allowed = {P['eyew'], P['pupil'], P['hi']}
    names = {v: k for k, v in P.items()}
    found = []
    for name, emotion, kw in [(e, e, {}) for e in EMOTIONS + POSES] + [('blink', 'neutral', {'eyes_closed': True})]:
        g = pg.gugu(emotion, **kw)
        for i, (x0, y0, w, h) in enumerate(pg.LAST_GEOM['eye_boxes']):
            mask = pg.lens_mask(x0, y0, w, h); ring = pg.lens_ring(mask)
            stray = sorted((x - x0, y - y0, names.get(g.get(x, y))) for (x, y) in mask if g.get(x, y) not in allowed)
            closed = name in CLOSED or (name == 'wave' and i == 0)
            touch = sorted((x - x0, y - y0) for (x, y) in mask if closed and g.get(x, y) == P['pupil']
                           and any((x + dx, y + dy) in ring for dx in (-1, 0, 1) for dy in (-1, 0, 1)))
            if stray: found.append(f'{name} lens {i}: {len(stray)} stray pixel(s) inside, e.g. {stray[:4]}')
            if touch: found.append(f'{name} lens {i}: the mark touches the frame at {touch[:4]}')
        wings = pg.LAST_GEOM.get('wings') or []
        seen = reached(g, P) if wings else set()
        for pts in wings:                                               # a wing outlined all round sits on the body like a sticker
            end = visible_end(g, P, pts)
            if end is None: found.append(f'{name}: a wing is hidden entirely')
            elif end not in seen: found.append(f'{name}: the wing that ends at {end} is cut off from the body by an outline')
        feet = [y for y in range(g.h) for x in range(g.w) if g.get(x, y) == P['foot']]
        if name != 'blink' and (not feet or max(feet) != pg.FEET_ROW):   # one ground line for every frame (pixelgen FEET_ROW)
            found.append(f'{name}: the feet end on row {max(feet) if feet else None}, not {pg.FEET_ROW}: Gugu would sink or float as the frame changes')
    return found


if __name__ == '__main__':
    p = problems()
    print('\n'.join(p) if p else f'faces ok: {len(EMOTIONS)} faces, {len(POSES)} pose frames and the blink, one mark per lens, nothing touching a frame, the feet on row {pg.FEET_ROW}, every wing grown out of the body')
    sys.exit(1 if p else 0)
