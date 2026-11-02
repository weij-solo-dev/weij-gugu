#!/usr/bin/env python3
"""Send console commands to the device and print what comes back.
  python3 tools/dev/serial_cmd.py [--port P] [--reset] [--wait 6] "stat" "ls /content" ...
Each command is sent once the console prompt (--prompt, default "gugu> ") comes back, or, failing that,
after the output has been quiet for --settle seconds (default 1.5). --times prints each command's wall time.
"""
import argparse, sys, time
import serial
ap = argparse.ArgumentParser()
ap.add_argument('--port', default='/dev/cu.usbmodem83401')
ap.add_argument('--reset', action='store_true')
ap.add_argument('--wait', type=float, default=6, help='seconds to wait after open/reset before the first command')
ap.add_argument('--settle', type=float, default=1.5)
ap.add_argument('--timeout', type=float, default=20, help='max seconds to wait for one command')
ap.add_argument('--grep', default='')
ap.add_argument('--prompt', default='gugu> ', help='console prompt that marks the end of a command')
ap.add_argument('--times', action='store_true', help='print the wall time of each command')
ap.add_argument('cmds', nargs='+')
a = ap.parse_args()
s = serial.Serial(); s.port = a.port; s.baudrate = 115200; s.timeout = 0.2; s.dtr = False; s.rts = False; s.open()
if a.reset:
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
pats = [p for p in a.grep.split(',') if p]
# The console prints its prompt exactly once per command it has finished (and once at boot), often glued to
# the next log line, so the end of command i is "the (i)th prompt after the ones seen during the initial
# wait", counted in the raw byte stream.
prompt = a.prompt.encode()
prompts_seen = 0; tail = b''
def count_prompts(chunk):
    global prompts_seen, tail
    data = tail + chunk
    prompts_seen += data.count(prompt) if prompt else 0
    tail = data[-(len(prompt) - 1):] if prompt and len(prompt) > 1 else b''
def pump(seconds, target=None):
    t0 = time.time(); last = time.time(); buf = b''
    while time.time() - t0 < seconds:
        chunk = s.read(4096)
        if chunk:
            last = time.time(); buf += chunk; count_prompts(chunk)
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode('utf-8', 'replace').rstrip('\r')
                if not pats or any(p in text for p in pats): print(text); sys.stdout.flush()
            if target is not None and prompts_seen >= target:
                break
        elif target is not None and time.time() - last > a.settle and (not prompt or prompts_seen >= target):
            break
    if buf.strip():
        text = buf.decode('utf-8', 'replace').rstrip('\r')
        if not pats or any(p in text for p in pats): print(text); sys.stdout.flush()
pump(a.wait)
base = prompts_seen
for i, c in enumerate(a.cmds, 1):
    print(f'>>> {c}'); sys.stdout.flush()
    t_cmd = time.time()
    s.write((c + '\n').encode()); s.flush()
    pump(a.timeout, target=base + i)
    if a.times: print(f'[{c.split()[0]}: {time.time() - t_cmd:.2f} s]'); sys.stdout.flush()
s.close()
