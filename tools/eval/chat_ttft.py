#!/usr/bin/env python3
"""How soon does a chat model start answering? Streams the same Gugu-sized request (persona, canon, tags, examples,
~2k tokens, cached like the device sends it) N times per configuration over one keep-alive connection, and reports
the time to the response headers, to the first text, to the end of the first sentence, and to the end.

  set -a; . ~/.config/gugu/secrets.env; set +a
  python3 tools/eval/chat_ttft.py [--runs 10] [--configs m3,flash]

Absolute numbers depend on the network this Mac is on; compare the configurations with each other.
"""
import argparse, http.client, json, os, re, ssl, statistics, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
HOST = 'api.minimax.cn'
# The device's endpoint (Anthropic-compatible). M3 runs with thinking off, as the preset asks for a call; M3.1-Flash
# cannot turn thinking off (400 "requires adaptive thinking"), so it thinks a little before the first word. (MiniMax's
# OpenAI-compatible endpoint put M3's thinking into the reply text on 2026-11-05: not a fair comparison, left out.)
CONFIGS = {
    'm3':    dict(api='anthropic', model='MiniMax-M3', extra={'thinking': {'type': 'disabled'}}),
    'flash': dict(api='anthropic', model='MiniMax-M3.1-Flash', extra={'output_config': {'effort': 'low'}}),
}
QUESTIONS = ['咕咕，你在干嘛呀？', '我今天在幼儿园画了一只恐龙。', '为什么天是蓝色的？', '我不想睡觉。', '你去过海边吗？',
             '我今天吃了西瓜。', '咕咕你会飞吗？', '妈妈说明天带我去动物园。', '我有一个好朋友叫小宝。', '你那边下雨了吗？']
SENTENCE_END = re.compile(r'[。！？!?]')


def system_blocks():
    rd = lambda p: open(os.path.join(ROOT, 'content', 'prompts', p), encoding='utf-8').read()
    a = rd('persona.md').replace('{child_name}', '小朋友') + '\n\n【设定集】\n' + rd('canon_seed.md') + '\n\n【表情标签】\n' + rd('tags.md')
    a2 = '这一通电话用普通话回答，写简体中文。\n\n【对话示例】\n' + rd('examples/zh.md')
    b = '【你记得的事】\n（还没有，这是你们最早的几次聊天。）\n\n【设定集 · 后来知道的】\n（还没有。）\n'
    c = '今天是2026年11月5日，星期四，下午。你现在在家里的鸽舍。你今天心情不错。'
    return [{'type': 'text', 'text': a, 'cache_control': {'type': 'ephemeral'}},
            {'type': 'text', 'text': a2, 'cache_control': {'type': 'ephemeral'}},
            {'type': 'text', 'text': b, 'cache_control': {'type': 'ephemeral'}},
            {'type': 'text', 'text': c}]


def request(conn, key, cfg, question):
    if cfg['api'] == 'anthropic':
        path = '/anthropic/v1/messages'
        body = {'model': cfg['model'], 'max_tokens': 400, 'stream': True, 'temperature': 0.8, 'system': system_blocks(),
                'messages': [{'role': 'user', 'content': question}], **cfg['extra']}
        headers = {'x-api-key': key, 'anthropic-version': '2023-06-01', 'content-type': 'application/json'}
    else:
        path = '/v1/chat/completions'
        sys_text = '\n\n'.join(b['text'] for b in system_blocks())
        body = {'model': cfg['model'], 'max_tokens': 400, 'stream': True, 'temperature': 0.8,
                'messages': [{'role': 'system', 'content': sys_text}, {'role': 'user', 'content': question}], **cfg['extra']}
        headers = {'Authorization': f'Bearer {key}', 'content-type': 'application/json'}
    data = json.dumps(body, ensure_ascii=False).encode()
    t0 = time.perf_counter()
    conn.request('POST', path, body=data, headers=headers)
    resp = conn.getresponse()
    t_head = time.perf_counter() - t0
    if resp.status != 200:
        return {'error': f'http {resp.status}: {resp.read()[:200]!r}'}
    text = ''; t_first = t_sentence = None; usage = {}
    for raw in resp:                                      # SSE lines as they come
        line = raw.decode('utf-8', 'replace').strip()
        if not line.startswith('data:'): continue
        payload = line[5:].strip()
        if payload == '[DONE]': break
        try: ev = json.loads(payload)
        except ValueError: continue
        piece = ''
        if cfg['api'] == 'anthropic':
            if ev.get('type') == 'content_block_delta' and ev['delta'].get('type') == 'text_delta': piece = ev['delta']['text']
            if ev.get('type') == 'message_start': usage.update(ev['message'].get('usage', {}))
            if ev.get('type') == 'message_delta': usage.update(ev.get('usage', {}))
        else:
            for ch in ev.get('choices', []): piece += (ch.get('delta') or {}).get('content') or ''
            if ev.get('usage'): usage.update(ev['usage'])
        if piece:
            now = time.perf_counter() - t0
            if t_first is None: t_first = now
            text += piece
            if t_sentence is None and SENTENCE_END.search(re.sub(r'\[[a-z]+\]', '', text)): t_sentence = now
    t_done = time.perf_counter() - t0
    return {'head': t_head, 'first': t_first, 'sentence': t_sentence, 'done': t_done, 'text': text, 'usage': usage}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', type=int, default=10)
    ap.add_argument('--configs', default='m3,flash')
    a = ap.parse_args()
    key = os.environ.get('GUGU_MINIMAX_API_KEY')
    if not key: sys.exit('GUGU_MINIMAX_API_KEY is not set (source ~/.config/gugu/secrets.env)')
    names = [n for n in a.configs.split(',') if n]
    conns = {n: http.client.HTTPSConnection(HOST, context=ssl.create_default_context(), timeout=30) for n in names}
    results = {n: [] for n in names}
    for i in range(a.runs):                               # round robin, so drift in the network hits every configuration alike
        for n in names:
            try: r = request(conns[n], key, CONFIGS[n], QUESTIONS[i % len(QUESTIONS)])
            except Exception as e:                          # a dropped keep-alive connection: open a new one and go on
                conns[n] = http.client.HTTPSConnection(HOST, context=ssl.create_default_context(), timeout=30); r = {'error': repr(e)}
            results[n].append(r)
            if 'error' in r: print(f'{n} run {i + 1}: {r["error"]}', flush=True); continue
            print(f'{n:12s} run {i + 1:2d}: head {r["head"]*1000:5.0f} first {r["first"]*1000 if r["first"] else -1:5.0f} '
                  f'sentence {r["sentence"]*1000 if r["sentence"] else -1:5.0f} done {r["done"]*1000:5.0f} ms | {r["text"][:40]!r}', flush=True)
    print()
    for n in names:
        ok = [r for r in results[n] if 'error' not in r and r['first']]
        if not ok: print(f'{n}: no answers'); continue
        med = lambda k: statistics.median([r[k] for r in ok if r[k] is not None]) * 1000
        print(f'{n:12s} n={len(ok):2d}  median ms: headers {med("head"):5.0f}  first text {med("first"):5.0f}  '
              f'first sentence {med("sentence"):5.0f}  done {med("done"):5.0f}')


if __name__ == '__main__':
    main()
