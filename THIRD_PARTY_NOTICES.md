# Source provenance and licenses

The root `LICENSE` is the existing ConkerRecomp GPLv3 license. Authored Conker
integration, preparation, and release tooling in this checkout use those terms.
Original upstream notices remain applicable to their respective components.

| Component | Origin | Notice retained |
| --- | --- | --- |
| `tools/xboxrecomp/`, `runtime/xbox/`, and reusable `src/xbox/` components | [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp), with local changes | [XboxRecomp MIT](licenses/XboxRecomp-MIT.txt), copyright 2026 sp00nz |
| Capstone | Installed separately through `requirements.txt` | Capstone package's upstream BSD license and bundled notices |

The exact XboxRecomp revision from which the research checkout originally
started is unknown. Do not invent a commit ID or present its reusable tools as
original Conker game code. The source manifest records hashes of the selected
files; it does not reconstruct missing Git history.

The existing README logo remains in `assets/` for repository branding. The
launcher does not load or distribute that image; its preview reads artwork
from the user's selected ISO into memory.

The runtime also uses these open-source components. Their existing per-file
notices and applicable license texts are retained; they are not original Conker
game code or a source of game assets.

| Component | Origin and revision evidence | Notices |
| --- | --- | --- |
| `src/xbox/nv2a/` and `src/xbox/apu/` | xemu/QEMU-derived register and processor models with standalone changes | Existing per-file copyright notices; LGPL-2.0-or-later where specified; [LGPL text](src/xbox/apu/dsp/COPYING.LIB) |
| `src/xbox/apu/dsp/` | xemu DSP bridge, revision recorded in [UPSTREAM.txt](src/xbox/apu/dsp/UPSTREAM.txt) | GPL-2.0-or-later bridge, LGPL-2.0-or-later DMA; [GPL text](src/xbox/apu/dsp/COPYING) |
| `third_party/dsp56300/` | DSP56300 engine; [revision](third_party/dsp56300/UPSTREAM.txt) | [MIT license](third_party/dsp56300/LICENSE) |
| `third_party/libsamplerate/` | libsamplerate 0.2.2; [revision](third_party/libsamplerate/UPSTREAM.txt) | [BSD-2-Clause](third_party/libsamplerate/COPYING) |

Rust dependencies are resolved from the retained Cargo.lock during local builds.
Their own package notices apply. Guest DSP microcode is loaded from the user's
locally extracted game data; none is included in these dependencies.

No game binary, asset, translated game function, or emulator capture is an
included third-party dependency.
