"""Verify an explicitly recognized disc and generate private analysis/C locally.

This recipe generates local C for the intro/tavern development runtime. It never
imports research outputs, accepts arbitrary checksums, or downloads game content.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import sys
import uuid

from .disc import Disc

ROOT = Path(__file__).resolve().parents[2]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def match_version(fingerprint, versions, development=False):
    candidates = versions['supported_versions']
    if development:
        candidates = candidates + versions.get('development_versions', [])
    matches = [v for v in candidates if all(v.get(k) == fingerprint[k]
               for k in ('image_size', 'image_sha1', 'image_sha256'))]
    if len(matches) != 1:
        raise ValueError('Image is not uniquely recognized by this recipe (size/SHA-1/SHA-256).')
    return matches[0]


def output_target(root, output):
    """Refuse arbitrary output roots, links, or overwrites of an existing run."""
    root = root.resolve()
    output = Path(output)
    if not output.is_absolute():
        output = root / output
    lexical = Path(os.path.abspath(output))
    if not lexical.is_relative_to(root):
        raise ValueError('Preparation outputs must be inside this checkout')
    relative = lexical.relative_to(root)
    if len(relative.parts) < 2 or relative.parts[0] not in ('local', 'generated', 'build'):
        raise ValueError('Use a new directory under local/, generated/, or build/')
    current = root
    for component in relative.parts:
        current = current / component
        if current.is_symlink() or (hasattr(current, 'is_junction') and current.is_junction()):
            raise ValueError('Output path contains a link or junction')
        if current.exists() and current.resolve() != current:
            raise ValueError('Output path resolves through a reparse point')
    if lexical.exists():
        raise ValueError('Output already exists; choose a new run directory')
    return lexical


def generate(xbe, work, recipe, progress=lambda msg: print(msg, flush=True)):
    import capstone
    if capstone.__version__ != '5.0.7':
        raise ValueError('Use the pinned dependencies in requirements.txt')
    from tools.xboxrecomp.xbe_parser.xbe_parser import XBEParser, export_json
    from tools.xboxrecomp.disasm.disasm import Disassembler
    from tools.xboxrecomp.recomp import config
    from tools.xboxrecomp.recomp.translator import BatchTranslator

    metadata = work / 'generated' / 'analysis'
    metadata.mkdir(parents=True)
    analysis = metadata / 'xbe.json'
    parsed = XBEParser(str(xbe)).parse()
    if parsed.certificate.title_id != int(recipe['title_id'], 16):
        raise ValueError('Unexpected executable title ID')
    export_json(parsed, str(analysis))
    progress('Discovering functions from the local executable...')
    detector = Disassembler(str(xbe), analysis_json=str(analysis),
        output_dir=str(metadata), force=True, extra_sections=recipe['extra_code_sections'],
        seed_functions=[int(a, 16) for a in recipe['function_seeds']],
        icall_targets=[int(a, 16) for a in recipe['alternate_entries']])
    if not detector.run():
        raise ValueError('Function discovery failed')
    missing = set(int(a, 16) for a in recipe['function_seeds']) - detector.func_detector.functions.keys()
    if missing:
        raise ValueError('Configured functions were not discovered: ' + ', '.join(f'{a:08X}' for a in sorted(missing)))
    # Do not retain the large disassembler object while translating.
    del detector
    import gc
    gc.collect()
    config.FORCE_CODE_SECTIONS = set(recipe['extra_code_sections'])
    config.FORCE_DATA_SECTIONS = set(recipe['data_sections'])
    config.configure_from_xbe(str(xbe))
    source = work / 'generated' / 'recomp'
    progress('Translating discovered functions into private C sources...')
    translator = BatchTranslator(str(xbe), str(metadata / 'functions.json'),
                                labels_json_path=str(metadata / 'labels.json'),
                                output_dir=str(source))
    stats = translator.translate_batch_split(translator.get_functions_by_category(),
                                             str(source), chunk_size=1000)
    from .hooks import apply_entry_hooks
    apply_entry_hooks(source, recipe.get('entry_hooks', []))
    sources = []
    for path in sorted(source.iterdir()):
        data = path.read_bytes()
        sources.append(dict(path=path.name, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    return dict(translated=stats['translated'], failed=stats['failed'],
                unresolved_stubs=stats.get('unresolved_stubs', 0), files=sources)


def prepare(image, output, root=ROOT, development=False, extract_all=False, generate_code=True,
            progress=lambda msg: print(msg, flush=True)):
    root = Path(root).resolve()
    target = output_target(root, output)
    versions = json.loads((root / 'config/conker/versions.json').read_text(encoding='utf-8'))
    recipe_bytes = (root / 'config/conker/generation.json').read_bytes()
    recipe = json.loads(recipe_bytes)
    if not development:
        raise ValueError('Use --development to prepare the explicitly recognized intro/tavern preview version.')
    with Disc(image) as disc:
        fingerprint = disc.fingerprint(progress)
        version = match_version(fingerprint, versions, development)
        if version['generation_recipe_version'] != recipe['version']:
            raise ValueError('Image and generation recipe versions differ')
        entries = disc.entries()
        main = [entry for entry in entries if entry.path.casefold() == 'default.xbe']
        if len(main) != 1 or main[0].size != version['xbe_size']:
            raise ValueError('Missing or unexpected default.xbe')
        data = disc.read(main[0].offset, main[0].size)
        if (hashlib.sha1(data).hexdigest() != version['xbe_sha1'] or
                hashlib.sha256(data).hexdigest() != version['xbe_sha256']):
            raise ValueError('Executable fingerprint does not match the selected image version')
        target.parent.mkdir(parents=True, exist_ok=True)
        work = target.parent / ('.preparing-' + uuid.uuid4().hex)
        work.mkdir()
        # On failure, keep the private transaction and its error for diagnosis.
        # A failed/partial run never replaces a complete run or receives ready=true.
        try:
            file_manifest = disc.extract(entries if extract_all else main, work / 'game', progress)
            imported = next(p for p in (work / 'game').iterdir() if p.name.casefold() == 'default.xbe')
            if hashlib.sha256(imported.read_bytes()).hexdigest() != version['xbe_sha256']:
                raise ValueError('Extracted executable changed during import')
            generated = generate(imported, work, recipe, progress) if generate_code else None
            disc.unchanged()
            result = dict(schema_version=1, stage='analysis-generated' if generated else 'extracted',
                          game_ready=False, version_id=version['id'], input=fingerprint,
                          recipe_version=recipe['version'], recipe_sha256=hashlib.sha256(recipe_bytes).hexdigest(),
                          extracted_files=file_manifest,
                          generated=generated,
                          next_step='Build the local runtime with this generated/recomp directory and test the intro and tavern.')
            write_json(work / 'preparation.json', result)
            work.rename(target)
        except Exception as error:
            write_json(work / 'failure.json', dict(game_ready=False, error=str(error)))
            raise
    print('Local preparation complete: ' + str(target), flush=True)
    print(('C sources generated locally; build the runtime next.' if generate_code else 'Disc files extracted only; generation is still required.'), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path, default=Path('local/preparation') / uuid.uuid4().hex)
    parser.add_argument('--development', action='store_true')
    parser.add_argument('--extract-all', action='store_true', help='Also extract the remaining disc files locally')
    args = parser.parse_args()
    try:
        prepare(args.image, args.output, development=args.development, extract_all=args.extract_all)
    except (OSError, ValueError, ImportError) as error:
        print('Preparation failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
