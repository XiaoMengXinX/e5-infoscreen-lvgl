#!/usr/bin/env python3
"""Build the LVGL screen program as a static aarch64 musl binary."""
import hashlib
import json
import os
import re
import subprocess
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / 'out'
# The e5-linux checkout supplies the backend's error translations and the shared
# info screen package; by default it sits next to this repository.
E5_LINUX = Path(os.environ.get('E5_LINUX', HERE.parent / 'e5-linux')).resolve()
COMMON_JS = E5_LINUX / 'openwrt/luci-app-t551m/htdocs/luci-static/resources/mu300/common.js'

SOURCES = {
    'lvgl': ('https://github.com/lvgl/lvgl/archive/refs/tags/v9.6.0.tar.gz', 'lvgl-9.6.0.tar.gz',
             'b20ee3acc1bba13c62d854f9ebd62e4c51e0b443b1e0225892e86442defa84df', 'lvgl-9.6.0'),
    'cjson': ('https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz', 'cJSON-1.7.19.tar.gz',
              '7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562', 'cJSON-1.7.19'),
}

DOCKERFILE = '''FROM alpine:3.22
RUN apk add --no-cache build-base linux-headers cmake samurai pkgconf freetype-dev freetype-static libdrm-dev \\
    zlib-static libpng-static bzip2-static brotli-static libjpeg-turbo-dev libjpeg-turbo-static
'''


def fetch(name):
    url, file, digest, _ = SOURCES[name]
    path = OUT / 'cache' / file
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, path.with_suffix('.part'))
        path.with_suffix('.part').replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError('Checksum mismatch: ' + file)
    return path


def platform_errors():
    """The screen shares the web interface's backend error translations."""
    text = COMMON_JS.read_text(encoding='utf-8')
    block = text[text.index('/* BEGIN SHARED PLATFORM MESSAGES'):text.index('/* END SHARED PLATFORM MESSAGES */')]
    rows = []
    for chunk in re.findall(r'(?:const PLATFORM_ERRORS = |PLATFORM_ERRORS\.push\(\.\.\.)(\[.*?\n\])', block, re.S):
        rows += json.loads(chunk)
    codes = dict(re.findall(r"(\w+):'([^']*)'", block[block.index('const PLATFORM_CODES'):]))
    rows.sort(key=lambda row: -len(row[0]))
    c = lambda s: json.dumps(s, ensure_ascii=False)
    lines = ['static const char *const PLATFORM_ERRORS[][2] = {']
    lines += [f'    {{{c(en)}, {c(zh)}}},' for en, zh, *_ in rows]
    lines += ['};', 'static const char *const PLATFORM_CODES[][2] = {']
    lines += [f'    {{{c(code)}, {c(en)}}},' for code, en in codes.items()]
    lines += ['};', '']
    return '\n'.join(lines)


def embedded_script(name):
    script = (HERE / ('src/' + name + '.py')).read_text(encoding='utf-8')
    return 'static const char ' + name.upper() + '_SCRIPT[] =\n    ' + json.dumps(script) + ';\n'


def version_info():
    """The binary's build timestamp and commit, also published in version.json."""
    commit = os.environ.get('GITHUB_SHA', '').strip()
    if not commit:
        try:
            commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=HERE, check=True,
                                    capture_output=True, text=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            commit = 'unknown'
    epoch = os.environ.get('SOURCE_DATE_EPOCH')
    return (int(epoch) if epoch else int(time.time())), commit


def version_header(stamp, commit):
    repo = os.environ.get('GITHUB_REPOSITORY', 'XiaoMengXinX/e5-infoscreen-lvgl')
    with urllib.request.urlopen('https://api.github.com/repos/' + repo) as response:
        maintainer = json.load(response)['owner']['login']
    return (f'#define BUILD_TIMESTAMP {stamp}LL\n#define BUILD_COMMIT {json.dumps(commit)}\n'
            f'#define BUILD_MAINTAINER {json.dumps(maintainer)}\n')


def image():
    tag = 'e5-infoscreen-lvgl-build:' + hashlib.sha256(DOCKERFILE.encode()).hexdigest()[:12]
    if subprocess.run(['docker', 'image', 'inspect', tag], capture_output=True).returncode:
        subprocess.run(['docker', 'build', '--platform', 'linux/arm64', '-t', tag, '-'],
                       input=DOCKERFILE.encode(), check=True)
    return tag


def build():
    archives = {name: fetch(name) for name in SOURCES}
    stamp, commit = version_info()
    gen = OUT / 'gen'
    gen.mkdir(parents=True, exist_ok=True)
    generated = {
        'platform_errors.h': platform_errors(),
        'modem_restart_script.h': embedded_script('modem_restart'),
        'lpac_bridge_script.h': embedded_script('lpac_bridge'),
        'build_version.h': version_header(stamp, commit),
    }
    for name, content in generated.items():
        header = gen / name
        if not header.exists() or header.read_text(encoding='utf-8') != content:
            header.write_text(content, encoding='utf-8')
    script = ' && '.join([
        'mkdir -p /out/deps /out/build',
        *[f'([ -d /out/deps/{SOURCES[n][3]} ] || tar -xzf /cache/{archives[n].name} -C /out/deps)' for n in SOURCES],
        'cmake -S /src -B /out/build -G Ninja -DCMAKE_BUILD_TYPE=Release'
        f' -DLVGL_DIR=/out/deps/{SOURCES["lvgl"][3]} -DCJSON_DIR=/out/deps/{SOURCES["cjson"][3]} -DGEN_DIR=/out/gen',
        'ninja -C /out/build',
        'cp /out/build/e5-infoscreen-ui /out/e5-infoscreen-ui',
    ])
    subprocess.run(['docker', 'run', '--rm', '--platform', 'linux/arm64',
                    '-v', f'{HERE}:/src:ro', '-v', f'{OUT / "cache"}:/cache:ro', '-v', f'{OUT}:/out',
                    image(), 'sh', '-c', script], check=True)
    binary = OUT / 'e5-infoscreen-ui'
    data = binary.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != 183:
        raise ValueError('Expected an arm64 ELF')
    (OUT / 'version.json').write_text(json.dumps({
        'build': stamp,
        'commit': commit,
        'sha256': hashlib.sha256(data).hexdigest(),
    }, indent=2) + '\n', encoding='utf-8')
    return binary


if __name__ == '__main__':
    print(build())
