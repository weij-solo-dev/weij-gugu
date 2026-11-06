#!/usr/bin/env python3
"""The latency bench: replay the three-language recordings on the device, one turn at a time, and keep how long each
answer took from the release of the key (the end of speaking) to the first sound, with every stage in between.

  PY=$(ls .derived_data/espressif/python_env/*/bin/python)      # IDF's Python has pyserial
  $PY tools/dev/latency_bench.py --label base [--turns 100] [--langs zh,en,yue] [--per-call 5] [--port P]

The recordings (tools/eval/make_fixtures.py speech -> test/fixtures/speech/*.wav, 20 per language) are copied to
/data/bench on the device once (console `put`). Each language runs with the device set to it (setting `lang`, read at
boot), in calls of --per-call turns; the device is reset between languages and the setting put back at the end.
Results: host/data/bench/<label>/turns.jsonl (one record per turn), serial.log, summary.txt.
  $PY tools/dev/latency_bench.py --report host/data/bench/base [host/data/bench/fixed ...]   # summaries again, side by side
"""
import argparse, glob, json, os, re, statistics, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FIXTURES = os.path.join(ROOT, 'test', 'fixtures', 'speech')
PROMPT = b'gugu> '
ACTIVITY = re.compile(r'\b(minimax|qwen|volc|openai|gemini|life|engine|call|httptts):')   # vendor work in the background

RX = {
    'turn': re.compile(r'turn: asr (-?\d+) ms, sent (-?\d+) ms, first token (-?\d+) ms, first segment (-?\d+) ms, '
                       r'first tts (-?\d+) ms, first play (-?\d+) ms, done (-?\d+) ms \| (.*?) -> (.*)$'),
    'asr': re.compile(r'asr upload (\d+) ms audio \((\d+) KB\): http (\d+) in (\d+) ms \(connect (-?\d+), body sent (-?\d+), '
                      r'answer (-?\d+), reused (\d)\)'),
    'chat': re.compile(r'chat: http (\d+), first byte (-?\d+) ms, done (-?\d+) ms, reused (\d), in (\d+) out (\d+) cached (\d+), '
                       r'stop (\S+), thinking (\d+) B'),
    'seg0': re.compile(r'segment 0 \[(\w*)\] (.*)$'),
    'result': re.compile(r'replay turn \d+: (\S+)'),
    'voice': re.compile(r'voice delivered: (\d+) ms for (\d+) characters'),
    'failed': re.compile(r'turn failed at (\w+): (.*)'),
    'nothing': re.compile(r'nothing heard'),
    'replay': re.compile(r'replay: press (\d+) ms, inject (\d+) ms, release (\d+) ms'),
    'prompt': re.compile(r'prompt: transcript handed on in (\d+) ms, built in (\d+) ms'),
}
STAGES = ['asr', 'chat_sent', 'first_token', 'first_seg', 'first_tts', 'first_play', 'done']


class Device:
    def __init__(self, port, log_path):
        import serial
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.05
        self.s.dtr = False; self.s.rts = False
        self.s.open()                                   # opening the port resets the board
        self.log = open(log_path, 'a', encoding='utf-8')
        self.partial = b''; self.prompts = 0; self.tail = b''; self.t0 = time.time()

    def reset(self):
        self.s.dtr = False; self.s.rts = True; time.sleep(0.1); self.s.rts = False

    def read(self, seconds):
        """Read for up to `seconds` (returns at once if nothing more is waiting after the first quiet poll when 0)."""
        out = []; until = time.time() + seconds
        while True:
            chunk = self.s.read(4096)
            if chunk:
                data = self.tail + chunk
                self.prompts += data.count(PROMPT)
                self.tail = data[-(len(PROMPT) - 1):]
                self.partial += chunk
                while b'\n' in self.partial:
                    line, self.partial = self.partial.split(b'\n', 1)
                    text = line.decode('utf-8', 'replace').rstrip('\r')
                    while text.startswith('gugu> '): text = text[6:]
                    self.log.write(f'{time.time() - self.t0:9.3f} {text}\n')
                    out.append(text)
            if time.time() >= until: break
        self.log.flush()
        return out

    def wait_for(self, pattern, seconds):
        rx = re.compile(pattern); seen = []; until = time.time() + seconds
        while time.time() < until:
            for line in self.read(0):                  # one poll (the port's 50 ms timeout), so a reply is seen at once
                seen.append(line)
                m = rx.search(line)
                if m: return m, seen
            if self.partial and rx.search(self.partial.decode('utf-8', 'replace')):   # a prompt-glued line
                return rx.search(self.partial.decode('utf-8', 'replace')), seen
        return None, seen

    def cmd(self, text, timeout=30):
        """Send one console command and return every line until its prompt comes back."""
        before = self.prompts
        self.s.write((text + '\n').encode()); self.s.flush()
        lines = []; until = time.time() + timeout
        while time.time() < until and self.prompts <= before:
            lines += self.read(0.1)
        if self.prompts <= before: lines.append(f'(bench: no prompt after {timeout} s)')
        return lines

    def settle(self, quiet=8, cap=90):
        """After a boot: wait until the console answers and nothing has talked to a vendor for `quiet` seconds."""
        self.wait_for(r'console ready', 30)
        start = time.time(); last = time.time()
        while time.time() - start < cap:
            for line in self.read(0.5):
                if ACTIVITY.search(line): last = time.time()
            if time.time() - last >= quiet: break
        stat = ' '.join(self.cmd('stat', 10))
        return 'online 1' in stat

    def put(self, local, remote):
        data = open(local, 'rb').read()
        before = self.prompts
        self.s.write(f'put {remote} {len(data)}\n'.encode()); self.s.flush()
        m, _ = self.wait_for(r'put: ready (\d+)', 10)
        if not m: raise RuntimeError(f'put {remote}: the device did not get ready')
        piece = int(m.group(1)); off = 0
        while off < len(data):
            self.s.write(data[off:off + piece]); self.s.flush(); off = min(off + piece, len(data))
            m, _ = self.wait_for(rf'put: (ok {off}\b|stopped)', 15)
            if not m or 'stopped' in m.group(0): raise RuntimeError(f'put {remote}: stopped near byte {off}')
        m, _ = self.wait_for(r'put: (saved|write failed) (\d+) bytes fnv ([0-9a-f]{8})', 20)
        h = 2166136261
        for b in data: h = ((h ^ b) * 16777619) & 0xffffffff
        if not m or m.group(1) != 'saved' or int(m.group(3), 16) != h: raise RuntimeError(f'put {remote}: {m.group(0) if m else "no answer"}')
        until = time.time() + 5
        while self.prompts <= before and time.time() < until: self.read(0.1)   # the command's prompt, so the next command counts from here


def parse_turn(lines):
    r = {}
    for line in lines:
        for key, rx in RX.items():
            m = rx.search(line)
            if not m: continue
            g = m.groups()
            if key == 'turn':
                r['ms'] = dict(zip(STAGES, map(int, g[:7]))); r['transcript'] = g[7]; r['reply'] = g[8]
            elif key == 'asr':
                r['asr'] = dict(zip(['audio_ms', 'kb', 'http', 'total', 'connect', 'sent', 'answer', 'reused'], map(int, g)))
            elif key == 'chat':
                r['chat'] = dict(zip(['http', 'first_byte', 'done', 'reused', 'in', 'out', 'cached'], map(int, g[:7])))
                r['chat']['stop'] = g[7]; r['chat']['thinking'] = int(g[8])
            elif key == 'seg0' and 'seg0' not in r: r['seg0'] = {'emotion': g[0], 'text': g[1]}
            elif key == 'result': r['result'] = g[0]
            elif key == 'voice': r['voice_ms'], r['tts_chars'] = int(g[0]), int(g[1])
            elif key == 'failed': r['failed'] = f'{g[0]}: {g[1]}'
            elif key == 'nothing': r['nothing_heard'] = True
            elif key == 'replay': r['release_ms'] = int(g[2])     # how long the key's release took to hand the turn over
            elif key == 'prompt': r['prompt'] = {'handed_on': int(g[0]), 'built': int(g[1])}
    return r


def pct(v, p):
    v = sorted(v); k = (len(v) - 1) * p; f = int(k); c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)


def summarize(turns, title):
    ok = [t for t in turns if t.get('ms') and t['ms'].get('first_play', -1) >= 0]
    out = [f'== {title}: {len(ok)} of {len(turns)} turns answered ==']
    if not ok: return '\n'.join(out)
    def row(name, vals):
        vals = [x for x in vals if x is not None]
        if not vals: return
        out.append(f'{name:28s} {pct(vals, .5):7.0f} {pct(vals, .9):7.0f} {statistics.mean(vals):7.0f} {min(vals):7.0f} {max(vals):7.0f}  (n={len(vals)})')
    out.append(f'{"ms from the release":28s} {"P50":>7s} {"P90":>7s} {"mean":>7s} {"min":>7s} {"max":>7s}')
    for s in STAGES: row(s, [t['ms'][s] for t in ok if t['ms'].get(s, -1) >= 0])
    out.append('-- each stage on its own --')
    def gap(a, b): return [t['ms'][b] - t['ms'][a] for t in ok if t['ms'].get(a, -1) >= 0 and t['ms'].get(b, -1) >= 0]
    row('asr (release -> words)', [t['ms']['asr'] for t in ok])
    row('  upload (body sent)', [t['asr']['sent'] for t in ok if t.get('asr') and t['asr']['sent'] > 0])
    row('  server (sent -> answer)', [t['asr']['answer'] - t['asr']['sent'] for t in ok if t.get('asr') and t['asr']['sent'] > 0])
    row('prompt (words -> request)', gap('asr', 'chat_sent'))
    row('  transcript handed on', [t['prompt']['handed_on'] for t in ok if t.get('prompt')])
    row('  prompt built', [t['prompt']['built'] for t in ok if t.get('prompt')])
    row('model (request -> 1st word)', gap('chat_sent', 'first_token'))
    row('  of which headers', [t['chat']['first_byte'] for t in ok if t.get('chat')])
    row('sentence (1st word -> seg)', gap('first_token', 'first_seg'))
    row('voice (seg -> 1st audio)', gap('first_seg', 'first_tts'))
    row('speaker (audio -> played)', gap('first_tts', 'first_play'))
    row('release handling (key up)', [t['release_ms'] for t in ok if 'release_ms' in t])
    fp = [t['ms']['first_play'] for t in ok]
    out.append(f'first play <= 2000 ms: {sum(1 for x in fp if x <= 2000)}/{len(fp)}; <= 2500 ms: {sum(1 for x in fp if x <= 2500)}/{len(fp)}')
    langs = sorted({t['lang'] for t in ok})
    for l in langs:
        v = [t['ms']['first_play'] for t in ok if t['lang'] == l]
        out.append(f'  {l:4s} first play P50 {pct(v, .5):5.0f}  P90 {pct(v, .9):5.0f}  (n={len(v)})')
    firsts = [t['seg0']['text'] for t in ok if t.get('seg0')]
    if firsts:
        lens = [len(x) for x in firsts]
        out.append(f'first segment: median {pct(lens, .5):.0f} characters, P90 {pct(lens, .9):.0f}')
    chats = [t['chat'] for t in ok if t.get('chat')]
    if chats:
        out.append(f'chat tokens: in median {pct([c["in"] for c in chats], .5):.0f}, cached median {pct([c["cached"] for c in chats], .5):.0f}, '
                   f'out median {pct([c["out"] for c in chats], .5):.0f}; thinking bytes median {pct([c["thinking"] for c in chats], .5):.0f}')
    first_in_call = [t['ms']['first_play'] for t in ok if t.get('turn_in_call') == 1]
    later = [t['ms']['first_play'] for t in ok if t.get('turn_in_call', 1) > 1]
    if first_in_call and later:
        out.append(f'first turn of a call P50 {pct(first_in_call, .5):.0f} (n={len(first_in_call)}); later turns P50 {pct(later, .5):.0f} (n={len(later)})')
    bad = [t for t in turns if t not in ok]
    for t in bad: out.append(f'  not answered: {t.get("file")} {t.get("result", "?")} {t.get("failed", "")}{" nothing heard" if t.get("nothing_heard") else ""}')
    return '\n'.join(out)


def load(dirpath):
    turns = [json.loads(l) for l in open(os.path.join(dirpath, 'turns.jsonl'), encoding='utf-8') if l.strip()]
    log = os.path.join(dirpath, 'serial.log')
    if os.path.exists(log) and turns and not any('release_ms' in t for t in turns):   # an older run: fill in what the parser learnt since
        chunks, cur = [], []
        for line in open(log, encoding='utf-8'):
            cur.append(line.rstrip('\n'))
            if RX['result'].search(line): chunks.append(cur); cur = []
        if len(chunks) == len(turns):
            for t, ch in zip(turns, chunks):
                extra = parse_turn(ch)
                for k in ('release_ms', 'prompt'):
                    if k in extra: t[k] = extra[k]
    return turns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/cu.usbmodem83401')
    ap.add_argument('--label', default=time.strftime('bench-%Y%m%d-%H%M'))
    ap.add_argument('--turns', type=int, default=100)
    ap.add_argument('--langs', default='zh,en,yue')
    ap.add_argument('--per-call', type=int, default=5)
    ap.add_argument('--upload', action='store_true', help='copy the recordings again even when /data/bench has them')
    ap.add_argument('--report', nargs='+', help='print the summaries of finished runs (directories) and exit')
    a = ap.parse_args()
    if a.report:
        for d in a.report: print(summarize(load(d), os.path.basename(d.rstrip('/')))); print()
        return

    out_dir = os.path.join(ROOT, 'host', 'data', 'bench', a.label)
    os.makedirs(out_dir, exist_ok=True)
    langs = [l for l in a.langs.split(',') if l]
    files = {l: sorted(os.path.basename(p) for p in glob.glob(os.path.join(FIXTURES, f'{l}_*.wav'))) for l in langs}
    if any(not v for v in files.values()): sys.exit('missing recordings: run tools/eval/make_fixtures.py speech')
    texts = {}
    mf = os.path.join(FIXTURES, 'manifest.json')
    if os.path.exists(mf): texts = {e['file']: e['text'] for e in json.load(open(mf, encoding='utf-8'))}
    share = [a.turns // len(langs) + (1 if i < a.turns % len(langs) else 0) for i in range(len(langs))]

    dev = Device(a.port, os.path.join(out_dir, 'serial.log'))
    print(f'device reset; waiting for it to settle ...', flush=True)
    dev.settle()
    original = ' '.join(dev.cmd('get lang')).split('=')[-1].strip()
    have = ' '.join(dev.cmd('ls /data/bench'))
    need = [f for l in langs for f in files[l] if a.upload or f not in have]
    if need:
        print(f'copying {len(need)} recordings to /data/bench ...', flush=True)
        t0 = time.time()
        for f in need: dev.put(os.path.join(FIXTURES, f), f'/data/bench/{f}')
        print(f'  done in {time.time() - t0:.0f} s', flush=True)

    turns_path = os.path.join(out_dir, 'turns.jsonl')
    all_turns = []
    current = original if original != '(unset)' else 'zh'
    for li, lang in enumerate(langs):
        if lang != current:
            dev.cmd(f'set lang {lang}'); dev.reset(); current = lang
            print(f'[{lang}] reset into {lang}; waiting for it to settle ...', flush=True)
            if not dev.settle(): print('  (not online yet)', flush=True)
        n = share[li]; call = 0; k = 0
        while k < n:
            call += 1
            for j in range(min(a.per_call, n - k)):
                f = files[lang][k % len(files[lang])]
                lines = dev.cmd(f'replay /data/bench/{f}', 90)
                t = parse_turn(lines)
                t.update({'lang': lang, 'file': f, 'said': texts.get(f, ''), 'call': f'{lang}-{call}', 'turn_in_call': j + 1, 'label': a.label})
                all_turns.append(t)
                with open(turns_path, 'a', encoding='utf-8') as fh: fh.write(json.dumps(t, ensure_ascii=False) + '\n')
                ms = t.get('ms', {})
                print(f'[{lang} {len(all_turns):3d}] {f} {t.get("result", "?"):10s} play {ms.get("first_play", -1):5d} | '
                      f'asr {ms.get("asr", -1):5d} tok {ms.get("first_token", -1):5d} seg {ms.get("first_seg", -1):5d} '
                      f'tts {ms.get("first_tts", -1):5d} | {t.get("transcript", "")[:24]} -> {(t.get("seg0") or {}).get("text", "")[:30]}', flush=True)
                k += 1
            dev.cmd('hang'); dev.read(1.0)
    dev.cmd('rm /data/bench')                     # the recordings filled the data partition once (6 MB of 6.25): never leave them
    if current != (original if original != '(unset)' else 'zh'):
        dev.cmd(f'set lang {original if original != "(unset)" else "zh"}'); dev.reset(); dev.settle(quiet=3, cap=30)
    summary = summarize(all_turns, a.label)
    open(os.path.join(out_dir, 'summary.txt'), 'w', encoding='utf-8').write(summary + '\n')
    print(summary)


if __name__ == '__main__':
    main()
