#!/usr/bin/python3
"""Stage camera modules matching the running kernel for the UI to install."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

MANIFEST_URL = 'https://github.com/XiaoMengXinX/e5-linux-camera/releases/download/modules/manifest.json'
MODULES = ('sprd_sensor.ko', 'sprd_flash_drv.ko', 'sprd_camera.ko')


def select_build(manifest, profile, release):
    if profile not in ('e5', 'lym10k', 't512n'):
        raise ValueError('Unsupported camera profile: ' + profile)
    if not isinstance(manifest, dict) or type(manifest.get('schema')) is not int or manifest['schema'] != 1:
        raise ValueError('Unsupported camera manifest schema')
    builds = manifest.get('builds')
    if not isinstance(builds, list) or not all(isinstance(build, dict) for build in builds):
        raise ValueError('Invalid camera manifest builds')
    matches = [build for build in builds
               if build.get('profile') == profile and build.get('kernel_release') == release]
    if len(matches) != 1:
        raise ValueError(f'Expected one camera build for {profile}, kernel {release}; found {len(matches)}')
    build = matches[0]
    if not isinstance(build.get('driver_commit'), str) or not re.fullmatch(r'[0-9a-f]{40}', build['driver_commit']):
        raise ValueError('Invalid camera driver commit')
    asset = build.get('asset')
    if not isinstance(asset, str) or not re.fullmatch(r'[A-Za-z0-9_+.-]+\.tar\.gz', asset):
        raise ValueError('Invalid camera archive asset name')
    modules = build.get('modules')
    if not isinstance(modules, dict) or set(modules) != set(MODULES):
        raise ValueError('Camera manifest must describe exactly three modules')
    for name, digest in [('archive', build.get('sha256')), *modules.items()]:
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise ValueError('Invalid SHA256 for ' + name)
    return build


def copy_hash(source, target=None):
    digest = hashlib.sha256()
    while True:
        chunk = source.read(65536)
        if not chunk:
            return digest.hexdigest()
        digest.update(chunk)
        if target is not None:
            target.write(chunk)


def unpack_modules(archive, stage, build):
    with archive.open('rb') as source:
        if copy_hash(source) != build['sha256']:
            raise ValueError('Camera archive checksum mismatch')
    expected = {'modules/' + name for name in MODULES}
    with tarfile.open(archive, 'r:gz') as tar:
        members = {}
        for member in tar:
            if member.name not in expected | {'manifest.json'}:
                raise ValueError('Unexpected camera archive member: ' + member.name)
            if not member.isfile() or member.issparse() or member.name in members:
                raise ValueError('Invalid or duplicate camera archive member: ' + member.name)
            members[member.name] = member
        if not expected <= members.keys():
            raise ValueError('Camera archive is missing required modules')
        for name in MODULES:
            with tar.extractfile(members['modules/' + name]) as source, (stage / name).open('xb') as target:
                if copy_hash(source, target) != build['modules'][name]:
                    raise ValueError('Camera module checksum mismatch: ' + name)


def download(url, destination):
    if shutil.which('curl'):
        command = ['curl', '-fL', '--progress-bar', '--connect-timeout', '10',
                   '--max-time', '300', '-o', str(destination), url]
    elif shutil.which('uclient-fetch'):
        command = ['uclient-fetch', '-T', '10', '-O', str(destination), url]
    else:
        raise RuntimeError('Camera download requires curl or uclient-fetch')
    result = subprocess.run(command, check=False)
    if result.returncode:
        raise RuntimeError(f'Camera download failed: {command[0]} exited {result.returncode}: {url}')


def install(profile, stage):
    if any(stage.iterdir()):
        raise ValueError('Camera staging directory must be empty')
    release = os.uname().release
    manifest = stage / 'manifest.json'
    archive = stage / 'driver.tar.gz'
    try:
        print('@check', flush=True)
        download(MANIFEST_URL, manifest)
        build = select_build(json.loads(manifest.read_text()), profile, release)
        manifest.unlink()
        print('@download', flush=True)
        download(MANIFEST_URL.rsplit('/', 1)[0] + '/' + build['asset'], archive)
        print('@install', flush=True)
        unpack_modules(archive, stage, build)
        archive.unlink()
        (stage / 'build.commit').write_text(build['driver_commit'] + '\n')
        (stage / 'kernel.release').write_text(release + '\n')
    finally:
        manifest.unlink(missing_ok=True)
        archive.unlink(missing_ok=True)


def main(argv):
    try:
        if len(argv) != 2:
            raise ValueError('Usage: camera_driver.py profile stage_dir')
        install(argv[0], Path(argv[1]))
        return 0
    except Exception as error:
        print('@error ' + str(error), flush=True)
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
