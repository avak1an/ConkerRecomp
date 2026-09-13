"""Configure the generic translator from the selected local executable.
No game layout is available until configure_from_xbe() is called.
"""

from dataclasses import dataclass
from typing import List, Optional

# Section flag bit from the XBE format (see tools/xbe_parser).
SECTION_EXECUTABLE = 0x00000004

# Sections that hold code despite not being marked executable in the XBE, or
# that we always want treated as code. Matched by exact section name.
FORCE_CODE_SECTIONS = set()

# Sections that are resources/data even if flagged executable.
FORCE_DATA_SECTIONS = {
    ".data", ".data1", ".rdata",
}


@dataclass
class Section:
    name: str
    va: int
    va_size: int
    raw_addr: int
    raw_size: int
    is_code: bool


# Unconfigured layouts contain no title-specific fallback.
_SECTIONS: List[Section] = []
SECTIONS = []
TEXT_VA_START = TEXT_VA_END = 0
RDATA_VA_START = RDATA_VA_END = 0
DATA_VA_START = DATA_VA_END = 0
KERNEL_THUNK_ADDR = ENTRY_POINT = 0

_configured_from: Optional[str] = None


def _classify(name: str, flags: int) -> bool:
    """Decide whether a section holds code."""
    if name in FORCE_DATA_SECTIONS:
        return False
    if name in FORCE_CODE_SECTIONS:
        return True
    return bool(flags & SECTION_EXECUTABLE)


def configure_from_xbe(xbe_path: str) -> None:
    """Load the real section layout from the XBE being recompiled.

    Without this the recompiler uses whatever layout happens to be baked into
    this module, and any address beyond it is treated as non-code -- which
    fails every function past that point instead of reporting a problem.
    """
    from tools.xboxrecomp.xbe_parser.xbe_parser import XBEParser

    xbe = XBEParser(xbe_path).parse()
    sections = [
        Section(
            name=s.name,
            va=s.virtual_addr,
            va_size=s.virtual_size,
            raw_addr=s.raw_addr,
            raw_size=s.raw_size,
            is_code=_classify(s.name, s.flags),
        )
        for s in xbe.sections
    ]
    if not sections:
        raise ValueError(f"XBE has no sections: {xbe_path}")

    _install(sections, xbe.header.entry_point, xbe.header.kernel_thunk_addr, xbe_path)


def _install(sections, entry_point, kernel_thunk_addr, origin):
    global _SECTIONS, SECTIONS, _configured_from
    global TEXT_VA_START, TEXT_VA_END, RDATA_VA_START, RDATA_VA_END
    global DATA_VA_START, DATA_VA_END, KERNEL_THUNK_ADDR, ENTRY_POINT

    _SECTIONS = sorted(sections, key=lambda s: s.va)
    SECTIONS = [(s.name, s.va, s.va_size, s.raw_addr) for s in _SECTIONS]
    _configured_from = origin

    by_name = {s.name: s for s in _SECTIONS}

    text = by_name.get(".text") or next(
        (s for s in _SECTIONS if s.is_code), _SECTIONS[0]
    )
    TEXT_VA_START = text.va
    TEXT_VA_END = text.va + text.va_size

    RDATA_VA_START = RDATA_VA_END = DATA_VA_START = DATA_VA_END = 0
    rdata = by_name.get(".rdata")
    data = by_name.get(".data")
    # Some titles (the Dashboard among them) ship no .rdata; fall back to .data
    # so the read-only-data range is never left pointing at another game.
    rdata = rdata or data
    data = data or rdata
    if rdata:
        RDATA_VA_START = rdata.va
        RDATA_VA_END = rdata.va + rdata.va_size
    if data:
        DATA_VA_START = data.va
        DATA_VA_END = data.va + data.va_size

    ENTRY_POINT = entry_point
    KERNEL_THUNK_ADDR = kernel_thunk_addr


def configured_from() -> Optional[str]:
    """Path of the XBE this layout came from, or None if still the fallback."""
    return _configured_from


def va_to_file_offset(va):
    """Convert virtual address to XBE file offset."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            offset = va - s.va + s.raw_addr
            # Sections are zero-padded when virtual_size exceeds raw_size (BSS
            # tails); those addresses exist at runtime but have no file bytes.
            if va - s.va >= s.raw_size:
                return None
            return offset
    return None


def is_code_address(va):
    """Check if VA is in an executable section (.text or XDK library sections)."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            return s.is_code
    return False


def is_data_address(va):
    """Check if VA is in a data section."""
    for s in _SECTIONS:
        if s.va <= va < s.va + s.va_size:
            return not s.is_code
    return False
