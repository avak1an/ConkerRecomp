"""Background verification/build service for the native launcher.

The GUI uses an INI result file, written atomically, and a separate local log.
Only explicitly recognized release/development identities can be imported.
"""
from pathlib import Path
import argparse
import configparser
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import uuid

# Support both module execution and the GUI's direct script invocation.
if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.prepare.disc import Disc


def fingerprint(path, progress=lambda message: None):
    """Use the same streaming size/SHA-1/SHA-256 checks as preparation."""
    with Disc(path) as disc:
        return disc.fingerprint(progress)


def verify_image(path, root, progress=lambda message: None):
    versions = json.loads((Path(root) / 'config/conker/versions.json').read_text(encoding='utf-8'))
    identity = fingerprint(path, progress)
    matches = [(v, state) for key, state in (('supported_versions', 'matched'),
                                            ('development_versions', 'development'))
               for v in versions.get(key, [])
               if all(v.get(k) == identity[k] for k in ('image_size', 'image_sha1', 'image_sha256'))]
    if len(matches) > 1:
        raise ValueError('The version list contains duplicate image identities.')
    detail = 'SHA-1: ' + identity['image_sha1']
    if matches:
        version, state = matches[0]
        # Import rechecks image identity, XBE and recipe; this is not game readiness.
        message = ('Disc verified. Development import/generation is available.' if state == 'development'
                   else 'Disc verified: ' + version['id'] + '.')
        return dict(state=state, message=message, detail=detail, **identity)
    return dict(state='unsupported', message='This image does not match a recognized Conker version.',
                detail=detail, **identity)


def prepare_image(path, root, progress=lambda message: None):
    import capstone
    if capstone.__version__ != '5.0.7':
        raise ValueError('Install the pinned dependencies: python -m pip install -r requirements.txt')
    from tools.prepare.__main__ import prepare
    root = Path(root).resolve()
    output = root / 'local/preparation' / ('import-' + uuid.uuid4().hex)
    result = prepare(path, output, root=root, development=True, extract_all=True, progress=progress)
    if result['stage'] != 'analysis-generated' or result['generated']['failed']:
        raise ValueError('Generation has failures. Inspect the private preparation report and task log.')
    return dict(state='prepared', message='Local files and C sources generated. Choose Build local game next.',
                detail=str(output), data_dir=str(output / 'game'), game_ready='false')


def find_cmake():
    found = shutil.which('cmake')
    if found:
        return found
    installer = Path(os.environ.get('ProgramFiles(x86)', '')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if installer.is_file():
        completed = subprocess.run([str(installer), '-latest', '-products', '*', '-find',
                                   'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'],
                                  capture_output=True, text=True, check=True)
        for line in completed.stdout.splitlines():
            if Path(line).is_file():
                return line
    raise ValueError('Install CMake and Visual Studio with Desktop development with C++.')


def validate_preparation(root, data_dir):
    """Bind a build to a complete, verified local generation transaction."""
    root = Path(root).resolve()
    if data_dir is None:
        raise ValueError('Import and generate your disc first, then select its game folder.')
    data = Path(data_dir).resolve()
    prepared = data.parent
    relative = prepared.relative_to(root) if prepared.is_relative_to(root) else None
    if not relative or relative.parts[0] not in ('local', 'generated', 'build') or data.name != 'game':
        raise ValueError('Select the game folder from a preparation run in this checkout.')
    manifest = prepared / 'preparation.json'
    if not manifest.is_file():
        raise ValueError('This folder has no completed preparation. Import and generate first.')
    report = json.loads(manifest.read_text(encoding='utf-8'))
    recipe_bytes = (root / 'config/conker/generation.json').read_bytes()
    recipe = json.loads(recipe_bytes)
    versions = json.loads((root / 'config/conker/versions.json').read_text(encoding='utf-8'))
    from tools.prepare.__main__ import match_version
    version = match_version(report['input'], versions, development=True)
    generated = report.get('generated')
    if (report.get('stage') != 'analysis-generated' or not generated or generated.get('failed') != 0
            or report.get('recipe_version') != recipe['version']
            or report.get('recipe_sha256') != hashlib.sha256(recipe_bytes).hexdigest()
            or report.get('version_id') != version['id']):
        raise ValueError('Preparation is incomplete or outdated. Import and generate again.')
    xbe = data / 'default.xbe'
    if not xbe.is_file() or hashlib.sha256(xbe.read_bytes()).hexdigest() != version['xbe_sha256']:
        raise ValueError('The prepared executable is missing or changed. Import again.')
    source = prepared / 'generated/recomp'
    entries = generated.get('files', [])
    names = [entry['path'] for entry in entries]
    if len(names) != len(set(names)) or 'recomp_dispatch.c' not in names or 'recomp_funcs.h' not in names:
        raise ValueError('The preparation source list is incomplete.')
    if set(p.name for p in source.iterdir() if p.is_file()) != set(names):
        raise ValueError('The generated source directory has unexpected or missing files.')
    for entry in entries:
        name = entry['path']
        if Path(name).name != name or '/' in name or '\\' in name or not name.endswith(('.c', '.h')):
            raise ValueError('Invalid generated source path.')
        path = source / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != entry['size']:
            raise ValueError('Generated source is missing or changed: ' + name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Generated source changed: ' + name + '. Generate a fresh run.')
    return data, source, manifest


def build_existing(root, progress=lambda message: None, run=subprocess.run, data_dir=None):
    root = Path(root).resolve()
    progress('Checking the selected preparation and generated sources...')
    data, generated, manifest = validate_preparation(root, data_dir)
    cmake = find_cmake()
    build = root / 'build'
    build.mkdir(exist_ok=True)
    receipt = build / 'runtime-ready.ini'
    receipt.unlink(missing_ok=True)
    progress('Configuring the game runtime and local C sources...')
    run([cmake, '-S', str(root), '-B', str(build), '-A', 'x64',
         '-DCONKER_GENERATED_DIR=' + str(generated)], cwd=root, check=True)
    progress('Building the game. The first build also compiles audio dependencies; see Task log.')
    run([cmake, '--build', str(build), '--config', 'Debug', '--target', 'conker_recomp',
         '--parallel', '2'], cwd=root, check=True)
    exe = build / 'Debug/conker_recomp.exe'
    if not exe.is_file():
        raise ValueError('The build completed but the expected game executable is missing.')
    # Validate that this executable can locate its prepared data before Play is enabled.
    run([str(exe), '--data-dir', str(data), '--check-startup'], cwd=root, check=True)
    ready = dict(state='complete', message='Local game build completed. Ready to test.',
                 detail=str(exe), data_dir=str(data),
                 preparation_sha256=hashlib.sha256(manifest.read_bytes()).hexdigest())
    write_result(receipt, ready)
    return ready


def write_result(path, values):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    parser = configparser.ConfigParser(interpolation=None)
    parser['task'] = {k: str(v).replace('\n', ' ').replace('\r', ' ') for k, v in values.items()}
    temporary = path.with_suffix(path.suffix + '.tmp')
    # UTF-16 is understood by the Windows profile APIs without locale loss.
    with temporary.open('w', encoding='utf-16') as stream:
        parser.write(stream, space_around_delimiters=False)
    # Antivirus/indexers or older launchers can briefly hold a handle without
    # FILE_SHARE_DELETE. Keep the previous complete snapshot while retrying.
    deadline = time.monotonic() + 2.0
    while True:
        try:
            temporary.replace(path)
            break
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.02)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['verify', 'build', 'prepare'])
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--data-dir', type=Path)
    parser.add_argument('--result', type=Path, required=True)
    args = parser.parse_args(argv)
    def progress(message):
        print(message, flush=True)
        write_result(args.result, dict(state='working', message=message, detail=''))
    try:
        if args.action in ('verify', 'prepare'):
            if args.image is None:
                raise ValueError('Choose a disc image first.')
            action = verify_image if args.action == 'verify' else prepare_image
            result = action(args.image, args.root, progress)
        else:
            result = build_existing(args.root, progress, data_dir=args.data_dir)
        write_result(args.result, result)
        return 0 if result['state'] in ('matched', 'development', 'prepared', 'complete') else 2
    except (OSError, ValueError, KeyError, ImportError, subprocess.SubprocessError) as error:
        message = ('Install generation dependencies: python -m pip install -r requirements.txt'
                   if isinstance(error, ImportError) else str(error))
        print(message, file=sys.stderr, flush=True)
        try:
            write_result(args.result, dict(state='error', message=message,
                         detail='Task did not complete. See the task log for details.'))
        except OSError as report_error:
            print('Could not publish the final status: ' + str(report_error), file=sys.stderr, flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
