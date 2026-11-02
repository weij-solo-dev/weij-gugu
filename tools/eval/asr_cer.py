#!/usr/bin/env python3
"""One-off evaluation helper: character error rate of MiniMax asr-1.0 on the fixture set
(test/fixtures/speech/manifest.json), per language, with per-request latency on a kept-alive connection.
Fixtures are TTS stand-ins; rerun with real child recordings (same manifest shape) when available."""
import http.client, json, os, re, sys, time, unicodedata

KEY = os.environ.get('GUGU_MINIMAX_API_KEY')
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
FIX = os.path.join(ROOT, 'test', 'fixtures', 'speech')

def norm(s):
    s = unicodedata.normalize('NFKC', s).lower()
    s = re.sub(r'[\s,.!?;:，。！？；：、「」“”"\'\-…~～]', '', s)
    return s

def cer(ref, hyp):
    r, h = norm(ref), norm(hyp)
    if not r: return 0.0
    d = list(range(len(h) + 1))
    for i in range(1, len(r) + 1):
        prev, d[0] = d[0], i
        for j in range(1, len(h) + 1):
            cur = d[j]
            d[j] = min(d[j] + 1, d[j - 1] + 1, prev + (r[i - 1] != h[j - 1]))
            prev = cur
    return d[len(h)] / len(r)

def recognise(conn, path):
    data = open(path, 'rb').read()
    b = '----gugucer'
    body = (f'--{b}\r\nContent-Disposition: form-data; name="model"\r\n\r\nasr-1.0\r\n--{b}\r\nContent-Disposition: form-data; name="file"; filename="s.wav"\r\nContent-Type: audio/wav\r\n\r\n').encode() + data + f'\r\n--{b}--\r\n'.encode()
    t0 = time.time()
    conn.request('POST', '/v1/speech_to_text', body=body, headers={'Authorization': 'Bearer ' + KEY, 'Content-Type': f'multipart/form-data; boundary={b}'})
    r = conn.getresponse(); txt = r.read().decode('utf-8', 'replace')
    dt = time.time() - t0
    try: return json.loads(txt).get('text', ''), dt, len(data) / 32000
    except json.JSONDecodeError: return f'<{r.status}>', dt, len(data) / 32000

def main():
    if not KEY: sys.exit('GUGU_MINIMAX_API_KEY not set')
    manifest = json.load(open(os.path.join(FIX, 'manifest.json'), encoding='utf-8'))
    conn = http.client.HTTPSConnection('api.minimax.cn', timeout=60)
    per = {}
    rows = ['| lang | file | sec | ms | CER | ref | hyp |', '|---|---|---|---|---|---|---|']
    for m in manifest:
        hyp, dt, sec = recognise(conn, os.path.join(FIX, m['file']))
        hyp = hyp.replace('姑姑', '咕咕')
        c = cer(m['text'], hyp)
        per.setdefault(m['lang'], []).append((c, dt, sec))
        rows.append(f"| {m['lang']} | {m['file']} | {sec:.1f} | {dt*1000:.0f} | {c*100:.0f}% | {m['text']} | {hyp} |")
        print(f"{m['file']} {sec:.1f}s {dt*1000:.0f}ms CER {c*100:3.0f}%  {m['text']} -> {hyp}")
    out = ['# ASR CER on the fixture set · ' + time.strftime('%Y-%m-%d %H:%M'), '', '## summary', '']
    for lang, v in per.items():
        cs = sorted(x[0] for x in v); ms = sorted(x[1] for x in v); secs = [x[2] for x in v]
        out.append(f"- {lang}: mean CER {100*sum(cs)/len(cs):.1f}%, median {100*cs[len(cs)//2]:.0f}%, {sum(1 for c in cs if c == 0)}/{len(cs)} exact; latency median {1000*ms[len(ms)//2]:.0f} ms for median {sorted(secs)[len(secs)//2]:.1f} s audio")
    out += ['', '## rows', ''] + rows
    os.makedirs(os.path.join(ROOT, 'host', 'data', 'eval'), exist_ok=True)
    p = os.path.join(ROOT, 'host', 'data', 'eval', 'asr_cer.md')
    open(p, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print('\n'.join(out[3:3 + len(per)])); print('written', p)

if __name__ == '__main__':
    main()
