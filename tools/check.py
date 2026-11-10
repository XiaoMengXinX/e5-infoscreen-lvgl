#!/usr/bin/env python3
"""Run regressions using the screen's build environment."""
import runpy
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
build = runpy.run_path(str(ROOT / 'build.py'))
if not (ROOT / 'out/build/liblvgl.a').is_file():
    raise SystemExit('Run python3 build.py first')
subprocess.run([sys.executable, str(ROOT / 'tests/modem_restart.py')], check=True)
subprocess.run([sys.executable, str(ROOT / 'tests/lpac_bridge.py')], check=True)
lvgl = '/out/deps/' + build['SOURCES']['lvgl'][3]
cjson = '/out/deps/' + build['SOURCES']['cjson'][3]
flags = ['cc', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-DLV_CONF_INCLUDE_SIMPLE', '-I/out/gen', '-I/src', '-I/src/src', '-I' + lvgl, '-I' + cjson,
         '-I/usr/include/libdrm', '-ffunction-sections', '-fdata-sections']
commands = ['mkdir -p /out/tests']
for name, sources in [('volkeys', ['/src/tests/volkeys.c']), ('drm', ['/src/tests/drm.c']), ('motion', ['/src/tests/motion.c', '/src/src/ui.c']),
                      ('lpac', ['/src/tests/lpac.c']), ('esim_text', ['/src/tests/esim_text.c']),
                      ('esim_flow', ['/src/tests/esim_flow.c', '/src/src/ui.c', '/src/src/dialog.c']),
                      ('install', ['/src/tests/install.c', '/src/src/ui.c', '/src/src/dialog.c'])]:
    command = flags + sources
    if name == 'motion':
        command += ['/out/build/liblvgl.a']
    elif name in ('lpac', 'esim_text', 'esim_flow', 'install'):
        command += ['/src/src/util.c', '/out/build/libcjson.a', '/out/build/liblvgl.a',
                    '-lfreetype', '-lbz2', '-lpng16', '-lz', '-lbrotlidec', '-lbrotlicommon']
    command += ['-Wl,--gc-sections', '-static', '-lm', '-lpthread', '-o', '/out/tests/' + name]
    commands += [shlex.join(command), '/out/tests/' + name]
subprocess.run(['docker', 'run', '--rm', '--platform', 'linux/arm64',
                '-v', f'{ROOT}:/src:ro', '-v', f'{ROOT / "out"}:/out',
                build['image'](), 'sh', '-ec', '\n'.join(commands)], check=True)
