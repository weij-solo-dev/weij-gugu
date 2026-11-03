#!/usr/bin/env python3
"""One-off evaluation: can trip backgrounds be generated per story instead of pre-drawn?

For each destination prompt: ask MiniMax image-01 for a picture, then do what the device would do with it:
shrink it to the 116 x 116 base grid of our pixel art (the scene is drawn at 4x on the 466 px display),
reduce it to a small palette, derive a night variant by recolouring that palette, and composite the pigeon
sprite on top at its home position. Writes originals, pixelised PNGs and a comparison grid.

    source ~/.config/gugu/secrets.env
    pip install pillow numpy
    python3 tools/eval/bg_pixelize.py --out /tmp/bg_eval [--grid 116] [--colors 20]
"""
import argparse, base64, io, json, os, sys, time, urllib.request
from PIL import Image, ImageDraw, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, '..', '..', 'assets', 'samples')
API = 'https://api.minimax.cn/v1/image_generation'

STYLE = ('simple flat colours, bold shapes, children\'s picture-book illustration, '
         'wide view, the landmark in the upper two thirds, plain empty ground across the bottom third, '
         'no people, no animals, no text, no watermark')
# Four times of day for one place, same seed: does the composition hold when only the sky phrase changes?
TOD = [('sunrise', 'at sunrise, soft pink and orange sky, long shadows'),
       ('day',     'in bright daylight, clear blue sky'),
       ('dusk',    'at dusk, warm orange sunset sky'),
       ('night',   'at night, deep blue sky with stars and a moon, a few warm lights')]
SCENES = [
    ('egypt_giza',   'the Great Pyramids of Giza and the Sphinx in a sandy desert'),
    ('beijing',      'the old imperial palace of Beijing: red walls, golden tiled roofs, a white stone bridge in front'),
    ('guangzhou',    'Guangzhou riverside with the Canton Tower, the Pearl River and a few palm trees'),
    ('grandma_home', 'a small village house in southern China with a tiled roof, a persimmon tree and green hills behind'),
]


def generate(key, prompt, aspect='1:1', seed=None):
    req_body = {'model': 'image-01', 'prompt': prompt, 'aspect_ratio': aspect, 'response_format': 'url', 'n': 1, 'prompt_optimizer': False}
    if seed is not None: req_body['seed'] = seed
    body = json.dumps(req_body).encode()
    req = urllib.request.Request(API, data=body, headers={'Authorization': 'Bearer ' + key, 'Content-Type': 'application/json'})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=120) as r:
        res = json.loads(r.read())
    t_gen = time.time() - t0
    br = res.get('base_resp', {})
    if br.get('status_code', 0) != 0:
        raise RuntimeError(f"image-01: {br}")
    data = res.get('data') or {}
    if not data.get('image_urls') and not data.get('image_base64'):
        raise RuntimeError(f"image-01 returned no image: metadata={res.get('metadata')} (failed_count counts safety blocks)")
    if data.get('image_urls'):
        t1 = time.time()
        with urllib.request.urlopen(data['image_urls'][0], timeout=60) as r:
            raw = r.read()
        t_dl = time.time() - t1
    else:
        raw = base64.b64decode(data['image_base64'][0]); t_dl = 0
    return raw, t_gen, t_dl, res.get('metadata')


def pixelize(img, n, colors, boost=1.15):
    """What the firmware would do: box-shrink to the base grid, slight saturation lift, median-cut palette."""
    im = img.convert('RGB').resize((n, n), Image.BOX)
    im = ImageEnhance.Color(im).enhance(boost)
    return im.quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)


def night_of(q):
    """Night = the same indexed pixels with a recoloured palette (cheap on the device: 20 entries)."""
    pal = q.getpalette()[:256 * 3]
    out = []
    for i in range(0, len(pal), 3):
        r, g, b = pal[i:i + 3]
        lum = (r * 299 + g * 587 + b * 114) / 1000
        nr, ng, nb = r * 0.28 + 8, g * 0.33 + 14, b * 0.55 + 40        # dim and cool; sky goes navy
        if lum > 235: nr, ng, nb = 230, 225, 200                      # whites become warm lamp/moon light
        out += [int(min(255, nr)), int(min(255, ng)), int(min(255, nb))]
    nq = q.copy(); nq.putpalette(out)
    return nq


def composite(q, scale, pigeon):
    """466 px round frame, background at 4x like the device, pigeon sprite at its home grid position (34, 46)."""
    bg = q.convert('RGB').resize((q.width * scale, q.height * scale), Image.NEAREST)
    frame = Image.new('RGB', (466, 466), (0, 0, 0))
    frame.paste(bg, (1, 1))
    p = pigeon.resize((pigeon.width * 4, pigeon.height * 4), Image.NEAREST)
    frame.paste(p, (34 * 4 + 1, 46 * 4 + 1), p)
    mask = Image.new('L', (466, 466), 0); ImageDraw.Draw(mask).ellipse((0, 0, 465, 465), fill=255)
    black = Image.new('RGB', (466, 466), (0, 0, 0))
    return Image.composite(frame, black, mask)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--grid', type=int, default=116, help='base pixel grid (116 = the current scene size)')
    ap.add_argument('--colors', type=int, default=20)
    ap.add_argument('--reuse', action='store_true', help='reuse originals already in --out instead of calling the API')
    ap.add_argument('--tod', metavar='SLUG', help='one scene at four times of day with a fixed seed (composition consistency test)')
    ap.add_argument('--tod-remap', metavar='SLUG[,SLUG]', help='four times of day from the one day image by palette remapping (no API calls)')
    a = ap.parse_args()
    if a.tod: return times_of_day(a)
    if a.tod_remap: return times_of_day_remap(a)
    key = os.environ.get('GUGU_MINIMAX_API_KEY')
    if not key and not a.reuse:
        sys.exit('GUGU_MINIMAX_API_KEY is not set')
    os.makedirs(a.out, exist_ok=True)
    pigeon = Image.open(os.path.join(SAMPLES, 'gugu_neutral_glasses@1x.png')).convert('RGBA')
    scale = 464 // a.grid
    rows, report = [], []
    for slug, what in SCENES:
        orig_path = os.path.join(a.out, slug + '_orig.jpg')
        if a.reuse and os.path.exists(orig_path):
            raw = open(orig_path, 'rb').read(); t_gen = t_dl = 0; meta = None
        else:
            try:
                raw, t_gen, t_dl, meta = generate(key, f'{what}, {STYLE}')
            except RuntimeError as e:
                print(json.dumps({'scene': slug, 'error': str(e)}), flush=True)
                report.append({'scene': slug, 'error': str(e)})
                continue
            open(orig_path, 'wb').write(raw)
        img = Image.open(io.BytesIO(raw))
        q = pixelize(img, a.grid, a.colors)
        nq = night_of(q)
        q.save(os.path.join(a.out, f'{slug}_{a.grid}.png'))
        day = composite(q, scale, pigeon); night = composite(nq, scale, pigeon)
        day.save(os.path.join(a.out, f'{slug}_day.png')); night.save(os.path.join(a.out, f'{slug}_night.png'))
        report.append({'scene': slug, 'orig_px': img.size, 'orig_kb': len(raw) // 1024, 'gen_s': round(t_gen, 1), 'dl_s': round(t_dl, 1),
                       'indexed_bytes': a.grid * a.grid + a.colors * 3, 'meta': meta})
        print(json.dumps(report[-1], ensure_ascii=False), flush=True)
        rows.append([img.convert('RGB').resize((232, 232), Image.LANCZOS),
                     q.convert('RGB').resize((232, 232), Image.NEAREST),
                     day.resize((232, 232), Image.LANCZOS), night.resize((232, 232), Image.LANCZOS)])
    grid = Image.new('RGB', (4 * 232 + 5 * 8, len(rows) * (232 + 8) + 28), (24, 24, 28))
    d = ImageDraw.Draw(grid)
    for i, t in enumerate(['image-01 original', f'{a.grid} px, {a.colors} colours', 'on the device (day)', 'night by palette']):
        d.text((8 + i * 240 + 4, 8), t, fill=(230, 230, 230))
    for r, row in enumerate(rows):
        for c, im in enumerate(row):
            grid.paste(im, (8 + c * 240, 28 + r * 240))
    grid.save(os.path.join(a.out, 'grid.png'))
    json.dump(report, open(os.path.join(a.out, 'report.json'), 'w'), ensure_ascii=False, indent=1)
    print('grid:', os.path.join(a.out, 'grid.png'))


def remap_palette(q, mode, sky_idx):
    """Derive sunrise / dusk / night from the day image by recolouring its palette. Sky entries get the
    time-of-day sky (brightness order kept, so clouds stay lighter); the rest is blended toward a tint and
    dimmed. Integer blends on a 20-entry table: trivial on the device."""
    import colorsys
    tint = {'night': ((18, 26, 64), 0.62, 0.42), 'dusk': ((255, 140, 60), 0.30, 0.92), 'sunrise': ((255, 205, 175), 0.28, 1.0)}[mode]
    pal = q.getpalette()[:256 * 3]; out = []
    for i in range(0, len(pal), 3):
        r, g, b = pal[i:i + 3]
        if i // 3 in sky_idx:
            v = (r * 299 + g * 587 + b * 114) / 255000                   # 0..1 brightness of the original sky entry
            if mode == 'night':   h, s_, vv = 0.64, 0.70, 0.10 + 0.22 * v
            elif mode == 'dusk':  h, s_, vv = 0.04 + 0.06 * v, 0.72, 0.60 + 0.38 * v
            else:                 h, s_, vv = 0.93 + 0.05 * v, 0.28, 0.80 + 0.20 * v
            rr, gg, bb = [int(c * 255) for c in colorsys.hsv_to_rgb(h % 1.0, s_, min(1, vv))]
        else:
            (tr, tg, tb), k, dim = tint
            rr, gg, bb = [int(((1 - k) * c + k * t) * dim) for c, t in ((r, tr), (g, tg), (b, tb))]
        out += [rr, gg, bb]
    nq = q.copy(); nq.putpalette(out); return nq


def sky_entries(q):
    """Palette entries that live in the upper part of the picture: the sky bands and the clouds."""
    px = q.load(); w, h = q.size
    top, total = {}, {}
    for y in range(h):
        for x in range(w):
            i = px[x, y]; total[i] = total.get(i, 0) + 1
            if y < h * 0.42: top[i] = top.get(i, 0) + 1
    return {i for i, n in total.items() if n >= w * h * 0.01 and top.get(i, 0) / n >= 0.75}


def decorate(comp, mode):
    """Sun, moon and stars are overlay sprites on the device; here a few pixels stand in for them."""
    d = ImageDraw.Draw(comp)
    if mode == 'night':
        import random; rnd = random.Random(7)
        for _ in range(26):
            x, y = rnd.randint(60, 400), rnd.randint(40, 150); d.rectangle((x, y, x + 3, y + 3), fill=(230, 236, 255))
        d.ellipse((330, 60, 370, 100), fill=(242, 231, 201))
    elif mode in ('sunrise', 'dusk'):
        y = 120 if mode == 'sunrise' else 135
        d.ellipse((300, y, 344, y + 44), fill=(255, 217, 92) if mode == 'sunrise' else (255, 150, 70))
    return comp


def times_of_day_remap(a):
    pigeon = Image.open(os.path.join(SAMPLES, 'gugu_neutral_glasses@1x.png')).convert('RGBA')
    scale = 464 // a.grid
    scenes = a.tod_remap.split(',')
    grid = Image.new('RGB', (4 * 240 + 8, len(scenes) * 240 + 28), (24, 24, 28)); d = ImageDraw.Draw(grid)
    for r, slug in enumerate(scenes):
        raw = open(os.path.join(a.out, slug + '_orig.jpg'), 'rb').read()
        q = pixelize(Image.open(io.BytesIO(raw)), a.grid, a.colors)
        sky = sky_entries(q)
        for i, mode in enumerate(['sunrise', 'day', 'dusk', 'night']):
            mq = q if mode == 'day' else remap_palette(q, mode, sky)
            comp = decorate(composite(mq, scale, pigeon), mode)
            comp.save(os.path.join(a.out, f'{slug}_remap_{mode}.png'))
            if r == 0: d.text((12 + i * 240, 8), f'{mode} (one image, palette remap)', fill=(230, 230, 230))
            grid.paste(comp.resize((232, 232), Image.LANCZOS), (8 + i * 240, 28 + r * 240))
    out = os.path.join(a.out, 'remap_grid.png'); grid.save(out); print('grid:', out)


def times_of_day(a):
    key = os.environ.get('GUGU_MINIMAX_API_KEY') or sys.exit('GUGU_MINIMAX_API_KEY is not set')
    os.makedirs(a.out, exist_ok=True)
    what = dict(SCENES)[a.tod]
    pigeon = Image.open(os.path.join(SAMPLES, 'gugu_neutral_glasses@1x.png')).convert('RGBA')
    scale = 464 // a.grid
    tiles, report = [], []
    for name, sky in TOD:
        orig_path = os.path.join(a.out, f'{a.tod}_{name}_orig.jpg')
        if a.reuse and os.path.exists(orig_path):
            raw = open(orig_path, 'rb').read(); t_gen = 0
        else:
            raw, t_gen, _, _ = generate(key, f'{what}, {sky}, {STYLE}', seed=20261103)
            open(orig_path, 'wb').write(raw)
        q = pixelize(Image.open(io.BytesIO(raw)), a.grid, a.colors)
        comp = composite(q, scale, pigeon); comp.save(os.path.join(a.out, f'{a.tod}_{name}.png'))
        tiles.append((name, Image.open(io.BytesIO(raw)).convert('RGB').resize((232, 232), Image.LANCZOS), comp.resize((232, 232), Image.LANCZOS)))
        report.append({'tod': name, 'gen_s': round(t_gen, 1), 'kb': len(raw) // 1024}); print(json.dumps(report[-1]), flush=True)
    grid = Image.new('RGB', (4 * 240 + 8, 2 * 240 + 28), (24, 24, 28)); d = ImageDraw.Draw(grid)
    for i, (name, o, c) in enumerate(tiles):
        d.text((12 + i * 240, 8), f'{a.tod} {name} (seed fixed)', fill=(230, 230, 230))
        grid.paste(o, (8 + i * 240, 28)); grid.paste(c, (8 + i * 240, 28 + 240))
    grid.save(os.path.join(a.out, f'{a.tod}_tod_grid.png')); print('grid:', os.path.join(a.out, f'{a.tod}_tod_grid.png'))


if __name__ == '__main__':
    main()
