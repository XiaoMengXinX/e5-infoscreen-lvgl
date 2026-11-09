#!/usr/bin/env python3
"""Run rendering and motion regressions using the screen's build environment."""
import runpy
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
build = runpy.run_path(str(ROOT / 'build.py'))
if not (ROOT / 'out/build/liblvgl.a').is_file():
    raise SystemExit('Run python3 build.py first')
lvgl = '/out/deps/' + build['SOURCES']['lvgl'][3]
cjson = '/out/deps/' + build['SOURCES']['cjson'][3]
flags = ['cc', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-DLV_CONF_INCLUDE_SIMPLE', '-I/src', '-I/src/src', '-I' + lvgl, '-I' + cjson,
         '-I/usr/include/libdrm', '-ffunction-sections', '-fdata-sections']
commands = ['mkdir -p /out/tests']
for name, sources in [('drm', ['/src/tests/drm.c']), ('motion', ['/src/tests/motion.c', '/src/src/ui.c'])]:
    command = flags + sources
    if name == 'motion':
        command += ['/out/build/liblvgl.a']
    command += ['-Wl,--gc-sections', '-static', '-lm', '-lpthread', '-o', '/out/tests/' + name]
    commands += [shlex.join(command), '/out/tests/' + name]
subprocess.run(['docker', 'run', '--rm', '--platform', 'linux/arm64',
                '-v', f'{ROOT}:/src:ro', '-v', f'{ROOT / "out"}:/out',
                build['image'](), 'sh', '-ec', '\n'.join(commands)], check=True)
