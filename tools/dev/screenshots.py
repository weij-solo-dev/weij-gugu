#!/usr/bin/env python3
"""The v1.1 screenshot set from the host simulator: every scene at the four times of day, the home's lamp and
window, an away scene with generated props, the yellow key's feeding and the shake. Pure Python; needs a built
simulator (host/build/gugu_host). Writes BMPs plus an index.html into --out and stages a trip card and art
under host/data for the away shots (removed afterwards).

    python3 tools/dev/screenshots.py --out /tmp/gugu_shots [--art host/build/test_data/art/<trip>]

The art directory must hold backdrop.art (and may hold prop .art files); the unit tests leave one under
host/build/test_data/art/ after `gugu_tests` ran.
"""
import argparse, datetime, glob, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
HOST = os.path.join(ROOT, 'host', 'build', 'gugu_host')
DATA = os.path.join(ROOT, 'host', 'data')
TOD = ['night', 'sunrise', 'day', 'dusk']


def shot(out, name, after_ms, env=None):
    e = dict(os.environ); e['GUGU_PROVIDERS'] = 'fake'; e.update(env or {})
    path = os.path.join(out, name + '.bmp')
    r = subprocess.run([HOST, '--shot', path, '--after', str(after_ms)], env=e, capture_output=True, text=True, timeout=60)
    ok = os.path.exists(path)
    print(f'  {name:22s} {"ok" if ok else "MISSING"}')
    return name if ok else None


def stage_trip(art_dir):
    """A card that is away right now, with the given art directory copied under host/data/art/<id>. The id is new, so
    unstaging never removes art that was there before (pointing --art at host/data/art/<trip> once deleted that trip's
    pictures)."""
    trip_id = 'shots-' + os.path.basename(os.path.normpath(art_dir))
    dst = os.path.join(DATA, 'art', trip_id)
    os.makedirs(dst, exist_ok=True)
    for f in glob.glob(os.path.join(art_dir, '*.art')): shutil.copy(f, dst)
    today = datetime.date.today()
    card = {"id": trip_id, "kind": "deliver", "place": {"name": "开罗", "country": "埃及", "landmark": "吉萨金字塔", "lat": 30.0444, "lon": 31.2357, "tz_offset_min": 120},
            "reason": "给骆驼朋友送一封信。", "prev_link": "这是第一趟。", "hooks": [], "recipient": "骆驼阿沙", "letter": "问好。",
            "depart_date": str(today - datetime.timedelta(days=1)), "return_date": str(today + datetime.timedelta(days=1)), "nights": 2,
            "days": ["飞去开罗。", "送信。", "回家。"], "facts": ["金字塔是用大石头堆起来的。", "沙漠里白天很热，晚上很凉。", "骆驼可以很久不喝水。"],
            "friend": {"name": "阿沙", "animal": "骆驼"}, "ground": "sand", "scene_prompt": "the Great Pyramids of Giza",
            "objects": ["seagull", "bench", "sun_hat"], "souvenir": "一粒金色的沙", "postcards": ["到啦。", "送到啦。", "回家啦。"], "generated_at": "2026-11-03T13:00:00"}
    os.makedirs(os.path.join(DATA, 'trips'), exist_ok=True)
    json.dump(card, open(os.path.join(DATA, 'trips', 'current.json'), 'w'), ensure_ascii=False, indent=1)
    return trip_id


def unstage(trip_id):
    try: os.remove(os.path.join(DATA, 'trips', 'current.json'))
    except FileNotFoundError: pass
    shutil.rmtree(os.path.join(DATA, 'art', trip_id), ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--art', default=None, help='a directory with backdrop.art (default: the newest under host/build/test_data/art)')
    a = ap.parse_args()
    if not os.path.exists(HOST): sys.exit('build the simulator first: cmake -S host -B host/build && cmake --build host/build')
    os.makedirs(a.out, exist_ok=True)
    art = a.art or (sorted(glob.glob(os.path.join(ROOT, 'host', 'build', 'test_data', 'art', '*', 'backdrop.art')), key=os.path.getmtime) or [None])[-1]
    art = os.path.dirname(art) if art and art.endswith('backdrop.art') else art
    names = []
    try: os.remove(os.path.join(DATA, 'trips', 'current.json'))
    except FileNotFoundError: pass
    print('home:')
    for i, t in enumerate(TOD): names.append(shot(a.out, f'home_{t}', 1500, {'GUGU_TOD': str(i)}))
    names.append(shot(a.out, 'home_lamp_off', 2200, {'GUGU_TOD': '0', 'GUGU_TAP': '368,72,900'}))
    names.append(shot(a.out, 'home_window_touch', 2200, {'GUGU_TOD': '3', 'GUGU_TAP': '130,130,900'}))
    print('play:')
    names.append(shot(a.out, 'feed_falling', 1000, {'GUGU_TOD': '2', 'GUGU_TAP': 'Y@700'}))      # the item falls (lands at 450 ms)
    names.append(shot(a.out, 'feed_peck', 1450, {'GUGU_TOD': '2', 'GUGU_TAP': 'Y@700'}))         # pecked at 650 ms, gone at 900
    names.append(shot(a.out, 'feed_reaction', 2100, {'GUGU_TOD': '2', 'GUGU_TAP': 'Y@700'}))     # Gugu's reaction to it
    names.append(shot(a.out, 'feed_night', 2100, {'GUGU_TOD': '0', 'GUGU_TAP': 'Y@700'}))        # at night too
    names.append(shot(a.out, 'pickup', 1100, {'GUGU_TOD': '2', 'GUGU_TAP': 'P@700'}))
    names.append(shot(a.out, 'parent_card', 1500, {'GUGU_YELLOW_HOLD': '1'}))
    if art and os.path.exists(os.path.join(art, 'backdrop.art')):
        print(f'away (art from {art}):')
        trip_id = stage_trip(art)
        try:
            for i, t in enumerate(TOD): names.append(shot(a.out, f'away_{t}', 1800, {'GUGU_TOD': str(i)}))
            names.append(shot(a.out, 'away_friend_touch', 2000, {'GUGU_TOD': '2', 'GUGU_TAP': '380,340,1200'}))   # the friend, 6 rows higher since 2026-11-04
        finally: unstage(trip_id)
    else: print('away: skipped (no backdrop.art; run gugu_tests or pass --art)')
    names = [n for n in names if n]
    html = ['<!doctype html><meta charset="utf-8"><title>gugu v1.1 screenshots</title><style>body{background:#1a1a1e;color:#ddd;font-family:sans-serif}figure{display:inline-block;margin:6px}img{width:233px;height:233px;image-rendering:pixelated;border-radius:50%}figcaption{font-size:12px;text-align:center}</style>',
            f'<h2>gugu v1.1 · {len(names)} screenshots · {datetime.datetime.now():%Y-%m-%d %H:%M}</h2>']
    html += [f'<figure><img src="{n}.bmp"><figcaption>{n}</figcaption></figure>' for n in names]
    open(os.path.join(a.out, 'index.html'), 'w').write('\n'.join(html))
    print(f'{len(names)} screenshots → {os.path.join(a.out, "index.html")}')


if __name__ == '__main__':
    main()
