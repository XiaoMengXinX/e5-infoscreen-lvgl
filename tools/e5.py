#!/usr/bin/env python3
"""Debugging the screen on a device from this computer, over SSH.

  e5.py shot [FILE]                     screenshot (PNG) of what the screen shows
  e5.py record start|stop|pull [FILE]   the screen's own recording, up to a minute
  e5.py tap X Y                         a tap, in screen pixels (320 x 480)
  e5.py swipe X1 Y1 X2 Y2 [--ms N] [--rate HZ] [--hold MS] [--shot FILE]
  e5.py key NAME[+NAME] [--ms N]        power volup voldown ok up down left right
                                        back menu call hotspot 0-9 star pound
  e5.py wake                            light the screen if it is dark
  e5.py unlock                          light it and swipe the lock away
  e5.py touchrate [--seconds N]         the panel's report rate, while you touch it
  e5.py deploy                          build, install and restart the screen

The device is root@192.168.9.1, or E5_HOST, or --host.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, 'e5input.py')
REMOTE = '/tmp/e5input.py'
BACKLIGHT = 'cat /sys/class/backlight/*/brightness'


class Device:
    def __init__(self, host):
        self.host = host

    def run(self, command, data=None, capture=False):
        r = subprocess.run(['ssh', '-o', 'ConnectTimeout=5', self.host, command], input=data,
                           stdout=subprocess.PIPE if capture else None)
        if r.returncode:
            sys.exit(r.returncode)
        return r.stdout

    def helper(self, *args):
        """Runs e5input.py there, sending it first when it is missing or changed (/tmp
        does not last a reboot)."""
        script = open(HELPER, 'rb').read()
        digest = hashlib.md5(script).hexdigest()
        self.run('[ "$(md5sum %s 2>/dev/null | cut -d" " -f1)" = %s ] || cat > %s' % (REMOTE, digest, REMOTE),
                 data=script)
        self.run(' '.join(['python3', REMOTE] + [str(a) for a in args]))

    def fetch(self, remote, local):
        with open(local, 'wb') as f:
            f.write(self.run('cat %s' % remote, capture=True))
        print(local)


def shot(dev, a):
    dev.helper('shot')
    dev.fetch('/tmp/screen.png', a.file or time.strftime('screen-%Y%m%d-%H%M%S.png'))


def wake(dev, a=None):
    if dev.run(BACKLIGHT, capture=True).strip() == b'0':
        dev.helper('key', 'power')
        time.sleep(1.2)


def unlock(dev, a):
    wake(dev)
    dev.helper('swipe', 160, 440, 160, 150, '--ms', 200)
    time.sleep(1)


def record(dev, a):
    if a.action == 'start':
        wake(dev)
        # Both volume keys held offer a recording; the prompt's first button starts it.
        dev.helper('key', 'volup+voldown', '--ms', 1000)
        time.sleep(0.5)
        dev.helper('key', 'ok')
    elif a.action == 'stop':
        dev.helper('key', 'volup+voldown')
        time.sleep(1)
    else:
        dev.fetch('/tmp/record.mov', a.file or time.strftime('record-%Y%m%d-%H%M%S.mov'))


def swipe(dev, a):
    args = ['swipe', a.x1, a.y1, a.x2, a.y2, '--ms', a.ms, '--rate', a.rate]
    if a.shot:
        args += ['--hold', max(a.hold, 1000), '--shot']
    elif a.hold:
        args += ['--hold', a.hold]
    dev.helper(*args)
    if a.shot:
        dev.fetch('/tmp/screen.png', a.shot)


def deploy(dev, a):
    root = os.path.dirname(HERE)
    subprocess.run([sys.executable, os.path.join(root, 'build.py')], check=True)
    binary = open(os.path.join(root, 'out', 'e5-infoscreen-ui'), 'rb').read()
    dev.run('cat > /tmp/ui.new && chmod 755 /tmp/ui.new && mv /tmp/ui.new /usr/libexec/e5-infoscreen/ui && '
            '/etc/init.d/e5-infoscreen restart', data=binary)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', default=os.environ.get('E5_HOST', 'root@192.168.9.1'))
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('shot').add_argument('file', nargs='?')
    c = sub.add_parser('record')
    c.add_argument('action', choices=['start', 'stop', 'pull'])
    c.add_argument('file', nargs='?')
    c = sub.add_parser('tap')
    c.add_argument('x', type=int)
    c.add_argument('y', type=int)
    c = sub.add_parser('swipe')
    for n in ('x1', 'y1', 'x2', 'y2'):
        c.add_argument(n, type=int)
    c.add_argument('--ms', type=int, default=300)
    c.add_argument('--rate', type=float, default=60)
    c.add_argument('--hold', type=int, default=0)
    c.add_argument('--shot', metavar='FILE', help='a screenshot with the finger still down')
    c = sub.add_parser('key')
    c.add_argument('names')
    c.add_argument('--ms', type=int, default=80)
    sub.add_parser('wake')
    sub.add_parser('unlock')
    sub.add_parser('touchrate').add_argument('--seconds', type=int, default=20)
    sub.add_parser('deploy')
    a = p.parse_args()
    dev = Device(a.host)
    if a.cmd == 'tap':
        dev.helper('tap', a.x, a.y)
    elif a.cmd == 'key':
        dev.helper('key', a.names, '--ms', a.ms)
    elif a.cmd == 'touchrate':
        dev.helper('touchrate', '--seconds', a.seconds)
    else:
        {'shot': shot, 'record': record, 'swipe': swipe, 'wake': wake, 'unlock': unlock, 'deploy': deploy}[a.cmd](dev, a)


if __name__ == '__main__':
    main()
