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
    'black':  (0x00, 0x00, 0x00),
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
    if emotion == 'surprised': ry = 20.5; cy = 28.0
    body_set = {P['body'], P['shade'], P['belly'], P['hi']}

    # wings (behind body)
    wing_dy = {'wave': -9, 'happy': -2, 'laughing': -4, 'surprised': -6}.get(emotion, 0)
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

    # face
    ey = int(cy - 11)
    exl, exr = int(cx) - 7, int(cx) + 3
    def eye_open(x, y, pw=2, ph=3, look=(0, 0), big=False):
        if big: c.rect(x - 1, y - 1, 6, 6, P['eyew']); eye_boxes.append((x - 1, y - 1, 6, 6))
        else:   c.rect(x, y, 4, 5, P['eyew']); eye_boxes.append((x, y, 4, 5))
        c.rect(x + 1 + look[0], y + 1 + look[1], pw, ph, P['pupil'])
        c.put(x + 1 + look[0], y + 1 + look[1], P['hi'])
    def eye_arc(x, y, down=False):  # closed smiling eye
        eye_boxes.append((x, y, 4, 5))
        if not down:
            c.hline(x, x + 3, y + 2, P['line']); c.put(x - 1, y + 3, P['line']); c.put(x + 4, y + 3, P['line'])
        else:
            c.hline(x, x + 3, y + 3, P['line']); c.put(x - 1, y + 2, P['line']); c.put(x + 4, y + 2, P['line'])
    def eye_half(x, y):
        eye_boxes.append((x, y, 4, 5))
        c.rect(x, y + 2, 4, 3, P['eyew']); c.rect(x + 1, y + 3, 2, 2, P['pupil'])
        c.hline(x, x + 3, y + 1, P['line'])

    if eyes_closed:
        eye_arc(exl, ey); eye_arc(exr, ey)
    elif emotion in ('neutral', 'curious'):
        tilt = 1 if emotion == 'curious' else 0
        eye_open(exl, ey + tilt); eye_open(exr, ey - tilt, big=(emotion == 'curious'))
    elif emotion == 'happy':
        eye_arc(exl, ey); eye_arc(exr, ey)
    elif emotion == 'laughing':
        eye_arc(exl, ey); eye_arc(exr, ey)
    elif emotion == 'surprised':
        eye_open(exl, ey - 1, pw=2, ph=2, big=True); eye_open(exr, ey - 1, pw=2, ph=2, big=True)
    elif emotion == 'shy':
        eye_open(exl, ey, look=(0, 1)); eye_open(exr, ey, look=(0, 1))
    elif emotion == 'sad':
        eye_open(exl, ey + 1, look=(0, 1)); eye_open(exr, ey + 1, look=(0, 1))
        c.hline(exl, exl + 2, ey - 1, P['line']); c.hline(exr + 1, exr + 3, ey - 1, P['line'])
    elif emotion == 'sleepy':
        eye_half(exl, ey); eye_half(exr, ey)
    elif emotion == 'puff':
        eye_arc(exl, ey + 1); eye_arc(exr, ey + 1)
    elif emotion == 'wave':
        eye_arc(exl, ey); eye_open(exr, ey)

    # cheeks
    blush = P['cheek'] if emotion in ('shy', 'happy', 'laughing') else P['cheek']
    if emotion not in ('sad', 'sleepy'):
        c.rect(exl - 3, ey + 5, 2, 1, blush); c.rect(exr + 5, ey + 5, 2, 1, blush)

    # beak
    bx, by = int(cx) - 1, ey + 4
    if beak_open or emotion in ('laughing', 'surprised'):
        c.rect(bx, by, 3, 1, P['beak']); c.rect(bx, by + 1, 3, 2, P['mouth']); c.rect(bx, by + 3, 3, 1, P['beak2'])
    elif emotion == 'sad':
        c.rect(bx, by + 1, 3, 1, P['beak']); c.put(bx + 1, by + 2, P['beak2'])
    else:
        c.rect(bx, by, 3, 1, P['beak']); c.put(bx + 1, by + 1, P['beak2'])

    if 'glasses' in parts and len(eye_boxes) == 2:
        draw_glasses(c, eye_boxes)
    if 'beanie' in parts:
        draw_beanie(c, cx, cy, rx, ry)

    # sleepy "z"
    if emotion == 'sleepy':
        zx, zy = int(cx) + 13, ey - 8
        c.hline(zx, zx + 2, zy, P['hi']); c.put(zx + 1, zy + 1, P['hi']); c.hline(zx, zx + 2, zy + 2, P['hi'])
    if emotion == 'surprised':
        c.put(int(cx) + 14, ey - 6, P['hi']); c.vline(int(cx) + 14, ey - 5, ey - 3, P['hi'])
    if emotion == 'curious':
        qx, qy = int(cx) + 13, ey - 8
        c.hline(qx, qx + 2, qy, P['hi']); c.put(qx + 2, qy + 1, P['hi']); c.put(qx + 1, qy + 2, P['hi']); c.put(qx + 1, qy + 4, P['hi'])
    if emotion == 'happy':
        c.put(int(cx) - 15, ey - 6, P['glow']); c.put(int(cx) + 14, ey - 4, P['glow'])
    return c

# ---------- home scene ----------
def room(state='home', night=False, moons=2, W=116, H=116):
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
    lamp_on = (state == 'home')
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
