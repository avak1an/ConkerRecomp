# Build and test the intro and tavern

The Windows development preview generates the game on your PC from your own
recognized Conker disc image. The repository contains the launcher, generation
tools, host runtime, and open-source dependencies. Campaign and multiplayer
are not validated yet.

## Requirements

- Windows x64 and a Direct3D 11 GPU.
- Python 3.10 or newer, available as `python` or `py`.
- Visual Studio 2022 / Build Tools with **Desktop development with C++** and
  the Windows SDK, plus CMake 3.20 or newer.
- Rust/Cargo 1.98 or newer on PATH. CMake uses the installed toolchain and
  Cargo.lock to build the DSP library; Cargo may download open-source crates.
- Your supported disc image and free space for extracted files, generated C,
  and the local build. These outputs can occupy several gigabytes.

Install the generation dependency once from the repository root:

```powershell
python -m pip install -r requirements.txt
```

## Launcher workflow

1. Open `conker-launcher.exe` in the repository root.
2. Choose your own ISO / XISO and select **Verify SHA-1**.
3. Select **Import & generate**. Leave the launcher open while it verifies,
   extracts, discovers functions, and generates C.
4. Select **Build local game**. The first build compiles the runtime and its
   audio dependencies. Progress and compiler output are available in **Task log**.
5. Select **Play**. Press controller A to skip the intro video, or let it finish.
   Use the D-pad or left stick to navigate the six tavern menu items.

The preview uses 640 x 480, 4:3 and shows FPS in the window title. The first
visit to a scene compiles shaders locally; later visits reuse valid cached
bytecode. Loading and performance still need testing across more machines.
Keep the launcher with the checkout so it can locate the setup tools.

Only exact image identities listed in `config/conker/versions.json` are
recognized. Verification checks image size, SHA-1 and SHA-256, then checks
the extracted XBE too. A matching hash identifies compatible bytes, not ownership.
The catalog currently describes a development preview, not a complete-game release.

## Local files

Each import creates a new ignored transaction:

```text
local/preparation/import-<id>/
  game/                  locally extracted files, saves, shader cache
  generated/analysis/    local executable metadata and disassembly
  generated/recomp/      locally translated C and headers
  preparation.json       input, recipe and generated-file hashes
```

Generation does not by itself enable Play. Build checks the selected preparation
and its generated files, compiles them, and runs a startup/data-path check. A
failed build removes its ready receipt. Original game function bodies are never
stored in project patches or downloaded as a pre-generated archive.

The launcher preserves existing import folders and saves. To reuse a preparation,
select that run's `game` folder. If the generation recipe changes, import again.
Saves can be copied between your local runs separately; import never overwrites
an existing run. The runtime and generated shaders remain local build/data outputs.

## Command-line development

```powershell
python -m tools.prepare "PATH_TO_YOUR_OWN_ISO" --development --extract-all --output local/preparation/my-run
python -m tools.launcher.backend build --root . --data-dir local/preparation/my-run/game --result local/build-result.ini
```

The executable is `build/Debug/conker_recomp.exe`. Always pass its prepared data:

```powershell
.\build\Debug\conker_recomp.exe --data-dir .\local\preparation\my-run\game
```

For direct CMake work, set `CONKER_GENERATED_DIR` to the absolute path of that
run's `generated/recomp`. Without this setting, root CMake builds only the
independent launcher. The launcher can also be rebuilt with
`.\LaunchConker.ps1 -BuildOnly`.

## Source boundary and tests

`config/source-map.json` distinguishes reusable Xbox code, Conker integration,
locally generated game content, debug outputs and build artifacts. Third-party
notices and revisions are recorded in `THIRD_PARTY_NOTICES.md`.

`config/public-files.json` lists explicitly reviewed public files and hashes.
Before committing, stage the intended sources and run:

```powershell
python -m tools.release.check
git diff --cached --stat
python -m unittest tools.test_prepare tools.test_source_boundary tools.test_launcher_backend tools.test_generator_config
cmake -S src/launcher -B build/launcher -A x64 -DCONKER_LAUNCHER_TEST=ON
cmake --build build/launcher --config Release
python -m unittest tools.test_launcher_title_art
```

The checker reads Git's index blobs. It rejects unlisted files, changed hashes,
game/build paths, personal paths and binary payloads. The existing README logo
and standalone `conker-launcher.exe` are explicit reviewed exceptions. No game
executable is distributed. After launcher source review, rebuild its packaged
EXE with `python -m tools.release.package_launcher` and stage it with the manifest.

Committed tests use synthetic inputs. Tests using your disc, generated code,
screen captures or debugger output belong only in ignored local directories.
Further campaign and multiplayer work should use the same source/generation
boundary rather than editing or committing translated game C.
