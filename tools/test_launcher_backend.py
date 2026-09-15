"""Synthetic launcher checks: no ISO or game-derived fixtures required."""
from pathlib import Path
import configparser
import hashlib
import json
import tempfile
import ctypes
import os
import threading
import time
import unittest
import subprocess
from unittest.mock import patch
from tools.launcher.backend import fingerprint, verify_image, build_existing, write_result, main, prepare_image


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'source with spaces & accents é'
        (self.root / 'config/conker').mkdir(parents=True)
        self.image = self.root / 'synthetic.iso'
        self.image.write_bytes(b'synthetic test data only\0' * 1024)
        self.versions = self.root / 'config/conker/versions.json'
        self.versions.write_text('{"supported_versions": []}', encoding='utf-8')

    def test_empty_allowlist_does_not_verify_or_extract(self):
        result = verify_image(self.image, self.root)
        self.assertEqual(result['state'], 'unsupported')
        self.assertEqual(result['image_sha1'], hashlib.sha1(self.image.read_bytes()).hexdigest())
        self.assertFalse((self.root / 'game').exists())

    def test_exact_size_and_sha1_required(self):
        identity = fingerprint(self.image)
        self.versions.write_text(json.dumps({'supported_versions': [
            dict(id='synthetic', **identity)]}), encoding='utf-8')
        self.assertEqual(verify_image(self.image, self.root)['state'], 'matched')
        self.image.write_bytes(b'other')
        self.assertEqual(verify_image(self.image, self.root)['state'], 'unsupported')

    def test_development_identity_is_recognized_without_certifying_game(self):
        identity = fingerprint(self.image)
        version = dict(id='synthetic-development', **identity)
        self.versions.write_text(json.dumps(dict(supported_versions=[], development_versions=[version])))
        result = verify_image(self.image, self.root)
        self.assertEqual(result['state'], 'development')
        self.assertNotIn('game_ready', result)
        version['image_sha256'] = 'wrong'
        self.versions.write_text(json.dumps(dict(supported_versions=[], development_versions=[version])))
        self.assertEqual(verify_image(self.image, self.root)['state'], 'unsupported')

    def test_duplicate_release_development_identity_is_rejected(self):
        version = dict(id='synthetic', **fingerprint(self.image))
        self.versions.write_text(json.dumps(dict(supported_versions=[version], development_versions=[version])))
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            verify_image(self.image, self.root)

    def test_import_reverifies_before_creating_outputs(self):
        (self.root / 'config/conker/generation.json').write_text('{"version": 1}')
        with self.assertRaisesRegex(ValueError, 'not uniquely recognized'):
            prepare_image(self.image, self.root)
        self.assertFalse((self.root / 'local').exists())

    def test_import_generates_locally_and_reports_not_playable(self):
        from tools.test_prepare import disc_bytes
        data = disc_bytes()
        self.image.write_bytes(data)
        version = dict(id='synthetic-development', **fingerprint(self.image), xbe_size=8,
            xbe_sha1=hashlib.sha1(b'SYNTH123').hexdigest(),
            xbe_sha256=hashlib.sha256(b'SYNTH123').hexdigest(), generation_recipe_version=1)
        self.versions.write_text(json.dumps(dict(supported_versions=[], development_versions=[version])))
        (self.root / 'config/conker/generation.json').write_text('{"version": 1}')
        with patch('tools.prepare.__main__.generate', return_value={'failed': 0, 'translated': 1}) as generator:
            result = prepare_image(self.image, self.root)
        self.assertEqual(result['state'], 'prepared')
        self.assertEqual(result['game_ready'], 'false')
        target = Path(result['detail'])
        # TEMP can use an 8.3 alias on Windows; preparation resolves it.
        self.assertTrue(target.is_relative_to((self.root / 'local/preparation').resolve()))
        self.assertEqual((Path(result['data_dir']) / 'default.xbe').read_bytes(), b'SYNTH123')
        self.assertFalse(json.loads((target / 'preparation.json').read_text())['game_ready'])
        generator.assert_called_once()
        # A changed image must not reuse a previous verification result.
        self.image.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'not uniquely recognized'):
            prepare_image(self.image, self.root)
        self.assertEqual(len(list((self.root / 'local/preparation').iterdir())), 1)

    @unittest.skipUnless(os.name == 'nt', 'Windows file-sharing regression')
    def test_progress_recovers_from_a_temporarily_locked_snapshot(self):
        result = self.root / 'status.ini'
        write_result(result, dict(state='working', message='10%'))
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
            ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        # Mimic a reader which prevents rename until it releases its handle.
        handle = kernel.CreateFileW(str(result), 0x80000000, 3, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        release = threading.Timer(0.15, lambda: kernel.CloseHandle(handle))
        release.start()
        try:
            write_result(result, dict(state='development', message='done'))
        finally:
            release.join()
        parser = configparser.ConfigParser(interpolation=None)
        parser.read(result, encoding='utf-16')
        self.assertEqual(parser['task']['state'], 'development')

    def test_changing_input_is_rejected(self):
        def changed(message):
            with self.image.open('ab') as stream: stream.write(b'changed')
        # Only change once; an endlessly growing file is not a finite fixture.
        def once(message):
            if not getattr(once, 'called', False):
                once.called = True
                changed(message)
        with self.assertRaisesRegex(ValueError, 'changed during preparation'):
            fingerprint(self.image, once)

    def synthetic_preparation(self):
        prepared = self.root / 'local/preparation/test'
        data = prepared / 'game'
        source = prepared / 'generated/recomp'
        data.mkdir(parents=True); source.mkdir(parents=True)
        (data / 'default.xbe').write_bytes(b'synthetic executable')
        entries = []
        for name in ('recomp_dispatch.c', 'recomp_funcs.h'):
            content = b'/* authored test fixture */'
            (source / name).write_bytes(content)
            entries.append(dict(path=name, size=len(content), sha256=hashlib.sha256(content).hexdigest()))
        identity = fingerprint(self.image)
        version = dict(id='synthetic', **identity,
                       xbe_sha256=hashlib.sha256((data / 'default.xbe').read_bytes()).hexdigest())
        self.versions.write_text(json.dumps(dict(supported_versions=[], development_versions=[version])))
        (self.root / 'config/conker/generation.json').write_text('{"version": 2}')
        report = dict(stage='analysis-generated', recipe_version=2, version_id='synthetic', input=identity,
                      generated=dict(failed=0, files=entries),
                      recipe_sha256=hashlib.sha256((self.root / 'config/conker/generation.json').read_bytes()).hexdigest())
        (prepared / 'preparation.json').write_text(json.dumps(report))
        return data, source

    def test_missing_preparation_does_not_start_a_build(self):
        with patch('tools.launcher.backend.find_cmake') as cmake:
            with self.assertRaisesRegex(ValueError, 'Import and generate'):
                build_existing(self.root)
            cmake.assert_not_called()

    def test_build_uses_argv_and_checks_startup(self):
        data, source = self.synthetic_preparation()
        calls = []
        def run(argv, **kwargs):
            calls.append((argv, kwargs))
            exe = self.root / 'build/Debug/conker_recomp.exe'
            exe.parent.mkdir(exist_ok=True); exe.touch()
        with patch('tools.launcher.backend.find_cmake', return_value='cmake.exe'):
            result = build_existing(self.root, run=run, data_dir=data)
        self.assertEqual(result['state'], 'complete')
        self.assertEqual(len(calls), 3)
        # The backend passes canonical paths, including expanded Windows aliases.
        self.assertIn('-DCONKER_GENERATED_DIR=' + str(source.resolve()), calls[0][0])
        self.assertIn('--check-startup', calls[-1][0])
        self.assertIn(str(data.resolve()), calls[-1][0])
        for argv, kwargs in calls:
            self.assertNotIn('shell', kwargs)
            self.assertTrue(kwargs['check'])
        self.assertTrue((self.root / 'build/runtime-ready.ini').is_file())

    def test_modified_or_extra_sources_are_rejected_before_build(self):
        data, source = self.synthetic_preparation()
        original = (source / 'recomp_dispatch.c').read_bytes()
        def run(*args, **kwargs): self.fail('Compiler must not run')
        (source / 'recomp_dispatch.c').write_bytes(original.replace(b'authored', b'MODIFIED'))
        with self.assertRaisesRegex(ValueError, 'source changed'):
            build_existing(self.root, run=run, data_dir=data)
        (source / 'recomp_dispatch.c').write_bytes(original)
        (source / 'recomp_extra.c').write_text('/* unexpected fixture */')
        with self.assertRaisesRegex(ValueError, 'unexpected or missing'):
            build_existing(self.root, run=run, data_dir=data)

    def test_failed_compile_invalidates_old_ready_receipt(self):
        data, source = self.synthetic_preparation()
        ready = self.root / 'build/runtime-ready.ini'
        ready.parent.mkdir(); ready.write_bytes(b'old successful build')
        def fail(argv, **kwargs): raise subprocess.CalledProcessError(1, argv)
        with patch('tools.launcher.backend.find_cmake', return_value='cmake.exe'):
            with self.assertRaises(subprocess.CalledProcessError):
                build_existing(self.root, run=fail, data_dir=data)
        self.assertFalse(ready.exists())

    def test_unicode_atomic_ini_and_error_reporting(self):
        result = self.root / 'local/task.ini'
        write_result(result, {'message': 'é & % complete\nnext', 'state': 'working'})
        p = configparser.ConfigParser(interpolation=None); p.read(result, encoding='utf-16')
        self.assertEqual(p['task']['message'], 'é & % complete next')
        code = main(['verify', '--root', str(self.root), '--image', str(self.root / 'missing.iso'),
                     '--result', str(result)])
        self.assertEqual(code, 1)
        p.read(result, encoding='utf-16'); self.assertEqual(p['task']['state'], 'error')


if __name__ == '__main__':
    unittest.main()
