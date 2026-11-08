#!/usr/bin/env python3
"""Runs on the device: injects touches and keys into its input devices, and
measures the touch panel's report rate. tools/e5.py uploads and calls it.

  e5input.py shot
  e5input.py tap X Y [--ms 80]
  e5input.py swipe X1 Y1 X2 Y2 [--ms 300] [--rate 60] [--hold MS] [--shot]
  e5input.py key NAME[+NAME...] [--ms 80]
  e5input.py touchrate [--seconds 20]
"""
import argparse
import os
import select
import struct
import subprocess
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39
BTN_TOUCH = 0x14a

# Each name lists the codes the screen accepts for it, in the order to try.
KEYS = {
    'power': [116], 'volup': [115], 'voldown': [114], 'ok': [352, 28, 353], 'up': [103], 'down': [108],
    'left': [105], 'right': [106], 'back': [158, 1, 14], 'menu': [139], 'call': [169], 'hotspot': [59],
    'star': [522, 55], 'pound': [523], '0': [11, 512],
    **{str(n): [1 + n, 512 + n] for n in range(1, 10)},
}


def devices():
    """(path, name, key bits, abs bits) for each event device."""
    found, block = [], {}
    with open('/proc/bus/input/devices') as f:
        for line in f.read().split('\n') + ['']:
            if not line.strip():
                if 'handlers' in block:
                    event = next((h for h in block['handlers'].split() if h.startswith('event')), None)
                    if event:
                        found.append(('/dev/input/' + event, block.get('name', ''), block.get('KEY', 0), block.get('ABS', 0)))
                block = {}
            elif line.startswith('N: Name='):
                block['name'] = line[8:].strip('"')
            elif line.startswith('H: Handlers='):
                block['handlers'] = line[12:]
            elif line.startswith('B: KEY=') or line.startswith('B: ABS='):
                words = line[7:].split()
                block[line[3:6]] = sum(int(w, 16) << (64 * i) for i, w in enumerate(reversed(words)))
    return found


def touch_device():
    for path, name, keys, absbits in devices():
        if absbits >> ABS_MT_POSITION_X & 1:
            return path
    sys.exit('no touch panel found')


def key_device(code):
    for path, name, keys, absbits in devices():
        if keys >> code & 1 and not absbits >> ABS_MT_POSITION_X & 1:
            return path
    return None


def emit(fd, kind, code, value):
    now = time.time()
    os.write(fd, struct.pack('qqHHi', int(now), int(now % 1 * 1e6), kind, code, value))


class Touch:
    """A one-finger touch in screen pixels: the panel's range is the screen's."""

    def __init__(self):
        self.fd = os.open(touch_device(), os.O_RDWR)
        self.down = False

    def move(self, x, y):
        emit(self.fd, EV_ABS, ABS_MT_SLOT, 0)
        if not self.down:
            emit(self.fd, EV_ABS, ABS_MT_TRACKING_ID, 100)
            emit(self.fd, EV_KEY, BTN_TOUCH, 1)
            self.down = True
        for mt, single, v in ((ABS_MT_POSITION_X, ABS_X, x), (ABS_MT_POSITION_Y, ABS_Y, y)):
            emit(self.fd, EV_ABS, mt, int(v))
            emit(self.fd, EV_ABS, single, int(v))
        emit(self.fd, EV_SYN, 0, 0)

    def up(self):
        emit(self.fd, EV_ABS, ABS_MT_SLOT, 0)
        emit(self.fd, EV_ABS, ABS_MT_TRACKING_ID, -1)
        emit(self.fd, EV_KEY, BTN_TOUCH, 0)
        emit(self.fd, EV_SYN, 0, 0)
        self.down = False


SCREEN = '/tmp/screen.png'


def screenshot():
    """The screen writes SCREEN on SIGUSR1, from its next pass of the main loop."""
    pid = subprocess.run(['pidof', 'ui'], capture_output=True, text=True).stdout.split()
    if not pid:
        sys.exit('the screen is not running')
    before = os.stat(SCREEN).st_mtime_ns if os.path.exists(SCREEN) else 0
    os.kill(int(pid[0]), 10)
    for _ in range(50):
        time.sleep(0.1)
        if os.path.exists(SCREEN) and os.stat(SCREEN).st_mtime_ns != before:
            # Written in place: give the writer a moment to finish.
            time.sleep(0.2)
            return
    sys.exit('no screenshot')


def tap(a):
    t = Touch()
    t.move(a.x, a.y)
    time.sleep(a.ms / 1000)
    t.up()


def swipe(a):
    t = Touch()
    steps = max(1, round(a.ms / 1000 * a.rate))
    for i in range(steps + 1):
        t.move(a.x1 + (a.x2 - a.x1) * i / steps, a.y1 + (a.y2 - a.y1) * i / steps)
        time.sleep(a.ms / 1000 / steps)
    if a.hold:
        start = time.time()
        if a.shot:
            screenshot()
        time.sleep(max(0, a.hold / 1000 - (time.time() - start)))
    t.up()


def key(a):
    held = []
    for name in a.names.split('+'):
        codes = KEYS.get(name.lower())
        if not codes:
            sys.exit('unknown key %s; one of %s' % (name, ' '.join(KEYS)))
        target = next(((key_device(c), c) for c in codes if key_device(c)), None)
        if not target:
            sys.exit('no input device has the %s key' % name)
        fd = os.open(target[0], os.O_RDWR)
        emit(fd, EV_KEY, target[1], 1)
        emit(fd, EV_SYN, 0, 0)
        held.append((fd, target[1]))
    time.sleep(a.ms / 1000)
    for fd, code in reversed(held):
        emit(fd, EV_KEY, code, 0)
        emit(fd, EV_SYN, 0, 0)


def touchrate(a):
    """Intervals between the panel's reports while a finger is down."""
    fd = os.open(touch_device(), os.O_RDONLY)
    print('touch the screen for %d s...' % a.seconds, flush=True)
    end, strokes, stroke, down = time.time() + a.seconds, [], [], False
    while time.time() < end:
        if not select.select([fd], [], [], 0.5)[0]:
            continue
        data = os.read(fd, 24 * 64)
        for i in range(0, len(data), 24):
            s, us, kind, code, value = struct.unpack('qqHHi', data[i:i + 24])
            if kind == EV_KEY and code == BTN_TOUCH or kind == EV_ABS and code == ABS_MT_TRACKING_ID:
                down = value >= (1 if kind == EV_KEY else 0)
            elif kind == EV_SYN and code == 0:
                if down:
                    stroke.append(s + us / 1e6)
                elif stroke:
                    strokes.append(stroke)
                    stroke = []
    if stroke:
        strokes.append(stroke)
    gaps = sorted((b - a) * 1000 for st in strokes for a, b in zip(st, st[1:]))
    if not gaps:
        sys.exit('no touches seen')
    median = gaps[len(gaps) // 2]
    print('%d strokes, %d reports: median %.1f ms (%.0f Hz), p10 %.1f, p90 %.1f, max %.1f ms' % (
        len(strokes), len(gaps), median, 1000 / median, gaps[len(gaps) // 10], gaps[len(gaps) * 9 // 10], gaps[-1]))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('shot')
    c = sub.add_parser('tap')
    c.add_argument('x', type=float)
    c.add_argument('y', type=float)
    c.add_argument('--ms', type=int, default=80)
    c = sub.add_parser('swipe')
    for n in ('x1', 'y1', 'x2', 'y2'):
        c.add_argument(n, type=float)
    c.add_argument('--ms', type=int, default=300, help='time the finger moves')
    c.add_argument('--rate', type=float, default=60, help='reports per second (the panel gives about 57)')
    c.add_argument('--hold', type=int, default=0, help='ms the finger rests at the end before lifting')
    c.add_argument('--shot', action='store_true', help='take a screenshot while the finger rests')
    c = sub.add_parser('key')
    c.add_argument('names', help='one key, or several held together: volup+voldown')
    c.add_argument('--ms', type=int, default=80, help='how long the keys are held')
    c = sub.add_parser('touchrate')
    c.add_argument('--seconds', type=int, default=20)
    a = p.parse_args()
    {'shot': lambda a: screenshot(), 'tap': tap, 'swipe': swipe, 'key': key, 'touchrate': touchrate}[a.cmd](a)


if __name__ == '__main__':
    main()
