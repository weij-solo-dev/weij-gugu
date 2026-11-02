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
def tone(freqs, dur, amp=6000, vib=0.0):
    n = int(SR * dur); out = []
    for i in range(n):
        t = i / SR
        v = sum(math.sin(2 * math.pi * (f + vib * math.sin(2 * math.pi * 5 * t)) * t) for f in freqs) / len(freqs)
        out.append(amp * env(i, n) * v)
    return out
sil = lambda d: [0] * int(SR * d)
write('dial', tone([350, 440], 0.9, 4500) + sil(0.25) + tone([350, 440], 0.9, 4500))
write('nosignal', tone([330], 0.12, 6000) + sil(0.08) + tone([330], 0.12, 6000) + sil(0.08) + tone([262], 0.25, 6000))
write('coo', tone([392], 0.16, 5000, vib=6) + tone([330], 0.28, 5000, vib=6))
write('whistle', [6000 * env(i, int(SR * 0.5)) * math.sin(2 * math.pi * (900 + 1400 * (i / (SR * 0.5))) * (i / SR)) for i in range(int(SR * 0.5))])
write('chirp', tone([1760], 0.06, 5000) + sil(0.04) + tone([2093], 0.06, 5000))
print('sfx written to', OUT)
