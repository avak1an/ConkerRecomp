"""Synthetic disc corruption/containment tests; no game fixtures."""
from pathlib import Path
import hashlib
import json
import struct
import tempfile
import unittest
from tools.prepare.disc import Disc, MAGIC, SECTOR, safe_name
from tools.prepare.__main__ import match_version, output_target, prepare


def disc_bytes(name='default.xbe', left=0, right=0, sector=40, length=8, extra=b''):
    data = bytearray(42 * SECTOR)
    data[32 * SECTOR:32 * SECTOR + len(MAGIC)] = MAGIC
    struct.pack_into('<II', data, 32 * SECTOR + 0x14, 34, SECTOR)
    encoded = name.encode('ascii')
    struct.pack_into('<HHIIBB', data, 34 * SECTOR, left, right, sector, length, 0, len(encoded))
    data[34 * SECTOR + 14:34 * SECTOR + 14 + len(encoded)] = encoded
    data[40 * SECTOR:40 * SECTOR + 8] = b'SYNTH123'
    if extra:
        data[34 * SECTOR + 32:34 * SECTOR + 32 + len(extra)] = extra
    return data


class PrepareTests(unittest.TestCase):
    def parse(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.iso'
            path.write_bytes(data)
            with Disc(path) as disc:
                return disc.entries()

    def test_valid_directory(self):
        entries = self.parse(disc_bytes())
        self.assertEqual([(e.path, e.size) for e in entries], [('default.xbe', 8)])

    def test_path_traversal_windows_aliases(self):
        for name in ('../escape', 'CON', 'aux.txt', 'a:b', 'name.', 'name ', 'x\\y', 'x/y'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                safe_name(name)

    def test_cycle_and_bad_node(self):
        for data in (disc_bytes(left=1), disc_bytes(right=65000)):
            with self.assertRaises(ValueError):
                self.parse(data)

    def test_out_of_bounds(self):
        for data in (disc_bytes(sector=1000), disc_bytes(length=1000000), disc_bytes()[:100]):
            with self.assertRaises(ValueError):
                self.parse(data)

    def test_duplicate_case_insensitive(self):
        name = b'DEFAULT.XBE'
        extra = struct.pack('<HHIIBB', 0, 0, 40, 8, 0, len(name)) + name
        with self.assertRaises(ValueError):
            self.parse(disc_bytes(right=8, extra=extra))

    def test_version_match_requires_all_hashes(self):
        fp = dict(image_size=8, image_sha1='a', image_sha256='b')
        versions = dict(supported_versions=[], development_versions=[fp])
        with self.assertRaises(ValueError):
            match_version(fp, versions)
        self.assertEqual(match_version(fp, versions, True), fp)
        for key in fp:
            bad = dict(fp, **{key: 'wrong'})
            with self.assertRaises(ValueError):
                match_version(bad, versions, True)

    def test_output_containment_and_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for name in ('src/oops', '../outside', 'local'):
                with self.assertRaises(ValueError):
                    output_target(root, name)
            target = output_target(root, 'local/run')
            target.mkdir(parents=True)
            with self.assertRaises(ValueError):
                output_target(root, 'local/run')

    def test_unknown_iso_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'config/conker').mkdir(parents=True)
            (root / 'config/conker/versions.json').write_text(json.dumps(dict(supported_versions=[])))
            (root / 'config/conker/generation.json').write_text('{}')
            image = root / 'test.iso'
            image.write_bytes(disc_bytes())
            with self.assertRaises(ValueError):
                prepare(image, 'local/run', root, development=True, generate_code=False)
            self.assertFalse((root / 'local').exists())

    def test_extract_verified_file_and_complete_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'config/conker').mkdir(parents=True)
            data = disc_bytes()
            version = dict(id='synthetic', image_size=len(data), image_sha1=hashlib.sha1(data).hexdigest(),
                image_sha256=hashlib.sha256(data).hexdigest(), xbe_size=8,
                xbe_sha1=hashlib.sha1(b'SYNTH123').hexdigest(),
                xbe_sha256=hashlib.sha256(b'SYNTH123').hexdigest(), generation_recipe_version=1)
            (root / 'config/conker/versions.json').write_text(json.dumps(dict(supported_versions=[], development_versions=[version])))
            (root / 'config/conker/generation.json').write_text('{"version": 1}')
            image = root / 'test.iso'
            image.write_bytes(data)
            result = prepare(image, 'local/run', root, development=True, generate_code=False)
            self.assertFalse(result['game_ready'])
            self.assertEqual((root / 'local/run/game/default.xbe').read_bytes(), b'SYNTH123')
            self.assertEqual(result['extracted_files'][0]['sha256'], version['xbe_sha256'])


    def test_entry_hooks_are_repeatable_and_validate_before_writing(self):
        from tools.prepare.hooks import apply_entry_hooks
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            p = root / 'recomp_0000.c'
            original = '#include "recomp_funcs.h"\nvoid sub_00010000(void)\n{\n    return;\n}\n'
            p.write_text(original)
            first = dict(address='00010000', handler='conker_input_get_devices')
            missing = dict(address='00020000', handler='conker_input_get_state')
            with self.assertRaisesRegex(ValueError, 'Expected one'):
                apply_entry_hooks(root, [first, missing])
            self.assertEqual(p.read_text(), original)
            apply_entry_hooks(root, [first])
            self.assertIn('if (conker_input_get_devices()) return;', p.read_text())
            once = p.read_bytes()
            apply_entry_hooks(root, [first])
            self.assertEqual(p.read_bytes(), once)
            with self.assertRaisesRegex(ValueError, 'Invalid'):
                apply_entry_hooks(root, [dict(address='00010000', handler='wrong();')])


if __name__ == '__main__':
    unittest.main()
