"""Build and package only the standalone ConkerRecomp launcher.

Run after reviewing source changes and updating their source-manifest hashes.
The game target, generated C, and assets are not inputs to this CMake project.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile

from tools.launcher.backend import find_cmake
from tools.release.check import LAUNCHER, LAUNCHER_INPUTS, MANIFEST, check_launcher_binary

ROOT = Path(__file__).resolve().parents[2]


def source_hashes(root):
    return {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in LAUNCHER_INPUTS}


def package(root=ROOT):
    root = Path(root).resolve()
    manifest_path = root / MANIFEST
    manifest_data = manifest_path.read_bytes()
    manifest = json.loads(manifest_data)
    reviewed = {row['path']: row for row in manifest['files']}
    inputs = source_hashes(root)
    for name, digest in inputs.items():
        row = reviewed.get(name, {})
        if row.get('reviewed') is not True or row.get('sha256') != digest:
            raise ValueError('Review and update the source manifest before packaging: ' + name)
    build_parent = root / 'build'
    build_parent.mkdir(exist_ok=True)
    if build_parent.resolve() != build_parent:
        raise ValueError('Build directory must not be a link or junction')
    build = Path(tempfile.mkdtemp(prefix='launcher-package-', dir=build_parent))
    cmake = find_cmake()
    subprocess.run([cmake, '-S', str(root / 'src/launcher'), '-B', str(build),
                    '-A', 'x64', '-DCONKER_LAUNCHER_TEST=OFF'], cwd=root, check=True)
    subprocess.run([cmake, '--build', str(build), '--config', 'Release',
                    '--target', 'conker-launcher', '--parallel', '2'], cwd=root, check=True)
    if source_hashes(root) != inputs or manifest_path.read_bytes() != manifest_data:
        raise ValueError('Sources or manifest changed during packaging; nothing installed')
    data = (build / 'Release' / LAUNCHER).read_bytes()
    check_launcher_binary(data)
    entry = dict(path=LAUNCHER, category='CONKER_SPECIFIC', reviewed=True,
                 provenance='Standalone ConkerRecomp launcher built from project source by avak1an',
                 sha256=hashlib.sha256(data).hexdigest(), build_target='conker-launcher',
                 build_configuration='Release', build_inputs=inputs)
    manifest['files'] = sorted([row for row in manifest['files'] if row['path'] != LAUNCHER]
                               + [entry], key=lambda row: row['path'])
    temporary = build / 'packaged-launcher.exe'
    temporary.write_bytes(data)
    temporary.replace(root / LAUNCHER)
    temporary = build / 'public-files.json'
    temporary.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    temporary.replace(manifest_path)
    print(f'Packaged {LAUNCHER}: {len(data):,} bytes, SHA-256 {entry["sha256"]}')
    print('Only the standalone launcher was copied; stage it together with the reviewed manifest.')


if __name__ == '__main__':
    package()
