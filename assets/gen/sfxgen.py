#!/usr/bin/env python3
"""Synthesised placeholder sound effects → content/sfx/*.wav (16 kHz mono 16-bit).
dial: soft dual tone; nosignal: three short low beeps; coo: a gentle two-note "coo"; whistle: rising chirp.
Pure Python; replaced by recorded/designed sounds later."""
import math, os, struct, wave
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'content', 'sfx')
SR = 16000
def write(name, samples):
    os.makedirs(OUT, exist_ok=True)
    w = wave.open(os.path.join(OUT, name + '.wav'), 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes(b''.join(struct.pack('<h', max(-32767, min(32767, int(s)))) for s in samples)); w.close()
def env(i, n, a=0.02, r=0.08):
    t = i / SR; d = n / SR
    return min(1.0, t / a) * min(1.0, max(0.0, (d - t) / r))
def tone(freqs, dur, amp=6000, vib=0.0, a=0.02, r=0.08):
    n = int(SR * dur); out = []
    for i in range(n):
        t = i / SR
        v = sum(math.sin(2 * math.pi * (f + vib * math.sin(2 * math.pi * 5 * t)) * t) for f in freqs) / len(freqs)
        out.append(amp * env(i, n, a, r) * v)
    return out
sil = lambda d: [0] * int(SR * d)
write('nosignal', tone([330], 0.12, 6000) + sil(0.08) + tone([330], 0.12, 6000) + sil(0.08) + tone([262], 0.25, 6000))
write('coo', tone([392], 0.16, 5000, vib=6) + tone([330], 0.28, 5000, vib=6))
write('whistle', [6000 * env(i, int(SR * 0.5)) * math.sin(2 * math.pi * (900 + 1400 * (i / (SR * 0.5))) * (i / SR)) for i in range(int(SR * 0.5))])
write('chirp', tone([1760], 0.06, 5000) + sil(0.04) + tone([2093], 0.06, 5000))
import random
rnd = random.Random(5)
write('tap', [5000 * env(i, int(SR * 0.05), 0.002, 0.03) * math.sin(2 * math.pi * 1900 * i / SR) for i in range(int(SR * 0.05))])
write('bell', [6000 * math.exp(-6 * i / SR) * (math.sin(2 * math.pi * 1318 * i / SR) + 0.5 * math.sin(2 * math.pi * 2637 * i / SR)) / 1.5 for i in range(int(SR * 0.45))])
write('beep', tone([1046], 0.16, 9500))
# Released, the walkie-talkie says "over": the press's note, then one a fourth lower, short and soft. It replaced a burst of
# radio static (2026-11-06, the owner: "按压对话键，松开时，扬声器会有短暂的沙沙声"), which sounded like a fault in the speaker.
write('over', tone([1046], 0.065, 6000, a=0.004, r=0.03) + sil(0.025) + tone([784], 0.09, 6000, a=0.004, r=0.05))
write('crunch', [5200 * env(i % int(SR * 0.07), int(SR * 0.07), 0.003, 0.05) * rnd.uniform(-1, 1) * (1 if (i // int(SR * 0.07)) % 2 == 0 else 0) for i in range(int(SR * 0.28))])   # two bites: nom nom
# Applause for the gift key's show (2026-11-06): a small crowd, claps scattered over a second, fading.
claps = [0.0] * int(SR * 1.3)
for k in range(46):
    start = int(rnd.uniform(0, 1.05) * SR); amp = 3600 * rnd.uniform(0.5, 1.0) * (1.0 - start / (SR * 1.3)) ** 0.7
    for i in range(int(SR * 0.018)):
        if start + i < len(claps): claps[start + i] += amp * math.exp(-i / (SR * 0.004)) * rnd.uniform(-1, 1)
write('clap', claps)
write('pop', [5500 * env(i, int(SR * 0.08), 0.003, 0.05) * math.sin(2 * math.pi * (600 - 400 * i / (SR * 0.08)) * i / SR) for i in range(int(SR * 0.08))])
print('sfx written to', OUT)
