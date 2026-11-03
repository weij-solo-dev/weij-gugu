#!/usr/bin/env python3
"""Walk the device through a whole trip in a few minutes with the console's clock: plan a card, generate its
pictures, step a day at a time (the diary each night), come home (the recap), let the next night's tick plan
the following trip on the journey. Prints the device's own log lines for each step. Needs Wi-Fi and the
MiniMax key on the device; costs one card, one recap, one card and a handful of pictures.

    .derived_data/espressif/python_env/*/bin/python tools/dev/trip_walk.py --port /dev/cu.usbmodemXXXX [--kind deliver] [--no-art]

The device is left with the clock reset and the second card planned; `trip clear` on the console undoes that.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SERIAL = os.path.join(HERE, 'serial_cmd.py')


def run(port, cmds, timeout=600):
    r = subprocess.run([sys.executable, SERIAL, '--port', port, '--wait', '12', '--timeout', str(timeout), '--times', *cmds], capture_output=True, text=True)
    out = r.stdout + r.stderr
    keep = ('trip', 'diary', 'recap', 'tick', 'art', 'journey', 'phase', 'planned', 'returned', 'scene:', 'backdrop', 'objects for', 'FAIL', 'Guru', '>>>', 'day ', 'fact:', 'reason', '旅程志', '线头')
    for line in out.splitlines():
        if any(k in line for k in keep) and 'heartbeat' not in line: print(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/cu.usbmodem83401')
    ap.add_argument('--kind', default='deliver')
    ap.add_argument('--no-art', action='store_true')
    a = ap.parse_args()
    print('== plan a trip departing tomorrow 08:00')
    run(a.port, ['trip clear', f'trip new {a.kind}', 'trip'])
    if not a.no_art:
        print('== its pictures')
        run(a.port, ['art gen', 'art'])
    print('== day 1 away (+24 h): the scene changes, the diary is written')
    run(a.port, ['clock +24', 'sleep 3000', 'trip', 'diary', 'tod night', 'sleep 2000', 'tod auto'])
    print('== day 2 (+48 h)')
    run(a.port, ['clock +24', 'sleep 2000', 'diary'])
    print('== home again (+96 h): the recap, then the next night plans the following trip on the journey')
    run(a.port, ['clock +48', 'sleep 2000', 'diary', 'recap', 'journey'])
    run(a.port, ['clock +12', 'set life_tick_date 0', 'tick', 'trip', 'journey'], timeout=900)
    print('== clock back to real time')
    run(a.port, ['clock reset', 'trip', 'stat'])


if __name__ == '__main__':
    main()
