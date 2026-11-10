#!/usr/bin/env python3
import contextlib
import fcntl
import io
import json
import os
from pathlib import Path
import runpy
import select
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'src/lpac_bridge.py'


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.api = runpy.run_path(str(SOURCE))
        self.g = self.api['main'].__globals__
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.addCleanup(signal.signal, signal.SIGINT, signal.getsignal(signal.SIGINT))

    def system_api(self):
        api = dict(RUN=self.root, CONTROL={'cancelled': False, 'process': None},
                   Owner=Mock(), APDU=Mock(), lpac=Mock(return_value={'eidValue': '123'}))
        def lock(fd, exclusive):
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        api['lock_file'] = lock
        return api

    def test_system_backend_is_preferred_and_preserves_slot_aid_and_result(self):
        api = self.system_api()
        self.g['legacy_run'] = Mock(side_effect=AssertionError('fallback'))
        output = io.StringIO()
        with patch('runpy.run_path', return_value=api), \
             patch.object(sys, 'argv', ['bridge', 'system', '1', 'SE2', 'csim', 'chip', 'info']), contextlib.redirect_stdout(output):
            self.api['main']()
        api['Owner'].assert_called_once_with(1)
        api['APDU'].assert_called_once_with(api['Owner'].return_value, 'csim', 'SE2')
        self.assertEqual(json.loads(output.getvalue())['payload']['data'], {'eidValue': '123'})
        self.g['legacy_run'].assert_not_called()

    def test_shared_system_lock_rejects_parallel_job(self):
        api = self.system_api()
        with (self.root / 'operation.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with patch('runpy.run_path', return_value=api):
                with self.assertRaises(BlockingIOError):
                    self.api['system_run'](0, '', ['chip', 'info'])
        api['Owner'].assert_not_called()
        with (self.root / 'operation.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_system_failure_does_not_repeat_using_local_transport(self):
        api = self.system_api()
        api['lpac'].side_effect = RuntimeError('server_tls')
        self.g['legacy_run'] = Mock(side_effect=AssertionError('fallback'))
        output = io.StringIO()
        with patch('runpy.run_path', return_value=api), \
             patch.object(sys, 'argv', ['bridge', 'system', '0', '', 'csim', 'profile', 'download']), contextlib.redirect_stdout(output):
            with self.assertRaises(SystemExit):
                self.api['main']()
        self.assertEqual(json.loads(output.getvalue())['payload']['message'], 'server_tls')
        self.g['legacy_run'].assert_not_called()

    def test_system_failure_preserves_lpac_message_and_data(self):
        api = self.system_api()
        error = RuntimeError('lpac_failed')
        error.code = 'lpac_failed'
        error.detail = {'code': -1, 'message': 'confirmation_code', 'data': 'required'}
        api['lpac'].side_effect = error
        output = io.StringIO()
        with patch('runpy.run_path', return_value=api), \
             patch.object(sys, 'argv', ['bridge', 'system', '0', '', 'csim', 'profile', 'download']), \
             contextlib.redirect_stdout(output):
            with self.assertRaises(SystemExit):
                self.api['main']()
        payload = json.loads(output.getvalue())['payload']
        self.assertEqual(payload, {'code': -1, 'message': 'confirmation_code', 'data': 'required'})

    def test_system_probe_uses_owner_and_closes_its_channel(self):
        api = self.system_api()
        with patch('runpy.run_path', return_value=api):
            self.api['system_run'](1, '', ['__probe'])
        adapter = api['APDU'].return_value
        adapter.request.assert_called_once_with({'func': 'logic_channel_open', 'param': self.api['ESTK']})
        adapter.cleanup.assert_called_once()
        api['lpac'].assert_not_called()

    def test_system_interface_selection_routes_both_se_aids(self):
        for interface in ('csim', 'logical'):
            for aid in ('A06573746B6D65FFFF4953442D522030', 'A06573746B6D65FFFF4953442D522031'):
                with self.subTest(interface=interface, aid=aid):
                    api = self.system_api()
                    with patch('runpy.run_path', return_value=api):
                        self.api['system_run'](1, aid, ['chip', 'info'], interface)
                    api['APDU'].assert_called_once_with(api['Owner'].return_value, interface, aid)

    def test_system_refresh_uses_saved_slot_owner(self):
        api = self.system_api()
        with patch('runpy.run_path', return_value=api), patch('time.sleep'):
            self.api['system_run'](1, '', ['__refresh'])
        self.assertEqual([call.args[0] for call in api['Owner'].return_value.command.call_args_list],
                         ['+SFUN=5', '+SFUN=4', '+SFUN=2'])

    def test_system_cancel_reaches_current_lpac_process(self):
        api = self.system_api()
        child = Mock()
        api['CONTROL']['process'] = child
        api['lpac'].side_effect = lambda *_: signal.raise_signal(signal.SIGINT)
        with patch('runpy.run_path', return_value=api):
            self.api['system_run'](0, '', ['chip', 'info'])
        self.assertTrue(api['CONTROL']['cancelled'])
        child.send_signal.assert_called_once_with(signal.SIGINT)

    def test_legacy_at_exec_preserves_existing_arguments_and_environment(self):
        inventory = {'payload': {'LPAC_APDU': ['at', 'stdio'], 'LPAC_HTTP': ['curl', 'stdio']}}
        with patch('subprocess.run', return_value=subprocess.CompletedProcess([], 0, json.dumps(inventory))), \
             patch.object(Path, 'is_file', return_value=True), \
             patch('os.execve', side_effect=SystemExit) as execute, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.api['legacy_run'](['profile', 'nickname', '123', '测试名称'])
        self.assertEqual(execute.call_args.args[1], [self.api['LPAC'], 'profile', 'nickname', '123', '测试名称'])

    def test_legacy_curl_requires_gnutls_dependency_before_accessing_card(self):
        inventory = {'payload': {'LPAC_APDU': ['at'], 'LPAC_HTTP': ['curl']}}
        with patch('subprocess.run', return_value=subprocess.CompletedProcess([], 0, json.dumps(inventory))), \
             patch.object(Path, 'is_file', return_value=False), patch('os.execve') as execute:
            with self.assertRaisesRegex(RuntimeError, 'curl_gnutls'):
                self.api['legacy_run'](['chip', 'info'])
        execute.assert_not_called()

    def test_stdio_inventory_selects_supported_http_driver(self):
        for http in (['curl', 'stdio'], ['stdio']):
            with self.subTest(http=http):
                inventory = {'payload': {'LPAC_APDU': ['stdio'], 'LPAC_HTTP': http}}
                self.g['Terminal'] = Mock()
                self.g['stdio_run'] = Mock(return_value={'code': 0})
                with patch('subprocess.run', return_value=subprocess.CompletedProcess([], 0, json.dumps(inventory))), \
                     patch.object(Path, 'is_file', return_value=True), \
                     patch.dict(os.environ, LPAC_APDU_AT_DEVICE='/test/pty'), contextlib.redirect_stdout(io.StringIO()):
                    self.api['legacy_run'](['chip', 'info'])
                self.assertEqual(self.g['stdio_run'].call_args.args[1]['LPAC_APDU'], 'stdio')
                self.assertEqual(self.g['stdio_run'].call_args.args[1]['LPAC_HTTP'], http[0])

    def test_forced_stdio_does_not_choose_available_at_driver(self):
        inventory = {'payload': {'LPAC_APDU': ['at', 'stdio'], 'LPAC_HTTP': ['curl', 'stdio']}}
        self.g['Terminal'] = Mock()
        self.g['stdio_run'] = Mock(return_value={'code': 0})
        with patch('subprocess.run', return_value=subprocess.CompletedProcess([], 0, json.dumps(inventory))), \
             patch.dict(os.environ, LPAC_APDU_AT_DEVICE='/test/pty'), contextlib.redirect_stdout(io.StringIO()):
            self.api['legacy_run'](['chip', 'info'], 'stdio')
        self.assertEqual(self.g['stdio_run'].call_args.args[1]['LPAC_APDU'], 'stdio')
        self.assertEqual(self.g['stdio_run'].call_args.args[1]['LPAC_HTTP'], 'stdio')

    def test_backend_inventory_matches_installed_drivers_and_dependencies(self):
        for drivers, expected in ((['at'], {'system': True, 'at:curl': True, 'stdio': False}),
                                  (['stdio'], {'system': True, 'at:curl': False, 'stdio': True})):
            inventory = {'payload': {'LPAC_APDU': drivers, 'LPAC_HTTP': ['curl', 'stdio']}}
            output = io.StringIO()
            with patch('subprocess.run', return_value=subprocess.CompletedProcess([], 0, json.dumps(inventory))), \
                 patch.object(Path, 'is_file', return_value=True), patch('os.access', return_value=True), \
                 patch.dict(self.g, stdio_supported=Mock(return_value=True)), \
                 patch.object(sys, 'argv', ['bridge', 'system', '0', '', 'csim', '__backends']), \
                 contextlib.redirect_stdout(output):
                self.api['main']()
            self.assertEqual(json.loads(output.getvalue())['payload']['data'], expected)

    def test_stdio_probe_rejects_driver_that_fails_on_successful_connect(self):
        for function, expected in (('connect', False), ('logic_channel_open', True)):
            output = json.dumps({'type': 'apdu', 'payload': {'func': function}}) + '\n'
            with patch('subprocess.run', return_value=subprocess.CompletedProcess([], 1, output)) as run:
                self.assertEqual(self.api['stdio_supported']('stdio'), expected)
            sent = json.loads(run.call_args.kwargs['input'])
            self.assertEqual(sent, {'type': 'apdu', 'payload': {'ecode': 0}})
            self.assertEqual(run.call_args.kwargs['env']['LPAC_APDU'], 'stdio')

    def test_missing_drivers_fail_before_opening_card(self):
        inventory = {'payload': {'LPAC_APDU': ['pcsc'], 'LPAC_HTTP': ['curl']}}
        self.g['Terminal'] = Mock()
        with patch('subprocess.run', return_value=subprocess.CompletedProcess([], 0, json.dumps(inventory))):
            with self.assertRaisesRegex(RuntimeError, 'lpac_apdu_driver'):
                self.api['legacy_run'](['chip', 'info'])
        self.g['Terminal'].assert_not_called()

    def test_cached_transport_skips_inventory(self):
        self.g['Terminal'] = Mock()
        self.g['stdio_run'] = Mock(return_value={'code': 0})
        with patch('subprocess.run') as inventory, patch.dict(os.environ, LPAC_APDU_AT_DEVICE='/test/pty'):
            self.api['legacy_run'](['chip', 'info'], 'stdio:stdio')
            self.api['legacy_run'](['profile', 'list'], 'stdio:stdio')
        inventory.assert_not_called()
        self.assertEqual(self.g['stdio_run'].call_count, 2)

    def test_stdio_without_python_tls_requests_only_that_dependency(self):
        self.g['Terminal'] = Mock()
        with patch.dict(sys.modules, ssl=None):
            with self.assertRaisesRegex(RuntimeError, 'python_ssl'):
                self.api['legacy_run'](['chip', 'info'], 'stdio:stdio')
        self.g['Terminal'].assert_not_called()

    def test_stdio_http_uses_https_verification_and_preserves_payload(self):
        import http.client
        import ssl
        connection = Mock()
        connection.getresponse.return_value.status = 200
        connection.getresponse.return_value.read.return_value = b'{"ok":true}'
        with patch.object(http.client, 'HTTPSConnection', return_value=connection) as create, \
             patch.object(ssl, 'create_default_context') as context, patch.object(Path, 'is_file', return_value=False):
            reply = self.api['http_request']({'url': 'https://example.com/rsp?test=1',
                                              'tx': '7B7D', 'headers': ['Content-Type:application/json']})
        self.assertEqual(create.call_args.kwargs['context'], context.return_value)
        connection.request.assert_called_once_with('POST', '/rsp?test=1', b'{}', {'Content-Type': 'application/json'})
        connection.close.assert_called_once()
        self.assertEqual(reply, {'rcode': 200, 'rx': b'{"ok":true}'.hex().upper()})

    def test_apdu_translates_requests_and_closes_only_owned_channel(self):
        terminal = Mock()
        terminal.command.side_effect = ['OK\r\n', '+CCHO: 2\r\nOK\r\n', '+CGLA: 4,"9000"\r\nOK\r\n', 'OK\r\n']
        adapter = self.api['APDU'](terminal)
        self.assertEqual(adapter.request({'func': 'connect'}), {'ecode': 0})
        self.assertEqual(adapter.request({'func': 'logic_channel_open', 'param': 'ABCD'}), {'ecode': 2})
        self.assertEqual(adapter.request({'func': 'transmit', 'param': '02CA000000'}), {'ecode': 0, 'data': '9000'})
        adapter.request({'func': 'logic_channel_close', 'param': '02'})
        adapter.request({'func': 'disconnect'})
        self.assertEqual([call.args[0] for call in terminal.command.call_args_list],
                         ['', '+CCHO="ABCD"', '+CGLA=2,10,"02CA000000"', '+CCHC=2'])

    def fake_lpac(self):
        fake = self.root / 'lpac'
        fake.write_text('#!' + sys.executable + '\n' + '''
import json, sys
def send(kind, payload):
    print(json.dumps({'type':kind,'payload':payload}), flush=True)
def apdu(func, param=None):
    send('apdu', {'func':func,'param':param})
    return json.loads(sys.stdin.readline())['payload']
assert apdu('connect')['ecode'] == 0
assert apdu('logic_channel_open', 'ABCD')['ecode'] == 1
send('progress', {'message':'es8p_metadata_parse', 'data':{'profileName':'测试'}})
send('progress', {'message':'preview'})
accept = sys.stdin.read(1) == 'y'
assert apdu('transmit', '01CA000000')['data'] == '9000'
apdu('logic_channel_close', '01')
send('lpa', {'code':0 if accept else -1,'data':'x'*50000})
apdu('disconnect')
''')
        fake.chmod(0o755)
        return fake

    def test_stdio_preview_single_byte_keeps_following_apdu_json_valid(self):
        fake = self.fake_lpac()
        self.g['LPAC'] = str(fake)
        terminal = Mock()
        for accept in (b'y', b'n'):
            terminal.fd = os.open(os.devnull, os.O_RDWR)
            terminal.command.side_effect = ['OK\r\n', '+CCHO: 1\r\nOK\r\n', '+CGLA: 4,"9000"\r\nOK\r\n', 'OK\r\n']
            output = io.StringIO()
            with patch.object(sys, 'stdin', io.TextIOWrapper(io.BytesIO(accept))), \
                 contextlib.redirect_stdout(output):
                result = self.api['stdio_run'](['profile', 'download'], dict(os.environ), terminal)
            self.assertEqual(result['code'], 0 if accept == b'y' else -1)
            self.assertEqual(len(result['data']), 50000)
            self.assertIn('测试', json.loads(output.getvalue().splitlines()[0])['payload']['data']['profileName'])

    def test_stdio_nonzero_exit_preserves_structured_failure(self):
        fake = self.root / 'lpac'
        fake.write_text('#!' + sys.executable + '\n' + '''
import json, sys
print(json.dumps({'type':'lpa','payload':{'code':-1,'message':'confirmation_code','data':'required'}}), flush=True)
sys.exit(1)
''')
        fake.chmod(0o755)
        self.g['LPAC'] = str(fake)
        terminal = Mock(fd=os.open(os.devnull, os.O_RDWR))
        result = self.api['stdio_run'](['profile', 'download'], dict(os.environ), terminal)
        self.assertEqual(result, {'code': -1, 'message': 'confirmation_code', 'data': 'required'})

    def test_stdio_cancel_finishes_child_process(self):
        fake = self.root / 'lpac'
        fake.write_text('#!' + sys.executable + '\n' + '''
import json, signal, sys
def cancel(*_):
    print(json.dumps({'type':'lpa','payload':{'code':-1,'message':'cancelled'}}), flush=True)
    sys.exit(0)
signal.signal(signal.SIGINT, cancel)
print(json.dumps({'type':'progress','payload':{'message':'working'}}), flush=True)
while True:
    signal.pause()
''')
        fake.chmod(0o755)
        harness = '''
import os, runpy, sys
api = runpy.run_path(sys.argv[1])
api['stdio_run'].__globals__['LPAC'] = sys.argv[2]
terminal = type('Terminal', (), {'fd':os.open(os.devnull, os.O_RDWR)})()
result = api['stdio_run'](['profile','download'], dict(os.environ), terminal)
api['emit']('lpa', **result)
'''
        child = subprocess.Popen([sys.executable, '-c', harness, str(SOURCE), str(fake)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertTrue(select.select([child.stdout], [], [], 5)[0])
            self.assertEqual(json.loads(child.stdout.readline())['type'], 'progress')
            child.send_signal(signal.SIGINT)
            output, error = child.communicate(timeout=5)
            self.assertEqual(child.returncode, 0, error.decode())
            self.assertEqual(json.loads(output)['payload']['message'], 'cancelled')
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate()

    def test_real_system_backend_protocol_with_simulated_card(self):
        backend = Path(os.environ.get('E5_LINUX', ROOT.parent / 'e5-linux')) / \
            'openwrt/luci-app-t551m/root/usr/libexec/unisoc-modem/euicc'
        api = runpy.run_path(str(backend))
        api['lpac'].__globals__['LPAC'] = str(self.fake_lpac())
        api['RUN'] = self.root
        owner = Mock()
        owner.command.side_effect = ['OK\r\n', '+CSIM: 6,"019000"\r\nOK\r\n',
                                     '+CSIM: 4,"9000"\r\nOK\r\n', '+CSIM: 4,"9000"\r\nOK\r\n',
                                     '+CSIM: 4,"9000"\r\nOK\r\n']
        api['Owner'] = Mock(return_value=owner)
        # The simulated child selects ABCD; use it as this fixture's ISD-R.
        api['APDU'].__init__.__globals__['ISDR'] = 'ABCD'
        self.g['ISDR'] = 'ABCD'
        output = io.StringIO()
        read_script = runpy.run_path
        with patch('runpy.run_path', side_effect=lambda path: api if path == str(self.g['BACKEND']) else read_script(path)), \
             patch.object(sys, 'stdin', io.TextIOWrapper(io.BytesIO(b'y'))), contextlib.redirect_stdout(output):
            result = self.api['system_run'](1, '', ['profile', 'download', '-p', '-a', 'LPA:1$example.com$test'])
        self.assertEqual(len(result), 50000)
        commands = [call.args[0] for call in owner.command.call_args_list]
        self.assertEqual(commands[0], '+CSIM=?')
        self.assertEqual(commands[-1], '+CSIM=10,"0070800100"')
        with (self.root / 'operation.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)


if __name__ == '__main__':
    unittest.main()
