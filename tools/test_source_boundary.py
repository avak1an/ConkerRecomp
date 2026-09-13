"""Synthetic tests for the Git source-only inclusion boundary."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest
from tools.release.check import MANIFEST, LAUNCHER, LAUNCHER_INPUTS, validate, git_files


def synthetic_launcher():
    data = bytearray(512)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, 0x8664, 1)
    struct.pack_into('<HH', data, 148, 240, 2)
    struct.pack_into('<H', data, 152, 0x20b)
    struct.pack_into('<H', data, 220, 2)
    return bytes(data)


def manifest_for(files):
    return {'files': [dict(path=p, sha256=hashlib.sha256(data).hexdigest(),
                          reviewed=True, category='CONKER_SPECIFIC')
                      for p, (_, data) in files.items() if p != MANIFEST]}


class SourceBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.files = {p: ('100644', b'synthetic source\n') for p in (
            'LICENSE', 'THIRD_PARTY_NOTICES.md', 'licenses/XboxRecomp-MIT.txt', 'src/tool.c')}
        self.files[MANIFEST] = ('100644', b'{}')

    def test_reviewed_source_passes(self):
        self.assertEqual(validate(self.files, manifest_for(self.files)), [])

    def test_forced_added_game_file_rejected_even_if_manifest_lists_it(self):
        self.files['game/default.xbe'] = ('100644', b'disc payload')
        self.assertTrue(validate(self.files, manifest_for(self.files)))

    def test_generated_c_and_disguised_binary_rejected(self):
        for name, data in [('src/recomp_0000.c', b'generated translation'), ('src/data.c', b'abc\0def')]:
            files = dict(self.files, **{name: ('100644', data)})
            self.assertTrue(validate(files, manifest_for(files)))

    def test_cargo_cli_sources_do_not_allow_arbitrary_bin_contents(self):
        name = 'third_party/dsp56300/crates/asm/src/bin/asm.rs'
        self.files[name] = ('100644', b'fn main() {}')
        self.assertEqual(validate(self.files, manifest_for(self.files)), [])
        for name in ('third_party/dsp56300/crates/asm/src/bin/tool.exe', 'src/bin/unknown.rs'):
            files = dict(self.files, **{name: ('100644', b'not approved')})
            self.assertTrue(validate(files, manifest_for(files)))

    def test_changed_source_rejected(self):
        manifest = manifest_for(self.files)
        self.files['src/tool.c'] = ('100644', b'changed')
        self.assertTrue(validate(self.files, manifest))

    def test_unreviewed_new_file_rejected(self):
        manifest = manifest_for(self.files)
        self.files['src/new.c'] = ('100644', b'new')
        self.assertTrue(validate(self.files, manifest))

    def test_symlink_rejected(self):
        self.files['src/link'] = ('120000', b'../outside')
        self.assertTrue(validate(self.files, manifest_for(self.files)))

    def packaged_fixture(self):
        for name in LAUNCHER_INPUTS:
            self.files[name] = ('100644', b'synthetic source input')
        self.files[LAUNCHER] = ('100644', synthetic_launcher())
        manifest = manifest_for(self.files)
        row = next(r for r in manifest['files'] if r['path'] == LAUNCHER)
        row.update(build_target='conker-launcher', build_configuration='Release',
                   build_inputs={name: hashlib.sha256(self.files[name][1]).hexdigest()
                                 for name in LAUNCHER_INPUTS})
        return manifest

    def test_reviewed_root_launcher_exception(self):
        manifest = self.packaged_fixture()
        self.assertEqual(validate(self.files, manifest), [])

    def test_other_executables_still_rejected(self):
        for name in ('conker_recomp.exe', 'src/conker-launcher.exe', 'build/conker-launcher.exe'):
            files = dict(self.files, **{name: ('100644', synthetic_launcher())})
            self.assertTrue(any('artifact included' in e for e in validate(files, manifest_for(files))))

    def test_launcher_requires_hash_and_source_inventory(self):
        manifest = self.packaged_fixture()
        damaged = bytearray(self.files[LAUNCHER][1])
        damaged[-1] = 1
        self.files[LAUNCHER] = ('100644', bytes(damaged))
        self.assertTrue(any('Source changed' in e for e in validate(self.files, manifest)))
        self.assertTrue(any('build input inventory' in e for e in validate(self.files, manifest_for(self.files))))

    def test_changed_build_input_requires_new_launcher(self):
        manifest = self.packaged_fixture()
        source = LAUNCHER_INPUTS[0]
        self.files[source] = ('100644', b'new reviewed source')
        next(r for r in manifest['files'] if r['path'] == source)['sha256'] = hashlib.sha256(self.files[source][1]).hexdigest()
        self.assertTrue(any('Rebuild packaged launcher' in e for e in validate(self.files, manifest)))

    def test_non_windows_payload_named_as_launcher_rejected(self):
        manifest = self.packaged_fixture()
        self.files[LAUNCHER] = ('100644', b'unrelated data')
        self.assertTrue(any('native Windows executable' in e for e in validate(self.files, manifest)))

    def test_reads_staged_bytes_instead_of_working_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            for name, (_, data) in self.files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            manifest = manifest_for(self.files)
            (root / MANIFEST).write_text(json.dumps(manifest), encoding='utf-8')
            subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
            (root / 'src/tool.c').write_bytes(b'unstaged edit')
            staged = git_files(root)
            self.assertEqual(validate(staged, json.loads(staged[MANIFEST][1])), [])
            subprocess.run(['git', '-C', str(root), 'add', 'src/tool.c'], check=True)
            self.assertTrue(validate(git_files(root), manifest))

    def test_force_added_ignored_payload_is_rejected_in_actual_index(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / '.gitignore').write_text('game/\n')
            (root / 'game').mkdir()
            (root / 'game/default.xbe').write_bytes(b'synthetic payload')
            subprocess.run(['git', '-C', str(root), 'add', '-f', 'game/default.xbe'], check=True)
            staged = git_files(root)
            self.assertTrue(any('artifact included' in e for e in validate(staged, manifest_for(staged))))


if __name__ == '__main__':
    unittest.main()
