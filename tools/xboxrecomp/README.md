# Reusable XboxRecomp tools

These are reusable XBE parsing, x86 analysis, and C translation tools originally
from [XboxRecomp](https://github.com/sp00nznet/xboxrecomp), with local fixes.
See [the preserved MIT notice](../../licenses/XboxRecomp-MIT.txt).

Conker addresses, fingerprints, and section-selection rules belong under
`config/conker/`. Parsed executable data, disassemblies, discovered functions,
and translated C belong under ignored local preparation outputs. The tools
load those bytes from the importing user's executable.

The initial separation changes package paths and removes stale embedded layout
defaults. It preserves the lifter's instruction semantics. The old installer
that required pre-existing hand-maintained generated files is intentionally
absent; preparation must operate in an empty directory.
