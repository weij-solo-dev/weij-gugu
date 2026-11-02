#!/usr/bin/env python3
"""Summarise call logs (data/calls/**/*.jsonl) into the acceptance table: per-stage P50/P90 in ms.
  python3 tools/dev/replay_report.py [host/data]
"""
import glob, json, os, statistics, sys
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..', 'host', 'data')
rows = []
for f in sorted(glob.glob(os.path.join(root, 'calls', '*', '*.jsonl'))):
    for line in open(f, encoding='utf-8'):
        line = line.strip()
        if line: 
            try: rows.append(json.loads(line))
            except json.JSONDecodeError: pass
if not rows:
    print('no turns found under', root); sys.exit(1)
def pct(v, p):
    v = sorted(v); k = (len(v) - 1) * p; f = int(k); c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)
stages = ['asr', 'first_token', 'first_tts', 'first_play', 'done']
print(f'{len(rows)} turns')
print(f"{'stage':12s} {'P50':>8s} {'P90':>8s} {'min':>8s} {'max':>8s}")
for s in stages:
    v = [r['ms'][s] for r in rows if r.get('ms', {}).get(s, -1) >= 0]
    if v: print(f'{s:12s} {pct(v,.5):8.0f} {pct(v,.9):8.0f} {min(v):8.0f} {max(v):8.0f}')
langs = {}
for r in rows: langs[r.get('lang','?')] = langs.get(r.get('lang','?'), 0) + 1
print('languages:', langs)
import re
tagged = sum(1 for r in rows if r.get('tags', 0) >= 1)
print(f'tag hit rate: {tagged}/{len(rows)} turns carry at least one valid tag ({100*tagged/len(rows):.0f}%)')
yue = [r for r in rows if r.get('lang') == 'yue']
if yue:
    coll = sum(1 for r in yue if re.search(r'[係唔佢嘅咗啲喺乜嘢]', r.get('reply', '')))
    print(f'cantonese colloquial markers: {coll}/{len(yue)} yue replies')
firsts = [len(re.split(r'[。！？!?]', r.get('reply','').strip())[0].strip()) for r in rows if r.get('lang') != 'en']
if firsts: print(f'first sentence length (zh/yue): median {sorted(firsts)[len(firsts)//2]} chars, max {max(firsts)}')
tok = sum(r['usage'].get('in_tokens',0) for r in rows), sum(r['usage'].get('out_tokens',0) for r in rows), sum(r['usage'].get('cached',0) for r in rows), sum(r['usage'].get('tts_chars',0) for r in rows)
print(f'tokens in {tok[0]} out {tok[1]} cached {tok[2]}; tts chars {tok[3]}')
fen = 0.0; n = 0
for f in glob.glob(os.path.join(root, 'ledger', '*.jsonl')):
    for line in open(f, encoding='utf-8'):
        try: fen += json.loads(line).get('fen', 0); n += 1
        except json.JSONDecodeError: pass
if n: print(f'ledger: {n} turns, ¥{fen/100:.3f} total, ¥{fen/100/n:.4f} per turn (list prices)')
