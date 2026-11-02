#!/usr/bin/env python3
"""One-off evaluation helper (not part of the product).

  make_fixtures.py speech   — synthesise child-side utterances (20 per language) as 16 kHz WAVs under
                              test/fixtures/speech/ for the replay harness (stand-ins until real child recordings).
  make_fixtures.py voices   — synthesise the same three sentences with candidate Gugu voices into
                              assets/samples/voices/ for the blind-listening check.
Needs GUGU_MINIMAX_API_KEY in the environment. Uses the China site.
"""
import json, os, struct, sys, time, urllib.request

KEY = os.environ.get('GUGU_MINIMAX_API_KEY')
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
URL = 'https://api.minimax.cn/v1/t2a_v2'

def tts(text, voice, boost, speed=1.0):
    body = {"model": "speech-2.8-turbo", "text": text, "stream": False, "language_boost": boost,
            "voice_setting": {"voice_id": voice, "speed": speed, "vol": 1.0, "pitch": 0},
            "audio_setting": {"sample_rate": 16000, "format": "pcm", "channel": 1}}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), headers={"Authorization": "Bearer " + KEY, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        d = json.load(r)
    if d.get('base_resp', {}).get('status_code') != 0:
        raise RuntimeError(d.get('base_resp'))
    return bytes.fromhex(d['data']['audio']), d.get('extra_info', {}).get('usage_characters', 0)

def write_wav(path, pcm):
    hdr = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 16000, 32000, 2, 16) + b'data' + struct.pack('<I', len(pcm))
    with open(path, 'wb') as f: f.write(hdr + pcm)

SPEECH = {
    'zh': ("cute_boy", "Chinese", ["咕咕你在干嘛呀", "我今天在幼儿园画了一只恐龙", "为什么天是蓝色的", "我不想睡觉", "你去过海边吗", "我今天吃了西瓜",
            "咕咕你会飞吗", "妈妈说明天带我去动物园", "我有一个好朋友叫小宝", "你那边下雨了吗", "我会自己穿鞋了", "我害怕打雷", "你最喜欢吃什么",
            "我们一起唱歌吧", "你几岁啦", "我今天摔了一跤", "你什么时候回来", "我想你了", "奶奶家有小狗吗", "晚安咕咕"]),
    'yue': ("Cantonese_PlayfulMan", "Chinese,Yue", ["咕咕你喺邊度呀", "我今日食咗雪糕", "點解你會飛㗎", "我唔想瞓覺", "你去過海邊未", "我今日返學校畫畫",
            "你識唔識游水", "媽咪話聽日去公園", "我有個好朋友叫小寶", "你嗰度落雨呀", "我識自己著鞋喇", "我怕行雷", "你最鍾意食乜嘢",
            "我哋一齊唱歌好唔好", "你幾多歲呀", "我今日跌咗一跤", "你幾時返嚟呀", "我掛住你", "嫲嫲屋企有冇狗仔", "早抖咕咕"]),
    'en': ("Cute_Elf", "English", ["Gugu what are you doing", "I drew a dinosaur at school today", "Why is the sky blue", "I don't want to sleep",
            "Have you been to the beach", "I ate watermelon today", "Can you fly", "Mum says we go to the zoo tomorrow", "My best friend is Bobo",
            "Is it raining where you are", "I can put on my shoes by myself", "I'm scared of thunder", "What's your favourite food",
            "Let's sing a song", "How old are you", "I fell down today", "When are you coming home", "I miss you", "Does grandma have a dog", "Good night Gugu"]),
}

VOICE_TEXT = {'zh': ("Chinese", "咕，我回来啦！今天我在公园看到好多鸭子，它们排着队游泳。你今天开心吗？"),
              'yue': ("Chinese,Yue", "咕，我返嚟喇！今日我喺公園見到好多鴨仔，佢哋排住隊游水。你今日開唔開心呀？"),
              'en': ("English", "Coo, I'm home! Today I saw so many ducks in the park, swimming in a line. Did you have a good day?")}
VOICES = ["Cantonese_CuteGirl", "Cantonese_KindWoman", "Cantonese_GentleLady", "lovely_girl", "Chinese (Mandarin)_Cute_Spirit", "Cute_Elf"]

def main():
    if not KEY: sys.exit('GUGU_MINIMAX_API_KEY not set')
    mode = sys.argv[1] if len(sys.argv) > 1 else 'speech'
    total_chars = 0
    if mode == 'speech':
        out = os.path.join(ROOT, 'test', 'fixtures', 'speech'); os.makedirs(out, exist_ok=True)
        manifest = []
        for lang, (voice, boost, sents) in SPEECH.items():
            for i, s in enumerate(sents):
                path = os.path.join(out, f'{lang}_{i+1:02d}.wav')
                if os.path.exists(path): manifest.append({'file': os.path.basename(path), 'lang': lang, 'text': s}); continue
                pcm, chars = tts(s, voice, boost, speed=1.05); total_chars += chars
                write_wav(path, pcm); manifest.append({'file': os.path.basename(path), 'lang': lang, 'text': s})
                print(f'{lang}_{i+1:02d} {len(pcm)/32000:.1f}s {s}'); time.sleep(0.2)
        json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), ensure_ascii=False, indent=1)
    else:
        out = os.path.join(ROOT, 'assets', 'samples', 'voices'); os.makedirs(out, exist_ok=True)
        rows = []
        for v in VOICES:
            for lang, (boost, text) in VOICE_TEXT.items():
                name = f"{v.replace(' ', '_').replace('(', '').replace(')', '')}__{lang}.wav"
                path = os.path.join(out, name)
                try:
                    pcm, chars = tts(text, v, boost, speed=0.95); total_chars += chars
                    write_wav(path, pcm); rows.append((v, lang, name, f'{len(pcm)/32000:.1f}s'))
                    print(v, lang, f'{len(pcm)/32000:.1f}s')
                except Exception as e:
                    print(v, lang, 'FAILED', e); rows.append((v, lang, '', str(e)))
                time.sleep(0.2)
        html = ['<!doctype html><meta charset="utf-8"><title>gugu voice candidates</title><body style="font-family:sans-serif;background:#111;color:#ddd;padding:24px">',
                '<h1>Gugu voice candidates · same lines in zh / yue / en</h1><p>Question for the listener: is it the same "person" across the three languages, and does it sound like a warm, slightly husky older-kid pigeon?</p><table>']
        for v in VOICES:
            html.append(f'<tr><th style="text-align:left;padding:8px">{v}</th>')
            for lang in ('zh', 'yue', 'en'):
                r = next((x for x in rows if x[0] == v and x[1] == lang), None)
                html.append(f'<td style="padding:8px">{lang}<br>' + (f'<audio controls src="{r[2]}"></audio>' if r and r[2] else f'<i>{r[3] if r else ""}</i>') + '</td>')
            html.append('</tr>')
        html.append('</table></body>')
        open(os.path.join(out, 'index.html'), 'w').write('\n'.join(html))
    print('billed characters:', total_chars)

if __name__ == '__main__':
    main()
