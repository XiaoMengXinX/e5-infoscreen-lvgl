#!/usr/bin/python3
"""Restart the platform CP and restore its saved SIM state."""
import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

DASH = Path('/usr/libexec/unisoc-modem')
POLICY = Path('/etc/e5/sim-radio')
CARD = Path('/sys/module/sipc_wwan/parameters/card')
MODEM = Path('/dev/modem')
RUN = Path('/run/unisoc-data-sim')
SCREEN = Path('/usr/libexec/e5-infoscreen/ui')
STK = Path('/tmp/e5-stk')
MARKER = Path('/run/unisoc-modem-lock-replay-done')
SMS_LOCK = Path('/tmp/run/e5-sms/modem.lock')
SWITCH_LOCK = RUN / 'switch.lock'
NETWORK_LOCK = Path('/tmp/unisoc-modem/lock-apply.lock')
MODEM_STOP = ord('M') << 8 | 0x0c
MODEM_START = ord('M') << 8 | 0x0d


class ModemRestart:
    def __init__(self):
        self.deadline = time.monotonic() + 210
        self.manager_stopped = False

    @staticmethod
    def stage(name):
        print('@' + name, flush=True)

    def run(self, argv, timeout=10, check=True, **kwargs):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('Modem restart timed out')
        result = subprocess.run(argv, capture_output=True, text=True, timeout=min(timeout, remaining), **kwargs)
        if check and result.returncode:
            raise RuntimeError((result.stderr or result.stdout or 'Command failed: ' + str(argv[0])).strip()[-300:])
        return result

    def wait(self, read, message):
        while time.monotonic() < self.deadline:
            value = read()
            if value:
                return value
            time.sleep(.5)
        raise TimeoutError(message)

    def availability(self, enabled):
        self.run(['ubus', '-t', '3', 'call', 'network.interface.' + self.iface,
                  'notify_proto', json.dumps({'action': 5, 'available': enabled})])

    def wan(self):
        return json.loads(self.run(['ubus', '-t', '3', 'call',
                                    'network.interface.' + self.iface, 'status']).stdout)

    def at(self, slot, command):
        return self.run([str(DASH / 'at'), '-t', '5', f'AT+SPACTCARD={slot};+{command}'],
                        timeout=12, check=False)

    def select_data_sim(self):
        return self.at(self.own, 'CFUN?')

    def present(self, slot):
        reply = self.at(slot, 'CPIN?')
        if re.search(r'\+CPIN:\s*(?:SIM )?NOT INSERTED\s*(?:\r?\n|$)', reply.stdout):
            return False
        if re.search(r'\+CME ERROR:\s*10\s*(?:\r?\n|$)', reply.stdout):
            return False
        if not reply.returncode and '+CPIN:' in reply.stdout:
            return True
        raise RuntimeError(f'Cannot determine whether SIM {slot + 1} is present')

    def reset_cp(self):
        with MODEM.open('r+b', buffering=0) as modem:
            fcntl.ioctl(modem.fileno(), MODEM_STOP)
            fcntl.ioctl(modem.fileno(), MODEM_START)

    def map_sim_channels(self):
        (STK / 'mapped').unlink(missing_ok=True)
        self.run([str(SCREEN), '--map-sim-channels'], timeout=30)

    def modem(self):
        reply = self.run(['mmcli', '-m', 'unisoc-sipc', '--timeout=5', '-K'], check=False)
        if reply.returncode:
            return None
        values = dict(line.split(':', 1) for line in reply.stdout.splitlines() if ':' in line)
        return {key.strip(): value.strip() for key, value in values.items()}

    def restore(self, present):
        for path in (MARKER, Path(str(MARKER) + '-sim0'), Path(str(MARKER) + '-sim1')):
            path.unlink(missing_ok=True)
        for slot in (0, 1):
            if not present[slot]:
                continue
            environment = dict(os.environ, MU300_SIM_SLOT=str(slot))
            self.run([str(DASH / 'lock'), 'replay', 'early'], timeout=60, env=environment)
        MARKER.write_text('verified\n')
        self.select_data_sim()
        self.run(['/etc/init.d/modemmanager', 'start'])
        self.manager_stopped = False

        def initialized():
            values = self.modem()
            if not values:
                return None
            state = values.get('modem.generic.state')
            if not state or state in ('unknown', 'initializing'):
                return None
            if state == 'failed' and (present[self.own] or
                                      values.get('modem.generic.state-failed-reason') != 'sim-missing'):
                return None
            return values

        self.wait(initialized, 'ModemManager did not initialize')
        if any(flag and loaded for flag, loaded in zip(self.flags, present)):
            self.run(['mmcli', '-m', 'unisoc-sipc', '--timeout=30', '--enable'], timeout=35)
        try:
            for slot in (0, 1):
                if not present[slot]:
                    continue
                reply = self.at(slot, 'CFUN?')
                match = re.search(r'\+CFUN:\s*(\d+)', reply.stdout)
                expected = (1,) if self.flags[slot] else (0, 2)
                if reply.returncode or not match or int(match[1]) not in expected:
                    raise RuntimeError(f'SIM {slot + 1} radio state was not restored')
        finally:
            self.select_data_sim()
        return present[self.own] and self.flags[self.own]

    def restart(self):
        locks = []
        network = None
        available = False
        prepared = False
        try:
            for path in (SMS_LOCK, SWITCH_LOCK, NETWORK_LOCK):
                path.parent.mkdir(parents=True, exist_ok=True)
                lock = path.open('a')
                locks.append(lock)
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            network = locks.pop()
            text = POLICY.read_text().strip() if POLICY.exists() else '1 1'
            if text not in ('0 0', '0 1', '1 0', '1 1'):
                raise ValueError('Invalid saved SIM radio policy')
            self.flags = [value == '1' for value in text.split()]
            self.own = int(CARD.read_text().strip())
            if self.own not in (0, 1):
                raise ValueError('Cannot read the data SIM')
            iface = self.run(['uci', '-q', 'get', 'unisoc_modem.main.data_interface'], check=False).stdout.strip()
            self.iface = iface or 'wan'
            saved = self.wan()
            if type(saved.get('autostart')) is not bool:
                raise ValueError('Cannot read the data connection policy')
            if self.run(['pidof', 'lpac'], check=False).returncode == 0:
                raise RuntimeError('An eSIM operation is running')
            available = bool(saved.get('available')) and self.flags[self.own]
            self.stage('preparing')
            try:
                present = [self.present(slot) for slot in (0, 1)]
            finally:
                self.select_data_sim()
            prepared = True
            if saved['autostart'] or saved.get('up') or saved.get('pending'):
                self.stage('data')
                self.run(['ifdown', self.iface])
            self.availability(False)

            def data_stopped():
                state = self.wan()
                return not state.get('up') and not state.get('pending')

            self.wait(data_stopped, 'Data teardown did not finish')
            self.stage('modem')
            self.manager_stopped = True
            self.run(['/etc/init.d/modemmanager', 'stop'])
            self.wait(lambda: self.run(['pidof', 'ModemManager'], check=False).returncode != 0,
                      'ModemManager did not stop')
            available = False
            self.stage('reset')
            self.reset_cp()
            self.run(['/etc/init.d/t551m-at', 'restart'])
            self.wait(lambda: self.run([str(DASH / 'at'), '-t', '3', 'AT'], check=False).returncode == 0,
                      'AT service did not recover')
            self.map_sim_channels()
            network.close()
            network = None
            self.stage('restore')
            available = self.restore(present)
            self.stage('verify')
        finally:
            failure = sys.exc_info()[1]
            self.deadline = max(self.deadline, time.monotonic() + 15)
            cleanup_failure = None
            try:
                if prepared:
                    if self.manager_stopped:
                        subprocess.run(['/etc/init.d/modemmanager', 'start'], timeout=10, check=False)
                    self.availability(available)
                    if saved['autostart']:
                        self.run(['ifup', self.iface])
            except Exception as error:
                cleanup_failure = error
            if network:
                network.close()
            for lock in reversed(locks):
                lock.close()
            if failure is None and cleanup_failure:
                raise cleanup_failure
        self.stage('done')


if __name__ == '__main__':
    try:
        ModemRestart().restart()
    except Exception as error:
        print('@error ' + str(error), flush=True)
        sys.exit(1)
