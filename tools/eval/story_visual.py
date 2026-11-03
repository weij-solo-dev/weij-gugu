#!/usr/bin/env python3
"""From a trip card to the whole picture: backdrop, props, friend, pigeon, four times of day. One-off evaluation
of the v1.1 generate-first pipeline on the host (needs `pip install pillow numpy`; image-01 key from the environment).

    GUGU_MINIMAX_API_KEY=... python3 tools/eval/story_visual.py --cards dir_with_cards/*.json --out /tmp/story_eval [--reuse]

Per card: the backdrop from scene_prompt (116 grid, 20 colours), one generated sprite per object id and one for the
friend (single object on a flat background, keyed out and pixelized exactly as obj_keyout.py does), the pre-made
suitcase sprite when the card asks for one, the pigeon, then sunrise / day / dusk / night by palette remap.
Writes <out>/<slug>/{backdrop_orig.jpg, <object>_orig.jpg, scene_<mode>.png, sheet.png} and <out>/overview.png.
"""
import argparse, glob, io, json, os, random, sys, time
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bg_pixelize import generate, pixelize, remap_palette, sky_entries, decorate   # noqa: E402
from obj_keyout import keyout, pixelize_prop                                        # noqa: E402
sys.path.insert(0, os.path.join(HERE, '..', '..', 'assets', 'gen'))
import pixelgen as pg                                                              # noqa: E402

SAMPLES = os.path.join(HERE, '..', '..', 'assets', 'samples')
BG_STYLE = 'simple flat colours, bold shapes, children\'s picture-book illustration, no people, no animals, no text, no watermark'
OBJ_STYLE = ('simple flat colours, bold shapes, children\'s picture-book illustration, a single object, whole object '
             'visible, centred, on a plain flat {bg} background, no shadow, no ground, no scenery, no text')
# catalogue id → (prompt subject, longer side in base px, background colour to key on)
CATALOGUE = {
    'lamp': ('an old-fashioned street lamp post', 40, 'white'), 'bench': ('a wooden park bench', 26, 'white'),
    'mailbox': ('a red pillar mailbox', 16, 'white'), 'tree': ('a round leafy tree', 40, 'white'),
    'palm': ('a palm tree', 44, 'white'), 'umbrella': ('a striped beach umbrella', 34, 'white'),
    'lounger': ('a striped beach lounger chair', 30, 'white'), 'swim_ring': ('a red and white swim ring', 14, 'white'),
    'camera': ('a small retro camera', 10, 'white'), 'sun_hat': ('a straw sun hat', 12, 'white'),
    'sunglasses': ('a pair of sunglasses', 10, 'white'), 'cat': ('a sitting orange cat', 16, 'white'),
    'dog': ('a small sitting dog', 16, 'white'), 'seagull': ('a seagull standing', 18, 'green'),
    'camel': ('a friendly standing camel', 26, 'white'), 'bicycle': ('a bicycle', 26, 'white'),
    'boat': ('a small wooden boat', 30, 'white'), 'lantern': ('a red paper lantern', 12, 'white'),
    'snowman': ('a snowman', 22, 'green'), 'kite': ('a colourful kite', 18, 'white'),
    'balloon': ('a red balloon on a string', 18, 'white'), 'ice_cream': ('an ice cream cone', 10, 'white'),
    'flag': ('a small flag on a pole', 24, 'white'), 'suitcase': None,   # the pre-made red case sprite
}
# where things go on the 116 grid: (x of the sprite's left, y of its bottom); the pigeon stands at (34..82, 46..100)
GROUND_TOP = 98   # the pigeon sprite at (34, 46) has its feet on rows 98..99
SLOTS = [(2, GROUND_TOP + 1), (92, GROUND_TOP + 1), (100, GROUND_TOP + 1), (14, GROUND_TOP + 1), (78, GROUND_TOP + 1)]
FRIEND_SLOT = (84, GROUND_TOP + 1)
GROUND = {   # body, speckle, edge: the strip the firmware draws under Gugu, whatever the backdrop shows
    'sand': ((232, 206, 150), (214, 186, 128), (166, 138, 86)),
    'grass': ((122, 170, 92), (104, 150, 78), (70, 104, 52)),
    'pavement': ((178, 172, 164), (160, 154, 146), (112, 106, 100)),
    'stone': ((160, 150, 136), (142, 132, 118), (96, 88, 78)),
    'snow': ((236, 240, 246), (218, 224, 234), (168, 178, 196)),
    'wood': ((190, 140, 90), (168, 120, 74), (112, 76, 44)),
}


def draw_ground(base, kind):
    body, speck, edge = GROUND.get(kind, GROUND['pavement'])
    d = ImageDraw.Draw(base)
    d.rectangle((0, GROUND_TOP, 115, 115), fill=body)
    d.line((0, GROUND_TOP, 115, GROUND_TOP), fill=edge)
    rnd = random.Random(3)
    for _ in range(90): d.point((rnd.randint(0, 115), rnd.randint(GROUND_TOP + 2, 115)), fill=speck)
    if kind == 'wood':
        for y in range(GROUND_TOP + 4, 116, 4): d.line((0, y, 115, y), fill=edge)


def guess_ground(t, objs):
    if t.get('ground') in GROUND: return t['ground']
    if {'palm', 'lounger', 'umbrella', 'swim_ring'} & set(objs): return 'sand'
    if {'tree', 'bamboo', 'kite'} & set(objs): return 'grass'
    return 'pavement'
MODES = ['sunrise', 'day', 'dusk', 'night']
LIVING = {'cat', 'dog', 'seagull', 'camel', 'swan', 'bird', 'duck', 'otter', 'egret', '__friend'}   # out of sight at night: everybody rests
MAX_PROPS = 3
KIND_ZH = {'deliver': '送信', 'vacation': '度假', 'visit': '探亲'}


def font(size):
    for f in ('/System/Library/Fonts/STHeiti Light.ttc', '/System/Library/Fonts/PingFang.ttc', '/System/Library/Fonts/Supplemental/Songti.ttc'):
        if os.path.exists(f):
            try: return ImageFont.truetype(f, size)
            except Exception: pass
    return ImageFont.load_default()


def fetch(key, path, prompt, reuse):
    if reuse and os.path.exists(path): return open(path, 'rb').read(), 0.0
    raw, t_gen, t_dl, _ = generate(key, prompt, aspect='1:1')
    open(path, 'wb').write(raw)
    return raw, t_gen + t_dl


def sprite_for(key, d, obj_id, reuse, log):
    """A keyed, pixelized sprite for a catalogue id (or any id the model invented), or None."""
    entry = CATALOGUE.get(obj_id, (obj_id.replace('_', ' '), 24, 'white'))
    if entry is None:   # suitcase: the pre-made sprite, drawn by pixelgen the way the firmware's spr_case_up is
        c = pg.Canvas(20, 26)
        pg.draw_case(c, 4, 8, 'red', handle_up=True)
        im = Image.new('RGBA', (c.w, c.h), (0, 0, 0, 0))
        for yy in range(c.h):
            for xx in range(c.w):
                col = c.px[yy][xx]
                if col is not None: im.putpixel((xx, yy), (*col, 255))
        return im.crop(im.getbbox())
    subject, longer, bg = entry
    try:
        raw, secs = fetch(key, os.path.join(d, f'{obj_id}_orig.jpg'), f'{subject}, {OBJ_STYLE.format(bg=bg)}', reuse)
    except Exception as e:
        log.append({'object': obj_id, 'error': str(e)}); print(f'   {obj_id}: generation failed: {e}'); return None
    rgba, coverage = keyout(Image.open(io.BytesIO(raw)))
    res = pixelize_prop(rgba, longer)
    if not res: log.append({'object': obj_id, 'error': 'empty after keying'}); return None
    spr, ncomp, fill = res
    ok = coverage >= 0.02 and ncomp <= 3 and 0.15 <= fill <= 0.9
    log.append({'object': obj_id, 'subject': subject, 'secs': round(secs, 1), 'sprite': f'{spr.width}x{spr.height}', 'components': ncomp, 'fill': round(fill, 2), 'pass': ok})
    print(f'   {obj_id}: {spr.width}x{spr.height}, {ncomp} piece(s), fill {fill:.2f}, {"ok" if ok else "FAILED gate"}, {secs:.0f}s')
    spr.save(os.path.join(d, f'{obj_id}_sprite.png'))
    return spr if ok else None


def round_frame(canvas116, pigeon, mode):
    """The device view: 116 grid ×4 in the 466 round window, the pigeon on top (never recoloured), sun/moon/stars."""
    bg = canvas116.convert('RGB').resize((464, 464), Image.NEAREST)
    frame = Image.new('RGB', (466, 466), (0, 0, 0)); frame.paste(bg, (1, 1))
    p = pigeon.resize((pigeon.width * 4, pigeon.height * 4), Image.NEAREST)
    frame.paste(p, (34 * 4 + 1, 46 * 4 + 1), p)
    frame = decorate(frame, mode)
    mask = Image.new('L', (466, 466), 0); ImageDraw.Draw(mask).ellipse((0, 0, 465, 465), fill=255)
    return Image.composite(frame, Image.new('RGB', (466, 466), (0, 0, 0)), mask)


def render_card(key, card_path, out, reuse):
    t = json.load(open(card_path, encoding='utf-8'))
    slug = os.path.splitext(os.path.basename(card_path))[0]
    d = os.path.join(out, slug); os.makedirs(d, exist_ok=True)
    log = []
    print(f'== {slug}: {KIND_ZH.get(t["kind"], t["kind"])} {t["place"]["name"]} / {t["place"].get("landmark", "")}')
    # 1. backdrop
    raw, secs = fetch(key, os.path.join(d, 'backdrop_orig.jpg'), f'{t["scene_prompt"]}, {BG_STYLE}', reuse)
    print(f'   backdrop {len(raw) // 1024} KB in {secs:.0f}s')
    q = pixelize(Image.open(io.BytesIO(raw)), 116, 20)
    sky = sky_entries(q)
    # 2. props and the friend
    objs = [o for o in t.get('objects', []) if o][:MAX_PROPS]   # the composition rule: three things at most
    ground = guess_ground(t, objs)
    print(f'   ground: {ground}')
    sprites = []
    for i, o in enumerate(objs):
        spr = sprite_for(key, d, o, reuse, log)
        if spr: sprites.append((o, spr, SLOTS[i % len(SLOTS)]))
    friend = t.get('friend') or {}
    friend_spr = None
    if friend.get('animal'):
        CATALOGUE['__friend'] = (f'a friendly {friend["animal"]}', 22, 'green')
        friend_spr = sprite_for(key, d, '__friend', reuse, log)
    # 3. compose on the grid per time of day: backdrop, props (bottom-aligned), friend beside the pigeon;
    #    living things are on screen by day and at dusk and gone at night (the object layer schedules them)
    pigeon = Image.open(os.path.join(SAMPLES, 'gugu_neutral_glasses@1x.png')).convert('RGBA')
    frames = {}
    for mode in MODES:
        awake = mode != 'night'
        base = q.convert('RGB')
        draw_ground(base, ground)
        for o, spr, (x, y) in sprites:
            if o in LIVING and not awake: continue
            base.paste(spr, (x, y - spr.height), spr)
        if friend_spr and awake: base.paste(friend_spr, (FRIEND_SLOT[0], FRIEND_SLOT[1] - friend_spr.height), friend_spr)
        comp = base.quantize(colors=48, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        mq = comp if mode == 'day' else remap_palette(comp, mode, sky_entries(comp))
        frames[mode] = round_frame(mq, pigeon, mode)
        frames[mode].save(os.path.join(d, f'scene_{mode}.png'))
    # 4. the sheet: four times of day and the story
    f_big, f_small = font(22), font(16)
    sheet = Image.new('RGB', (466 * 2 + 24 + 420, 466 * 2 + 24), (24, 24, 28)); dr = ImageDraw.Draw(sheet)
    for i, mode in enumerate(MODES):
        sheet.paste(frames[mode], (8 + (i % 2) * 474, 8 + (i // 2) * 474))
    x0, y = 466 * 2 + 32, 12
    def line(txt, f=f_small, gap=6, color=(230, 230, 230)):
        nonlocal y
        words, cur, maxw = list(txt), '', 400
        for ch in words:
            if dr.textlength(cur + ch, font=f) > maxw: dr.text((x0, y), cur, font=f, fill=color); y += f.size + gap; cur = ch
            else: cur += ch
        dr.text((x0, y), cur, font=f, fill=color); y += f.size + gap
    line(f'{KIND_ZH.get(t["kind"], t["kind"])} · {t["place"]["name"]}（{t["place"].get("country", "")}）', f_big, 10, (246, 210, 122))
    line(f'{t["place"].get("landmark", "")} · {t["depart_date"]} 出发，住 {t["nights"]} 晚')
    y += 6; line('为什么去：' + t['reason'])
    if t.get('recipient'): line('收件人：' + t['recipient'])
    if t.get('letter'): line('信里写：' + t['letter'])
    y += 6
    for i, dplan in enumerate(t.get('days', [])): line(f'第 {i + 1} 天：{dplan}')
    y += 6; line(f'朋友：{friend.get("name", "")}（{friend.get("animal", "")}）· 纪念品：{t.get("souvenir", "")}')
    line('场景物件：' + '、'.join(objs) + f' · 地面：{ground}')
    y += 6; line('它会讲的事：', f_small, 6, (246, 210, 122))
    for fact in t.get('facts', [])[:6]: line('· ' + fact)
    y += 6; line('右侧文字来自趟卡 JSON；四格 = 同一张底图换色板得到的日出 / 白天 / 黄昏 / 夜晚', f_small, 6, (150, 150, 150))
    sheet.save(os.path.join(d, 'sheet.png'))
    json.dump(log, open(os.path.join(d, 'objects.json'), 'w'), indent=1, ensure_ascii=False)
    return slug, t, frames, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cards', nargs='+', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--reuse', action='store_true')
    a = ap.parse_args()
    key = os.environ.get('GUGU_MINIMAX_API_KEY')
    if not key and not a.reuse: sys.exit('set GUGU_MINIMAX_API_KEY (or use --reuse)')
    cards = [c for pat in a.cards for c in sorted(glob.glob(pat))]
    os.makedirs(a.out, exist_ok=True)
    results = []
    t0 = time.time()
    for c in cards:
        try: results.append(render_card(key, c, a.out, a.reuse))
        except Exception as e: print(f'!! {c}: {e}')
    if results:
        ov = Image.new('RGB', (len(results) * 474 + 8, 2 * 474 + 48), (24, 24, 28)); dr = ImageDraw.Draw(ov); f = font(20)
        for i, (slug, t, frames, log) in enumerate(results):
            ov.paste(frames['day'], (8 + i * 474, 8))
            ov.paste(frames['night'], (8 + i * 474, 474 + 8))
            n_ok = sum(1 for r in log if r.get('pass')); n_all = sum(1 for r in log if 'pass' in r or 'error' in r)
            dr.text((8 + i * 474, 2 * 474 + 14), f'{KIND_ZH.get(t["kind"], t["kind"])} · {t["place"]["name"]} · 白天 / 夜里 · 生成件 {n_ok}/{n_all} 过门', font=f, fill=(230, 230, 230))
        ov.save(os.path.join(a.out, 'overview.png'))
    print(f'done in {time.time() - t0:.0f}s: {os.path.join(a.out, "overview.png")}')


if __name__ == '__main__':
    main()
