#!/usr/bin/env python3
import hashlib
import io
import json
from pathlib import Path
import runpy
import tarfile
import tempfile
import unittest
from unittest.mock import Mock, patch

SCRIPT = Path(__file__).resolve().parents[1] / 'src/camera_driver.py'
driver = runpy.run_path(str(SCRIPT))


def sha(data):
    return hashlib.sha256(data).hexdigest()


class DriverTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.stage = Path(temporary.name)
        self.payloads = {name: (name + ' contents').encode() for name in driver['MODULES']}
        self.config = 'CONFIG_Z=y\nCONFIG_A="hello"\n'
        self.build = dict(profile='lym10k', kernel_release='6.12-test',
                          asset='sprd-camera-lym10k-6.12-test-arm64.tar.gz', sha256='0' * 64,
                          config_sha256=sha(b'CONFIG_A="hello"\nCONFIG_Z=y\n'),
                          modules={name: sha(data) for name, data in self.payloads.items()})

    def select(self, manifest=None, **changes):
        build = dict(self.build, **changes)
        return driver['select_build'](manifest or {'schema': 1, 'builds': [build]},
                                      'lym10k', '6.12-test', self.build['config_sha256'])

    def archive(self, entries=None):
        path = self.stage / 'driver.tar.gz'
        if entries is None:
            entries = [('modules/' + name, data, tarfile.REGTYPE) for name, data in self.payloads.items()]
            entries.append(('manifest.json', b'{}', tarfile.REGTYPE))
        with tarfile.open(path, 'w:gz') as tar:
            for name, data, kind in entries:
                member = tarfile.TarInfo(name)
                member.type = kind
                member.size = len(data) if kind == tarfile.REGTYPE else 0
                member.linkname = '../outside'
                tar.addfile(member, io.BytesIO(data) if member.size else None)
        self.build['sha256'] = sha(path.read_bytes())
        return path

    def test_normalized_config_matches_publisher(self):
        noise = '# CONFIG_DISABLED is not set\nCONFIG_BAD-NAME=y\n CONFIG_SPACE=y\n'
        noise += ''.join(name + '=123\n' for name in driver['TOOLCHAIN_METADATA'])
        self.assertEqual(driver['config_sha256'](noise + self.config), self.build['config_sha256'])
        self.assertNotEqual(driver['config_sha256'](self.config + 'CONFIG_ZZ=m\n'), self.build['config_sha256'])

    def test_exact_metadata_match(self):
        self.assertEqual(self.select(), self.build)
        other = dict(self.build, profile='t512n')
        manifest = {'schema': 1, 'builds': [other, self.build]}
        self.assertEqual(self.select(manifest), self.build)
        self.assertEqual(driver['select_build'](manifest, 't512n', '6.12-test', other['config_sha256']), other)
        for field, value in [('profile', 'LYM10K'), ('kernel_release', '6.12'),
                             ('config_sha256', '1' * 64), ('asset', '../bad.tar.gz'),
                             ('asset', 'https://other/driver.tar.gz'), ('sha256', 'oops'),
                             ('modules', {}), ('modules', dict(self.build['modules'], extra='0' * 64))]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.select(**{field: value})

    def test_e5_profile_selects_its_own_build(self):
        e5 = dict(self.build, profile='e5', kernel_release='6.18.54-e5-mobile4',
                  asset='sprd-camera-e5-6.18.54-e5-mobile4-arm64.tar.gz')
        manifest = {'schema': 1, 'builds': [self.build, e5]}
        self.assertEqual(driver['select_build'](manifest, 'e5', e5['kernel_release'],
                                                e5['config_sha256']), e5)

    def test_manifest_schema_shape_and_ambiguity(self):
        for manifest in ({'schema': 2, 'builds': []}, {'schema': True, 'builds': []},
                         {'schema': 1, 'builds': {}}, {'schema': 1, 'builds': [None]},
                         {'schema': 1, 'builds': []}, {'schema': 1, 'builds': [self.build] * 2}):
            with self.subTest(manifest=manifest), self.assertRaises(ValueError):
                self.select(manifest)
        with self.assertRaisesRegex(ValueError, 'Unsupported camera profile'):
            driver['select_build']({}, 'lym10k-extra', '', '')

    def test_archive_hash_precedes_extraction(self):
        path = self.archive()
        self.build['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'archive checksum mismatch'):
            driver['unpack_modules'](path, self.stage, self.build)
        self.assertEqual(list(self.stage.iterdir()), [path])

    def test_extracts_only_verified_modules(self):
        path = self.archive()
        driver['unpack_modules'](path, self.stage, self.build)
        self.assertEqual({p.name for p in self.stage.iterdir()}, {'driver.tar.gz', *self.payloads})
        for name, data in self.payloads.items():
            self.assertEqual((self.stage / name).read_bytes(), data)

    def test_module_checksum_mismatch(self):
        path = self.archive()
        self.build['modules']['sprd_sensor.ko'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'module checksum mismatch: sprd_sensor.ko'):
            driver['unpack_modules'](path, self.stage, self.build)

    def test_tar_rejects_missing_duplicate_paths_and_nonregular_members(self):
        entries = [('modules/' + name, data, tarfile.REGTYPE) for name, data in self.payloads.items()]
        cases = [entries[:-1], entries + [entries[0]], entries + [('../outside', b'x', tarfile.REGTYPE)],
                 entries + [('/absolute', b'x', tarfile.REGTYPE)]]
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.DIRTYPE, tarfile.FIFOTYPE):
            cases.append([(entries[0][0], b'', kind)] + entries[1:])
        for case in cases:
            with self.subTest(case=case):
                path = self.archive(case)
                with self.assertRaises(ValueError):
                    driver['unpack_modules'](path, self.stage, self.build)
                self.assertEqual(list(self.stage.iterdir()), [path])

    def run_install(self, failure=None, module_mismatch=False):
        path = self.archive()
        archive = path.read_bytes()
        path.unlink()
        if module_mismatch:
            self.build['modules']['sprd_camera.ko'] = '0' * 64
        manifest = json.dumps({'schema': 1, 'builds': [self.build]}).encode()
        calls = []

        def download(url, destination):
            calls.append((url, destination.name))
            destination.write_bytes(manifest if destination.name == 'manifest.json' else archive)
            if failure == destination.name:
                raise RuntimeError('download interrupted')

        def read_config(*args):
            self.assertEqual(args, ('/proc/config.gz', 'rt'))
            return io.StringIO(self.config)

        with patch.dict(driver['install'].__globals__, download=download), \
                patch.object(driver['gzip'], 'open', side_effect=read_config), \
                patch.object(driver['os'], 'uname', return_value=Mock(release='6.12-test')):
            if module_mismatch:
                with self.assertRaisesRegex(ValueError, 'module checksum mismatch: sprd_camera.ko'):
                    driver['install']('lym10k', self.stage)
            elif failure:
                with self.assertRaisesRegex(RuntimeError, 'download interrupted'):
                    driver['install']('lym10k', self.stage)
            else:
                driver['install']('lym10k', self.stage)
        return calls

    def test_install_leaves_exact_c_handoff_files(self):
        calls = self.run_install()
        self.assertEqual(calls, [(driver['MANIFEST_URL'], 'manifest.json'),
                                (driver['MANIFEST_URL'].rsplit('/', 1)[0] + '/' + self.build['asset'], 'driver.tar.gz')])
        self.assertEqual({p.name for p in self.stage.iterdir()}, {*self.payloads, 'kernel.release'})
        self.assertEqual((self.stage / 'kernel.release').read_bytes(), b'6.12-test\n')

    def test_failed_downloads_remove_temporary_files_without_ready_marker(self):
        for name in ('manifest.json', 'driver.tar.gz'):
            with self.subTest(name=name):
                self.run_install(name)
                self.assertEqual(list(self.stage.iterdir()), [])

    def test_failed_module_verification_leaves_only_c_cleanup_files(self):
        self.run_install(module_mismatch=True)
        self.assertEqual({p.name for p in self.stage.iterdir()}, set(self.payloads))

    def test_invalid_archive_fails_before_writing_modules(self):
        path = self.stage / 'driver.tar.gz'
        path.write_bytes(b'not a tar archive')
        self.build['sha256'] = sha(path.read_bytes())
        with self.assertRaises(tarfile.ReadError):
            driver['unpack_modules'](path, self.stage, self.build)
        self.assertEqual(list(self.stage.iterdir()), [path])

    def test_downloader_selection_and_exit_status(self):
        for tool in ('curl', 'uclient-fetch'):
            with self.subTest(tool=tool), patch.object(driver['shutil'], 'which', side_effect=lambda name: name == tool), \
                    patch.object(driver['subprocess'], 'run', return_value=Mock(returncode=0)) as run:
                driver['download']('https://example/test', self.stage / 'manifest.json')
                command = run.call_args.args[0]
                self.assertEqual(command[0], tool)
                self.assertIn(str(self.stage / 'manifest.json'), command)
                self.assertEqual(command[-1], 'https://example/test')
                run.return_value.returncode = 22
                with self.assertRaisesRegex(RuntimeError, 'exited 22'):
                    driver['download']('https://example/test', self.stage / 'manifest.json')
        with patch.object(driver['shutil'], 'which', return_value=None), self.assertRaisesRegex(RuntimeError, 'requires'):
            driver['download']('https://example/test', self.stage / 'manifest.json')

    def test_main_prints_concrete_error_and_returns_failure(self):
        with patch.dict(driver['main'].__globals__,
                        install=Mock(side_effect=ValueError('Camera archive checksum mismatch'))), \
                patch('sys.stdout', new_callable=io.StringIO) as output:
            self.assertEqual(driver['main'](['lym10k', str(self.stage)]), 1)
            self.assertEqual(output.getvalue(), '@error Camera archive checksum mismatch\n')


if __name__ == '__main__':
    unittest.main()
