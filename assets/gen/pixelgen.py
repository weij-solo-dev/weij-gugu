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

def rot_ellipse(c, ecx, ecy, a, b, ang, col):
    """A filled ellipse with semi-axes a (along ang, radians) and b, centred at (ecx, ecy)."""
    ca, sa = math.cos(ang), math.sin(ang)
    r = int(max(a, b)) + 2
    for y in range(int(ecy) - r, int(ecy) + r + 1):
        for x in range(int(ecx) - r, int(ecx) + r + 1):
            dx, dy = x + 0.5 - ecx, y + 0.5 - ecy
            u, v = dx * ca + dy * sa, -dx * sa + dy * ca
            if (u / a) ** 2 + (v / b) ** 2 <= 1.0: c.put(x, y, col)

def bubble(icon='setup'):
    """The thought bubble Gugu looks at while a call could not start: a white cloud, two little puffs trailing down-left
    toward its head, and one icon (2026-11-03, owner: a bubble with a settings icon; then "需要有多种，比如配置未完成、
    wifi 异常、服务异常"): 'setup' a gear, 'wifi' the Wi-Fi fan crossed out in red, 'service' a yellow warning triangle."""
    P = PAL
    W, H = 30, 26
    c = Canvas(W, H)
    white, ink, grey = P['eyew'], P['line'], P['grey2']
    blobs = [(14.0, 9.0, 8.5, 7.0), (21.5, 8.5, 6.5, 6.0), (9.5, 10.5, 5.5, 4.8), (17.0, 13.0, 8.0, 4.5), (22.0, 4.5, 4.0, 3.5), (12.5, 4.5, 4.5, 3.8)]
    inside = lambda x, y: any(((x + 0.5 - bx) / rx_) ** 2 + ((y + 0.5 - by_) / ry_) ** 2 <= 1.0 for (bx, by_, rx_, ry_) in blobs)
    puff_px = {(x, y) for (x, y) in ((4, 18), (5, 18), (3, 19), (4, 19), (5, 19), (6, 19), (3, 20), (4, 20), (5, 20), (6, 20), (4, 21), (5, 21))}   # a round 4×4 puff
    puff_px |= {(1, 23), (2, 23), (1, 24), (2, 24)}         # and a 2×2 one, trailing down-left toward the head
    for y in range(H):
        for x in range(W):
            if inside(x, y) or (x, y) in puff_px: c.put(x, y, white)
    c.outline_of({white}, ink)
    gx, gy = 16.0, 9.5                                       # the icon's centre
    if icon == 'setup':                                      # a gear: a ring with eight teeth and a hole
        for y in range(H):
            for x in range(W):
                dx, dy = x + 0.5 - gx, y + 0.5 - gy
                r = math.hypot(dx, dy); t = (math.degrees(math.atan2(dy, dx)) + 360 + 22.5) % 45
                if r <= 1.6: continue
                if r <= 3.9 or (r <= 5.4 and abs(t - 22.5) <= 9.5): c.put(x, y, grey)
    elif icon == 'wifi':                                     # the Wi-Fi fan (a dot and three arcs), crossed out
        ox, oy = 16.0, 14.0
        for y in range(H):
            for x in range(W):
                dx, dy = x + 0.5 - ox, y + 0.5 - oy
                r = math.hypot(dx, dy); ang = math.degrees(math.atan2(dy, dx))
                if -138 <= ang <= -42 and any(abs(r - R) <= 0.62 for R in (3.4, 6.2, 9.0)): c.put(x, y, grey)
        c.rect(15, 12, 2, 2, grey)
        for i in range(12):                                  # a red slash, two pixels wide
            c.put(10 + i, 3 + i, P['red']); c.put(11 + i, 3 + i, P['red'])
    elif icon == 'service':                                  # a warning triangle with a "!"
        tri = set()
        for y in range(3, 16):
            hw = (y - 3) * 0.55 + 0.6
            for x in range(W):
                if abs(x + 0.5 - 16.0) <= hw: tri.add((x, y))
        for (x, y) in tri: c.put(x, y, P['glow'])
        for (x, y) in tri:
            if any((x + dx, y + dy) not in tri for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))): c.put(x, y, ink)
        c.rect(15, 7, 2, 5, ink); c.rect(15, 13, 2, 1, ink)
    elif icon == 'hello':                                    # "Hi!" in fat two-pixel letters
        c.rect(10, 6, 2, 7, ink); c.rect(14, 6, 2, 7, ink); c.rect(12, 9, 2, 1, ink)   # H
        c.rect(17, 6, 2, 2, ink); c.rect(17, 9, 2, 4, ink)                              # i
        c.rect(20, 6, 2, 5, ink); c.rect(20, 12, 2, 1, ink)                             # !
    elif icon == 'idea':                                     # a lit bulb: glass, a neck, a screw base, short rays
        glass = set()
        for y in range(H):
            for x in range(W):
                if math.hypot(x + 0.5 - 16.0, y + 0.5 - 8.0) <= 4.3: glass.add((x, y))
        glass |= {(15, 12), (16, 12), (15, 13), (16, 13)}  # the neck
        for (x, y) in glass: c.put(x, y, P['glow'])
        for (x, y) in glass:
            if any((x + dx, y + dy) not in glass for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))): c.put(x, y, ink)
        c.put(14, 6, P['eyew']); c.put(14, 7, P['eyew']); c.put(15, 5, P['eyew'])   # a glint
        c.rect(14, 14, 4, 1, grey); c.rect(15, 15, 2, 1, grey)   # the screw base
        for (x, y) in ((16, 2), (10, 3), (11, 4), (22, 3), (21, 4), (8, 8), (9, 8), (23, 8), (24, 8)): c.put(x, y, P['glow2'] if 'glow2' in P else P['straw2'])   # rays
    return c

def lens_mask(x0, y0, w, h):
    """A round lens: the pixels whose centres fall inside the ellipse in the eye box, a touch inside so a 7×7 lens is the
    classic 3-5-7-7-7-5-3 pixel circle (2026-11-03, owner: "镜片是否也可以改为圆框")."""
    cx, cy, rx, ry = x0 + w / 2, y0 + h / 2, w / 2, h / 2
    return {(x, y) for y in range(y0, y0 + h) for x in range(x0, x0 + w) if ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2 <= 0.95}

def lens_ring(mask):
    """The frame: every pixel outside the lens that shares an edge with it (a one-pixel, 8-connected ring)."""
    return {(nx, ny) for (x, y) in mask for (nx, ny) in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)) if (nx, ny) not in mask}

def draw_glasses(c, eyes, color='rim'):
    """Round frames around each eye box, a bridge, and temples running out to the head.
    eyes: list of (x, y, w, h) eye boxes as drawn."""
    P = PAL
    rim = P[color]
    rings = []
    for (x, y, w, h) in eyes:
        ring = lens_ring(lens_mask(x, y, w, h))
        for (px, py) in ring: c.put(px, py, rim)
        rings.append(ring)
    by = eyes[0][1] + (eyes[0][3] - 1) // 2                  # the left lens's middle row
    def span(ring, box):
        xs = [px for (px, py) in ring if py == by]
        return (min(xs), max(xs)) if xs else (box[0] - 1, box[0] + box[2])
    (l0, l1), (r0, r1) = span(rings[0], eyes[0]), span(rings[1], eyes[1])
    if r0 - 1 >= l1 + 1: c.hline(l1 + 1, r0 - 1, by, rim)   # bridge (none when the frames touch)
    for x in list(range(l0 - 3, l0)) + list(range(r1 + 1, r1 + 4)):   # temples, only where the head is
        if c.get(x, by) not in (None, PAL.get('bg')): c.put(x, by, rim)

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
# Wings as hands (2026-11-04, the owner: "它翅膀可以当做手，应该是用手拾起"): in these poses the wing on one side is an arm
# drawn in front of the body, its tip a little mitten with two feather fingers; LAST_GEOM['hand'] says where the hand is,
# so the UI can put the held item in it. reach: bowed, the hand at the ground beside the feet; hold: the item in front of
# the chest; lift: up beside the glasses for a good look; sneak: held at the chest, glancing about (a, b); bite: up to the
# open beak; hide: the arm behind the back, the item gone.
HAND_KINDS = ('reach', 'hold', 'lift', 'sneak', 'bite', 'hide')
def hand_pose(emotion):
    for k in HAND_KINDS:
        for side, tag in ((-1, '_l'), (1, '_r')):
            if emotion == k + tag or emotion in (k + tag + '_a', k + tag + '_b'): return k, side
    return None, 0

def wing_arm(c, sx, sy, hx, hy, P):
    """A wing used as an arm: a feathered limb from the shoulder (sx, sy) to the hand (hx, hy), outlined, with a rounded
    mitten of a tip and two feather fingers pointing on."""
    steps = max(1, int(max(abs(hx - sx), abs(hy - sy)) * 2))
    for col, grow in ((P['line'], 1.0), (P['shade'], 0.0)):
        for i in range(steps + 1):
            t = i / steps
            r = 3.0 - 0.6 * t + grow
            c.ellipse(sx + (hx - sx) * t, sy + (hy - sy) * t, r, r, col)
        c.ellipse(hx, hy, 2.6 + grow, 2.6 + grow, col)
    dx, dy = hx - sx, hy - sy
    n = max(1e-6, (dx * dx + dy * dy) ** 0.5); ux, uy = dx / n, dy / n
    for k in (-1, 1):
        c.put(int(round(hx + ux * 2.2 - uy * 1.3 * k)), int(round(hy + uy * 2.2 + ux * 1.3 * k)), P['hi'])

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
    # poses (2026-11-04, the owner: idle life without bobbing, food handled with the beak and the feet): not tags,
    # frames the UI strings together. peck bows (body lower and wider, the face low); stretch stands tall.
    hk, hs = hand_pose(emotion)
    if hk == 'reach': rx, ry = 18.5, 16.5; cy = 33.0
    if emotion == 'stretch':   ry = 21.0; cy = 28.0
    body_set = {P['body'], P['shade'], P['belly'], P['hi']}

    # wings (behind body)
    wing_dy = {'wave': -9, 'happy': -6, 'laughing': -10, 'surprised': -8, 'proud': -4, 'scared': -12, 'sad': 4, 'angry': -5, 'shy': 2,
               'stretch': -13, 'read_l': -3, 'read_r': -3, 'kick_l': -6, 'kick_r': -6}.get(emotion, 0)
    for side in (-1, 1):
        dy = wing_dy if side == 1 or emotion != 'wave' else 0
        if emotion in ('preen_a', 'preen_b'): dy = (-5 if emotion == 'preen_b' else -8) if side == 1 else 0   # the right wing lifted to be groomed
        if hk and side == hs and hk != 'hide': continue      # this wing is an arm, drawn in front after the body
        if hk == 'hide': dy = -6 if side == hs else 0          # the arm behind the back, up a little
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

    # feet: (x, lift); a walking frame lifts one, a kick swings one out on a short leg
    feet = [[int(cx) - 6, 0], [int(cx) + 3, 0]]
    if emotion in ('walk_l_a', 'walk_r_b'): feet[0] = [feet[0][0] - 1, 2]
    if emotion in ('walk_l_b', 'walk_r_a'): feet[1] = [feet[1][0] + 1, 2]
    gy = int(cy + ry)
    for i, (fx, lift) in enumerate(feet):
        if (emotion == 'kick_l' and i == 0) or (emotion == 'kick_r' and i == 1):
            k = -1 if i == 0 else 1                                   # the leg swings out and up, the foot flat at its end
            base = fx + 1
            for t in range(1, 6): c.rect(base + k * t, gy - 1 - t // 2, 1, 2, P['foot'])
            tip = base + k * 6
            c.rect(tip - 1 if k < 0 else tip - 1, gy - 5, 3, 2, P['foot']); c.put(tip + k * 2, gy - 4, P['foot'])
            continue
        c.rect(fx, gy - 1 - lift, 3, 2, P['foot'])
        c.put(fx - 1, gy - lift, P['foot']); c.put(fx + 3, gy - lift, P['foot'])

    # outline of body-ish colors
    c.outline_of(body_set | {P['foot']}, P['line'])

    if 'vest' in parts:
        draw_vest(c, cx, cy, rx, ry, vest_color)

    hand = None
    if hk and hk != 'hide':
        if hk == 'reach':           S, H = (cx + hs * (rx - 4), cy - 4), (cx + hs * (rx + 2), cy + ry - 3)
        elif hk in ('hold', 'sneak'): S, H = (cx + hs * (rx - 3), cy - 3), (cx + hs * 2, cy + 4)
        elif hk == 'lift':          S, H = (cx + hs * (rx - 3), cy + 1), (cx + hs * (rx + 1), cy - 12)
        else:                       S, H = (cx + hs * (rx - 3), cy - 1), (cx, cy + 1)        # bite: up to the open beak (its top row still shows)
        wing_arm(c, S[0], S[1], H[0], H[1], P)
        hand = (int(round(H[0])), int(round(H[1])))

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

    # face — cartoon-big: every mood is a different shape, readable from across the room (2026-11-03, "再夸张一点").
    # Round glasses with white lenses D pixels across (owner, 2026-11-03: white lenses, round frames, "再大一点半径"). The
    # left lens spans exl-1 .. exl-2+D, the right one exr-1 .. exr-2+D; lo / ro are the frames' outer columns, and
    # everything beside the glasses (blush, brows, tears) is placed from them. Each lens holds one simple mark (a pupil,
    # one arch, one line) that keeps clear of the frame ("眼镜内线条太多太复杂").
    D = 9
    ey = int(cy - 9) if hk == 'reach' else int(cy - 11)              # bowing: the face drops toward the ground
    exl, exr = int(cx) - 1 - D, int(cx) + 3
    lo, ro = exl - 2, exr - 1 + D
    LY = 1 + (D - 7) // 2                                   # lens top = eye y - LY: the lens stays centred where the 7-pixel one was
    FEATHERS = (P['body'], P['shade'], P['hi'], P['belly'])
    def body_paint(x, y, w, h, col):                        # blush only lands on feathers: never on a lens, never past the outline
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if c.get(xx, yy) in FEATHERS: c.put(xx, yy, col)
    def eye_box(x0, y0, w, h, pw=3, ph=3, look=(0, 0)):
        for (x, y) in lens_mask(x0, y0, w, h): c.put(x, y, P['eyew'])
        eye_boxes.append((x0, y0, w, h))
        px, py = x0 + (w - pw) // 2 + look[0], y0 + (h - ph) // 2 + look[1]
        c.rect(px, py, pw, ph, P['pupil']); c.put(px, py, P['hi'])
    def eye_open(x, y, pw=3, ph=3, look=(0, 0)):            # the everyday eye: a round lens, a 3×3 pupil with a glint
        eye_box(x - 1, y - LY, D, D, pw, ph, look)
    def lens(x, y):                                         # a closed eye's lens; returns its centre
        x0, y0 = x - 1, y - LY
        for (px, py) in lens_mask(x0, y0, D, D): c.put(px, py, P['eyew'])
        eye_boxes.append((x0, y0, D, D))
        return x0 + D // 2, y0 + D // 2
    def eye_arc(x, y, down=False, wide=False):              # ︵ a happy closed eye, 7 wide (︶ when down)
        mx, my = lens(x, y); ink = P['pupil']; k = 1 if down else -1
        c.hline(mx - 1, mx + 1, my + 2 * k, ink); c.put(mx - 2, my + k, ink); c.put(mx + 2, my + k, ink); c.put(mx - 3, my, ink); c.put(mx + 3, my, ink)   # ends on the full-width row
    def eye_squeeze(x, y, right=False):                     # > on the left eye, < on the right: a bold arrow, laughing hard
        mx, my = lens(x, y)
        for dy in range(-2, 3):
            t = 2 - abs(dy)                                 # how far toward the tip this row reaches
            xs = (mx + 1 - t, mx + 2 - t) if right else (mx - 2 + t, mx - 1 + t)
            c.hline(xs[0], xs[1], my + dy, P['pupil'])
    def eye_shut(x, y):                                     # asleep (and the blink): one flat line
        mx, my = lens(x, y)
        c.hline(mx - 2, mx + 2, my + 1, P['pupil'])
    def eye_spiral(x, y):                                   # knocked out: a × in the lens
        mx, my = lens(x, y)
        for k in range(-2, 3): c.put(mx + k, my + k, P['pupil']); c.put(mx + k, my - k, P['pupil'])
    def star(sx, sy, big=False):
        c.hline(sx - 1, sx + 1, sy, P['glow']); c.vline(sx, sy - 1, sy + 1, P['glow'])
        if big: c.hline(sx - 2, sx + 2, sy, P['glow']); c.vline(sx, sy - 2, sy + 2, P['glow'])
    def brows(outer_low, thick=1, steps=2, gap=1):          # from each frame's outer edge to its lens centre, gap empty rows above the frame
        n = D // 2 + 1
        for side, (x0, y0, w, h) in zip((0, 1), eye_boxes):
            lowest = y0 - 2 - gap                           # the frame's top row is y0 - 1
            for i in range(n):
                xx = lo + i if side == 0 else ro - i
                lift = (i * steps) // n                       # 0 at the outer end … steps-1 at the inner end
                up = lift if outer_low else steps - 1 - lift  # how far this column sits above the brow's lowest end
                for t in range(thick): c.put(xx, lowest - up - t, P['line'])

    # cheeks first: the lenses drawn next cover any blush that reaches under them
    if emotion in ('happy', 'laughing', 'proud', 'wave', 'chew_a', 'chew_b', 'preen_a', 'preen_b', 'sly_l', 'sly_r') or hk in ('sneak', 'bite', 'hide'):
        body_paint(lo - 3, ey + 5, 3, 2, P['cheek']); body_paint(ro + 1, ey + 5, 3, 2, P['cheek'])
    elif emotion == 'angry':
        body_paint(lo - 3, ey + 5, 3, 3, P['red2']); body_paint(ro + 1, ey + 5, 3, 3, P['red2'])
    elif emotion in ('neutral', 'curious', 'puff', 'peek', 'look_l', 'look_r', 'read_l', 'read_r', 'walk_l_a', 'walk_l_b', 'walk_r_a', 'walk_r_b') or hk in ('reach', 'hold', 'lift'):
        body_paint(lo - 2, ey + 5, 2, 1, P['cheek']); body_paint(ro + 1, ey + 5, 2, 1, P['cheek'])

    if eyes_closed:
        eye_shut(exl, ey); eye_shut(exr, ey)
    elif emotion == 'neutral':
        eye_open(exl, ey); eye_open(exr, ey)
    elif emotion == 'happy':
        eye_arc(exl, ey); eye_arc(exr, ey)
    elif emotion == 'laughing':
        eye_squeeze(exl, ey - 2); eye_squeeze(exr, ey - 2, right=True)                  # the laugh pushes the glasses up
        for sgn in (-1, 1):                                                                # "ha ha" lines beside the head
            x = int(cx) + sgn * 21
            for yy in (ey - 4, ey, ey + 4): c.hline(x - 1, x + 1, yy, P['hi'])
    elif emotion == 'surprised':                                                           # goggle eyes: lenses two pixels bigger again
        G = D + 2
        eye_box(int(cx) - 1 - G, ey - 3, G, G, 2, 2); eye_box(int(cx) + 1, ey - 3, G, G, 2, 2)
        c.hline(int(cx) - 11, int(cx) - 7, ey - 6, P['line']); c.hline(int(cx) + 6, int(cx) + 10, ey - 6, P['line'])   # brows shot up
    elif emotion == 'curious':                                                             # "hmm?": eyes up and to the side, one brow up, head tilted a pixel
        eye_open(exl, ey + 1, look=(1, -1)); eye_open(exr, ey - 1, look=(1, -1))
        (lx0, ly0, _, _), (rx0, ry0, _, _) = eye_boxes
        c.hline(rx0 + 1, rx0 + D - 3, ry0 - 3, P['line']); c.put(rx0 + D - 2, ry0 - 2, P['line'])   # the raised brow
        c.hline(lx0 + 2, lx0 + D - 3, ly0 - 3, P['line'])                                            # the other one flat and low
    elif emotion == 'eyeroll':                                                             # 无语: the eyes rolled up out of sight, a "…"
        eye_open(exl, ey, look=(0, -3)); eye_open(exr, ey, look=(0, -3))
        for i in range(3): c.put(int(cx) + 14 + 3 * i, ey - 9, P['hi'])
    elif emotion == 'peek':                                                                # something to say: a calm look up at the bubble
        eye_open(exl, ey, look=(2, -2)); eye_open(exr, ey, look=(2, -2))
    elif emotion in ('urge', 'urge_you'):                                                  # nothing is set up: in a hurry, eyes on you, then on the gear bubble
        look = (2, -2) if emotion == 'urge' else (0, 0)                                    # urge: hard up and right, at the bubble; urge_you: at you
        eye_open(exl, ey, look=look); eye_open(exr, ey, look=look)
        brows(outer_low=True, steps=3)                                                     # worried: inner ends up
        sx, sy = int(cx) - 16, ey - 3                                                      # a bead of sweat
        c.put(sx, sy, P['skyd']); c.rect(sx - 1, sy + 1, 3, 2, P['skyd']); c.put(sx - 1, sy + 1, P['cloud'])
    elif emotion == 'shy':                                                                 # peeking from behind a wing
        eye_open(exl, ey + 1, look=(-1, 1)); eye_open(exr, ey + 1, look=(-1, 1))             # eyes down and away
        body_paint(lo - 6, ey + 5, 6, 3, P['cheek']); body_paint(ro + 1, ey + 5, 6, 3, P['cheek'])   # enormous blush
        body_paint(lo - 7, ey + 6, 1, 1, P['cheek']); body_paint(ro + 7, ey + 6, 1, 1, P['cheek'])
        for i, (sx, sy) in enumerate(((int(cx) + 15, ey - 9), (int(cx) + 18, ey - 6))):     # two little hearts
            c.put(sx, sy, P['cheek']); c.put(sx + 2, sy, P['cheek']); c.hline(sx, sx + 2, sy + 1, P['cheek']); c.put(sx + 1, sy + 2, P['cheek'])
    elif emotion == 'sad':
        eye_open(exl, ey + 1, look=(0, 1)); eye_open(exr, ey + 1, look=(0, 1))
        brows(outer_low=True, steps=3)                                                     # inner ends up
        for (x0, y0, w, h) in eye_boxes:                                                   # tears streaming from under each lens
            tx, ty = x0 + w // 2 - 1, y0 + h + 1
            c.rect(tx, ty, 2, 6, P['skyd']); c.put(tx, ty, P['cloud']); c.put(tx + 1, ty + 6, P['skyd'])
    elif emotion == 'sleepy':
        eye_shut(exl, ey); eye_shut(exr, ey)
        fb = max(y0 + h for (_, y0, _, h) in eye_boxes)                                    # a snot bubble just under the glasses
        c.ellipse(cx + 1, fb + 4, 3.0, 3.0, P['skyd']); c.put(int(cx), fb + 2, P['cloud'])
    elif emotion == 'puff':
        eye_arc(exl, ey + 1); eye_arc(exr, ey + 1)
    elif emotion == 'wave':
        eye_arc(exl, ey); eye_open(exr, ey)
    elif emotion == 'angry':
        eye_open(exl, ey + 1, pw=2, ph=2, look=(1, 1)); eye_open(exr, ey + 1, pw=2, ph=2, look=(0, 1))   # a pixel lower: room for the brows
        brows(outer_low=False, thick=2, steps=2)                                           # slanting down to the middle, clear of the frames
        for sx in (int(cx) - 17, int(cx) + 12):                                            # steam
            c.rect(sx, ey - 11, 5, 3, P['cloud']); c.rect(sx + 1, ey - 12, 3, 1, P['cloud']); c.put(sx + 2, ey - 13, P['cloud'])
    elif emotion == 'scared':
        G = D + 2
        eye_box(int(cx) - 1 - G, ey - 3, G, G, 1, 1, look=(-2, -2)); eye_box(int(cx) + 1, ey - 3, G, G, 1, 1, look=(-2, -2))
        for sx, sy in ((int(cx) + 15, ey - 8), (int(cx) + 18, ey - 3), (int(cx) - 17, ey - 5)):   # sweat flying
            c.put(sx, sy, P['skyd']); c.rect(sx - 1, sy + 1, 3, 2, P['skyd'])
        for sgn in (-1, 1):                                                                # trembling lines
            x = int(cx) + sgn * 21
            for yy in (ey + 6, ey + 10, ey + 14): c.vline(x, yy, yy + 2, P['hi'])
    elif emotion == 'proud':
        eye_arc(exl, ey - 3, down=True); eye_arc(exr, ey - 3, down=True)   # ︶ smug, chin up
        c.ellipse(cx, cy + 4, rx * 0.56, ry * 0.5, P['hi'], pred=lambda x, y: c.get(x, y) == P['belly'])
        for sx, sy, b in ((int(cx) - 16, ey - 8, True), (int(cx) + 15, ey - 9, True), (int(cx) + 18, ey, False), (int(cx) - 19, ey + 1, False)): star(sx, sy, b)
    elif emotion == 'dizzy':
        eye_spiral(exl, ey - 1); eye_spiral(exr, ey + 1)                                   # a tilt: one eye higher than the other
        for sx, sy in ((int(cx) - 17, ey - 9), (int(cx) + 15, ey - 12), (int(cx) - 3, ey - 15), (int(cx) + 19, ey - 1)): star(sx, sy, True)
    elif emotion in ('look_l', 'look_r', 'walk_l_a', 'walk_l_b', 'walk_r_a', 'walk_r_b'):   # eyes on something to one side
        lx = -2 if emotion in ('look_l', 'walk_l_a', 'walk_l_b') else 2
        eye_open(exl, ey, look=(lx, 0)); eye_open(exr, ey, look=(lx, 0))
    elif emotion in ('read_l', 'read_r'):                                                  # down at the paper, along the line
        lx = -1 if emotion == 'read_l' else 1
        eye_open(exl, ey, look=(lx, 2)); eye_open(exr, ey, look=(lx, 2))
    elif emotion in ('chew_a', 'chew_b', 'preen_a', 'preen_b'):                            # contented
        eye_arc(exl, ey); eye_arc(exr, ey)
    elif emotion == 'stretch':                                                             # a big stretch and a yawn
        eye_squeeze(exl, ey); eye_squeeze(exr, ey, right=True)
    elif hk == 'reach':                                                                    # down at it, on that side
        eye_open(exl, ey, look=(hs * 2, 2)); eye_open(exr, ey, look=(hs * 2, 2))
    elif hk == 'hold':                                                                     # at what is in the hand
        eye_open(exl, ey, look=(0, 2)); eye_open(exr, ey, look=(0, 2))
    elif hk == 'lift':                                                                     # a good look at it, up by the glasses
        eye_open(exl, ey, look=(hs * 2, 0)); eye_open(exr, ey, look=(hs * 2, 0))
    elif hk == 'bite':                                                                     # mmm
        eye_arc(exl, ey); eye_arc(exr, ey)
    elif hk in ('sneak', 'hide'):                                                          # 偷摸, the thing held close (or already behind the back)
        lx = (-2 if emotion.endswith('_a') else 2) if hk == 'sneak' else -hs * 2
        eye_open(exl, ey, look=(lx, 1)); eye_open(exr, ey, look=(lx, 1))
        for (x0, y0, w, h) in eye_boxes: c.hline(x0 + 1, x0 + w - 2, y0 - 3, P['line'])
    elif emotion in ('sly_l', 'sly_r'):                                                    # 偷摸: a sideways glance under low, flat brows
        lx = -2 if emotion == 'sly_l' else 2
        eye_open(exl, ey, look=(lx, 1)); eye_open(exr, ey, look=(lx, 1))
        for (x0, y0, w, h) in eye_boxes: c.hline(x0 + 1, x0 + w - 2, y0 - 3, P['line'])
    elif emotion in ('kick_l', 'kick_r'):                                                  # hmph, away with it
        lx = -2 if emotion == 'kick_l' else 2
        eye_open(exl, ey + 1, pw=2, ph=2, look=(lx, 1)); eye_open(exr, ey + 1, pw=2, ph=2, look=(lx, 1))
        brows(outer_low=False, thick=1, steps=2)

    # beak: Gugu's only mouth, always the orange beak (2026-11-03, owner: "有些表情是用黑线，有些时候用橙色块，到底是用哪个？";
    # the black grin, frown, teeth and wavy lines are gone). A mood changes the beak itself: shut, chirping, talking, wide
    # open with a tongue, a tall O, drooping, pressed, askew. It sits in the gap between the lenses (a row lower where the
    # lenses reach down to it), and under the frames when it is wider than the gap or the goggles meet.
    bx, by = int(cx) - 1, ey + 4
    under = max([by] + [y0 + h + 1 for (_, y0, _, h) in eye_boxes])                       # the first row under the frames
    beak_y = under if emotion in ('surprised', 'scared', 'laughing') else by + 1 if emotion in ('sad', 'angry', 'puff', 'dizzy') else by
    def shut(y): c.rect(bx, y, 3, 1, P['beak']); c.put(bx + 1, y + 1, P['beak2'])
    def opened(y, rows): c.rect(bx, y, 3, 1, P['beak']); c.rect(bx, y + 1, 3, rows, P['mouth']); c.rect(bx, y + 1 + rows, 3, 1, P['beak2'])
    if emotion == 'laughing':                                                              # wide open with a tongue
        c.rect(bx - 2, beak_y, 7, 1, P['beak']); c.rect(bx - 2, beak_y + 1, 7, 4, P['mouth']); c.rect(bx - 1, beak_y + 3, 5, 2, P['cheek']); c.rect(bx - 2, beak_y + 5, 7, 1, P['beak2'])
    elif emotion == 'surprised':                                                           # a tall O
        c.rect(bx - 1, beak_y, 5, 1, P['beak']); c.rect(bx - 1, beak_y + 1, 5, 4, P['mouth']); c.rect(bx - 1, beak_y + 5, 5, 1, P['beak2'])
    elif beak_open:                                                                        # talking
        opened(beak_y, 2)
    elif emotion == 'shy':
        pass                                                                               # behind the wing
    elif emotion in ('happy', 'wave', 'scared'):                                           # chirping (scared: a gasp)
        opened(beak_y, 1)
    elif emotion == 'angry':                                                               # pressed shut
        c.rect(bx, beak_y, 3, 1, P['beak']); c.rect(bx, beak_y + 1, 3, 1, P['beak2'])
    elif emotion == 'dizzy':                                                               # open and askew
        c.rect(bx, beak_y, 3, 1, P['beak']); c.rect(bx, beak_y + 1, 3, 1, P['mouth']); c.put(bx, beak_y + 2, P['beak2'])
    elif emotion == 'chew_a':
        opened(beak_y, 1)
    elif emotion == 'stretch':                                                             # the yawn
        c.rect(bx - 1, beak_y, 5, 1, P['beak']); c.rect(bx - 1, beak_y + 1, 5, 3, P['mouth']); c.rect(bx - 1, beak_y + 4, 5, 1, P['beak2'])
    elif emotion in ('preen_a', 'preen_b'):                                                # the head turned into the lifted wing
        edge = int(cx + rx)
        px0 = edge - 6 if emotion == 'preen_a' else edge - 7
        py0 = beak_y + 3 if emotion == 'preen_a' else beak_y + 4
        c.rect(px0, py0, 3, 1, P['beak']); c.put(px0 + 1, py0 + 1, P['beak2'])
        for fx, fy in ((edge - 1, py0 - 2), (edge + 1, py0 - 4)) if emotion == 'preen_b' else ((edge, py0 - 3),):
            c.put(fx, fy, P['hi'])                                                         # a feather tip ruffled up
    elif emotion in ('sly_l', 'sly_r') or hk == 'sneak':                                   # hehe: a tiny chirp
        opened(beak_y, 1)
    elif hk == 'bite':                                                                     # wide, for the bite
        opened(beak_y, 2)
    elif emotion in ('kick_l', 'kick_r'):                                                  # pressed shut
        c.rect(bx, beak_y, 3, 1, P['beak']); c.rect(bx, beak_y + 1, 3, 1, P['beak2'])
    else:                                                                                  # neutral, curious, sleepy, proud, puff, sad (a row lower: drooping)
        shut(beak_y)

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
    LAST_GEOM = {'eye_boxes': list(eye_boxes), 'head_top': cy - ry, 'cx': cx, 'cy': cy, 'rx': rx, 'ry': ry, 'hand': hand}   # for accessory and hand anchors (export_lvgl.py)
    return c

# ---------- home scene ----------
# The room sits ROOM_LIFT rows higher than its coordinates below say (2026-11-04, the owner: Gugu sat too low in the round
# window). It is drawn on a canvas that much taller with the circle moved down, then the top is cropped: every object
# keeps its numbers, the floor gains rows at the bottom. app/ui (GUGU_BASE_Y, the fittings' hit boxes, the pane, the
# case, the bubble, the food's ground) and art.h (ART_GROUND_TOP) moved by the same amount.
ROOM_LIFT = 6

def room(state='home', night=False, moons=2, W=116, H=116, lamp_on=None, lift=ROOM_LIFT):
    T = H + lift
    c = Canvas(W, T, PAL['black'])
    P = PAL
    cx, cy, R = W / 2, T - H / 2, W / 2
    # wall + floor
    c.ellipse(cx, cy, R, R, P['wall'])
    c.ellipse(cx, cy, R, R, P['floor'], pred=lambda x, y: y >= 84)
    for y in range(84, T):
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
    out = Canvas(W, H, PAL['black'])
    out.px = c.px[lift:lift + H]
    return out

def compose_home(night=False, accessory='scarf', worn='glasses', hook='case', case_color='red'):
    scene = room('home', night=night, moons=0)
    g = gugu('neutral', scarf=False, bag=False, accessory=worn)
    scene.blit(g, 58 - 24, 96 - 54 + 4 - ROOM_LIFT)
    # the accessory hangs on the hook when home
    c = scene; P = PAL
    if hook == 'case':
        draw_case(c, 94, 70 - ROOM_LIFT, color=case_color, handle_up=False)   # parked in the corner by the chalkboard
    elif hook == 'bag':
        hook_bag(c, 71, 44 - ROOM_LIFT)
    elif hook != 'none':
        hook_item(c, 72, 44 - ROOM_LIFT, accessory)
    # redraw nest front rim over gugu's feet
    c.ellipse(58, 100 - ROOM_LIFT, 22, 6, P['nest'], pred=lambda x, y: y >= 100 - ROOM_LIFT)
    c.ellipse(58, 100 - ROOM_LIFT, 22, 6, P['nest2'], pred=lambda x, y: y >= 100 - ROOM_LIFT and (x + y) % 3 == 0)
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

# ---- the yellow key's food: what falls from the top for Gugu to peck up (2026-11-04, owner: "投喂一颗食物（坚果、汉堡等）也可能是物件（比如金币等）") ----
def food_corn():
    c = Canvas(12, 14); P = PAL
    c.ellipse(6, 5.5, 3.2, 5.2, P['glow'])
    for y in range(1, 11, 2):
        for x in range(4, 9, 2): c.put(x, y, P['straw2'])
    c.ellipse(3.2, 10, 2.6, 3.8, P['green'], pred=lambda x, y: x <= 5); c.ellipse(8.8, 10, 2.6, 3.8, P['green2'], pred=lambda x, y: x >= 6)
    return outline(c)

def food_nut():
    c = Canvas(12, 12); P = PAL
    c.ellipse(6, 7.5, 4.0, 4.0, P['brown']); c.ellipse(5, 6.5, 1.4, 1.6, P['wood'] if 'wood' in P else P['straw'])
    c.ellipse(6, 4.0, 5.0, 2.4, P['brown2']); c.rect(5, 0, 2, 2, P['brown2'])
    return outline(c)

def food_burger():
    c = Canvas(14, 12); P = PAL
    c.ellipse(7, 3.5, 6.0, 3.2, P['straw'], pred=lambda x, y: y <= 4)
    for (x, y) in ((4, 2), (7, 1), (10, 2), (6, 3), (9, 3)): c.put(x, y, P['white'])
    c.rect(1, 5, 12, 1, P['green']); c.put(0, 5, P['green']); c.put(13, 5, P['green'])
    c.rect(1, 6, 12, 1, P['glow']); c.put(10, 7, P['glow'])
    c.rect(1, 7, 12, 2, P['brown2'])
    c.rect(1, 9, 12, 2, P['straw']); c.rect(2, 10, 10, 1, P['straw2'])
    return outline(c)

def food_cake():
    c = Canvas(12, 14); P = PAL
    for y in range(8, 13):
        inset = (y - 8) // 2
        c.hline(2 + inset, 9 - inset, y, P['red'] if (y % 2) else P['red2'])
    c.ellipse(6, 6.5, 4.6, 2.6, P['cheek']); c.ellipse(6, 4.5, 3.2, 2.0, P['white'])
    c.rect(5, 1, 2, 2, P['red']); c.put(6, 0, P['green2'])
    return outline(c)

def food_broccoli():
    c = Canvas(12, 14); P = PAL
    c.rect(5, 8, 2, 5, P['green3']); c.put(4, 12, P['green3']); c.put(7, 12, P['green3'])
    c.ellipse(3.6, 5.5, 2.8, 2.8, P['green2']); c.ellipse(8.4, 5.5, 2.8, 2.8, P['green2']); c.ellipse(6, 3.6, 3.2, 3.0, P['green'])
    c.put(5, 2, P['green3']); c.put(8, 4, P['green3'])
    return outline(c)

def food_chili():
    c = Canvas(12, 14); P = PAL
    rot_ellipse(c, 6.2, 7.8, 5.4, 2.4, math.radians(62), P['red'])     # a plump pod, leaning
    rot_ellipse(c, 5.0, 6.6, 2.6, 0.9, math.radians(62), P['white'])   # the shine
    c.put(8, 12, P['red2']); c.put(8, 13, P['red2']); c.put(7, 11, P['red2'])   # the tip curling
    c.rect(3, 1, 4, 2, P['green']); c.put(4, 0, P['green2'])           # the cap and stalk
    return outline(c)

def food_coin():
    c = Canvas(12, 12); P = PAL
    c.ellipse(6, 6, 5.0, 5.0, P['straw2']); c.ellipse(6, 6, 3.9, 3.9, P['glow'])
    c.rect(5, 3, 2, 6, P['straw2']); c.rect(3, 5, 6, 2, P['straw2'])    # a plus pressed into it
    c.put(3, 3, P['white']); c.put(4, 2, P['white'])
    return outline(c)

def food_sock():
    c = Canvas(12, 14); P = PAL
    c.rect(3, 0, 5, 9, P['white']); c.rect(3, 8, 8, 4, P['white']); c.ellipse(9.5, 10, 1.6, 2.0, P['white'])
    for y in (2, 5): c.rect(3, y, 5, 1, P['red'])
    c.rect(3, 0, 5, 1, P['red2']); c.rect(9, 9, 2, 3, P['grey'])
    return outline(c)

FOODS = {'corn': food_corn, 'nut': food_nut, 'burger': food_burger, 'cake': food_cake, 'broccoli': food_broccoli,
         'chili': food_chili, 'coin': food_coin, 'sock': food_sock}

def newspaper(turn=False):
    """The paper Gugu reads when idle (2026-11-04, the owner: "看报纸"): two pages, a headline, lines, a little picture;
    turn: the right page's corner lifted. Drawn in front of the belly, the wings at its edges."""
    c = Canvas(30, 17); P = PAL
    c.rect(1, 1, 28, 15, P['paper'])
    c.vline(15, 1, 15, P['hi'])                                        # the fold
    c.rect(3, 3, 10, 2, P['line']); c.rect(17, 3, 10, 2, P['line'])    # headlines
    for y in (7, 9, 11, 13): c.hline(3, 12, y, P['ink'])
    c.rect(17, 7, 5, 4, P['skyd']); c.put(19, 8, P['sun'])            # a picture
    for y in (7, 9): c.hline(23, 27, y, P['ink'])
    c.hline(17, 27, 13, P['ink'])
    if turn:                                                           # the right page's corner lifted, about to turn
        c.rect(24, 1, 5, 5, P['hi']); c.put(28, 1, None); c.put(27, 1, None); c.put(28, 2, None)
    c.outline_of({P['paper'], P['hi'], P['line'], P['ink'], P['skyd'], P['sun']}, P['line'])
    return c

def acc_sun_hat():
    c = Canvas(24, 9); P = PAL
    c.rect(1, 6, 22, 2, P['straw2']); c.rect(6, 1, 12, 5, P['straw']); c.rect(7, 0, 10, 1, P['straw']); c.rect(6, 4, 12, 1, P['red2'])
    return outline(c)

def acc_camera():
    c = Canvas(13, 9); P = PAL
    c.rect(1, 2, 11, 6, P['grey2']); c.rect(4, 1, 4, 1, P['grey']); c.ellipse(6.5, 5, 2.5, 2.2, P['grey']); c.ellipse(6.5, 5, 1.2, 1.2, P['skyd']); c.put(10, 3, P['red'])
    return outline(c)

def acc_sunglasses(eye_boxes, cx=None, rx=None):
    """Round dark lenses over one face's glasses (each lens and its frame), a glint, the bridge and the temples (kept
    inside the head when cx / rx are given). Returns (canvas, x, y) in sprite coordinates."""
    xs0 = min(b[0] for b in eye_boxes) - 4; ys0 = min(b[1] for b in eye_boxes) - 2
    xs1 = max(b[0] + b[2] for b in eye_boxes) + 3; ys1 = max(b[1] + b[3] for b in eye_boxes) + 1
    c = Canvas(xs1 - xs0 + 1, ys1 - ys0 + 1); P = PAL
    rings = []
    for (x, y, w, h) in eye_boxes:
        mask = lens_mask(x, y, w, h); ring = lens_ring(mask); rings.append(ring)
        for (px, py) in mask | ring: c.put(px - xs0, py - ys0, P['pupil'])
        gx, gy = x + w // 4, y + h // 4                      # a glint up and to the left
        for (dx, dy) in ((0, 0), (1, 0), (0, 1)): c.put(gx + dx - xs0, gy + dy - ys0, P['lensdk'])
    order = sorted(range(len(eye_boxes)), key=lambda i: eye_boxes[i][0])
    l, r = eye_boxes[order[0]], eye_boxes[order[-1]]
    by = l[1] + (l[3] - 1) // 2
    def span(ring, box):
        xs = [px for (px, py) in ring if py == by]
        return (min(xs), max(xs)) if xs else (box[0] - 1, box[0] + box[2])
    (l0, l1), (r0, r1) = span(rings[order[0]], l), span(rings[order[-1]], r)
    if r0 - 1 >= l1 + 1: c.hline(l1 + 1 - xs0, r0 - 1 - xs0, by - ys0, P['rim'])
    for x in list(range(l0 - 3, l0)) + list(range(r1 + 1, r1 + 4)):
        if cx is None or abs(x + 0.5 - cx) <= rx - 1: c.put(x - xs0, by - ys0, P['rim'])
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

