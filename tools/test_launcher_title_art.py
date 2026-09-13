"""Exercise the native ISO/XBE artwork reader using synthetic bytes only."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / 'build/launcher/Release/conker-title-art-test.exe'
SECTOR = 2048


def synthetic_xbe(transparent=False):
    data = bytearray(0x428)
    data[:4] = b'XBEH'
    def put(offset, value): struct.pack_into('<I', data, offset, value)
    put(0x104, 0x10000); put(0x108, 0x400)
    put(0x118, 0x10180); put(0x188, 0x4d530051)
    put(0x11c, 1); put(0x120, 0x10200)
    put(0x20c, 0x400); put(0x210, 40); put(0x214, 0x10250)
    data[0x250:0x25a] = b'$$XTIMAGE\0'
    data[0x400:0x404] = b'XPR0'
    put(0x404, 40); put(0x408, 32); put(0x40c, 0x40001)
    put(0x418, 0x02210c20)  # 4x4, two-dimensional BC1
    struct.pack_into('<HHI', data, 0x420, *( (0, 1, 0xffffffff) if transparent else (0xf800, 0x07e0, 0) ))
    return data


def synthetic_iso(xbe=None, base=0, duplicate=False):
    xbe = synthetic_xbe() if xbe is None else xbe
    data = bytearray(base + 36 * SECTOR)
    descriptor = base + 32 * SECTOR
    data[descriptor:descriptor+20] = b'MICROSOFT*XBOX*MEDIA'
    struct.pack_into('<II', data, descriptor + 0x14, 33, SECTOR)
    entry = struct.pack('<HHIIBB', 0, 0, 34, len(xbe), 0, 11) + b'default.xbe'
    entry += b'\0' * ((-len(entry)) % 4)
    root = base + 33 * SECTOR
    data[root:root+len(entry)] = entry
    if duplicate: data[root+len(entry):root+2*len(entry)] = entry
    data[base+34*SECTOR:base+34*SECTOR+len(xbe)] = xbe
    return data


@unittest.skipUnless(DRIVER.is_file(), 'Build the native launcher smoke-test targets first')
class TitleArtTests(unittest.TestCase):
    def run_image(self, data, expected=False, pixel=None):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'disc with spaces é.iso'; path.write_bytes(data)
            command = [str(DRIVER), str(path), '1' if expected else '0']
            if pixel: command += ['4', '4', pixel]
            result = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_bare_and_shifted_image(self):
        for base in (0, 0x30600):
            self.run_image(synthetic_iso(base=base), True, 'ffff0000')

    def test_bc1_transparency_uses_background(self):
        self.run_image(synthetic_iso(synthetic_xbe(True)), True, 'ff111319')

    def test_invalid_and_truncated_images(self):
        self.run_image(b'not an ISO')
        self.run_image(synthetic_iso()[:33 * SECTOR + 12])

    def test_root_bounds_and_duplicate_xbe(self):
        data = synthetic_iso(); struct.pack_into('<I', data, 32*SECTOR+0x18, 0xffffffff)
        self.run_image(data)
        self.run_image(synthetic_iso(duplicate=True))

    def test_xbe_bounds_title_and_resource_format(self):
        for offset, value in [(0x108, 0xffffffff), (0x11c, 0xffffffff), (0x120, 0xffffffff),
                              (0x118, 0xffffffff), (0x188, 0), (0x214, 0xffffffff),
                              (0x20c, 0xffffffff), (0x210, 0xffffffff), (0x404, 0xffffffff),
                              (0x408, 0xffffffff), (0x410, 0xffffffff), (0x418, 0x02210e20)]:
            with self.subTest(offset=offset):
                xbe = synthetic_xbe(); struct.pack_into('<I', xbe, offset, value)
                self.run_image(synthetic_iso(xbe))

    def test_missing_art_and_truncated_blocks(self):
        xbe = synthetic_xbe(); xbe[0x250] = ord('!')
        self.run_image(synthetic_iso(xbe))
        xbe = synthetic_xbe(); struct.pack_into('<I', xbe, 0x210, 39)
        self.run_image(synthetic_iso(xbe))


if __name__ == '__main__': unittest.main()
