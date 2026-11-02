#!/usr/bin/env python3
"""Non-interactive serial monitor for the StopWatch (USB-Serial/JTAG console).

Opening the port with DTR/RTS asserted holds the chip in the bootloader handshake, so we open,
drop both lines, optionally pulse a reset, and then just print for --seconds.
  python3 tools/dev/monitor.py [--port /dev/cu.usbmodem83401] [--seconds 30] [--reset] [--grep heartbeat,app:]
"""
import argparse, sys, time
import serial

ap = argparse.ArgumentParser()
ap.add_argument('--port', default='/dev/cu.usbmodem83401')
ap.add_argument('--seconds', type=float, default=30)
ap.add_argument('--reset', action='store_true', help='pulse EN via DTR/RTS before listening')
ap.add_argument('--grep', default='', help='comma-separated substrings; print only matching lines')
ap.add_argument('--out', default='', help='also append raw output to this file')
a = ap.parse_args()

s = serial.Serial()
s.port = a.port; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
if a.reset:
    # esptool "classic" reset: RTS low→high pulse with DTR low means EN toggles.
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
pats = [p for p in a.grep.split(',') if p]
t0 = time.time(); buf = b''; out = open(a.out, 'ab') if a.out else None
try:
    while time.time() - t0 < a.seconds:
        chunk = s.read(4096)
        if not chunk: continue
        if out: out.write(chunk); out.flush()
        buf += chunk
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            text = line.decode('utf-8', 'replace').rstrip('\r')
            if not pats or any(p in text for p in pats):
                print(text); sys.stdout.flush()
finally:
    s.close()
