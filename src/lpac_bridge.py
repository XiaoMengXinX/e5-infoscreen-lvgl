"""Adapt the installed eUICC backend or lpac drivers to the screen's event stream."""
import contextlib
import json
import os
from pathlib import Path
import queue
import re
import runpy
import select
import signal
import subprocess
import sys
import threading
import time

BACKEND = Path('/usr/libexec/unisoc-modem/euicc')
LPAC = '/usr/lib/lpac'
ESTK = 'A06573746B6D65FFFFFFFFFFFF6D6774'
ISDR = 'A0000005591010FFFFFFFF8900000100'


def emit(kind, **payload):
    print(json.dumps({'type': kind, 'payload': payload}), flush=True)


def progress(stage, data=None):
    if stage == 'metadata':
        stage = 'es8p_metadata_parse'
    elif stage == 'writing':
        stage = 'es10b_load_bound_profile_package'
    emit('progress', message=stage, data=data)
    if stage == 'preview':
        return sys.stdin.buffer.read(1) == b'y'


def system_run(slot, aid, args, interface='csim'):
    api = runpy.run_path(str(BACKEND))
    run = api['RUN']
    run.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (run / 'operation.lock').open('a') as lock:
        api['lock_file'](lock.fileno(), True)
        def cancel(*_):
            api['CONTROL']['cancelled'] = True
            child = api['CONTROL']['process']
            if child is not None:
                child.send_signal(signal.SIGINT)
        signal.signal(signal.SIGINT, cancel)
        owner = api['Owner'](slot)
        if args == ['__refresh']:
            for index, command in enumerate(('+SFUN=5', '+SFUN=4', '+SFUN=2')):
                if index:
                    time.sleep(1)
                owner.command(command)
            return None
        adapter = api['APDU'](owner, interface, ESTK if args == ['__probe'] else aid or ISDR)
        if args == ['__probe']:
            try:
                adapter.request({'func': 'logic_channel_open', 'param': ESTK})
                return None
            finally:
                adapter.cleanup()
        return api['lpac'](args, adapter, progress)


class Terminal:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)

    def command(self, body):
        os.write(self.fd, ('AT' + body + '\r').encode())
        reply = b''
        until = time.monotonic() + 35
        while time.monotonic() < until:
            if not select.select([self.fd], [], [], max(0, until - time.monotonic()))[0]:
                break
            chunk = os.read(self.fd, 4096)
            if not chunk:
                break
            reply += chunk
            text = reply.decode()
            if re.search(r'(?:^|[\r\n])(?:OK|ERROR|\+CME ERROR:[^\r\n]*)[\r\n]', text):
                if not re.search(r'(?:^|[\r\n])OK[\r\n]', text):
                    raise RuntimeError('at_rejected')
                return text
        raise RuntimeError('at_timeout')


class APDU:
    def __init__(self, terminal):
        self.terminal = terminal
        self.channel = None

    def close(self):
        if self.channel is not None:
            self.terminal.command(f'+CCHC={self.channel}')
            self.channel = None

    def request(self, request):
        function, param = request['func'], request.get('param')
        if function == 'connect':
            self.terminal.command('')
        elif function == 'disconnect':
            self.close()
        elif function == 'logic_channel_open':
            reply = self.terminal.command(f'+CCHO="{param}"')
            match = re.search(r'\+CCHO:\s*(\d+)', reply)
            if not match:
                raise RuntimeError('apdu_channel')
            self.channel = int(match[1])
            return {'ecode': self.channel}
        elif function == 'logic_channel_close':
            if int(param, 16) != self.channel:
                raise RuntimeError('apdu_channel')
            self.close()
        elif function == 'transmit':
            if self.channel is None:
                raise RuntimeError('apdu_channel')
            reply = self.terminal.command(f'+CGLA={self.channel},{len(param)},"{param}"')
            match = re.search(r'\+CGLA:\s*(\d+),\s*"?([0-9A-Fa-f]+)"?', reply)
            if not match or int(match[1]) != len(match[2]):
                raise RuntimeError('apdu_reply')
            return {'ecode': 0, 'data': match[2]}
        else:
            raise RuntimeError('apdu_request')
        return {'ecode': 0}


def http_request(request):
    import http.client
    import ssl
    from urllib.parse import urlsplit
    url = urlsplit(request['url'])
    if url.scheme != 'https' or not url.hostname or url.username or url.password:
        raise RuntimeError('server_address')
    headers = dict(header.split(':', 1) for header in request.get('headers', []))
    context = ssl.create_default_context()
    ca = Path('/usr/share/unisoc-euicc/ca/gsma-rsp2-root-ci1.pem')
    if ca.is_file():
        context.load_verify_locations(cafile=str(ca))
    connection = http.client.HTTPSConnection(url.hostname, url.port, timeout=40, context=context)
    try:
        connection.request('POST', url.path + ('?' + url.query if url.query else ''),
                           bytes.fromhex(request['tx']), headers)
        reply = connection.getresponse()
        return {'rcode': reply.status, 'rx': reply.read().hex().upper()}
    finally:
        connection.close()


def stdio_run(args, env, terminal):
    adapter = APDU(terminal)
    child = subprocess.Popen([LPAC, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, env=env)
    signal.signal(signal.SIGINT, lambda *_: child.send_signal(signal.SIGINT))
    events = queue.Queue()
    def read_output():
        try:
            for line in child.stdout:
                events.put(line)
        finally:
            events.put(None)
    threading.Thread(target=read_output, daemon=True).start()
    final = None
    try:
        while True:
            line = events.get(timeout=600)
            if line is None:
                break
            item = json.loads(line)
            kind, payload = item['type'], item['payload']
            if kind in ('apdu', 'http'):
                answer = adapter.request(payload) if kind == 'apdu' else http_request(payload)
                child.stdin.write((json.dumps({'type': kind, 'payload': answer}) + '\n').encode())
                child.stdin.flush()
            elif kind == 'progress':
                accept = progress(payload.get('message', ''), payload.get('data'))
                if payload.get('message') == 'preview':
                    child.stdin.write(b'y' if accept else b'n')
                    child.stdin.flush()
            elif kind == 'lpa':
                final = payload
        status = child.wait(timeout=3)
        if final is None or (status and final.get('code') == 0):
            raise RuntimeError('lpac_failed')
        return final
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        with contextlib.suppress(OSError, RuntimeError):
            adapter.close()
        child.stdin.close()
        child.stdout.close()
        os.close(terminal.fd)


def driver_inventory():
    inventory = subprocess.run([LPAC, 'driver', 'list'], capture_output=True, text=True, timeout=10)
    if inventory.returncode:
        raise RuntimeError('lpac_drivers')
    return json.loads(inventory.stdout)['payload']


def stdio_supported(http):
    reply = json.dumps({'type': 'apdu', 'payload': {'ecode': 0}}) + '\n'
    result = subprocess.run([LPAC, 'chip', 'info'], input=reply, capture_output=True, text=True,
                            timeout=5, env=dict(os.environ, LPAC_APDU='stdio', LPAC_HTTP=http))
    # A working stdio driver advances from connect to opening the application.
    return any(json.loads(line).get('payload', {}).get('func') == 'logic_channel_open'
               for line in result.stdout.splitlines() if line.strip())


def select_transport(stdio=False):
    drivers = driver_inventory()
    order = ('stdio', 'curl') if stdio else ('curl', 'stdio')
    http = next((name for name in order if name in drivers['LPAC_HTTP']), None)
    if http is None:
        raise RuntimeError('lpac_http_driver')
    apdu = 'at' if not stdio and http == 'curl' and 'at' in drivers['LPAC_APDU'] else 'stdio'
    if apdu not in drivers['LPAC_APDU']:
        raise RuntimeError('lpac_apdu_driver')
    if http == 'curl' and not Path('/usr/lib/libcurl-gnutls.so.4').is_file():
        raise RuntimeError('curl_gnutls')
    return apdu + ':' + http


def legacy_run(args, mode='auto'):
    if mode in ('auto', 'stdio'):
        mode = select_transport(mode == 'stdio')
        emit('transport', mode=mode)
    apdu, http = mode.split(':')
    env = dict(os.environ)
    env.update(LPAC_APDU=apdu, LPAC_HTTP=http)
    if http == 'stdio':
        try:
            import ssl
        except ImportError:
            raise RuntimeError('python_ssl') from None
    if apdu == 'at':
        os.execve(LPAC, [LPAC, *args], env)
    return stdio_run(args, env, Terminal(env['LPAC_APDU_AT_DEVICE']))


def main():
    mode, slot, aid, interface, *args = sys.argv[1:]
    try:
        if args == ['__backends']:
            drivers = driver_inventory()
            apdu, http = drivers['LPAC_APDU'], drivers['LPAC_HTTP']
            curl = 'curl' in http and Path('/usr/lib/libcurl-gnutls.so.4').is_file()
            try:
                import ssl
                tls = True
            except ImportError:
                tls = False
            stdio = 'stdio' in apdu and (tls if 'stdio' in http else curl)
            if stdio:
                stdio = stdio_supported('stdio' if 'stdio' in http else 'curl')
            result = {'code': 0, 'data': {'system': os.access(BACKEND, os.X_OK),
                      'at:curl': 'at' in apdu and curl,
                      'stdio': stdio}}
        elif mode == 'system':
            data = system_run(int(slot), aid, args, interface)
            result = {'code': 0, 'data': data}
        else:
            result = legacy_run(args, mode)
        emit('lpa', **result)
    except BlockingIOError:
        emit('lpa', code=-1, message='operation_busy')
    except Exception as error:
        code = getattr(error, 'code', None)
        if code is None:
            code = str(error) if isinstance(error, RuntimeError) else 'esim_backend'
        detail = getattr(error, 'detail', None)
        message = detail.get('message') if isinstance(detail, dict) else None
        data = detail.get('data') if isinstance(detail, dict) else None
        if data is not None and not isinstance(data, str):
            data = json.dumps(data, ensure_ascii=False)
        emit('lpa', code=-1, message=message or code, data=data)
        sys.exit(1)


if __name__ == '__main__':
    main()
