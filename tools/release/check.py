"""Check exactly what Git would commit, against an explicit source manifest.

This is an inclusion/integrity check, not automatic copyright classification.
The manifest is maintained through source review; never generate it from `git
add .` or from an unreviewed recursive copy of a research workspace.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import re
import struct
import subprocess

MANIFEST = 'config/public-files.json'
LOGO = 'assets/conker-live-recomped-logo-v2-c.png'
LOGO_BLOB = '65431d5521bb81eff8d0f938cafc7248b80cd23d'
LAUNCHER = 'conker-launcher.exe'
LAUNCHER_INPUTS = (
    'src/launcher/CMakeLists.txt', 'src/launcher/launcher.c',
    'src/launcher/pages.inc', 'src/launcher/title_art.c',
    'src/launcher/title_art.h', 'src/launcher/launcher.manifest',
    'tools/release/package_launcher.py',
)
DENY_DIRS = {'game', 'game_files', 'generated', 'extracted', 'local', 'build',
             'out', 'bin', 'obj', 'artifacts', 'dumps', 'logs', '.vs', 'xemu'}
DENY_EXT = {'.iso', '.xiso', '.xbe', '.xmv', '.wav', '.wma', '.rbm', '.exe',
            '.dll', '.pdb', '.bin', '.dmp', '.dump', '.log', '.gdb', '.orig', '.bak'}
CATEGORIES = {'GENERIC_XBOXRECOMP', 'CONKER_SPECIFIC'}
# Cargo convention uses src/bin for authored CLI sources, not build output.
DEPENDENCY_SOURCE_EXCEPTIONS = {
    'third_party/dsp56300/crates/asm/src/bin/asm.rs',
    'third_party/dsp56300/crates/disasm/src/bin/disasm.rs',
    'third_party/dsp56300/crates/emu/src/bin/difftest.rs',
    'third_party/dsp56300/crates/emu/src/bin/emu.rs',
}

PERSONAL = re.compile(rb'(?:[A-Za-z]:[/\\](?:Users|xbox)[/\\]|'
                      + rb'/' + rb'home/[^/\s]+/|' + rb'/' + rb'Users/[^/\s]+/)', re.I)


def check_launcher_binary(data):
    """Check the one allowed native launcher artifact, in addition to its hash.

    A PE header does not prove provenance. The reviewed source input hashes and
    controlled standalone build supply that connection; arbitrary EXEs remain
    excluded even if they are renamed or added to the source manifest.
    """
    if len(data) < 256 or len(data) > 8 * 1024 * 1024 or data[:2] != b'MZ':
        raise ValueError('Launcher must be a bounded native Windows executable')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe < 64 or pe + 112 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid launcher PE header')
    machine, sections = struct.unpack_from('<HH', data, pe + 4)
    optional_size, flags = struct.unpack_from('<HH', data, pe + 20)
    if (machine != 0x8664 or not sections or optional_size < 112
            or pe + 24 + optional_size > len(data) or flags & 0x2000
            or struct.unpack_from('<H', data, pe + 24)[0] != 0x20b
            or struct.unpack_from('<H', data, pe + 92)[0] != 2):
        raise ValueError('Launcher must be a Windows x64 GUI executable, not a DLL')
    if PERSONAL.search(data) or PERSONAL.search(data.replace(b'\0', b'')):
        raise ValueError('Personal machine path embedded in launcher executable')


def validate(files, manifest):
    """files maps Git path -> (Git mode, bytes), for index or committed tree."""
    errors = []
    entries = manifest.get('files', [])
    selected = {}
    for row in entries:
        name = row.get('path', '')
        if name in selected:
            errors.append('Duplicate manifest entry: ' + name)
        selected[name] = row
    if set(files) != set(selected) | {MANIFEST}:
        errors.extend('Unreviewed file in Git: ' + p for p in sorted(set(files) - set(selected) - {MANIFEST}))
        errors.extend('Reviewed file missing from Git: ' + p for p in sorted(set(selected) - set(files)))
    for name, (mode, data) in files.items():
        p = PurePosixPath(name)
        if (p.is_absolute() or '..' in p.parts or '\\' in name or ':' in name
                or str(p) != name or mode not in ('100644', '100755')):
            errors.append('Unsafe Git path or mode: ' + name)
            continue
        if ((name not in DEPENDENCY_SOURCE_EXCEPTIONS and any(part.casefold() in DENY_DIRS for part in p.parts))
                or (p.suffix.casefold() in DENY_EXT and name != LAUNCHER)
                or re.fullmatch(r'recomp_[0-9a-fA-F]{4,}\.c', p.name)
                or p.name.startswith('.env')):
            errors.append('Local/game/build artifact included: ' + name)
        if name == LOGO:
            blob = b'blob ' + str(len(data)).encode() + b'\0' + data
            if hashlib.sha1(blob).hexdigest() != LOGO_BLOB:
                errors.append('Existing README branding changed without review')
        elif name == LAUNCHER:
            try:
                check_launcher_binary(data)
            except ValueError as error:
                errors.append(str(error))
            row = selected.get(name, {})
            build_inputs = row.get('build_inputs', {})
            if (not isinstance(build_inputs, dict) or set(build_inputs) != set(LAUNCHER_INPUTS)
                    or row.get('build_target') != 'conker-launcher'
                    or row.get('build_configuration') != 'Release'):
                errors.append('Missing standalone launcher build input inventory')
                build_inputs = {}
            for source, digest in build_inputs.items():
                if source not in files or hashlib.sha256(files[source][1]).hexdigest() != digest:
                    errors.append('Rebuild packaged launcher after source change: ' + source)
        else:
            if b'\0' in data:
                errors.append('Binary payload in source: ' + name)
            if PERSONAL.search(data):
                errors.append('Personal machine path in source: ' + name)
        if name == MANIFEST:
            continue
        row = selected.get(name, {})
        if row.get('reviewed') is not True or row.get('category') not in CATEGORIES:
            errors.append('Missing content/provenance review: ' + name)
        if row.get('sha256') != hashlib.sha256(data).hexdigest():
            errors.append('Source changed since review: ' + name)
    for required in ('LICENSE', 'THIRD_PARTY_NOTICES.md', 'licenses/XboxRecomp-MIT.txt'):
        if required not in files:
            errors.append('Missing license/provenance file: ' + required)
    return errors


def git_files(root, revision=None):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args])
    records = git('ls-tree', '-rz', revision) if revision else git('ls-files', '--stage', '-z')
    files = {}
    for record in records.split(b'\0'):
        if not record:
            continue
        header, path = record.split(b'\t', 1)
        fields = header.decode('ascii').split()
        mode, oid = (fields[0], fields[2]) if revision else (fields[0], fields[1])
        if not revision and fields[2] != '0':
            raise ValueError('Resolve unmerged index entries before checking')
        files[path.decode('utf-8')] = (mode, git('cat-file', 'blob', oid))
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--revision', help='Audit a committed Git revision; default audits the staging index')
    args = parser.parse_args()
    files = git_files(args.root, args.revision)
    try:
        manifest = json.loads(files[MANIFEST][1])
    except (KeyError, ValueError):
        print('FAIL: a valid source manifest must be present in the audited Git tree/index')
        return 1
    errors = validate(files, manifest)
    for error in errors:
        print('FAIL: ' + error)
    if not errors:
        print(f'PASS: {len(files)} Git files match the reviewed inventory; only explicitly approved binaries included.')
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
