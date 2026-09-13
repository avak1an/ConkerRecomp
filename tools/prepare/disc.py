"""Bounded, read-only XDVDFS import. No game bytes are part of this module."""
from dataclasses import dataclass
from pathlib import Path
import hashlib
import os
import re
import struct

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
BASES = (0, 0xFD90, 0x30600, 0xFD90000, 0x18300000)
RESERVED = re.compile(r'^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)', re.I)


def safe_name(name):
    if (not name or name in ('.', '..') or name[-1] in ' .'
            or any(ord(c) < 32 or c in '/\\:<>"|?*' for c in name)
            or RESERVED.match(name)):
        raise ValueError('Unsafe disc filename')
    return name


@dataclass(frozen=True)
class Entry:
    path: str
    offset: int
    size: int


class Disc:
    def __init__(self, path):
        self.file = Path(path).open('rb')
        self.initial = os.fstat(self.file.fileno())
        self.size = self.initial.st_size

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.file.close()

    def unchanged(self):
        now = os.fstat(self.file.fileno())
        if (now.st_size, now.st_mtime_ns) != (self.size, self.initial.st_mtime_ns):
            raise ValueError('Disc image changed during preparation')

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset > self.size or size > self.size - offset:
            raise ValueError('Disc extent is outside image bounds')
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise ValueError('Truncated disc image')
        return data

    def fingerprint(self, progress=lambda _: None):
        self.file.seek(0)
        sha1, sha256 = hashlib.sha1(), hashlib.sha256()
        total, last = 0, -1
        while block := self.file.read(8 * 1024 * 1024):
            sha1.update(block)
            sha256.update(block)
            total += len(block)
            percent = total * 100 // max(self.size, 1)
            if percent != last:
                progress(f'Verifying image: {percent}%')
                last = percent
        self.unchanged()
        if total != self.size:
            raise ValueError('Image length changed')
        return dict(image_size=total, image_sha1=sha1.hexdigest(), image_sha256=sha256.hexdigest())

    def entries(self):
        bases = [b for b in BASES if b + 33 * SECTOR <= self.size
                 and self.read(b + 32 * SECTOR, len(MAGIC)) == MAGIC]
        if len(bases) != 1:
            raise ValueError('Expected exactly one supported XDVDFS game partition')
        base = bases[0]
        desc = self.read(base + 32 * SECTOR, SECTOR)
        sector, size = struct.unpack_from('<II', desc, 0x14)
        pending = [('', sector, size, 0)]
        directories, paths, files = set(), set(), []
        byte_budget = 0
        while pending:
            parent, sector, size, depth = pending.pop()
            key = (sector, size)
            if key in directories or depth > 32 or len(directories) >= 10000:
                raise ValueError('Cyclic or excessive directory structure')
            directories.add(key)
            byte_budget += size
            if not size or size > 4 * 1024 * 1024 or byte_budget > 64 * 1024 * 1024:
                raise ValueError('Excessive or empty directory extent')
            raw = self.read(base + sector * SECTOR, size)
            # Directory nodes are addressed in four-byte units. Visit the
            # actual tree, rejecting cycles/aliases instead of following padding.
            nodes, visited, spans = [0], set(), []
            while nodes:
                offset = nodes.pop()
                if offset in visited or offset + 14 > len(raw):
                    raise ValueError('Invalid or cyclic directory node')
                visited.add(offset)
                left, right, sec, length, attr, nlen = struct.unpack_from('<HHIIBB', raw, offset)
                end = offset + 14 + nlen
                if not nlen or end > len(raw) or end > ((offset // SECTOR) + 1) * SECTOR:
                    raise ValueError('Invalid directory filename length')
                end_aligned = (end + 3) & ~3
                if any(offset < b and a < end_aligned for a, b in spans):
                    raise ValueError('Overlapping directory entries')
                spans.append((offset, end_aligned))
                name = safe_name(raw[offset + 14:end].decode('ascii'))
                path = parent + name
                folded = path.casefold()
                if folded in paths or len(paths) >= 100000 or len(path) > 200:
                    raise ValueError('Duplicate, excessive, or overlong disc path')
                paths.add(folded)
                absolute = base + sec * SECTOR
                if absolute > self.size or length > self.size - absolute:
                    raise ValueError('File or directory exceeds image bounds')
                if attr & 0x10:
                    pending.append((path + '/', sec, length, depth + 1))
                else:
                    files.append(Entry(path, absolute, length))
                for child in (left, right):
                    if child:
                        nodes.append(child * 4)
        if sum(f.size for f in files) > self.size:
            raise ValueError('Overlapping file extents exceed image size')
        return sorted(files, key=lambda e: e.path.casefold())

    def extract(self, entries, destination, progress=lambda _: None):
        """Write only into a newly created transaction directory."""
        destination = Path(destination)
        destination.mkdir(parents=True, exist_ok=False)
        root = destination.resolve()
        result = []
        total = sum(entry.size for entry in entries)
        done, last = 0, -1
        for entry in entries:
            path = destination.joinpath(*entry.path.split('/'))
            if not path.resolve().is_relative_to(root):
                raise ValueError('Extraction path escaped destination')
            path.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            with path.open('xb') as out:
                remaining, offset = entry.size, entry.offset
                while remaining:
                    chunk = self.read(offset, min(1024 * 1024, remaining))
                    out.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
                    offset += len(chunk)
                    done += len(chunk)
                    percent = done * 100 // max(total, 1)
                    if percent != last:
                        progress(f'Extracting disc files: {percent}%')
                        last = percent
            result.append(dict(path=entry.path, size=entry.size, sha256=digest.hexdigest()))
        self.unchanged()
        return result
