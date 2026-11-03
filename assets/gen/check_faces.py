#!/usr/bin/env python3
"""check_faces.py — every lens holds one mark and nothing else (2026-11-03, owner: "眼镜内线条太多太复杂").

For each face (and the blink frame), the pixels inside each round lens may only be the lens white, the pupil colour and
the pupil's glint; a closed-eye mark (an arch, a line, an arrow, an ×) must not touch the frame, diagonals included.
export_lvgl.py runs this first and refuses to export when anything is found. Run it alone with: python3 assets/gen/check_faces.py
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import pixelgen as pg

EMOTIONS = ['neutral', 'happy', 'laughing', 'surprised', 'curious', 'shy', 'sad', 'sleepy', 'angry', 'scared', 'proud', 'dizzy', 'puff', 'wave']
CLOSED = {'happy', 'laughing', 'sleepy', 'proud', 'dizzy', 'puff', 'blink'}   # marks that must keep clear of the frame


def problems():
    P = pg.PAL
    allowed = {P['eyew'], P['pupil'], P['hi']}
    names = {v: k for k, v in P.items()}
    found = []
    for name, emotion, kw in [(e, e, {}) for e in EMOTIONS] + [('blink', 'neutral', {'eyes_closed': True})]:
        g = pg.gugu(emotion, **kw)
        for i, (x0, y0, w, h) in enumerate(pg.LAST_GEOM['eye_boxes']):
            mask = pg.lens_mask(x0, y0, w, h); ring = pg.lens_ring(mask)
            stray = sorted((x - x0, y - y0, names.get(g.get(x, y))) for (x, y) in mask if g.get(x, y) not in allowed)
            closed = name in CLOSED or (name == 'wave' and i == 0)
            touch = sorted((x - x0, y - y0) for (x, y) in mask if closed and g.get(x, y) == P['pupil']
                           and any((x + dx, y + dy) in ring for dx in (-1, 0, 1) for dy in (-1, 0, 1)))
            if stray: found.append(f'{name} lens {i}: {len(stray)} stray pixel(s) inside, e.g. {stray[:4]}')
            if touch: found.append(f'{name} lens {i}: the mark touches the frame at {touch[:4]}')
    return found


if __name__ == '__main__':
    p = problems()
    print('\n'.join(p) if p else f'faces ok: {len(EMOTIONS)} faces and the blink, one mark per lens, nothing touching a frame')
    sys.exit(1 if p else 0)
