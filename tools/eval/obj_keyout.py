#!/usr/bin/env python3
"""Can small interactive props be generated on the fly? One-off evaluation (needs `pip install pillow numpy`).

For each prop: ask image-01 for a single object on a plain white background, then do what the firmware would
do: key the background out by flood-filling from the corners, crop to the object's bounding box, shrink to a
few dozen base pixels, hard alpha, quantize, add the 1-px dark outline every pixelgen sprite has. Report the
quality gate (coverage, connected components) and composite the results onto an earlier pixelized backdrop
next to the pigeon.

    GUGU_MINIMAX_API_KEY=... python3 tools/eval/obj_keyout.py --out /tmp/obj_eval [--reuse]
"""
import argparse, json, os, sys, time
from collections import deque
from PIL import Image, ImageDraw
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bg_pixelize import generate, composite   # noqa: E402

SAMPLES = os.path.join(HERE, '..', '..', 'assets', 'samples')
STYLE = ('simple flat colours, bold shapes, children\'s picture-book illustration, a single object, whole object '
         'visible, centred, front view, on a plain pure white background, no shadow, no ground, no scenery, no text')
# slug, prompt subject, target size in base pixels (longer side), where it goes on the Giza backdrop (base grid x, y of the bottom-left)
PROPS = [
    ('lounger', 'a striped beach lounger chair', 30, (74, 112)),
    ('lamp',    'an old-fashioned street lamp post', 40, (6, 112)),
    ('mailbox', 'a red pillar mailbox', 16, (62, 100)),
    ('camel',   'a friendly standing camel', 26, (84, 96)),
    ('seagull', 'a seagull standing', 18, (44, 34)),
]
LINE = (0x2B, 0x36, 0x42)   # pixelgen's outline colour


def keyout(img, tol=48):
    """Alpha = 0 for everything connected to the corners within `tol` of the corner colour; returns RGBA + stats."""
    a = np.asarray(img.convert('RGB')).astype(np.int16)
    h, w, _ = a.shape
    bg = np.median(np.stack([a[0, 0], a[0, -1], a[-1, 0], a[-1, -1]]), axis=0)
    near = (np.abs(a - bg).sum(axis=2) < tol)
    keep = np.ones((h, w), dtype=bool)
    seen = np.zeros((h, w), dtype=bool)
    q = deque([(0, 0), (0, w - 1), (h - 1, 0), (h - 1, w - 1)])
    while q:
        y, x = q.popleft()
        if seen[y, x] or not near[y, x]: continue
        seen[y, x] = True; keep[y, x] = False
        if y > 0: q.append((y - 1, x))
        if y < h - 1: q.append((y + 1, x))
        if x > 0: q.append((y, x - 1))
        if x < w - 1: q.append((y, x + 1))
    alpha = (keep * 255).astype(np.uint8)
    rgba = np.dstack([a.astype(np.uint8), alpha])
    return Image.fromarray(rgba, 'RGBA'), float(keep.mean())


def components(mask):
    """Number of 4-connected opaque regions after one pixel of dilation (thin legs and handles may split the
    hard alpha into pieces one pixel apart; those still read as one object)."""
    pad = np.pad(mask, 1)
    mask = mask | pad[:-2, 1:-1] | pad[2:, 1:-1] | pad[1:-1, :-2] | pad[1:-1, 2:]
    h, w = mask.shape; seen = np.zeros_like(mask, dtype=bool); n = 0
    for y in range(h):
        for x in range(w):
            if not mask[y, x] or seen[y, x]: continue
            n += 1; q = deque([(y, x)]); seen[y, x] = True
            while q:
                cy, cx = q.popleft()
                for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]: seen[ny, nx] = True; q.append((ny, nx))
    return n


def pixelize_prop(rgba, longer, colors=10):
    """Crop to the object, shrink so the longer side is `longer` base pixels, hard alpha, quantize, outline."""
    bbox = rgba.getbbox()
    if not bbox: return None
    crop = rgba.crop(bbox)
    scale = longer / max(crop.width, crop.height)
    w, h = max(1, round(crop.width * scale)), max(1, round(crop.height * scale))
    # premultiply before the box shrink so the white background does not bleed into edge pixels
    arr = np.asarray(crop).astype(np.float32); al = arr[..., 3:4] / 255.0
    pre = Image.fromarray(np.dstack([(arr[..., :3] * al).astype(np.uint8), arr[..., 3].astype(np.uint8)]), 'RGBA').resize((w, h), Image.BOX)
    p = np.asarray(pre).astype(np.float32); a = p[..., 3:4]
    rgb = np.where(a > 0, p[..., :3] / np.maximum(a / 255.0, 1e-3), 0).clip(0, 255).astype(np.uint8)
    hard = (p[..., 3] >= 128)
    fill = Image.fromarray(rgb, 'RGB')
    q = fill.quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).convert('RGB')
    out = np.dstack([np.asarray(q), (hard * 255).astype(np.uint8)])
    # 1-px outline: transparent pixels touching an opaque one become the line colour
    pad = np.pad(hard, 1)
    touch = (pad[:-2, 1:-1] | pad[2:, 1:-1] | pad[1:-1, :-2] | pad[1:-1, 2:]) & ~hard
    out[touch] = (*LINE, 255)
    return Image.fromarray(out, 'RGBA'), components(hard), float(hard.mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--reuse', action='store_true')
    ap.add_argument('--backdrop', default=None, help='a 116x116 quantized backdrop PNG to composite onto (default <out>/backdrop_116.png)')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    key = os.environ.get('GUGU_MINIMAX_API_KEY')
    report = []
    sprites = {}
    for slug, subject, longer, pos in PROPS:
        orig = os.path.join(a.out, f'{slug}_orig.jpg')
        t_gen = t_dl = 0.0
        if a.reuse and os.path.exists(orig):
            raw = open(orig, 'rb').read()
        else:
            if not key: sys.exit('set GUGU_MINIMAX_API_KEY (or use --reuse)')
            try:
                raw, t_gen, t_dl, _ = generate(key, f'{subject}, {STYLE}', aspect='1:1')
            except Exception as e:
                print(f'{slug}: generation failed: {e}'); report.append({'prop': slug, 'error': str(e)}); continue
            open(orig, 'wb').write(raw)
        img = Image.open(orig)
        rgba, coverage = keyout(img)
        res = pixelize_prop(rgba, longer)
        if not res: print(f'{slug}: nothing left after keying'); continue
        spr, ncomp, fill_ratio = res
        spr.save(os.path.join(a.out, f'{slug}_{spr.width}x{spr.height}.png'))
        spr.resize((spr.width * 4, spr.height * 4), Image.NEAREST).save(os.path.join(a.out, f'{slug}@4x.png'))
        ok = coverage >= 0.02 and ncomp <= 3 and 0.15 <= fill_ratio <= 0.9   # thin props cover little of the frame; what matters is one clean piece
        row = {'prop': slug, 'gen_s': round(t_gen, 1), 'dl_s': round(t_dl, 1), 'jpeg_kb': len(raw) // 1024, 'coverage': round(coverage, 3),
               'sprite': f'{spr.width}x{spr.height}', 'components': ncomp, 'fill': round(fill_ratio, 2), 'pass': ok}
        print(json.dumps(row, ensure_ascii=False)); report.append(row)
        sprites[slug] = (spr, pos)
    json.dump(report, open(os.path.join(a.out, 'report.json'), 'w'), indent=1, ensure_ascii=False)

    # the composite: backdrop + props + pigeon, like the device would show it
    backdrop = a.backdrop or os.path.join(a.out, 'backdrop_116.png')
    if os.path.exists(backdrop):
        bd = Image.open(backdrop).convert('RGB')
        if bd.size != (116, 116): bd = bd.resize((116, 116), Image.NEAREST)   # a 4x export of the grid is fine too
        canvas = bd.copy()
        for slug, (spr, (x, y)) in sprites.items():
            canvas.paste(spr, (x, y - spr.height), spr)
        pigeon = Image.open(os.path.join(SAMPLES, 'gugu_neutral_glasses@1x.png')).convert('RGBA')
        final = composite(canvas.quantize(colors=256), 4, pigeon)
        final.save(os.path.join(a.out, 'composite.png'))
    # the sheet: original thumbnail, keyed sprite at 4x, for each prop
    cell = 200
    sheet = Image.new('RGB', (cell * 2 + 466, max(cell * len(PROPS), 466)), (28, 28, 28))
    d = ImageDraw.Draw(sheet)
    for i, (slug, _, _, _) in enumerate(PROPS):
        orig = os.path.join(a.out, f'{slug}_orig.jpg')
        if os.path.exists(orig): sheet.paste(Image.open(orig).convert('RGB').resize((cell - 10, cell - 10)), (5, i * cell + 5))
        if slug in sprites:
            spr = sprites[slug][0]; big = spr.resize((spr.width * 4, spr.height * 4), Image.NEAREST)
            sheet.paste(big, (cell + (cell - big.width) // 2, i * cell + (cell - big.height) // 2), big)
        d.text((cell + 4, i * cell + 4), slug, fill=(200, 200, 200))
    if os.path.exists(os.path.join(a.out, 'composite.png')): sheet.paste(Image.open(os.path.join(a.out, 'composite.png')), (cell * 2, 0))
    sheet.save(os.path.join(a.out, 'sheet.png'))
    print('wrote', os.path.join(a.out, 'sheet.png'))


if __name__ == '__main__':
    main()
