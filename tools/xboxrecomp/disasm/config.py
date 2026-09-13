"""Instruction classification and generic analysis settings.
Executable layout is always read from the local XBE.
"""

# ============================================================
# Instruction Classification
# ============================================================

# x86 control flow instructions (mnemonic sets)
CALL_MNEMONICS = {"call"}
RET_MNEMONICS = {"ret", "retn", "retf"}
JMP_MNEMONICS = {"jmp"}
COND_JMP_MNEMONICS = {
    "jo", "jno", "jb", "jnb", "jnae", "jae", "jc", "jnc",
    "jz", "je", "jnz", "jne", "jbe", "jna", "ja", "jnbe",
    "js", "jns", "jp", "jpe", "jnp", "jpo",
    "jl", "jnge", "jge", "jnl", "jle", "jng", "jg", "jnle",
    "jcxz", "jecxz",
    "loop", "loope", "loopz", "loopne", "loopnz",
}
BRANCH_MNEMONICS = JMP_MNEMONICS | COND_JMP_MNEMONICS

# NOP-like instructions
NOP_MNEMONICS = {"nop"}

# Instructions that terminate a basic block
TERMINATOR_MNEMONICS = RET_MNEMONICS | JMP_MNEMONICS | COND_JMP_MNEMONICS

# ============================================================
# Function Detection
# ============================================================

# Standard MSVC x86 function prologue patterns (byte sequences)
# push ebp; mov ebp, esp
PROLOGUE_PUSH_EBP_MOV = bytes([0x55, 0x8B, 0xEC])
# push ebp; mov ebp, esp (with rex/other encoding)
PROLOGUE_PUSH_EBP_MOV_ALT = bytes([0x55, 0x89, 0xE5])

# CC padding byte (int 3 / debug break)
CC_PADDING = 0xCC

# Minimum CC padding run length to consider as function boundary
MIN_CC_RUN = 1

# ============================================================
# Function Detection Confidence Scores
# ============================================================

# Alignment required before decode_at will manufacture an instruction at a
# direct call target the linear sweep stepped over (see functions.py
# _pass_call_targets, engine.py decode_at).
#
# Only applies to targets with no decoded instruction. Realigning there is
# creating evidence rather than reading it, so it demands corroboration; a
# target that already decoded is accepted as before, whatever its alignment.
#
# 16 because that is what MSVC emits for a function start, and because two
# independent measurements landed on it: seeding unaligned indirect-branch
# targets made Halo 2276 crash earlier (see tools/recomp/icall_feedback.py
# cmd_seeds), and on Steel Battalion LOC only 61 of 103 realigned targets were
# 16-aligned while the rest carried the same signature as that garbage --
# misdecoded call operands inside data, not functions.
#
# Set to 1 to disable the check.
CALL_TARGET_REALIGN_ALIGNMENT = 16

CONFIDENCE_KNOWN = 1.0       # Entry point, known addresses
CONFIDENCE_PROLOGUE = 0.95   # Standard prologue pattern
CONFIDENCE_CALL_TARGET = 0.90  # Destination of a call instruction
CONFIDENCE_TAIL_JUMP = 0.88   # Target of a jmp that leaves its function
CONFIDENCE_CC_BOUNDARY = 0.85  # After CC padding run following ret
# An address that code loads as an immediate (`push offset fn`) but never calls
# or jumps to directly.  Only trusted where no other pass claimed the bytes, so
# it ranks below every direct-evidence method.
CONFIDENCE_ADDRESS_TAKEN = 0.60

# Promote address-taken code in unclaimed gaps to function starts.  Indirect
# entry points -- thread start routines, callbacks, vtable slots -- are reached
# only through a stored pointer, so nothing else marks them.  Without this they
# are absent from the dispatch table and an indirect call to one fails at
# runtime.
DETECT_ADDRESS_TAKEN_GAPS = True

# Also promote code addresses that appear only in data -- vtables and
# function-pointer tables.  A C++ title reaches most virtual methods this way
# and never names them in an instruction.
DETECT_DATA_POINTER_GAPS = True

# Upper bound when reading a jump table, so a mis-identified base cannot
# invent functions from the whole section.
MAX_JUMP_TABLE_ENTRIES = 256

# ============================================================
# Disassembly Engine Settings
# ============================================================

# Chunk size for linear sweep (64 KB)
SWEEP_CHUNK_SIZE = 0x10000

# x86-32 mode
CS_MODE = 32

# ============================================================
# Output Settings
# ============================================================

# Default output directory (relative to tool root)
DEFAULT_OUTPUT_DIR = "generated/analysis"

# Maximum string length to extract from .rdata
MAX_STRING_LENGTH = 256

# Minimum string length to consider valid
MIN_STRING_LENGTH = 4

# ============================================================
# Cache Settings
# ============================================================

CACHE_FILENAME = ".disasm_cache.json"
CACHE_VERSION = 1
