#!/usr/bin/env python3
"""Procedural pixel-art generator for gugu art samples.

Pure Python (no PIL). Draws on a small base canvas (1 base px = 4 screen px on
the 466x466 AMOLED), writes 1x PNG, 4x nearest-neighbour PNG and crisp SVG.
The same generator is the intended production asset pipeline: sprites are
described as parameters, not hand-pixelled, so every expression stays
consistent and regeneration is deterministic.
"""
import os, sys, zlib, struct, math

OUT = os.path.join(os.path.dirname(__file__), '..', 'samples')

# ---------- palette ----------
PAL = {
    'body':   (0x80, 0x95, 0xAA), 'shade': (0x5E, 0x72, 0x87), 'line': (0x2B, 0x36, 0x42),
    'belly':  (0xB1, 0xC0, 0xCE), 'hi':    (0xD6, 0xE0, 0xE8),
    'scarf':  (0xD9, 0x41, 0x3B), 'scarf2':(0xA3, 0x2C, 0x29), 'knit': (0xEE, 0x6B, 0x62),
    'beak':   (0xE8, 0xA3, 0x3D), 'beak2': (0xB8, 0x74, 0x22), 'mouth': (0x6A, 0x2A, 0x2A),
    'eyew':   (0xFF, 0xFF, 0xFF), 'pupil': (0x1E, 0x24, 0x2B), 'cheek': (0xE9, 0x9A, 0x96),
    'foot':   (0xE0, 0x7A, 0x3F), 'bag':   (0x8B, 0x5A, 0x3C), 'bag2':  (0x5E, 0x3B, 0x26),
    'black':  (0x00, 0x00, 0x00), 'red2': (0xA3, 0x2C, 0x29),
    'yvest':  (0xE0, 0xB2, 0x3A), 'yvest2': (0xB8, 0x8A, 0x22), 'yknit': (0xF0, 0xCF, 0x6A),
    'gvest':  (0x5B, 0x8F, 0x6E), 'gvest2': (0x3F, 0x6B, 0x50), 'gknit': (0x86, 0xB8, 0x95),
    'lens':   (0xE8, 0xF2, 0xFA), 'rim':    (0x3A, 0x2E, 0x26),
    'case':   (0xD9, 0x41, 0x3B), 'case2':  (0xA3, 0x2C, 0x29), 'case3':  (0xEE, 0x6B, 0x62), 'tag':    (0xF4, 0xEA, 0xD5),
    'tcase':  (0x3F, 0x9A, 0x9E), 'tcase2': (0x2A, 0x6E, 0x72), 'tcase3': (0x7C, 0xC4, 0xC6), 'metal':  (0xB8, 0xBC, 0xC4), 'wheel':  (0x6A, 0x6E, 0x78),
    # room
    'wall':   (0x3A, 0x2B, 0x24), 'wall2': (0x4A, 0x37, 0x2D), 'floor': (0x7A, 0x52, 0x34), 'floor2': (0x5E, 0x3E, 0x28),
    'wood':   (0xB9, 0x8A, 0x5E), 'wood2': (0x8C, 0x64, 0x42), 'nest':  (0xA8, 0x7A, 0x48), 'nest2': (0x7D, 0x58, 0x32),
    'glow':   (0xF6, 0xD2, 0x7A), 'glow2': (0xC9, 0x9E, 0x4E), 'lampoff': (0x5A, 0x50, 0x48),
    'board':  (0x2E, 0x4A, 0x3F), 'chalk': (0xF2, 0xE7, 0xC9), 'frame': (0x6B, 0x4A, 0x30),
    'skyd':   (0x8F, 0xC7, 0xE8), 'skyd2': (0xBF, 0xE1, 0xF2), 'sun':   (0xFF, 0xD9, 0x5C), 'cloud': (0xFF, 0xFF, 0xFF),
    'skyn':   (0x10, 0x1A, 0x33), 'star':  (0xE6, 0xEE, 0xFF), 'moon':  (0xF2, 0xE7, 0xC9),
    'paper':  (0xF4, 0xEA, 0xD5), 'stamp': (0xC8, 0x5A, 0x4E), 'ink':   (0x6E, 0x86, 0x9E),
    'hook':   (0xC0, 0xB0, 0x98), 'shelf': (0x8C, 0x64, 0x42), 'sv1': (0x5FA0C8 >> 16, 0xA0, 0xC8), 'sv2': (0xE8, 0xC0, 0x5A),
}

class Canvas:
    def __init__(self, w, h, bg=None):
        self.w, self.h = w, h
        self.px = [[bg] * w for _ in range(h)]
    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h and c is not None:
            self.px[y][x] = c
    def get(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.px[y][x]
        return None
    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.put(xx, yy, c)
    def hline(self, x0, x1, y, c):
        for x in range(min(x0, x1), max(x0, x1) + 1): self.put(x, y, c)
    def vline(self, x, y0, y1, c):
        for y in range(min(y0, y1), max(y0, y1) + 1): self.put(x, y, c)
    def ellipse(self, cx, cy, rx, ry, c, pred=None):
        for y in range(int(cy - ry) - 1, int(cy + ry) + 2):
            for x in range(int(cx - rx) - 1, int(cx + rx) + 2):
                dx, dy = (x + 0.5 - cx) / rx, (y + 0.5 - cy) / ry
                if dx * dx + dy * dy <= 1.0 and (pred is None or pred(x, y)):
                    self.put(x, y, c)
    def outline_of(self, color_set, line):
        """1px outline around every pixel whose color is in color_set (drawn onto transparent/other)."""
        marks = []
        for y in range(self.h):
            for x in range(self.w):
                if self.px[y][x] in color_set: continue
                for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
                    if self.get(x+dx, y+dy) in color_set:
                        marks.append((x, y)); break
        for x, y in marks: self.put(x, y, line)
    def blit(self, other, ox, oy):
        for y in range(other.h):
            for x in range(other.w):
                c = other.px[y][x]
                if c is not None: self.put(ox + x, oy + y, c)
    def circle_mask(self, cx, cy, r):
        for y in range(self.h):
            for x in range(self.w):
                if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 > r * r:
                    self.px[y][x] = None

# ---------- encoders ----------
def png_bytes(canvas, scale=1, bg=None):
    w, h = canvas.w * scale, canvas.h * scale
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = canvas.px[y // scale]
        for x in range(w):
            c = row[x // scale]
            if c is None:
                if bg is None: raw += b'\x00\x00\x00\x00'
                else: raw += bytes(bg) + b'\xff'
            else:
                raw += bytes(c) + b'\xff'
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))

def svg_text(canvas, scale=4):
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{canvas.w*scale}" height="{canvas.h*scale}" '
             f'viewBox="0 0 {canvas.w} {canvas.h}" shape-rendering="crispEdges">']
    # run-length rows to keep the file small
    for y in range(canvas.h):
        x = 0
        while x < canvas.w:
            c = canvas.px[y][x]
            if c is None: x += 1; continue
            x0 = x
            while x < canvas.w and canvas.px[y][x] == c: x += 1
            parts.append(f'<rect x="{x0}" y="{y}" width="{x-x0}" height="1" fill="#{c[0]:02x}{c[1]:02x}{c[2]:02x}"/>')
    parts.append('</svg>')
    return '\n'.join(parts)

def save(canvas, name, scale=4, bg=None):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + '@1x.png'), 'wb') as f: f.write(png_bytes(canvas, 1, bg))
    with open(os.path.join(OUT, name + f'@{scale}x.png'), 'wb') as f: f.write(png_bytes(canvas, scale, bg))
    with open(os.path.join(OUT, name + '.svg'), 'w') as f: f.write(svg_text(canvas, scale))

# ---------- accessories ----------
def _knit_tail(c, x, y, length, P, spread=0):
    """A hanging scarf tail: 3 px wide, horizontal knit stripes, fringe at the end."""
    for i in range(length):
        xx = x + (spread if i > length // 2 else 0)
        col = P['scarf'] if (i // 2) % 2 == 0 else P['scarf2']
        c.rect(xx, y + i, 3, 1, col)
    fx = x + spread
    c.put(fx, y + length, P['scarf2']); c.put(fx + 2, y + length, P['scarf2'])
    c.put(fx, y + length + 1, P['scarf2']); c.put(fx + 2, y + length + 1, P['scarf2'])

def draw_scarf(c, cx, cy, rx, ry):
    """v2: a band that wraps the neck (dips in the middle), sticks out 1 px past the
    silhouette, with a knot on the right and two fringed tails hanging over the chest."""
    P = PAL
    sy = int(cy - 8)                       # top edge of the band at the sides
    half = int(rx * 0.93) + 1              # 1 px past the body on each side
    for x in range(int(cx) - half, int(cx) + half + 1):
        u = (x + 0.5 - cx) / half
        dip = int(round(2.4 * (1 - u * u)))
        top = sy + dip
        c.put(x, top, P['scarf2'])
        c.put(x, top + 1, P['scarf'])
        c.put(x, top + 2, P['knit'])
        c.put(x, top + 3, P['scarf'])
        c.put(x, top + 4, P['scarf2'])
    # knot: a bump on the right-front
    kx, ky = int(cx) + 6, sy + 2
    c.rect(kx, ky, 4, 4, P['scarf']); c.rect(kx + 1, ky + 1, 2, 2, P['knit']); c.put(kx, ky + 3, P['scarf2']); c.put(kx + 3, ky + 3, P['scarf2'])
    # two tails from the knot, the outer one longer
    _knit_tail(c, kx + 2, ky + 4, 9, P, spread=1)
    _knit_tail(c, kx - 2, ky + 4, 6, P, spread=0)
    c.outline_of({P['scarf'], P['scarf2'], P['knit']}, P['line'])

def draw_kerchief(c, cx, cy, rx, ry):
    """A triangular neckerchief: thin band round the neck, triangle hanging at the front, knot."""
    P = PAL
    sy = int(cy - 8)
    half = int(rx * 0.9)
    for x in range(int(cx) - half, int(cx) + half + 1):
        u = (x + 0.5 - cx) / half
        top = sy + int(round(2.0 * (1 - u * u)))
        c.put(x, top, P['scarf2']); c.put(x, top + 1, P['scarf'])
    apex = sy + 12
    for y in range(sy + 2, apex + 1):
        w = int(round(8 * (apex - y) / (apex - sy - 2)))
        c.hline(int(cx) - w, int(cx) + w, y, P['scarf'] if (y - sy) % 3 else P['knit'])
    c.rect(int(cx) - 2, sy + 1, 4, 3, P['scarf2']); c.rect(int(cx) - 1, sy + 2, 2, 1, P['knit'])
    c.outline_of({P['scarf'], P['scarf2'], P['knit']}, P['line'])

def draw_beanie(c, cx, cy, rx, ry):
    """A knitted beanie on top of the head with a folded brim and a pompom."""
    P = PAL
    top = cy - ry                          # top of the head
    dome_cy = top + 7
    c.ellipse(cx, dome_cy, rx * 0.86, 8.5, P['scarf'], pred=lambda x, y: y <= top + 7)
    c.ellipse(cx, dome_cy, rx * 0.86, 8.5, P['scarf2'], pred=lambda x, y: y <= top + 7 and (x + 0.5 - cx) > rx * 0.35)
    # knit ribs
    for x in range(int(cx - rx * 0.86), int(cx + rx * 0.86) + 1, 3):
        for y in range(int(top) - 1, int(top) + 6):
            if c.get(x, y) in (P['scarf'], P['scarf2']): c.put(x, y, P['knit'] if c.get(x, y) == P['scarf'] else P['scarf'])
    # folded brim
    bw = int(rx * 0.9)
    c.rect(int(cx) - bw, int(top) + 6, 2 * bw + 1, 3, P['scarf'])
    c.hline(int(cx) - bw, int(cx) + bw, int(top) + 7, P['knit'])
    # pompom
    c.ellipse(cx, top - 2, 2.6, 2.6, P['knit']); c.put(int(cx) - 1, int(top) - 3, P['eyew'])
    c.outline_of({P['scarf'], P['scarf2'], P['knit']}, P['line'])

def hook_item(c, x, y, accessory):
    """What hangs on the wall hook when Gugu is home."""
    P = PAL
    if accessory == 'scarf':
        for i in range(12):
            c.rect(x, y + i, 2, 1, P['scarf'] if (i // 2) % 2 == 0 else P['scarf2'])
            c.rect(x + 3, y + i - 2, 2, 1, P['scarf'] if (i // 2) % 2 else P['scarf2'])
        c.put(x, y + 12, P['scarf2']); c.put(x + 1, y + 13, P['scarf2']); c.put(x + 3, y + 10, P['scarf2']); c.put(x + 4, y + 11, P['scarf2'])
        c.rect(x, y - 1, 5, 2, P['scarf'])
    elif accessory == 'beanie':
        c.ellipse(x + 2.5, y + 4, 4.5, 4, P['scarf']); c.rect(x - 1, y + 7, 8, 2, P['knit']); c.ellipse(x + 2.5, y - 1, 1.6, 1.6, P['knit'])
    elif accessory == 'kerchief':
        c.rect(x - 1, y, 7, 2, P['scarf'])
        for i in range(6): c.hline(x + 2 - (5 - i) // 2, x + 2 + (5 - i) // 2, y + 2 + i, P['scarf'] if i % 3 else P['knit'])
    c.outline_of({P['scarf'], P['scarf2'], P['knit']}, P['line'])


VEST_COLORS = {'red': ('scarf', 'scarf2', 'knit'), 'yellow': ('yvest', 'yvest2', 'yknit'), 'green': ('gvest', 'gvest2', 'gknit')}

def draw_vest(c, cx, cy, rx, ry, color='red'):
    """A knitted vest: covers the torso below the shoulder line, V-neck, ribbed, hem at the bottom.
    Drawn only over body pixels so the silhouette (and the wings) stay readable."""
    P = PAL
    base, dark, light = (P[k] for k in VEST_COLORS[color])
    body_set = {P['body'], P['shade'], P['belly'], P['hi']}
    top = int(cy - 6)
    bottom = int(cy + ry) - 4
    vneck_depth = 5
    for y in range(top, bottom + 1):
        dy = (y + 0.5 - cy) / ry
        if dy * dy >= 1: continue
        half = rx * math.sqrt(1 - dy * dy)
        inner = half * 0.70 if y > top + 4 else half * 0.56      # armholes near the shoulders
        for x in range(int(cx - inner), int(cx + inner) + 1):
            if c.get(x, y) not in body_set: continue
            # V-neck
            vy = y - top
            if vy < vneck_depth and abs(x + 0.5 - cx) < (vneck_depth - vy) * 0.9: continue
            rib = (y - top) % 3
            col = light if rib == 2 else (dark if (x + 0.5 - cx) > inner * 0.55 else base)
            if y >= bottom - 1: col = dark
            c.put(x, y, col)
    c.outline_of({base, dark, light}, P['line'])

def draw_glasses(c, eyes, color='rim'):
    """Round frames around each eye box, a bridge, and temples running out to the head.
    eyes: list of (x, y, w, h) eye boxes as drawn."""
    P = PAL
    rim = P[color]
    boxes = []
    for (x, y, w, h) in eyes:
        fx0, fy0, fx1, fy1 = x - 1, y - 1, x + w, y + h
        boxes.append((fx0, fy0, fx1, fy1))
        # rounded rectangle: skip the four corners
        for xx in range(fx0 + 1, fx1):
            c.put(xx, fy0, rim); c.put(xx, fy1, rim)
        for yy in range(fy0 + 1, fy1):
            c.put(fx0, yy, rim); c.put(fx1, yy, rim)
        # a lens glint in the top-left of the frame if that pixel is body
        if c.get(fx0 + 1, fy0 + 1) in (P['body'], P['shade'], P['hi']): c.put(fx0 + 1, fy0 + 1, P['lens'])
    (l0, ly0, l1, ly1), (r0, ry0, r1, ry1) = boxes
    by = (ly0 + ly1) // 2
    c.hline(l1 + 1, r0 - 1, by, rim)                      # bridge
    c.hline(l0 - 3, l0 - 1, by, rim); c.hline(r1 + 1, r1 + 3, by, rim)   # temples

def hook_bag(c, x, y):
    """The satchel hanging from the wall hook (home state marker once the scarf is gone)."""
    P = PAL
    c.put(x + 2, y, P['bag2']); c.vline(x + 2, y, y + 4, P['bag2'])
    c.rect(x - 1, y + 5, 7, 6, P['bag']); c.rect(x - 1, y + 5, 7, 2, P['bag2']); c.put(x + 2, y + 8, P['beak'])
    c.outline_of({P['bag'], P['bag2']}, P['line'])

CASE_COLORS = {'red': ('case', 'case2', 'case3'), 'teal': ('tcase', 'tcase2', 'tcase3')}

def draw_case(c, x, y, color='red', handle_up=True, w=11, h=14):
    """A small rolling suitcase standing on its wheels: body, corner guards, a luggage tag,
    a telescopic handle (up when travelling / parked, down when stowed), two wheels."""
    P = PAL
    base, dark, light = (P[k] for k in CASE_COLORS[color])
    # handle
    if handle_up:
        c.vline(x + 3, y - 6, y - 1, P['metal']); c.vline(x + w - 4, y - 6, y - 1, P['metal'])
        c.hline(x + 2, x + w - 3, y - 7, P['wheel'])
    else:
        c.hline(x + 2, x + w - 3, y - 1, P['wheel'])
    # body with rounded corners
    c.rect(x, y, w, h, base)
    for cx_, cy_ in ((x, y), (x + w - 1, y), (x, y + h - 1), (x + w - 1, y + h - 1)): c.put(cx_, cy_, None)
    # shading on the right third and a light ridge on the left
    c.rect(x + w - 3, y + 1, 2, h - 2, dark)
    c.vline(x + 1, y + 1, y + h - 2, light)
    # ribs
    for yy in range(y + 2, y + h - 2, 4): c.hline(x + 2, x + w - 4, yy, dark)
    # luggage tag (cream) on the upper right
    c.rect(x + w - 6, y + 2, 4, 3, P['tag']); c.put(x + w - 4, y + 3, P['case2'] if color == 'red' else P['tcase2'])
    # corner guards
    for cx_, cy_ in ((x + 1, y + 1), (x + w - 2, y + 1), (x + 1, y + h - 2), (x + w - 2, y + h - 2)): c.put(cx_, cy_, dark)
    # wheels
    c.rect(x + 1, y + h, 2, 2, P['wheel']); c.rect(x + w - 3, y + h, 2, 2, P['wheel'])
    c.outline_of({base, dark, light, P['tag'], P['metal']}, P['line'])

def gugu_with_case(emotion='neutral', color='red', w=66, h=56):
    """Gugu standing beside the suitcase, handle up (the departure / travelling pose)."""
    c = Canvas(w, h)
    g = gugu(emotion)
    c.blit(g, 0, 0)
    draw_case(c, 51, 34, color=color, handle_up=True)
    return c

# ---------- gugu sprite ----------
def gugu(emotion='neutral', scarf=True, bag=False, w=48, h=54, accessory=None, beak_open=False, eyes_closed=False):
    """Front-facing round pigeon. Emotions change eyes/beak/wings/body only.

    accessory: 'scarf' (v2: curved wrap, knot, fringed tails), 'beanie', 'kerchief', None.
    The legacy `scarf` flag maps to accessory='scarf'.
    """
    if accessory is None:                      # decided 2026-11-02: round glasses + satchel, no scarf
        accessory = 'glasses'
    parts = set(accessory.split('+'))
    vest_color = 'red'
    for pfx, col in (('yvest', 'yellow'), ('gvest', 'green')):
        if pfx in parts: parts.discard(pfx); parts.add('vest'); vest_color = col
    eye_boxes = []
    c = Canvas(w, h)
    P = PAL
    cx, cy = w / 2, 29.0
    rx, ry = 17.0, 19.5
    squash = 1.0
    if emotion == 'puff':     rx, ry = 20.0, 17.5; cy = 31.0
    if emotion == 'sleepy':   ry = 18.5; cy = 30.0
    if emotion == 'surprised': rx, ry = 15.5, 22.0; cy = 27.0
    if emotion == 'laughing':  rx, ry = 19.5, 17.5; cy = 31.0
    if emotion == 'sad':       ry = 18.5; cy = 31.0
    if emotion == 'scared':    rx, ry = 16.0, 20.0; cy = 29.0
    if emotion == 'proud':     ry = 20.5; cy = 28.0
    body_set = {P['body'], P['shade'], P['belly'], P['hi']}

    # wings (behind body)
    wing_dy = {'wave': -9, 'happy': -6, 'laughing': -10, 'surprised': -8, 'proud': -4, 'scared': -12, 'sad': 4, 'angry': -5, 'shy': 2}.get(emotion, 0)
    for side in (-1, 1):
        dy = wing_dy if side == 1 or emotion != 'wave' else 0
        if emotion == 'puff':
            continue
        c.ellipse(cx + side * (rx - 1.5), cy + 3 + dy, 4.5, 8.0, P['shade'])

    # body + shading + belly
    c.ellipse(cx, cy, rx, ry, P['body'])
    c.ellipse(cx, cy, rx, ry, P['shade'], pred=lambda x, y: ((x + 0.5 - cx) / rx) * 0.55 + ((y + 0.5 - cy) / ry) > 0.72)
    c.ellipse(cx, cy + 6, rx * 0.56, ry * 0.5, P['belly'])
    c.ellipse(cx - rx * 0.45, cy - ry * 0.55, 3.5, 2.5, P['hi'])
    if emotion == 'puff':  # ruffled feather tips
        for ang in range(0, 360, 30):
            a = math.radians(ang)
            x = int(cx + math.cos(a) * (rx + 0.8)); y = int(cy + math.sin(a) * (ry + 0.8))
            c.put(x, y, P['body'])

    # feet
    for fx in (int(cx) - 6, int(cx) + 3):
        c.rect(fx, int(cy + ry) - 1, 3, 2, P['foot'])
        c.put(fx - 1, int(cy + ry), P['foot']); c.put(fx + 3, int(cy + ry), P['foot'])

    # outline of body-ish colors
    c.outline_of(body_set | {P['foot']}, P['line'])

    if 'vest' in parts:
        draw_vest(c, cx, cy, rx, ry, vest_color)

    # satchel strap + bag (left hip)
    if bag:
        for i in range(0, 14):
            x = int(cx - rx + 4 + i * 0.75); y = int(cy - 6 + i)
            c.put(x, y, P['bag2'])
        c.rect(int(cx - rx + 2), int(cy + 7), 7, 6, P['bag'])
        c.rect(int(cx - rx + 2), int(cy + 7), 7, 2, P['bag2'])
        c.put(int(cx - rx + 5), int(cy + 10), P['beak'])

    # ---- accessory ----
    if 'scarf' in parts:
        draw_scarf(c, cx, cy, rx, ry)
    elif 'kerchief' in parts:
        draw_kerchief(c, cx, cy, rx, ry)
    # beanie is drawn after the face so it can overlap the forehead

    # face — cartoon-big: every mood is a different shape, readable from across the room (2026-11-03, "再夸张一点")
    ey = int(cy - 11)
    exl, exr = int(cx) - 7, int(cx) + 3
    def eye_box(x0, y0, w, h, pw=2, ph=2, look=(0, 0)):
        c.rect(x0, y0, w, h, P['eyew']); eye_boxes.append((x0, y0, w, h))
        px, py = x0 + w // 2 - pw // 2 + look[0], y0 + h // 2 - ph // 2 + look[1]
        c.rect(px, py, pw, ph, P['pupil']); c.put(px, py, P['hi'])
    def eye_open(x, y, pw=2, ph=3, look=(0, 0)):            # the everyday eye: 6×7
        eye_box(x - 1, y - 1, 6, 7, pw, ph, look)
    def eye_arc(x, y, down=False, wide=False):              # a closed happy eye ︶ (︵ when down); wide = a tall laugh
        eye_boxes.append((x, y, 4, 5))
        if wide:
            c.hline(x - 1, x + 4, y + 1, P['line']); c.put(x - 2, y + 2, P['line']); c.put(x + 5, y + 2, P['line'])
            c.put(x - 3, y + 3, P['line']); c.put(x + 6, y + 3, P['line']); c.put(x - 3, y + 4, P['line']); c.put(x + 6, y + 4, P['line'])
            c.hline(x - 1, x + 4, y + 2, P['line'])
        elif not down:
            c.hline(x - 1, x + 4, y + 2, P['line']); c.put(x - 2, y + 3, P['line']); c.put(x + 5, y + 3, P['line']); c.hline(x, x + 3, y + 1, P['line'])
        else:
            c.hline(x - 1, x + 4, y + 3, P['line']); c.put(x - 2, y + 2, P['line']); c.put(x + 5, y + 2, P['line'])
    def eye_squeeze(x, y):                                  # > <  laughing hard, 6 px
        eye_boxes.append((x - 1, y, 6, 6))
        for i in range(4): c.put(x - 1 + i, y + i, P['line']); c.put(x - 1 + i, y + 5 - i, P['line']); c.put(x + i, y + i, P['line']); c.put(x + i, y + 5 - i, P['line'])
    def eye_shut(x, y):                                     # a heavy lid: fast asleep
        eye_boxes.append((x - 1, y, 6, 5))
        c.hline(x - 2, x + 5, y + 3, P['line']); c.hline(x - 1, x + 4, y + 4, P['line'])
    def eye_spiral(x, y):                                   # knocked-out X eyes, 7×7
        x0, y0 = x - 1, y - 1
        eye_boxes.append((x0, y0, 7, 7)); c.rect(x0, y0, 7, 7, P['eyew'])
        for i in range(7): c.put(x0 + i, y0 + i, P['pupil']); c.put(x0 + 6 - i, y0 + i, P['pupil'])
        for i in range(1, 6): c.put(x0 + i, y0 + i - 1, P['pupil']); c.put(x0 + 6 - i, y0 + i - 1, P['pupil'])
    def star(sx, sy, big=False):
        c.hline(sx - 1, sx + 1, sy, P['glow']); c.vline(sx, sy - 1, sy + 1, P['glow'])
        if big: c.hline(sx - 2, sx + 2, sy, P['glow']); c.vline(sx, sy - 2, sy + 2, P['glow'])
    def brow_v(thick=2):                                    # angry: thick brows slanting down to the middle
        for i in range(5):
            for t in range(thick):
                c.put(exl - 2 + i, ey - 4 + i // 2 + t, P['line']); c.put(exr + 5 - i, ey - 4 + i // 2 + t, P['line'])
    def brow_sad():                                         # inner ends up
        for i in range(5):
            c.put(exl - 2 + i, ey - 2 - i // 2, P['line']); c.put(exr + 5 - i, ey - 2 - i // 2, P['line'])

    if eyes_closed:
        eye_arc(exl, ey); eye_arc(exr, ey)
    elif emotion == 'neutral':
        eye_open(exl, ey); eye_open(exr, ey)
    elif emotion == 'happy':
        eye_arc(exl, ey, wide=True); eye_arc(exr, ey, wide=True)
    elif emotion == 'laughing':
        eye_squeeze(exl, ey); eye_squeeze(exr, ey)
        c.rect(exr + 6, ey + 3, 2, 3, P['skyd']); c.put(exr + 6, ey + 3, P['cloud'])        # a tear of joy
        for sgn in (-1, 1):                                                                # "ha ha" lines beside the head
            x = int(cx) + sgn * 21
            for yy in (ey - 4, ey, ey + 4): c.hline(x - 1, x + 1, yy, P['hi'])
    elif emotion == 'surprised':                                                           # goggle eyes
        eye_box(14, ey - 3, 10, 10, 2, 2); eye_box(24, ey - 3, 10, 10, 2, 2)
        c.hline(13, 17, ey - 6, P['line']); c.hline(30, 34, ey - 6, P['line'])            # brows shot up
    elif emotion == 'curious':                                                             # "hmm?": eyes up and to the side, one brow up, head tilted a pixel
        eye_open(exl, ey + 1, pw=2, ph=3, look=(1, -1)); eye_open(exr, ey - 1, pw=2, ph=3, look=(1, -1))
        c.hline(exr - 1, exr + 4, ey - 5, P['line']); c.put(exr + 5, ey - 4, P['line'])     # the raised brow
        c.hline(exl - 1, exl + 4, ey - 1, P['line'])                                           # the other one flat and low
    elif emotion == 'shy':                                                                 # peeking from behind a wing
        eye_open(exl, ey + 1, pw=2, ph=3, look=(-1, 1)); eye_open(exr, ey + 1, pw=2, ph=3, look=(-1, 1))   # eyes down and away
        c.rect(exl - 7, ey + 5, 6, 3, P['cheek']); c.rect(exr + 6, ey + 5, 6, 3, P['cheek'])              # enormous blush
        c.put(exl - 8, ey + 6, P['cheek']); c.put(exr + 12, ey + 6, P['cheek'])
        for i, (sx, sy) in enumerate(((int(cx) + 15, ey - 9), (int(cx) + 18, ey - 6))):                     # two little hearts
            c.put(sx, sy, P['cheek']); c.put(sx + 2, sy, P['cheek']); c.hline(sx, sx + 2, sy + 1, P['cheek']); c.put(sx + 1, sy + 2, P['cheek'])
    elif emotion == 'sad':
        eye_open(exl, ey + 1, look=(0, 1)); eye_open(exr, ey + 1, look=(0, 1))
        brow_sad()
        for x in (exl + 1, exr + 1):                                                       # tears streaming
            c.rect(x, ey + 7, 2, 6, P['skyd']); c.put(x, ey + 7, P['cloud']); c.put(x + 1, ey + 13, P['skyd'])
    elif emotion == 'sleepy':
        eye_shut(exl, ey + 1); eye_shut(exr, ey + 1)
        c.ellipse(cx + 4, ey + 9, 3.5, 3.5, P['skyd']); c.put(int(cx) + 3, ey + 7, P['cloud'])   # a snot bubble
    elif emotion == 'puff':
        eye_arc(exl, ey + 1); eye_arc(exr, ey + 1)
    elif emotion == 'wave':
        eye_arc(exl, ey, wide=True); eye_open(exr, ey)
    elif emotion == 'angry':
        eye_open(exl, ey, pw=2, ph=2, look=(1, 1)); eye_open(exr, ey, pw=2, ph=2, look=(0, 1))
        brow_v(3)
        for sx in (int(cx) - 17, int(cx) + 12):                                            # steam
            c.rect(sx, ey - 11, 5, 3, P['cloud']); c.rect(sx + 1, ey - 12, 3, 1, P['cloud']); c.put(sx + 2, ey - 13, P['cloud'])
    elif emotion == 'scared':
        eye_box(14, ey - 3, 10, 10, 1, 1, look=(-2, -2)); eye_box(24, ey - 3, 10, 10, 1, 1, look=(-2, -2))
        for sx, sy in ((int(cx) + 14, ey - 8), (int(cx) + 17, ey - 3), (int(cx) - 16, ey - 5)):   # sweat flying
            c.put(sx, sy, P['skyd']); c.rect(sx - 1, sy + 1, 3, 2, P['skyd'])
        for sgn in (-1, 1):                                                                # trembling lines
            x = int(cx) + sgn * 21
            for yy in (ey + 6, ey + 10, ey + 14): c.vline(x, yy, yy + 2, P['hi'])
    elif emotion == 'proud':
        eye_arc(exl, ey - 3); eye_arc(exr, ey - 3)
        c.ellipse(cx, cy + 4, rx * 0.56, ry * 0.5, P['hi'], pred=lambda x, y: c.get(x, y) == P['belly'])
        for sx, sy, b in ((int(cx) - 16, ey - 8, True), (int(cx) + 15, ey - 9, True), (int(cx) + 18, ey, False), (int(cx) - 19, ey + 1, False)): star(sx, sy, b)
    elif emotion == 'dizzy':
        eye_spiral(exl, ey - 1); eye_spiral(exr, ey + 1)                                   # a tilt: one eye higher than the other
        for sx, sy in ((int(cx) - 17, ey - 9), (int(cx) + 15, ey - 12), (int(cx) - 3, ey - 15), (int(cx) + 19, ey - 1)): star(sx, sy, True)

    # cheeks
    if emotion in ('happy', 'laughing', 'proud', 'wave'):
        c.rect(exl - 4, ey + 5, 4, 2, P['cheek']); c.rect(exr + 5, ey + 5, 4, 2, P['cheek'])
    elif emotion == 'angry':
        c.rect(exl - 4, ey + 5, 4, 3, P['red2']); c.rect(exr + 5, ey + 5, 4, 3, P['red2'])
    elif emotion in ('neutral', 'curious', 'puff'):
        c.rect(exl - 3, ey + 5, 2, 1, P['cheek']); c.rect(exr + 5, ey + 5, 2, 1, P['cheek'])

    # beak and mouth
    bx, by = int(cx) - 1, ey + 4
    if emotion == 'laughing':                                                              # a huge laugh with a tongue
        c.rect(bx - 2, by, 7, 1, P['beak']); c.rect(bx - 2, by + 1, 7, 4, P['mouth']); c.rect(bx - 1, by + 3, 5, 2, P['cheek']); c.rect(bx - 2, by + 5, 7, 1, P['beak2'])
    elif emotion == 'surprised':                                                           # a tall O
        c.rect(bx - 1, by + 1, 5, 1, P['beak']); c.rect(bx - 1, by + 2, 5, 4, P['mouth']); c.rect(bx - 1, by + 6, 5, 1, P['beak2'])
    elif beak_open:
        c.rect(bx, by, 3, 1, P['beak']); c.rect(bx, by + 1, 3, 2, P['mouth']); c.rect(bx, by + 3, 3, 1, P['beak2'])
    elif emotion in ('happy', 'proud', 'wave'):                                            # a grin from cheek to cheek
        c.rect(bx, by, 3, 1, P['beak']); c.put(bx + 1, by + 1, P['beak2'])
        c.put(bx - 4, by, P['line']); c.put(bx - 3, by + 1, P['line']); c.hline(bx - 2, bx + 4, by + 2, P['line']); c.put(bx + 5, by + 1, P['line']); c.put(bx + 6, by, P['line'])
    elif emotion == 'sad':                                                                 # a big frown
        c.rect(bx, by + 1, 3, 1, P['beak']); c.put(bx + 1, by + 2, P['beak2'])
        c.put(bx - 3, by + 5, P['line']); c.put(bx - 2, by + 4, P['line']); c.hline(bx - 1, bx + 3, by + 3, P['line']); c.put(bx + 4, by + 4, P['line']); c.put(bx + 5, by + 5, P['line'])
    elif emotion == 'scared':                                                              # a wobbly wail
        c.rect(bx, by, 3, 1, P['beak']); c.rect(bx, by + 1, 3, 2, P['mouth'])
        for i in range(7): c.put(bx - 2 + i, by + 3 + (i % 2), P['line'])
    elif emotion == 'angry':                                                               # gritted teeth
        c.rect(bx, by, 3, 1, P['beak']); c.rect(bx - 2, by + 1, 7, 2, P['line']); c.rect(bx - 1, by + 1, 5, 1, P['eyew']); c.put(bx, by + 1, P['line']); c.put(bx + 2, by + 1, P['line'])
    elif emotion == 'dizzy':                                                               # a wavy mouth
        c.rect(bx, by, 3, 1, P['beak'])
        for i in range(7): c.put(bx - 2 + i, by + 1 + (1 if i % 3 == 1 else 0), P['line'])
    elif emotion == 'shy':
        pass                                                                               # behind the wing
    else:
        c.rect(bx, by, 3, 1, P['beak']); c.put(bx + 1, by + 1, P['beak2'])

    if 'glasses' in parts and len(eye_boxes) == 2:
        draw_glasses(c, eye_boxes)
    if 'beanie' in parts:
        draw_beanie(c, cx, cy, rx, ry)

    # sleepy "Z z"
    if emotion == 'sleepy':
        zx, zy = int(cx) + 12, ey - 11
        c.hline(zx, zx + 4, zy, P['hi']); c.put(zx + 3, zy + 1, P['hi']); c.put(zx + 2, zy + 2, P['hi']); c.put(zx + 1, zy + 3, P['hi']); c.hline(zx, zx + 4, zy + 4, P['hi'])
        zx, zy = int(cx) + 17, ey - 15
        c.hline(zx, zx + 2, zy, P['hi']); c.put(zx + 1, zy + 1, P['hi']); c.hline(zx, zx + 2, zy + 2, P['hi'])
    if emotion == 'surprised':                                                     # a big !
        c.rect(int(cx) + 15, ey - 9, 2, 6, P['glow']); c.rect(int(cx) + 15, ey - 2, 2, 2, P['glow'])
    if emotion == 'curious':                                                       # a big ?
        qx, qy = int(cx) + 14, ey - 12
        c.hline(qx, qx + 3, qy, P['glow']); c.rect(qx + 3, qy + 1, 2, 2, P['glow']); c.rect(qx + 1, qy + 3, 2, 2, P['glow']); c.rect(qx + 1, qy + 6, 2, 2, P['glow'])
    if emotion == 'happy':
        c.put(int(cx) - 15, ey - 6, P['glow']); c.put(int(cx) + 14, ey - 4, P['glow'])
    global LAST_GEOM
    LAST_GEOM = {'eye_boxes': list(eye_boxes), 'head_top': cy - ry, 'cx': cx, 'cy': cy, 'rx': rx, 'ry': ry}   # for accessory anchors (export_lvgl.py)
    return c

# ---------- home scene ----------
def room(state='home', night=False, moons=2, W=116, H=116, lamp_on=None):
    c = Canvas(W, H, PAL['black'])
    P = PAL
    cx, cy, R = W / 2, H / 2, W / 2
    # wall + floor
    c.ellipse(cx, cy, R, R, P['wall'])
    c.ellipse(cx, cy, R, R, P['floor'], pred=lambda x, y: y >= 84)
    for y in range(84, H):
        for x in range(W):
            if c.get(x, y) == P['floor'] and ((x // 6 + y // 4) % 2 == 0): c.put(x, y, P['floor2'])
    # wall paneling hint
    for y in range(0, 84, 7):
        for x in range(W):
            if c.get(x, y) == P['wall']: c.put(x, y, P['wall2'])
    # lamp glow (home: on; away: off)
    if lamp_on is None: lamp_on = (state == 'home')
    if lamp_on:
        for r, col in ((26, P['wall2']), (18, P['glow2']), (10, P['glow'])):
            pass
        rings = ((19, (0x4C, 0x3A, 0x2C)), (14, (0x5E, 0x48, 0x32)), (9, (0x74, 0x58, 0x38)), (5, (0x8C, 0x6C, 0x40)))
        for r, col in rings:
            c.ellipse(92, 34, r, r * 0.85, col, pred=lambda x, y: c.get(x, y) in (P['wall'], P['wall2']) or (c.get(x, y) not in (P['frame'], P['hook'], P['glow'], P['glow2']) and y < 84 and 70 <= x <= 114 and 15 <= y <= 55))
    # lamp fixture
    c.rect(88, 20, 9, 6, P['glow'] if lamp_on else P['lampoff'])      # shade
    c.rect(89, 26, 7, 1, P['glow2'] if lamp_on else P['lampoff'])
    c.vline(92, 10, 19, P['hook'])                                      # cord
    # window (top-left) with real sky
    wx, wy, wr = 34, 34, 15
    c.ellipse(wx, wy, wr + 2, wr + 2, P['frame'])
    sky = P['skyn'] if night else P['skyd']
    c.ellipse(wx, wy, wr, wr, sky)
    if night:
        for sx, sy in ((26, 26), (40, 24), (44, 38), (30, 44), (38, 32)): c.put(sx, sy, P['star'])
        c.ellipse(28, 30, 4, 4, P['moon']); c.ellipse(30, 29, 3.2, 3.2, sky)
    else:
        c.ellipse(27, 27, 4, 4, P['sun'])
        c.rect(34, 38, 10, 3, P['cloud']); c.rect(36, 36, 6, 2, P['cloud']); c.rect(24, 42, 7, 2, P['cloud'])
    c.vline(wx, wy - wr, wy + wr, P['frame']); c.hline(wx - wr, wx + wr, wy, P['frame'])
    # shelf with souvenirs (top middle)
    c.rect(50, 16, 22, 2, P['shelf']); c.rect(50, 18, 22, 1, P['wood2'])
    c.rect(53, 10, 4, 6, P['sv1']); c.rect(54, 9, 2, 1, P['sv1'])        # a small blue shell/bottle
    c.rect(62, 11, 6, 5, P['sv2']); c.rect(63, 10, 4, 1, P['sv2'])        # a yellow something
    # chalkboard (right middle) with moons
    c.rect(82, 50, 24, 18, P['frame']); c.rect(84, 52, 20, 14, P['board'])
    for i in range(moons):
        mx = 88 + i * 7
        c.ellipse(mx, 59, 2.5, 2.5, P['chalk']); c.ellipse(mx + 1.2, 58.4, 2.0, 2.0, P['board'])
    if state == 'away' and moons >= 1:
        c.hline(86, 90, 59, P['chalk'])  # one crossed out
    # table (left) with postcard
    c.rect(8, 76, 26, 3, P['wood']); c.rect(10, 79, 3, 12, P['wood2']); c.rect(29, 79, 3, 12, P['wood2'])
    if state == 'away':
        c.rect(13, 64, 16, 12, P['paper']); c.rect(24, 66, 3, 3, P['stamp'])
        c.hline(15, 21, 68, P['ink']); c.hline(15, 23, 71, P['ink']); c.hline(15, 19, 74, P['ink'])
    # nest (bottom centre)
    c.ellipse(58, 98, 22, 8, P['nest']); c.ellipse(58, 98, 22, 8, P['nest2'], pred=lambda x, y: (x + y) % 3 == 0)
    c.ellipse(58, 95, 16, 5, P['floor2'])
    return c

def compose_home(night=False, accessory='scarf', worn='glasses', hook='case', case_color='red'):
    scene = room('home', night=night, moons=0)
    g = gugu('neutral', scarf=False, bag=False, accessory=worn)
    scene.blit(g, 58 - 24, 96 - 54 + 4)
    # the accessory hangs on the hook when home
    c = scene; P = PAL
    if hook == 'case':
        draw_case(c, 94, 70, color=case_color, handle_up=False)   # parked in the corner by the chalkboard
    elif hook == 'bag':
        hook_bag(c, 71, 44)
    elif hook != 'none':
        hook_item(c, 72, 44, accessory)
    # redraw nest front rim over gugu's feet
    c.ellipse(58, 100, 22, 6, P['nest'], pred=lambda x, y: y >= 100)
    c.ellipse(58, 100, 22, 6, P['nest2'], pred=lambda x, y: y >= 100 and (x + y) % 3 == 0)
    return scene

def compose_away(night=True):
    return room('away', night=night, moons=2)

# ---------- sprite sheet ----------
def sheet(names, cols=4, cell=(52, 58), bg=None, accessory='glasses'):
    rows = (len(names) + cols - 1) // cols
    s = Canvas(cols * cell[0], rows * cell[1], bg)
    for i, n in enumerate(names):
        g = gugu(n, accessory=accessory)
        s.blit(g, (i % cols) * cell[0] + 2, (i // cols) * cell[1] + 2)
    return s

def accessory_sheet(emotions, bg=None, cell=(52, 58), variants=('scarf', 'beanie', 'kerchief')):
    s = Canvas(len(emotions) * cell[0], len(variants) * cell[1], bg)
    for r, a in enumerate(variants):
        for i, e in enumerate(emotions):
            s.blit(gugu(e, accessory=a, bag=True), i * cell[0] + 2, r * cell[1] + 2)
    return s

if __name__ == '__main__':
    emotions = ['neutral', 'happy', 'laughing', 'surprised', 'curious', 'shy', 'sad', 'sleepy', 'puff', 'wave']
    for e in emotions:
        save(gugu(e), f'gugu_{e}', scale=4)
    save(sheet(emotions, cols=5), 'gugu_sheet', scale=4, bg=(0x10, 0x10, 0x14))
    for col in ('red', 'teal'):
        cs = Canvas(5 * 70, 2 * 60, (0x10, 0x10, 0x14))
        for i, e in enumerate(emotions):
            cs.blit(gugu_with_case(e, color=col), (i % 5) * 70 + 2, (i // 5) * 60 + 2)
        save(cs, f'gugu_case_{col}', scale=4, bg=(0x10, 0x10, 0x14))
        save(gugu_with_case('neutral', color=col), f'gugu_neutral_case_{col}', scale=4)
    save(compose_home(night=False, case_color='teal'), 'scene_home_day_case_teal', scale=4)
    save(accessory_sheet(['neutral', 'happy', 'surprised', 'sleepy', 'wave'], bg=(0x10, 0x10, 0x14)), 'gugu_accessories', scale=4)
    save(accessory_sheet(['neutral', 'happy', 'surprised', 'sleepy', 'wave', 'shy', 'puff'], bg=(0x10, 0x10, 0x14),
                         variants=('glasses', 'vest', 'vest+glasses', 'yvest+glasses', 'gvest+glasses')), 'gugu_accessories2', scale=4)
    for a in ('glasses', 'vest', 'vest+glasses', 'yvest+glasses'):
        save(gugu('neutral', accessory=a), f'gugu_neutral_{a.replace("+", "_")}', scale=4)
    save(compose_home(night=False, worn='glasses', hook='bag'), 'scene_home_day_glasses_bag', scale=4)
    save(compose_home(night=False, worn='vest+glasses', hook='bag'), 'scene_home_day_vest_glasses_bag', scale=4)
    for a in ('scarf', 'beanie', 'kerchief'):
        save(gugu('neutral', accessory=a), f'gugu_neutral_{a}', scale=4)
    save(compose_home(night=False, accessory='beanie', worn='none', hook='auto'), 'scene_home_day_beanie', scale=4)
    save(compose_home(night=False), 'scene_home_day', scale=4)
    save(compose_home(night=True), 'scene_home_night', scale=4)
    save(compose_away(night=True), 'scene_away_night', scale=4)
    save(compose_away(night=False), 'scene_away_day', scale=4)
    print('ok', OUT)


# ---------- v1.1: built-in props (the fallback behind generated ones), accessories, the window ----------
PAL.update({'grey': (0x6A, 0x6E, 0x78), 'grey2': (0x4A, 0x4E, 0x58), 'red': (0xD9, 0x41, 0x3B), 'red2': (0xA3, 0x2C, 0x29), 'white': (0xF4, 0xF4, 0xF0),
            'green': (0x5B, 0x8F, 0x6E), 'green2': (0x3F, 0x6B, 0x50), 'green3': (0x86, 0xB8, 0x95), 'brown': (0x8B, 0x5A, 0x3C), 'brown2': (0x5E, 0x3B, 0x26),
            'orange': (0xE8, 0x8A, 0x3D), 'yellow': (0xF6, 0xD2, 0x7A), 'straw': (0xF0, 0xCF, 0x6A), 'straw2': (0xE0, 0xB2, 0x3A), 'lensdk': (0x4A, 0x5A, 0x6A)})
LAST_GEOM = None

def outline(c):
    """A 1-px dark line around everything opaque (leave a 1-px margin on the canvas)."""
    opaque = {col for row in c.px for col in row if col is not None}
    c.outline_of(opaque, PAL['line'])
    return c

def prop_lamp():
    c = Canvas(13, 40); P = PAL
    c.rect(5, 8, 3, 29, P['grey2']); c.rect(6, 8, 1, 29, P['grey'])
    c.rect(3, 36, 7, 2, P['grey2'])
    c.rect(3, 2, 7, 6, P['yellow']); c.rect(4, 1, 5, 1, P['grey2']); c.rect(2, 8, 9, 1, P['grey2']); c.put(6, 0, P['grey2']); c.rect(5, 4, 3, 2, P['white'])
    return outline(c)

def prop_bench():
    c = Canvas(26, 14); P = PAL
    c.rect(2, 1, 22, 2, P['wood']); c.rect(2, 4, 22, 2, P['wood'])
    c.rect(1, 7, 24, 3, P['wood']); c.rect(1, 9, 24, 1, P['wood2'])
    c.rect(3, 10, 2, 3, P['wood2']); c.rect(21, 10, 2, 3, P['wood2']); c.rect(3, 3, 2, 4, P['wood2']); c.rect(21, 3, 2, 4, P['wood2'])
    return outline(c)

def prop_mailbox():
    c = Canvas(9, 16); P = PAL
    c.ellipse(4.5, 3.5, 3.5, 2.5, P['red2']); c.rect(1, 3, 7, 10, P['red']); c.rect(1, 12, 7, 1, P['red2'])
    c.rect(2, 6, 5, 1, P['line']); c.rect(1, 13, 7, 2, P['grey2'])
    return outline(c)

def prop_tree():
    c = Canvas(28, 40); P = PAL
    c.rect(12, 24, 4, 15, P['brown']); c.rect(12, 24, 1, 15, P['brown2'])
    c.ellipse(14, 14, 12.5, 11.5, P['green'])
    c.ellipse(14, 14, 12.5, 11.5, P['green2'], pred=lambda x, y: (x - 14) * 0.6 + (y - 14) > 5)
    c.ellipse(10, 9, 4, 3, P['green3'])
    return outline(c)

def prop_palm():
    c = Canvas(26, 44); P = PAL
    for i in range(30):
        y = 42 - i; x = 11 + i // 8
        c.rect(x, y, 3, 1, P['brown2'] if i % 4 == 0 else P['brown'])
    for ang, ln in ((-165, 11), (-125, 10), (-90, 8), (-55, 10), (-15, 11)):
        a = math.radians(ang)
        for t in range(ln):
            x = int(round(15 + math.cos(a) * t)); y = int(round(12 + math.sin(a) * t + 0.09 * t * t))
            c.put(x, y, P['green'] if t < ln - 3 else P['green2']); c.put(x, y + 1, P['green2'])
    c.rect(14, 11, 3, 3, P['brown2'])
    return outline(c)

def prop_umbrella():
    c = Canvas(34, 34); P = PAL
    c.ellipse(17, 11, 15.5, 8, P['red'], pred=lambda x, y: y <= 11)
    c.ellipse(17, 11, 15.5, 8, P['white'], pred=lambda x, y: y <= 11 and ((x - 2) // 5) % 2 == 1)
    c.rect(16, 11, 3, 1, P['red2']); c.rect(17, 12, 1, 21, P['grey2']); c.put(17, 1, P['grey2'])
    return outline(c)

def prop_lounger():
    c = Canvas(30, 16); P = PAL
    for i in range(8): c.rect(3 + i, 10 - i, 2, 1, P['red'] if i % 2 else P['white'])
    for x in range(9, 27):
        for y in range(9, 12): c.put(x, y, P['white'] if (x // 3) % 2 == 1 else P['red'])
    c.rect(2, 12, 26, 1, P['wood2']); c.rect(4, 13, 2, 2, P['wood2']); c.rect(24, 13, 2, 2, P['wood2'])
    return outline(c)

def prop_boat():
    c = Canvas(30, 12); P = PAL
    for i in range(6): c.rect(2 + i, 4 + i, 26 - 2 * i, 1, P['brown'] if i < 5 else P['brown2'])
    c.rect(1, 3, 28, 1, P['brown2']); c.rect(8, 5, 2, 3, P['wood']); c.rect(20, 5, 2, 3, P['wood'])
    return outline(c)

def prop_lantern():
    c = Canvas(10, 12); P = PAL
    c.rect(3, 1, 4, 1, P['yellow']); c.ellipse(5, 6, 3.8, 3.5, P['red']); c.rect(2, 5, 6, 1, P['red2']); c.rect(3, 10, 4, 1, P['yellow']); c.put(5, 11, P['yellow'])
    return outline(c)

def prop_flag():
    c = Canvas(16, 24); P = PAL
    c.rect(2, 1, 1, 22, P['grey2']); c.rect(3, 2, 11, 8, P['red']); c.rect(6, 4, 3, 3, P['yellow']); c.rect(1, 22, 3, 1, P['grey2'])
    return outline(c)

def prop_cat():
    c = Canvas(16, 16); P = PAL
    c.ellipse(9, 11, 5.5, 4, P['orange']); c.ellipse(5, 6, 4, 3.5, P['orange'])
    c.put(2, 3, P['orange']); c.put(3, 2, P['orange']); c.put(7, 2, P['orange']); c.put(8, 3, P['orange'])
    c.put(4, 6, P['line']); c.put(7, 6, P['line']); c.rect(5, 8, 2, 1, P['cheek'])
    for i in range(5): c.put(13 + (1 if i > 2 else 0), 11 - i, P['orange'])
    return outline(c)

def prop_seagull():
    c = Canvas(18, 18); P = PAL
    c.ellipse(9, 10, 6, 4, P['white']); c.ellipse(13, 7, 3, 2.8, P['white']); c.ellipse(8, 10, 4, 2.2, P['grey'])
    c.rect(15, 7, 3, 1, P['beak']); c.put(14, 6, P['line']); c.rect(7, 14, 1, 3, P['foot']); c.rect(10, 14, 1, 3, P['foot'])
    return outline(c)

PROPS = {'lamp': prop_lamp, 'bench': prop_bench, 'mailbox': prop_mailbox, 'tree': prop_tree, 'palm': prop_palm, 'umbrella': prop_umbrella,
         'lounger': prop_lounger, 'boat': prop_boat, 'lantern': prop_lantern, 'flag': prop_flag, 'cat': prop_cat, 'seagull': prop_seagull}

def acc_sun_hat():
    c = Canvas(24, 9); P = PAL
    c.rect(1, 6, 22, 2, P['straw2']); c.rect(6, 1, 12, 5, P['straw']); c.rect(7, 0, 10, 1, P['straw']); c.rect(6, 4, 12, 1, P['red2'])
    return outline(c)

def acc_camera():
    c = Canvas(13, 9); P = PAL
    c.rect(1, 2, 11, 6, P['grey2']); c.rect(4, 1, 4, 1, P['grey']); c.ellipse(6.5, 5, 2.5, 2.2, P['grey']); c.ellipse(6.5, 5, 1.2, 1.2, P['skyd']); c.put(10, 3, P['red'])
    return outline(c)

def acc_sunglasses(eye_boxes):
    """Dark lenses over one face's eye boxes, with the bridge and temples. Returns (canvas, x, y) in sprite coordinates."""
    xs0 = min(b[0] for b in eye_boxes) - 4; ys0 = min(b[1] for b in eye_boxes) - 2
    xs1 = max(b[0] + b[2] for b in eye_boxes) + 3; ys1 = max(b[1] + b[3] for b in eye_boxes) + 1
    c = Canvas(xs1 - xs0 + 1, ys1 - ys0 + 1); P = PAL
    for (x, y, w, h) in eye_boxes:
        c.rect(x - 1 - xs0, y - 1 - ys0, w + 2, h + 2, P['pupil']); c.put(x - xs0, y - ys0, P['lensdk'])
    l, r = sorted(eye_boxes)[0], sorted(eye_boxes)[-1]
    by = l[1] + 1 - ys0
    c.hline(l[0] + l[2] + 1 - xs0, r[0] - 2 - xs0, by, P['rim'])
    c.hline(l[0] - 4 - xs0, l[0] - 2 - xs0, by, P['rim']); c.hline(r[0] + r[2] + 1 - xs0, r[0] + r[2] + 3 - xs0, by, P['rim'])
    return c, xs0, ys0

def window(tod):
    """The round window's pane at one time of day: a 34×34 overlay for the home scene at base (17, 17)."""
    c = Canvas(34, 34); P = PAL
    sky = {'night': P['skyn'], 'sunrise': (0xF2, 0xB4, 0xB8), 'day': P['skyd'], 'dusk': (0xE8, 0x8A, 0x3D)}[tod]
    c.ellipse(17, 17, 17, 17, P['frame']); c.ellipse(17, 17, 15, 15, sky)
    if tod == 'night':
        for sx, sy in ((9, 9), (23, 7), (27, 21), (13, 27), (21, 15)): c.put(sx, sy, P['star'])
        c.ellipse(11, 13, 4, 4, P['moon']); c.ellipse(13, 12, 3.2, 3.2, sky)
    elif tod == 'day':
        c.ellipse(10, 10, 4, 4, P['sun']); c.rect(17, 21, 10, 3, P['cloud']); c.rect(19, 19, 6, 2, P['cloud']); c.rect(7, 25, 7, 2, P['cloud'])
    else:
        c.ellipse(17, 25 if tod == 'dusk' else 23, 5, 5, P['sun'] if tod == 'sunrise' else (0xFF, 0x96, 0x46))
        c.ellipse(17, 17, 15, 15, (0x6B, 0x4A, 0x30) if tod == 'dusk' else (0x9A, 0x7A, 0x60), pred=lambda x, y: y > 28)
    c.vline(17, 2, 32, P['frame']); c.hline(2, 32, 17, P['frame'])
    return c

