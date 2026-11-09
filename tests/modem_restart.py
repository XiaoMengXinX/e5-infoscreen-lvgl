#!/usr/bin/env python3
import fcntl
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'src/modem_restart.py'


class RestartTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.scope = runpy.run_path(str(SCRIPT))
        self.restart = self.scope['ModemRestart']()
        self.restart.restart.__globals__.update(
            POLICY=root / 'sim-radio', CARD=root / 'card', RUN=root / 'run',
            SCREEN=root / 'ui', STK=root / 'stk', MARKER=root / 'replay',
            SMS_LOCK=root / 'sms.lock', SWITCH_LOCK=root / 'switch.lock',
            NETWORK_LOCK=root / 'network.lock',
        )
        self.root = root
        self.restart.restart.__globals__['POLICY'].write_text('1 1\n')
        self.restart.restart.__globals__['CARD'].write_text('0\n')
        self.restart.restart.__globals__['RUN'].mkdir()
        self.restart.restart.__globals__['STK'].mkdir()
        self.restart.restart.__globals__['SCREEN'].touch()
        self.present = [True, True]
        self.absent_reply = 'cme'
        self.radios = [1, 1]
        self.wan = {'available': True, 'up': False, 'pending': False, 'autostart': False}
        self.fail = None
        self.calls = []
        self.restart.stage = lambda name: None
        self.restart.reset_cp = self.reset_cp
        self.restart.run = self.run_command

    def reset_cp(self):
        self.calls.append(['reset'])
        if self.fail == 'reset':
            raise TimeoutError('reset failed')

    def run_command(self, argv, **kwargs):
        argv = [str(value) for value in argv]
        self.calls.append(argv)
        code, text = 0, ''
        if argv[0] == 'uci':
            text = 'wan\n'
        elif argv[0] == 'pidof':
            code = 0 if argv[1] == 'e5-modemd' else 1
        elif argv[0] == 'ubus':
            if argv[-1] == 'status':
                text = json.dumps(self.wan)
            else:
                self.wan['available'] = json.loads(argv[-1])['available']
        elif argv[0] == 'ifdown':
            self.wan['up'] = self.wan['pending'] = False
        elif argv[0] == 'ifup':
            self.wan['autostart'] = True
        elif argv[0].endswith('/at'):
            if argv[-1] == 'AT':
                text = 'OK\n'
            else:
                slot = int(argv[-1].split('SPACTCARD=')[1][0])
                if argv[-1].endswith('CPIN?'):
                    if self.present[slot]:
                        text, code = '+CPIN: READY\nOK\n', 0
                    elif self.absent_reply == 'not-inserted':
                        text, code = '+CPIN: NOT INSERTED\n', 1
                    else:
                        text, code = '+CME ERROR: 10\n', 1
                else:
                    text = f'+CFUN: {self.radios[slot]}\nOK\n'
        elif argv[0].endswith('/lock'):
            text = '{"ok":true}'
        elif argv[0] == 'mmcli' and argv[-1] == '-K':
            text = 'modem.generic.state : registered\n'
        if self.fail == argv[0]:
            raise RuntimeError('simulated failure')
        return subprocess.CompletedProcess(argv, code, text, '')

    def test_restart_restores_mapping_locks_radios_and_data_policy(self):
        self.wan.update(up=True, autostart=True)
        self.restart.restart()
        mapping = self.calls.index([str(self.root / 'ui'), '--map-sim-channels'])
        replay = next(i for i, call in enumerate(self.calls) if call[0].endswith('/lock'))
        self.assertLess(mapping, replay)
        self.assertEqual(len([call for call in self.calls if call[0].endswith('/lock')]), 2)
        self.assertEqual(self.calls.count(['ifup', 'wan']), 1)
        self.assertTrue(self.wan['autostart'])
        self.assertTrue(self.wan['available'])
        for path in ('sms.lock', 'switch.lock', 'network.lock'):
            with (self.root / path).open('a') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_manual_data_off_stays_off(self):
        self.restart.restart()
        self.assertNotIn(['ifup', 'wan'], self.calls)
        self.assertFalse(self.wan['autostart'])
        self.assertTrue(self.wan['available'])

    def test_secondary_only_sim_restarts_for_both_absent_replies(self):
        for absent_reply in ('cme', 'not-inserted'):
            with self.subTest(absent_reply=absent_reply):
                self.present[:] = [False, True]
                self.absent_reply = absent_reply
                self.calls.clear()
                self.wan.update(available=True, up=False, pending=False, autostart=False)
                self.restart.restart()
                self.assertEqual(len([call for call in self.calls if call[0].endswith('/lock')]), 1)
                self.assertIn(['mmcli', '-m', 'unisoc-sipc', '--timeout=30', '--enable'], self.calls)
                self.assertFalse(self.wan['available'])

    def test_stale_running_job_does_not_block_once_switch_lock_is_owned(self):
        (self.root / 'run' / 'stale.json').write_text('{"state":"running"}\n')
        self.restart.restart()
        self.assertIn(['reset'], self.calls)

    def test_failed_reset_restarts_manager_and_keeps_wan_unavailable(self):
        self.fail = 'reset'
        self.wan.update(up=True, autostart=True)
        with patch.object(self.scope['subprocess'], 'run') as cleanup:
            with self.assertRaisesRegex(TimeoutError, 'reset failed'):
                self.restart.restart()
        cleanup.assert_called_once_with(['/etc/init.d/modemmanager', 'start'], timeout=10, check=False)
        self.assertIn(['ifup', 'wan'], self.calls)
        self.assertFalse(self.wan['available'])

    def test_busy_lock_changes_nothing(self):
        with (self.root / 'network.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                self.restart.restart()
        self.assertNotIn(['reset'], self.calls)
        self.assertTrue(self.wan['available'])

    def test_unknown_sim_presence_changes_nothing(self):
        def run_command(argv, **kwargs):
            if str(argv[0]).endswith('/at') and str(argv[-1]).endswith('CPIN?'):
                return subprocess.CompletedProcess(argv, 1, '+CME ERROR: 100\n', '')
            return self.run_command(argv, **kwargs)

        self.restart.run = run_command
        with self.assertRaisesRegex(RuntimeError, 'Cannot determine'):
            self.restart.restart()
        self.assertNotIn(['reset'], self.calls)
        self.assertTrue(self.wan['available'])


if __name__ == '__main__':
    unittest.main()
