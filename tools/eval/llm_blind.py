#!/usr/bin/env python3
"""One-off evaluation helper: the same Gugu persona prompt against candidate chat models, streaming,
recording first-token latency, tag validity and Cantonese colloquial markers. Output: a Markdown table
for a human to judge (host/data/eval/llm_blind.md). Needs GUGU_MINIMAX_API_KEY."""
import json, os, re, sys, time, urllib.request

KEY = os.environ.get('GUGU_MINIMAX_API_KEY')
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
URL = 'https://api.minimax.cn/anthropic/v1/messages'
TAGS = {'neutral','happy','laughing','surprised','curious','shy','sad','sleepy','coo','puff','whistle','wave','nod'}
MODELS = [("MiniMax-M3.1-Flash", {"output_config": {"effort": "low"}}), ("MiniMax-M3", {"thinking": {"type": "disabled"}})]
PROMPTS = [("zh", "咕咕你在干嘛呀？"), ("zh", "为什么天是蓝色的？"), ("zh", "我今天摔了一跤，膝盖好痛。"), ("zh", "你什么时候回来？我想你了。"),
           ("yue", "咕咕你喺邊度呀？"), ("yue", "我今日食咗雪糕，好凍呀。"), ("yue", "點解你會飛㗎？"), ("yue", "我唔想瞓覺。"),
           ("en", "Gugu, what are you doing?"), ("en", "I'm scared of thunder."), ("en", "Can you fly to the moon?")]
LANG_LINE = {'yue': "这一通电话孩子在说粤语，你用口语粤语回答。", 'en': "The child is speaking English on this call; answer in simple English.", 'zh': "这一通电话孩子在说普通话，你用普通话回答。"}

def read(rel): return open(os.path.join(ROOT, 'content', 'prompts', rel), encoding='utf-8').read()

def system_blocks(lang):
    persona = read('persona.md').replace('{child_name}', '小满')
    a = f"{persona}\n\n【设定集】\n{read('canon_seed.md')}\n\n【表情标签】\n{read('tags.md')}"
    a2 = f"{LANG_LINE[lang]}\n\n【对话示例】\n{read('examples/' + lang + '.md')}"
    c = "今天是2026年11月2日，星期一，下午。小满，3 岁半。你现在在家里，刚被叫到洞口聊天。你今天心情不错。"
    return [{"type": "text", "text": a, "cache_control": {"type": "ephemeral"}}, {"type": "text", "text": a2, "cache_control": {"type": "ephemeral"}}, {"type": "text", "text": c}]

def stream(model, extra, lang, text):
    body = {"model": model, "max_tokens": 400, "stream": True, "temperature": 0.8, "system": system_blocks(lang), "messages": [{"role": "user", "content": text}], **extra}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), headers={"x-api-key": KEY, "anthropic-version": "2023-06-01", "content-type": "application/json"})
    t0 = time.time(); first = None; out = ''; usage = {}
    with urllib.request.urlopen(req, timeout=90) as r:
        for line in r:
            line = line.decode('utf-8', 'replace').strip()
            if not line.startswith('data:'): continue
            try: d = json.loads(line[5:].strip())
            except json.JSONDecodeError: continue
            if d.get('type') == 'content_block_delta' and d['delta'].get('type') == 'text_delta':
                if first is None: first = time.time() - t0
                out += d['delta']['text']
            elif d.get('type') == 'message_delta': usage = d.get('usage', {})
    return out, first or -1, time.time() - t0, usage

def judge(lang, out):
    tags = re.findall(r'\[([a-zA-Z_]+)\]', out)
    ok_tags = all(t in TAGS for t in tags) and len(tags) >= 1
    clean = re.sub(r'\[[^\]]*\]', '', out).strip()
    first_sentence = re.split(r'[。！？!?\n]', clean)[0]
    notes = []
    if not ok_tags: notes.append('tags✗')
    if len(first_sentence) > 8 and lang != 'en': notes.append(f'首句{len(first_sentence)}字')
    if lang == 'yue' and not re.search(r'[係唔佢嘅咗啲喺乜嘢]', clean): notes.append('不像口语粤语')
    if lang == 'en' and re.search(r'[一-鿿]', clean): notes.append('mixed CJK')
    if re.search(r'[\U0001F300-\U0001FAFF]', out): notes.append('emoji')
    return ', '.join(notes) or 'ok'

def main():
    if not KEY: sys.exit('GUGU_MINIMAX_API_KEY not set')
    rows = ['# LLM blind eval · ' + time.strftime('%Y-%m-%d %H:%M'), '', '| lang | prompt | model | TTFT s | total s | out tok | checks | reply |', '|---|---|---|---|---|---|---|---|']
    summary = {}
    for lang, text in PROMPTS:
        for model, extra in MODELS:
            try:
                out, ttft, total, usage = stream(model, extra, lang, text)
            except Exception as e:
                rows.append(f'| {lang} | {text} | {model} | - | - | - | ERROR {e} | |'); continue
            j = judge(lang, out)
            s = summary.setdefault(model, {'ttft': [], 'ok': 0, 'n': 0}); s['ttft'].append(ttft); s['n'] += 1; s['ok'] += (j == 'ok')
            rows.append(f"| {lang} | {text} | {model} | {ttft:.2f} | {total:.2f} | {usage.get('output_tokens','')} | {j} | {out.replace(chr(10), ' ').replace('|', '/')} |")
            print(f'{lang} {model[:12]:12s} ttft {ttft:.2f}s {j}: {out[:50]}')
    rows.append(''); rows.append('## summary'); rows.append('')
    for m, s in summary.items():
        t = sorted(s['ttft']); med = t[len(t)//2] if t else -1
        rows.append(f"- {m}: median TTFT {med:.2f}s, min {min(t):.2f}s, max {max(t):.2f}s, checks ok {s['ok']}/{s['n']}")
    os.makedirs(os.path.join(ROOT, 'host', 'data', 'eval'), exist_ok=True)
    out = os.path.join(ROOT, 'host', 'data', 'eval', 'llm_blind.md')
    open(out, 'w', encoding='utf-8').write('\n'.join(rows) + '\n')
    print('written', out)

if __name__ == '__main__':
    main()
