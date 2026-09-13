"""
x86 → C instruction lifter.

Translates individual x86 instructions (and common multi-instruction
patterns like cmp+jcc) into C statements using the recomp_types.h macros.

Register model:
  - eax, ebx, ecx, edx, esi, edi, ebp: uint32_t locals
  - esp: uint32_t local (stack pointer)
  - FPU: shared double g_fp_stack[8] with g_fp_top index

Memory model:
  - MEM8/MEM16/MEM32 macros for memory access at flat addresses
  - Xbox data sections mapped at original VAs
"""

import re
import struct

from .disasm import Instruction, Operand
from .config import is_code_address, is_data_address, va_to_file_offset


# ── Operand formatting ──────────────────────────────────────

def _is_privileged_reg(name):
    """crN / drN / trN -- control, debug and test registers.

    A user-mode title never touches these, so every one that reaches the
    lifter comes from decoding data as code: padding and zero-fill inside an
    executable section, which the sweep turns into plausible-looking garbage.
    That garbage is harmless -- nothing calls it -- right up until it fails to
    compile, and `error C2065: 'cr0': undeclared identifier` says nothing about
    where it came from. Give them the same neutral treatment as the segment
    registers, so a phantom function costs a dead line instead of the build.
    """
    return (name and len(name) == 3 and name[0] in "cdt" and name[1] == "r"
            and name[2].isdigit())


def _fmt_reg(name, size=4):
    """Format a register name as a C expression."""
    if not name:
        return "0"

    # Segment registers → constants
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"0 /* seg:{name} */"

    if _is_privileged_reg(name):
        return f"0 /* {name} */"

    # Map sub-registers to expressions on 32-bit locals
    SUB_REGS = {
        "al": "LO8(eax)", "ah": "HI8(eax)", "ax": "LO16(eax)",
        "bl": "LO8(ebx)", "bh": "HI8(ebx)", "bx": "LO16(ebx)",
        "cl": "LO8(ecx)", "ch": "HI8(ecx)", "cx": "LO16(ecx)",
        "dl": "LO8(edx)", "dh": "HI8(edx)", "dx": "LO16(edx)",
        "si": "LO16(esi)", "di": "LO16(edi)",
        "bp": "LO16(ebp)", "sp": "LO16(esp)",
    }
    if name in SUB_REGS:
        return SUB_REGS[name]
    return name


def _fmt_set_reg(name, value_expr):
    """Format assignment to a register, handling sub-register writes."""
    # Segment registers → no-op
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"/* mov {name}, {value_expr} - segment register */;"

    if _is_privileged_reg(name):
        return f"/* mov {name}, {value_expr} - privileged register */;"

    SET_MAP = {
        "al": f"SET_LO8(eax, {value_expr})",
        "ah": f"SET_HI8(eax, {value_expr})",
        "ax": f"SET_LO16(eax, {value_expr})",
        "bl": f"SET_LO8(ebx, {value_expr})",
        "bh": f"SET_HI8(ebx, {value_expr})",
        "bx": f"SET_LO16(ebx, {value_expr})",
        "cl": f"SET_LO8(ecx, {value_expr})",
        "ch": f"SET_HI8(ecx, {value_expr})",
        "cx": f"SET_LO16(ecx, {value_expr})",
        "dl": f"SET_LO8(edx, {value_expr})",
        "dh": f"SET_HI8(edx, {value_expr})",
        "dx": f"SET_LO16(edx, {value_expr})",
        "si": f"SET_LO16(esi, {value_expr})",
        "di": f"SET_LO16(edi, {value_expr})",
        "bp": f"SET_LO16(ebp, {value_expr})",
        "sp": f"SET_LO16(esp, {value_expr})",
    }
    if name in SET_MAP:
        return SET_MAP[name] + ";"
    return f"{name} = {value_expr};"


def _fmt_imm(val):
    """Format an immediate value as a C hex literal."""
    if val == 0:
        return "0"
    if val <= 9:
        return str(val)
    if val > 0x7FFFFFFF:
        return f"0x{val:08X}u"
    return f"0x{val:X}"


def _mem_accessor(size):
    """Return the MEM macro name for a given operand size."""
    return {1: "MEM8", 2: "MEM16", 4: "MEM32"}.get(size, "MEM32")


def _smem_accessor(size):
    """Return the signed MEM macro for a given operand size."""
    return {
        1: "SMEM8", 2: "SMEM16", 4: "SMEM32", 8: "SMEM64",
    }.get(size, "SMEM32")


def _fmt_fpu_operand(op):
    """Format an x87 memory or stack-register operand for reading."""
    if op.type == "mem":
        accessor = "MEMD" if op.mem_size == 8 else "MEMF"
        return f"{accessor}({_fmt_mem(op)})"
    if op.type == "reg":
        name = op.reg or ""
        if name == "st":
            return "fp_st(0)"
        if name.startswith("st(") and name.endswith(")"):
            return f"fp_st({name[3:-1]})"
    return None


def _fmt_mem(op):
    """Format a memory operand as a C expression (the address computation)."""
    parts = []
    # fs: is the thread block. Relocate it instead of folding it away.
    #
    # Treating fs:[N] as guest address N put the fake TIB at VA 0 -- and VA 0
    # is ordinary Xbox RAM. Conker's XPP pool allocator carves blocks out of
    # 0x80000020..0x80001000 (the physical alias of the first page) and fills
    # each one with 0xCC, so it handed out memory straight on top of the TIB:
    # fs:[0x20] and the D3D cache pointer the title reads at [fs:[0x20]+0x250]
    # both became 0xCCCCCCCC, and the next dispatch through it faulted.
    #
    # Giving fs: its own base puts the block on a page nothing else can be
    # allocated from, which is what a real segment does.
    if getattr(op, "mem_segment", None) == "fs":
        parts.append("RECOMP_FS_BASE")
    if op.mem_base:
        parts.append(_fmt_reg(op.mem_base))
    if op.mem_index:
        idx = _fmt_reg(op.mem_index)
        if op.mem_scale and op.mem_scale > 1:
            parts.append(f"{idx} * {op.mem_scale}")
        else:
            parts.append(idx)
    if op.mem_disp:
        if op.mem_disp < 0:
            # Negative displacement - but we stored unsigned, check sign
            if op.mem_disp > 0x80000000:
                # Actually negative (two's complement)
                signed_disp = op.mem_disp - 0x100000000
                if parts:
                    parts.append(f"- {_fmt_imm(-signed_disp)}")
                else:
                    parts.append(_fmt_imm(op.mem_disp))
            else:
                parts.append(_fmt_imm(op.mem_disp))
        else:
            parts.append(_fmt_imm(op.mem_disp))
    if not parts:
        return "0"
    return " + ".join(parts)


def _fmt_mem_read(op):
    """Format reading from a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    addr = _fmt_mem(op)
    return f"{accessor}({addr})"


def _fmt_mem_write(op, value_expr):
    """Format writing to a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    addr = _fmt_mem(op)
    return f"{accessor}({addr}) = {value_expr};"


_REG_WIDTH = {
    "al": 1, "ah": 1, "bl": 1, "bh": 1, "cl": 1, "ch": 1, "dl": 1, "dh": 1,
    "ax": 2, "bx": 2, "cx": 2, "dx": 2, "si": 2, "di": 2, "bp": 2, "sp": 2,
}


def _operand_width(op):
    """Byte width of an operand, or None when it carries no width of its own.

    Memory operands know their size; registers imply it by name; immediates do
    not have one and take it from the other operand. Getting this wrong makes a
    narrow compare test a widened value - "cmp word ptr [x], -1" against
    0xFFFFFFFF never matches, because a 16-bit -1 is 0xFFFF.
    """
    if op.type == "mem":
        return getattr(op, "mem_size", None) or 4
    if op.type == "reg":
        return _REG_WIDTH.get(str(op.reg).lower(), 4)
    return None


def _fmt_operand_read(op):
    """Format reading any operand type."""
    if op.type == "reg":
        return _fmt_reg(op.reg)
    elif op.type == "imm":
        return _fmt_imm(op.imm)
    elif op.type == "mem":
        return _fmt_mem_read(op)
    return "/* unknown operand */"


def _fmt_operand_write(op, value_expr):
    """Format writing to any operand type. Returns a C statement."""
    if op.type == "reg":
        return _fmt_set_reg(op.reg, value_expr)
    elif op.type == "mem":
        return _fmt_mem_write(op, value_expr)
    return f"/* cannot write to {op.type} */;"


# ── Condition code mapping ───────────────────────────────────

# Maps jcc mnemonic → (cmp_macro, test_macro, description)
# cmp_macro takes (lhs, rhs), test_macro takes (lhs, rhs)
COND_MAP = {
    "je":   ("CMP_EQ",  "TEST_Z",  "equal / zero"),
    "jz":   ("CMP_EQ",  "TEST_Z",  "zero"),
    "jne":  ("CMP_NE",  "TEST_NZ", "not equal / not zero"),
    "jnz":  ("CMP_NE",  "TEST_NZ", "not zero"),
    "jb":   ("CMP_B",   None,      "below (unsigned <)"),
    "jnae": ("CMP_B",   None,      "below"),
    "jae":  ("CMP_AE",  None,      "above or equal (unsigned >=)"),
    "jnb":  ("CMP_AE",  None,      "above or equal"),
    "jbe":  ("CMP_BE",  None,      "below or equal (unsigned <=)"),
    "jna":  ("CMP_BE",  None,      "below or equal"),
    "ja":   ("CMP_A",   None,      "above (unsigned >)"),
    "jl":   ("CMP_L",   "TEST_S",  "less (signed <)"),
    "jge":  ("CMP_GE",  None,      "greater or equal (signed >=)"),
    "jle":  ("CMP_LE",  None,      "less or equal (signed <=)"),
    "jg":   ("CMP_G",   None,      "greater (signed >)"),
    "js":   (None,       "TEST_S",  "sign (negative)"),
    "jns":  (None,       None,      "not sign (positive)"),
    "jo":   (None,       None,      "overflow"),
    "jno":  (None,       None,      "not overflow"),
    "jp":   (None,       None,      "parity"),
    "jnp":  (None,       None,      "not parity"),
    "jecxz": (None,      None,      "ecx is zero"),
    "jcxz":  (None,      None,      "cx is zero"),
}

def _norm_mnem(m):
    """Strip a lock prefix.

    LOCK changes atomicity, not semantics, and this model runs one
    guest thread at a time through each instruction -- so "lock cmpxchg"
    lifts exactly like "cmpxchg".  Dropping the instruction instead, as
    an unrecognised mnemonic, silently deletes the write.
    """
    if m.startswith("lock "):
        return m[5:]
    return m


# Instructions that set arithmetic flags (primary set, fully handled)
FLAG_SETTERS = frozenset({
    "cmp", "test", "sub", "add", "and", "or", "xor",
    "cmpxchg",   # flags are the pre-operation EAX vs DEST comparison
    "xadd",     # snapshots preserve the addition flags across both writes
    "inc", "dec", "neg", "shl", "shr", "sar", "imul", "adc", "sbb",
    "comiss", "comisd", "ucomiss", "ucomisd",  # SSE float compare
})

# Additional instructions that modify EFLAGS (tracked but handled as generic)
_EFLAGS_SETTERS = frozenset({
    "shld", "shrd", "rol", "ror", "rcl", "rcr",  # Shifts/rotates set CF
    "bsf", "bsr",       # Bit scan sets ZF
    "bt", "bts", "btr", "btc",  # Bit test sets CF
})

# Instructions with undefined/unpredictable flags (clear tracking)
_FLAGS_UNDEFINED = frozenset({
    "mul", "div", "idiv",  # Flags partially undefined
    "rdtsc", "cpuid",      # Special instructions
})

# Instructions that do NOT modify EFLAGS (preserve flag tracking)
_EFLAGS_PRESERVE = frozenset({
    # General-purpose data movement / stack
    "mov", "lea", "push", "pop", "nop", "leave", "ret",
    "movzx", "movsx", "xchg", "bswap",
    "cdq", "cwde", "cbw", "cwd",
    "lahf",
    "not",  # NOT does not modify flags
    "call",
    "int3", "int", "wait",
    "cld", "std", "cli", "sti",
    "pushfd", "popfd", "pushal",
    "sgdt", "ljmp", "sfence",
    # SSE scalar float
    "movss", "movsd",
    "addss", "subss", "mulss", "divss",
    "minss", "maxss", "sqrtss", "rsqrtss", "rcpss",
    "addsd", "subsd", "mulsd", "divsd",
    "minsd", "maxsd", "sqrtsd",
    "cvtsi2ss", "cvtss2si", "cvttss2si",
    "cvtsi2sd", "cvtsd2si", "cvttsd2si",
    "cvtss2sd", "cvtsd2ss",
    "cmpss", "cmpsd",
    "cmpltss", "cmpeqss", "cmpleps", "cmpneqss",
    # SSE packed float
    "movaps", "movups", "movlps", "movhps", "movlhps", "movhlps",
    "addps", "subps", "mulps", "divps",
    "minps", "maxps", "sqrtps", "rsqrtps", "rcpps",
    "shufps", "unpcklps", "unpckhps",
    "andps", "orps", "xorps", "andnps",
    "cmpps", "cmpneqps",
    "movmskps",
    # SSE2 packed double
    "movapd", "movupd",
    "addpd", "subpd", "mulpd", "divpd",
    # SSE/MMX integer
    "movd", "movq", "movntq",
    "emms",
    "paddb", "paddw", "paddd", "paddq",
    "psubb", "psubw", "psubd",
    "pmullw", "pmulhw", "pmulhuw", "pmaddwd",
    "pand", "pandn", "por", "pxor",
    "pcmpeqb", "pcmpeqw", "pcmpeqd",
    "pcmpgtb", "pcmpgtw", "pcmpgtd",
    "psllw", "pslld", "psllq",
    "psrlw", "psrld", "psrlq",
    "psraw", "psrad",
    "pshufw", "pshufd", "pshufhw", "pshuflw",
    "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
    "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
    "packsswb", "packssdw", "packuswb",
    "pmovmskb",
    # String operations (without rep prefix)
    "stosb", "stosw", "stosd",
    "movsb", "movsw", "movsd",
    "lodsb", "lodsw", "lodsd",
    # Prefetch hints
    "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
})


def _make_condition(jcc, flag_setter, flag_ops, fused=False):
    """
    Generate a C condition expression for a jcc based on what set the flags.
    Returns (cond_expr, description) or None.
    """
    cond_info = COND_MAP.get(jcc)
    if not cond_info:
        return None
    cmp_macro, test_macro, desc = cond_info

    # A cmp/test that is not fused with its jcc snapshots its operands into
    # _fa/_fb (zero-extended) and _fas/_fbs (sign-extended) at the point the
    # comparison happens. Use those rather than re-reading registers that may
    # since have changed.
    SIGNED = {"CMP_L", "CMP_LE", "CMP_G", "CMP_GE", "TEST_S"}
    if flag_setter in ("cmp", "test", "cmpxchg") and len(flag_ops) >= 2:
        signed = (cmp_macro in SIGNED) or (test_macro in SIGNED)
        lhs, rhs = ("_fas", "_fbs") if signed else ("_fa", "_fb")
    elif len(flag_ops) >= 2:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = _fmt_operand_read(flag_ops[1])
    elif len(flag_ops) == 1:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = None
    else:
        lhs = None
        rhs = None

    # ── FPU compare-to-EFLAGS and sahf: no standard operands ──
    if flag_setter in ("fcompi", "fcomip", "fucomi", "fucompi",
                        "fucomip", "fcomi", "sahf"):
        fpu_cmp_map = {
            "ja": ">", "jnbe": ">",
            "jae": ">=", "jnb": ">=", "jnc": ">=",
            "jb": "<", "jnae": "<", "jc": "<",
            "jbe": "<=", "jna": "<=",
            "je": "==", "jz": "==",
            "jne": "!=", "jnz": "!=",
        }
        op = fpu_cmp_map.get(jcc)
        if op:
            return f"(g_fp_cmp {op} 0) /* {flag_setter} */", desc
        if jcc == "jp":
            return "0 /* fpu: unordered/NaN */", desc
        if jcc == "jnp":
            return "1 /* fpu: ordered */", desc
        return None

    # If no operands available for other flag-setters, can't generate condition
    if lhs is None:
        return None

    # These two flag producers have the same ZF, including at a CFG join.
    # Keep their expressions identical so agree() can resolve the branch.
    # XMV coefficient decoding joins `test eax,eax` (normal sign bit) with
    # `cmp [local],0` (escape sign bit); falling back to _flags there negated
    # every positive coefficient. _fa is the width-masked snapshot, so this
    # also survives later writes to the register or local that was tested.
    if jcc in ("je", "jz", "jne", "jnz") and len(flag_ops) >= 2:
        a, b = flag_ops[:2]
        self_test = (flag_setter == "test" and a.type == b.type == "reg"
                     and a.reg == b.reg)
        compare_zero = (flag_setter == "cmp" and b.type == "imm"
                        and b.imm == 0)
        if self_test or compare_zero:
            op = "==" if jcc in ("je", "jz") else "!="
            return f"(_fa {op} 0)", desc

    # ── comiss/ucomiss: float comparison, sets CF/ZF/PF ──
    if flag_setter in ("comiss", "comisd", "ucomiss", "ucomisd"):
        # Only re-read the operands when the branch is adjacent. Otherwise the
        # same hazard the _fa/_fb snapshot exists to prevent applies here, and
        # it bit: MSVC emits
        #     comiss xmm0, [esp+edx*4+0x28]
        #     lea    edx, [esp+edx*4+0x28]
        #     jbe    ...
        # and `lea` preserves EFLAGS, so the flag state survived it while edx
        # did not. The branch re-evaluated [esp+edx*4+0x28] against an edx that
        # was now itself a pointer, read guest 0x054FF434, and faulted.
        #
        # The non-adjacent form reads the compare's own result instead, which
        # the emitter materialised into _ah while the operands were still in
        # hand. AH is SF:ZF:0:AF:0:PF:1:CF, so CF is 0x01, PF 0x04, ZF 0x40 --
        # and comiss branches only ever ask about those three.
        if not fused:
            AH = {
                "jb": "(_ah & 0x01u)", "jnae": "(_ah & 0x01u)",
                "jc": "(_ah & 0x01u)",
                "jae": "(!(_ah & 0x01u))", "jnb": "(!(_ah & 0x01u))",
                "jnc": "(!(_ah & 0x01u))",
                "je": "(_ah & 0x40u)", "jz": "(_ah & 0x40u)",
                "jne": "(!(_ah & 0x40u))", "jnz": "(!(_ah & 0x40u))",
                "jbe": "(_ah & 0x41u)", "jna": "(_ah & 0x41u)",
                "ja": "(!(_ah & 0x41u))", "jnbe": "(!(_ah & 0x41u))",
                "jp": "(_ah & 0x04u)", "jpe": "(_ah & 0x04u)",
                "jnp": "(!(_ah & 0x04u))", "jpo": "(!(_ah & 0x04u))",
            }
            if jcc in AH:
                return AH[jcc], desc + " [via AH]"
            return None
        def _sse_op(op):
            if op.type == "reg" and op.reg and op.reg.startswith("xmm"):
                return f"{op.reg}.f[0]"
            elif op.type == "reg":
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            return _fmt_operand_read(op)
        a = _sse_op(flag_ops[0]) if len(flag_ops) >= 1 else "0.0f"
        b = _sse_op(flag_ops[1]) if len(flag_ops) >= 2 else "0.0f"
        # comiss uses unsigned condition codes (CF, ZF)
        if jcc in ("ja", "jnbe"):
            return f"({a} > {b})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({a} >= {b})", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({a} < {b})", desc
        if jcc in ("jbe", "jna"):
            return f"({a} <= {b})", desc
        if jcc in ("je", "jz"):
            return f"({a} == {b})", desc
        if jcc in ("jne", "jnz"):
            return f"({a} != {b})", desc
        if jcc == "jp":
            return f"0 /* {jcc}: unordered/NaN */", desc
        if jcc == "jnp":
            return f"1 /* {jcc}: ordered */", desc
        return None

    # SF is the sign bit of the result at the OPERAND's width, not at 32 bits.
    # `test dl, dl; jns` asks about bit 7; evaluating the zero-extended byte as
    # an int32 makes 0x80..0xFF look positive and the branch always goes the
    # same way. Same defect the signed compares had before the width-aware
    # CMP_L/CMP_G landed -- js/jns were simply missed at the time.
    _sf_width = _operand_width(flag_ops[0]) if flag_ops else None
    if _sf_width is None and len(flag_ops) > 1:
        _sf_width = _operand_width(flag_ops[1])
    _sf_cast = {1: "(int8_t)", 2: "(int16_t)"}.get(_sf_width, "(int32_t)")

    # ── cmp: flags from (a - b), operands unchanged ──
    # cmpxchg snapshots EAX into _fa and the destination into _fb, so every
    # condition reads exactly like a cmp of those two.
    if flag_setter in ("cmp", "cmpxchg"):
        if cmp_macro:
            return f"{cmp_macro}({lhs}, {rhs})", desc
        if jcc == "js":
            return f"({_sf_cast}(({lhs}) - ({rhs})) < 0)", desc
        if jcc == "jns":
            return f"({_sf_cast}(({lhs}) - ({rhs})) >= 0)", desc
        if jcc == "jp":
            return f"RECOMP_PARITY8(({lhs}) - ({rhs}))", desc
        if jcc == "jnp":
            return f"(!RECOMP_PARITY8(({lhs}) - ({rhs})))", desc
        return None

    # ── test: flags from (a & b), operands unchanged ──
    if flag_setter == "test":
        if test_macro:
            return f"{test_macro}({lhs}, {rhs})", desc
        if cmp_macro:
            return f"{cmp_macro}({lhs} & {rhs}, 0)", desc
        if jcc == "js":
            return f"({_sf_cast}(({lhs}) & ({rhs})) < 0)", desc
        if jcc == "jns":
            return f"({_sf_cast}(({lhs}) & ({rhs})) >= 0)", desc
        if jcc == "jo":
            return "0", desc  # OF=0 after test
        if jcc == "jno":
            return "1", desc
        if jcc == "jp":
            # PF from (a & b) low byte. This is the x87 float branch idiom
            # `fnstsw ax; test ah, mask; jp/jnp` (fnstsw put the compare bits
            # into ah). jp jumps on even parity (PF=1).
            return f"RECOMP_PARITY8(({lhs}) & ({rhs}))", desc
        if jcc == "jnp":
            return f"(!RECOMP_PARITY8(({lhs}) & ({rhs})))", desc
        return None

    # ── sub: a = a - b, flags from (a_orig - b) ──
    #
    # `lhs` is the result, already written back at the operand's width, so
    # every test below has to be evaluated at that width too.
    #
    # Two faults lived here, and Conker's PAGELK inflate tripped both with
    # `mov ch, 0x10 / ... / sub ch, cl / jl refill`:
    #
    #   * The signed cases were unreachable. cmp_macro is set for jl/jge/jle/jg
    #     too, so the "reconstruct the original operand" fallback below caught
    #     them first and answered a signed question with an unsigned
    #     reconstruction.
    #   * Neither that reconstruction nor the (int32_t) casts knew the operand
    #     width. For `sub ch, cl` with ch=4, cl=12 the byte result is 0xF8 --
    #     negative, so jl must be taken -- but 0xF8 + 12 in uint32_t is 260,
    #     and (int32_t)0xF8 is +248. Either way the branch was never taken, so
    #     the decoder never refilled its bit buffer and ran off the end of its
    #     output buffer, ~34MB past it.
    #
    # Signed ordering here uses SF alone and ignores OF. That is exact whenever
    # the subtraction cannot signed-overflow, which holds for a bit counter; a
    # fully general jl would have to compute OF as well.
    if flag_setter == "sub":
        _w_mask = {1: " & 0xFFu", 2: " & 0xFFFFu"}.get(_sf_width, "")
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("js", "jl", "jnge"):
            return f"({_sf_cast}({lhs}) < 0)", desc
        if jcc in ("jns", "jge", "jnl"):
            return f"({_sf_cast}({lhs}) >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"({_sf_cast}({lhs}) <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"({_sf_cast}({lhs}) > 0)", desc
        # Unsigned tests read CF, the borrow, so reconstruct the original
        # minuend a = (result + b) -- at the operand's width, not 32 bits.
        if rhs:
            orig = f"((({lhs}) + ({rhs})){_w_mask})"
            r = f"((uint32_t)({rhs}){_w_mask})"
            if cmp_macro:
                return f"{cmp_macro}({orig}, {r})", desc
            if jcc in ("jb", "jnae"):
                return f"({orig} < {r})", desc
            if jcc in ("jae", "jnb"):
                return f"({orig} >= {r})", desc
        return None

    # ── add: a = a + b, flags from result ──
    #
    # Same width rule as sub above: `lhs` is the result at the operand's width,
    # so a byte add that lands on 0x80..0xFF is negative and (int32_t) would
    # call it positive. Signed ordering uses SF alone (see the sub comment).
    if flag_setter == "add":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("js", "jl", "jnge"):
            return f"({_sf_cast}({lhs}) < 0)", desc
        if jcc in ("jns", "jge", "jnl"):
            return f"({_sf_cast}({lhs}) >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"({_sf_cast}({lhs}) <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"({_sf_cast}({lhs}) > 0)", desc
        # CF for an add is "the result wrapped", i.e. result < either addend.
        # Both are already at the operand's width, so no mask is needed.
        if jcc in ("jb", "jnae", "jc"):
            return f"({lhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} >= (uint32_t){rhs})", desc
        return None

    # ── adc/sbb: result-based (like add/sub but with carry) ──
    if flag_setter in ("adc", "sbb"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── and/or/xor: result-based, CF=0, OF=0 ──
    if flag_setter in ("and", "or", "xor"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("js", "jl"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jns", "jge"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc == "jle":
            return f"((int32_t){lhs} <= 0)", desc
        if jcc == "jg":
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jb", "jnae", "jbe", "jna"):
            return "0", desc  # CF=0 after and/or/xor
        if jcc in ("jae", "jnb", "ja", "jnbe"):
            return "1", desc
        return None

    # ── dec/inc: result-based, CF unchanged ──
    if flag_setter in ("dec", "inc"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"({_sf_cast}{lhs} < 0)", desc
        if jcc == "jns":
            return f"({_sf_cast}{lhs} >= 0)", desc
        if jcc in ("jl", "jle", "jg", "jge"):
            cast = "(int32_t)" + lhs
            op = {"jl": "<", "jle": "<=", "jg": ">", "jge": ">="}[jcc]
            return f"({cast} {op} 0)", desc
        return None

    # ── neg: flags from (0 - a_orig), result is -a ──
    if flag_setter == "neg":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            # CF=1 unless original was 0
            return f"({lhs} != 0)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} == 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        return None

    # ── shift: result-based ──
    if flag_setter in ("shl", "shr", "sar"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── shld/shrd: double-precision shift, result-based ──
    if flag_setter in ("shld", "shrd"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── rol/ror/rcl/rcr: rotation, only CF/OF affected ──
    if flag_setter in ("rol", "ror", "rcl", "rcr"):
        # ZF/SF not modified by rotations - can't resolve most conditions
        return None

    # ── bsf/bsr: bit scan, ZF set if source is zero ──
    if flag_setter in ("bsf", "bsr"):
        if jcc in ("je", "jz"):
            return "_flags", desc
        if jcc in ("jne", "jnz"):
            return "!_flags", desc
        return None

    # ── bt/bts/btr/btc: bit test, sets CF ──
    if flag_setter in ("bt", "bts", "btr", "btc"):
        if rhs is None:
            return None
        if jcc in ("jb", "jnae", "jc"):
            return f"(({lhs} >> ({rhs} & 31)) & 1)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"!(({lhs} >> ({rhs} & 31)) & 1)", desc
        return None

    # ── xadd: exchange and add, flags from addition ──
    if flag_setter == "xadd":
        size = _operand_width(flag_ops[0]) or 4
        mask = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}[size]
        sign = f"0x{1 << (size * 8 - 1):X}u"
        result = f"((_fa + _fb) & {mask})"
        zf = f"({result} == 0)"
        sf = f"(({result} & {sign}) != 0)"
        cf = f"(((uint64_t)_fa + _fb) > {mask})"
        of = f"((~(_fa ^ _fb) & (_fa ^ {result}) & {sign}) != 0)"
        pf = f"((0x9669u >> (({result} ^ ({result} >> 4)) & 15)) & 1)"
        conditions = {
            "je": zf, "jz": zf, "jne": f"!{zf}", "jnz": f"!{zf}",
            "js": sf, "jns": f"!{sf}", "jo": of, "jno": f"!{of}",
            "jb": cf, "jc": cf, "jnae": cf,
            "jae": f"!{cf}", "jnb": f"!{cf}", "jnc": f"!{cf}",
            "jbe": f"({cf} || {zf})", "jna": f"({cf} || {zf})",
            "ja": f"(!{cf} && !{zf})", "jnbe": f"(!{cf} && !{zf})",
            "jl": f"({sf} != {of})", "jnge": f"({sf} != {of})",
            "jge": f"({sf} == {of})", "jnl": f"({sf} == {of})",
            "jle": f"({zf} || ({sf} != {of}))", "jng": f"({zf} || ({sf} != {of}))",
            "jg": f"(!{zf} && ({sf} == {of}))", "jnle": f"(!{zf} && ({sf} == {of}))",
            "jp": pf, "jpe": pf, "jnp": f"!{pf}", "jpo": f"!{pf}",
        }
        cond = conditions.get(jcc)
        return (cond, desc) if cond is not None else None

    # ── repe cmpsb / repne scasb: string comparison ──
    if "cmps" in flag_setter or "scas" in flag_setter:
        if jcc in ("je", "jz"):
            return "(_flags != 0)", desc
        if jcc in ("jne", "jnz"):
            return "(_flags == 0)", desc
        return None

    return None


def _make_setcc_value(setcc_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a SETcc instruction."""
    cc = setcc_mnemonic[3:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


def _make_cmovcc_cond(cmov_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a CMOVcc instruction."""
    cc = cmov_mnemonic[4:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


# ── Pattern matching for flag-setter + jcc ────────────────────

def _emit_cond_goto(cond_expr, jcc, desc, target, lifter):
    """Emit a conditional goto or call for a jump target."""
    if target is None:
        return f"if ({cond_expr}) {{ /* {jcc}: {desc} - indirect */ }}"
    if lifter and lifter._is_external_target(target):
        # Conditional tail call: same frame bridge as the unconditional tail
        # jmp in _lift_jmp, applied only on the taken path.
        if target in lifter.manual_functions:
            return (f"if ({cond_expr}) {{ g_seh_ebp = ebp; "
                    f"RECOMP_ITAIL(0x{target:08X}u); return; }}"
                    f" /* {jcc}: {desc}, manual tail */")
        name = lifter._call_target_name(target)
        return (f"if ({cond_expr}) {{ g_seh_ebp = ebp; {name}(); return; }}"
                f" /* {jcc}: {desc} */")
    return f"if ({cond_expr}) goto loc_{target:08X}; /* {jcc}: {desc} */"


def try_match_cmp_jcc(insns, idx, lifter=None):
    """
    Try to match a cmp/test + jcc pattern starting at insns[idx].
    Returns (c_statement, num_consumed) or None.
    """
    if idx + 1 >= len(insns):
        return None

    first = insns[idx]
    second = insns[idx + 1]

    if first.mnemonic not in ("cmp", "test") or not second.is_cond_jump:
        return None

    if len(first.operands) < 2:
        return None

    # Adjacent: nothing runs between the compare and the branch, so reading
    # the operands directly is safe and reads better than the flag bits.
    result = _make_condition(second.mnemonic, first.mnemonic, first.operands,
                             fused=True)
    if not result:
        return None

    cond_expr, desc = result
    target = second.jump_target
    stmt = _emit_cond_goto(cond_expr, second.mnemonic, desc, target, lifter)
    return (stmt, 2)


# ── Single instruction lifting ───────────────────────────────

# MSVC's __SEH_prolog establishes the caller's frame pointer, so the lifter has
# to know which function it is. The address is per-title, and hardcoding it
# meant every other game silently got no frame set up after the call: ebp kept
# whatever stale value it had, and the first ebp-relative local access read
# through it. In Halo that surfaced as a read of 0xFFFFFFFC (ebp=0, [ebp-4]).
#
# Both helpers are compiler boilerplate with distinctive bodies, so detect them
# rather than asking every project to look them up by hand.
#
#   __SEH_prolog   mov eax, fs:[0]        64 A1 00 00 00 00
#                  lea ebp, [esp+N]       8D 6C 24 ??
#   __SEH_epilog   mov fs:[0], ecx        64 89 0D 00 00 00 00
#                  leave; push ecx; ret   C9 51 C3
#
# The `lea` displacement is NOT fixed at 0x10: it is the distance from the new
# esp up to the saved-ebp slot, so it tracks the size of the record the prolog
# builds. Conker links one variant that pushes a dword fewer and uses 0x0C.
# Matching the displacement byte literally found that prolog not at all -- and
# a missing prolog is silent: nothing reads back g_seh_ebp, and every SEH
# function that used it then ran on its *caller's* stale ebp, reading and
# writing its own locals through someone else's frame.
#
# Resist tightening this with the fs:[0] store: the variants disagree on how
# they install the record (`mov fs:[0], esp` = 64 89 25, versus `lea eax,
# [ebp-0x10]` then `mov fs:[0], eax` = 64 A3), so any one encoding excludes a
# real prolog. Reading fs:[0] and then pointing ebp into the stack frame is
# the pair that identifies this helper; the epilog does neither (it reads no
# fs:[0] and reaches its frame with `leave`), so the two do not collide.
_SEH_PROLOG_MARKERS = (b"\x64\xa1\x00\x00\x00\x00", b"\x8d\x6c\x24")
_SEH_EPILOG_MARKERS = (b"\x64\x89\x0d\x00\x00\x00\x00", b"\xc9\x51\xc3")

# Both are tiny; a large match is something else that happens to touch fs:[0].
_SEH_PROLOG_MAX_SIZE = 128
_SEH_EPILOG_MAX_SIZE = 64


def _as_addr_set(value):
    """Accept None, one address, or an iterable of them; return a frozenset."""
    if value is None:
        return frozenset()
    if isinstance(value, int):
        return frozenset((value,))
    return frozenset(a for a in value if a is not None)


def detect_seh_helpers(func_db, xbe_data, verbose=False):
    """Locate the __SEH_prolog / __SEH_epilog helpers in the target binary.

    Returns (prologs, epilogs) as tuples of addresses, each possibly empty --
    empty is normal for a title whose CRT does not use them.

    Plural because a title can genuinely link more than one. Conker carries
    two prologs from different CRT objects: 0x00470A34 takes its frame size
    and scope table as pushed arguments, 0x00472094 takes the scope table in
    eax. Both are live, and stopping at the first one found left every caller
    of the other with no frame read-back at all.
    """
    from .config import va_to_file_offset

    prologs = []
    epilogs = []

    def _size_of(info):
        # "end" is a hex string in functions.json but BatchTranslator rewrites
        # it to an int in place, so accept either.
        try:
            size = int(info.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size:
            return size
        end = info.get("end")
        if isinstance(end, str):
            try:
                end = int(end, 16)
            except ValueError:
                return 0
        return (end - addr) if isinstance(end, int) else 0

    for addr in sorted(func_db):
        info = func_db[addr]
        size = _size_of(info)
        if size <= 0 or size > _SEH_PROLOG_MAX_SIZE:
            continue

        offset = va_to_file_offset(addr)
        if offset is None or xbe_data is None or offset + size > len(xbe_data):
            continue
        body = xbe_data[offset:offset + size]

        if (size <= _SEH_PROLOG_MAX_SIZE
                and all(m in body for m in _SEH_PROLOG_MARKERS)):
            prologs.append(addr)
        elif (size <= _SEH_EPILOG_MAX_SIZE
                and all(m in body for m in _SEH_EPILOG_MARKERS)):
            epilogs.append(addr)

    if verbose:
        import sys
        fmt = lambda a: ", ".join(f"0x{x:08X}" for x in a) if a else "not found"
        print(f"  SEH helpers: __SEH_prolog {fmt(prologs)}, "
              f"__SEH_epilog {fmt(epilogs)}", file=sys.stderr)

    return tuple(prologs), tuple(epilogs)


class Lifter:
    """Translates x86 instructions to C statements."""

    def __init__(self, func_db=None, label_db=None, abi_db=None, xbe_data=None,
                 seh_prolog=None, seh_epilog=None, manual_functions=None):
        """
        func_db: dict of func_addr → func_info (for naming call targets)
        label_db: dict of addr → name (for kernel imports, etc.)
        abi_db: dict of addr → ABI info (for calling conventions)
        xbe_data: raw XBE file bytes (for reading jump tables)
        seh_prolog/seh_epilog: override the detected __SEH_prolog/__SEH_epilog
        manual_functions: addresses replaced through recomp_lookup_manual
        """
        self.func_db = func_db or {}
        self.label_db = label_db or {}
        self.abi_db = abi_db or {}
        self.xbe_data = xbe_data
        self.manual_functions = set(manual_functions or ())
        self._fp_top = 0  # FPU stack top index
        self.func_start = 0  # Set per-function by translator
        self.func_end = 0
        self.needs_flags = True  # Translator disables snapshots with no consumer
        self.needs_zf = False  # Capture divergent integer flags at CFG joins
        self.needs_cf = False  # Set per-function by translator (has adc/sbb)
        self.needs_ah = False  # Set per-function by translator (has lahf)
        self.publishes_ebp = False  # Set per-function: has a real frame
        self.trace_exit_name = None  # Set per-function when traced
        # Every direct call target we emit a name for, as {addr: name}. The
        # batch translator diffs this against the functions it actually defined
        # so it can stub out the remainder (see translate_batch_split).
        self.referenced_calls = {}

        # Detect if either is missing, so overriding one does not silently
        # leave the other unset -- that is the bug this whole path fixes.
        if (seh_prolog is None or seh_epilog is None) and self.func_db:
            found_prolog, found_epilog = detect_seh_helpers(self.func_db, xbe_data)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        # A title may link several of each, and the CLI override passes one
        # bare address, so normalise both shapes to a set.
        self.SEH_PROLOG = _as_addr_set(seh_prolog)
        self.SEH_EPILOG = _as_addr_set(seh_epilog)
        self.SEH_HELPERS = self.SEH_PROLOG | self.SEH_EPILOG
        self.jump_table_targets = {}

    def _call_target_name(self, addr):
        """Get the name for a call target address.

        func_db wins over label_db. The function definition is emitted from
        func_db, so consulting labels first meant a renamed function was
        *defined* as cseries__sub_0008DB80 but *called* as sub_0008DB80 -- the
        disassembler's generic auto-label -- and the link failed on every
        function any naming pass had touched. Labels still cover call targets
        that are not known function starts.
        """
        if addr in self.func_db:
            name = self.func_db[addr].get("name", f"sub_{addr:08X}")
        elif addr in self.label_db:
            name = self.label_db[addr]
        else:
            name = f"sub_{addr:08X}"
        self.referenced_calls[addr] = name
        return name

    def lift_instruction(self, insn):
        """
        Translate a single x86 instruction to one or more C statements.
        Returns a list of C statement strings.
        """
        m = _norm_mnem(insn.mnemonic)
        ops = insn.operands
        nops = len(ops)

        # ── NOP ──
        if m == "nop" or (m == "lea" and nops == 2 and
                          ops[0].type == "reg" and ops[1].type == "mem" and
                          ops[1].mem_base == ops[0].reg and
                          not ops[1].mem_index and ops[1].mem_disp == 0):
            return [f"/* nop */"]

        # ── Data movement ──
        if m == "mov":
            return self._lift_mov(insn, ops)
        if m == "movzx":
            return self._lift_movzx(insn, ops)
        if m == "movsx":
            return self._lift_movsx(insn, ops)
        if m == "lea":
            return self._lift_lea(insn, ops)
        if m == "xchg":
            return self._lift_xchg(insn, ops)
        if m == "cmpxchg":
            return self._lift_cmpxchg(insn, ops)
        if m == "xadd":
            return self._lift_xadd(insn, ops)

        # ── Stack ──
        if m == "push":
            return self._lift_push(insn, ops)
        if m == "pop":
            return self._lift_pop(insn, ops)

        # ── Arithmetic ──
        if m in ("add", "sub", "and", "or", "xor"):
            return self._lift_alu_binop(insn, ops, m)
        if m in ("inc", "dec"):
            return self._lift_inc_dec(insn, ops, m)
        if m == "neg":
            return self._lift_neg(insn, ops)
        if m == "not":
            return self._lift_not(insn, ops)
        if m == "imul":
            return self._lift_imul(insn, ops)
        if m in ("mul", "div", "idiv"):
            return self._lift_muldiv(insn, ops, m)
        if m == "sbb":
            return self._lift_sbb(insn, ops)
        if m == "adc":
            return self._lift_adc(insn, ops)
        if m in ("shl", "sal"):
            return self._lift_shift(insn, ops, "<<")
        if m == "shr":
            return self._lift_shift(insn, ops, ">>")
        if m == "sar":
            return self._lift_sar(insn, ops)
        if m in ("rol", "ror"):
            return self._lift_rotate(insn, ops, m)
        if m in ("bsf", "bsr"):
            return self._lift_bit_scan(ops, m)

        # ── Comparison / test (standalone, not part of cmp+jcc pattern) ──
        if m == "cmp":
            return self._lift_cmp(insn, ops)
        if m == "test":
            return self._lift_test(insn, ops)

        # ── Control flow ──
        if m == "call":
            return self._lift_call(insn, ops)
        if m in ("ret", "retn", "retf"):
            return self._lift_ret(insn, ops)
        if m == "jmp":
            return self._lift_jmp(insn, ops)
        if insn.is_cond_jump:
            return self._lift_jcc(insn)

        # ── String operations ──
        if m.startswith("rep ") or m.startswith("repe ") or m.startswith("repne "):
            return self._lift_rep_string(insn, m)
        if m in ("movsb", "movsd", "movsw", "stosb", "stosd", "stosw",
                 "lodsb", "lodsd", "lodsw"):
            return self._lift_string_op(insn, m)
        if m == "wait":
            return ["/* wait - FPU sync */"]

        # ── Misc ──
        if m == "cdq":
            return ["edx = ((int32_t)eax < 0) ? 0xFFFFFFFF : 0; /* cdq */"]
        if m == "cwde":
            return ["eax = SX16(eax); /* cwde */"]
        if m == "cbw":
            return ["SET_LO16(eax, SX8(eax)); /* cbw */"]
        if m == "bswap" and nops >= 1 and ops[0].type == "reg":
            r = _fmt_reg(ops[0].reg)
            return [f"{r} = BSWAP32({r}); /* bswap */"]
        # ── Bit test and modify ──
        # 386 instructions, so real Xbox code has them. The CRT's float-to-int
        # helper uses `btr` to clear a rounding-control bit of the x87 control
        # word, which is exactly the _control87 shape. Unhandled, they lifted to
        # a comment and the bit was silently left alone.
        if m in ("bt", "btr", "bts", "btc") and nops >= 2:
            dst, bit = ops[0], ops[1]
            index = (f"({_fmt_imm(bit.imm)})" if bit.type == "imm"
                     else f"({_fmt_operand_read(bit)} & 31)")
            value = _fmt_operand_read(dst)
            out = []
            if self.needs_cf:
                out.append(f"_cf = (int)(({value} >> {index}) & 1u);"
                           f" /* {m}: CF = bit */")
            update = {"btr": f"{value} & ~(1u << {index})",
                      "bts": f"{value} | (1u << {index})",
                      "btc": f"{value} ^ (1u << {index})"}.get(m)
            if update:
                out.append(_fmt_operand_write(dst, f"({update})")
                           + f" /* {m} */")
            elif not out:
                out.append(f"/* bt {insn.op_str}: no CF consumer */")
            return out

        if m == "int3":
            return ["__debugbreak(); /* int3 */"]
        if m in ("leave",):
            return ["esp = ebp;", "POP32(esp, ebp); /* leave */"]
        if m in ("cld", "std"):
            # Not a comment: string instructions step by this. Dropping it
            # makes a backwards scan run forwards, which is how the CRT's
            # strrchr came to return NULL for every path it was given.
            return [("_df = 1; /* cld */" if m == "cld"
                     else "_df = -1; /* std */")]
        if m == "lahf":
            if not self.needs_ah:
                return ["/* lahf: no consumer of AH in this function */"]
            return ["SET_HI8(eax, _ah); /* lahf */"]
        if m == "sahf":
            return ["/* sahf - store AH to flags */"]
        if m == "shld":
            return self._lift_shld(insn, ops)
        if m == "shrd":
            return self._lift_shrd(insn, ops)
        if m == "bt":
            if len(ops) >= 2:
                return [f"/* bt {_fmt_operand_read(ops[0])}, {_fmt_operand_read(ops[1])} - bit test */"]
            return [f"/* bt {insn.op_str} */"]
        if m == "emms":
            return ["/* emms - empty MMX state */"]
        if m in ("xlat", "xlatb"):
            # AL indexes the byte table at EBX. With an address-size override,
            # the effective offset is calculated and wrapped at 16 bits.
            raw_bytes = bytes.fromhex(insn.bytes_hex)
            if 0x67 in raw_bytes[:-1]:
                address = "(uint16_t)(LO16(ebx) + LO8(eax))"
            else:
                address = "ebx + LO8(eax)"
            return [f"SET_LO8(eax, MEM8({address})); /* xlatb */"]
        if m in ("sete", "setne", "setb", "setae", "setbe", "seta",
                 "setl", "setge", "setle", "setg", "sets", "setns"):
            return self._lift_setcc(insn, ops, m)
        if m in ("cmove", "cmovne", "cmovb", "cmovae", "cmovbe", "cmova",
                 "cmovl", "cmovge", "cmovle", "cmovg", "cmovs", "cmovns"):
            return self._lift_cmovcc(insn, ops, m)

        # ── SSE (scalar float) ──
        if m in ("movss", "movsd", "movaps", "movups", "movlps", "movhps",
                 "movlhps", "movhlps", "movapd", "movupd",
                 "addss", "subss", "mulss", "divss", "sqrtss",
                 "addsd", "subsd", "mulsd", "divsd", "sqrtsd",
                 "minss", "maxss", "minsd", "maxsd",
                 "comiss", "comisd", "ucomiss", "ucomisd",
                 "cvtsi2ss", "cvtss2si", "cvttss2si",
                 "cvtsi2sd", "cvtsd2si", "cvttsd2si",
                 "cvtss2sd", "cvtsd2ss",
                 "xorps", "xorpd", "andps", "orps", "andnps",
                 "movd", "movq",
                 "shufps", "unpcklps", "unpckhps",
                 "addps", "subps", "mulps", "divps",
                 "minps", "maxps", "rsqrtss", "rcpss",
                 "sqrtps", "rsqrtps", "rcpps",
                 "cmpneqps", "cmpeqps", "cmpltps", "cmpleps",
                 "movmskps",
                 "pand", "pandn", "por", "pxor",
                 # MMX integer -- the XMV pixel decoder.  These reached the
                 # generic TODO fallthrough before, which is why 1949 of
                 # them lifted to comments.
                 "movntq", "emms",
                 "paddb", "paddw", "paddd",
                 "psubb", "psubw", "psubd",
                 "pmullw", "pmaddwd",
                 "punpcklbw", "punpckhbw",
                 "punpcklwd", "punpckhwd",
                 "punpckldq", "punpckhdq",
                 "packuswb", "packsswb", "packssdw",
                 "psllw", "pslld", "psllq",
                 "psrlw", "psrld", "psrlq",
                 "psraw", "psrad",
                 "pcmpeqb", "pcmpeqw", "pcmpeqd",
                 "pcmpgtb", "pcmpgtw", "pcmpgtd",
                 "pshufw", "pavgb", "pminub", "pmovmskb",
                 "cvtps2pi", "cvtpi2ps", "movntps"):
            return self._lift_sse(insn, m, ops)

        # ── FPU ──
        if m.startswith("f"):
            return self._lift_fpu(insn, m, ops)

        # ── Explicit carry-flag manipulation ──
        # MSVC emits these around the multi-word arithmetic helpers, and around
        # the "return a bool in CF" idiom. They were unhandled, so the flag the
        # following adc/sbb reads kept whatever the last arithmetic left in it.
        # Only worth emitting when something downstream consumes CF -- that is
        # also the only time the enclosing function declares _cf.
        if m in ("stc", "clc", "cmc"):
            if not self.needs_cf:
                return [f"/* {m}: no adc/sbb in this function consumes CF */"]
            expr = {"stc": "1", "clc": "0", "cmc": "!_cf"}[m]
            return [f"_cf = {expr}; /* {m} */"]

        # ── Timestamp counter ──
        # Titles time themselves with this, so a no-op is not a harmless gap:
        # two reads return the same value, the delta is zero, and the game runs
        # its update loop as fast as it can with dt = 0 forever. Conker does
        # `rdtsc; _allmul(3); _alldiv(2200)` -- the console's 733.33 MHz counter
        # converted to microseconds -- and sat at 1.7 million updates in 73
        # seconds without advancing a frame.
        #
        # recomp_rdtsc() returns a counter ticking at the console's CPU rate, so
        # that arithmetic lands on real microseconds.
        if m == "rdtsc":
            return ["{ uint64_t _tsc = recomp_rdtsc();",
                    "  eax = (uint32_t)_tsc;",
                    "  edx = (uint32_t)(_tsc >> 32); } /* rdtsc */"]

        # ── Port I/O ──
        # Dropping these leaves the destination register holding whatever it
        # held, which reads as a real value to the guest.  Conker's vblank DPC
        # does `in al, 0x80C0; shr eax,5; not eax; and eax,ecx` and stores the
        # result as its flip-pending flag, so a missing read pinned the flag
        # set and stalled the boot video for the length of a run.
        #
        # Ports are handled in recomp_port_in/out; unknown ones read zero,
        # which is at least defined.  There are 114 of these sites in Conker.
        if m in ("in", "out"):
            regs = [o.strip() for o in insn.op_str.split(",")]
            if m == "in" and len(regs) == 2:
                dst, src = regs[0], regs[1]
                size = {"al": 1, "ax": 2, "eax": 4}.get(dst)
                if size is not None:
                    port = "(uint16_t)(edx & 0xFFFF)" if src == "dx" else f"(uint16_t)({src})"
                    call = f"recomp_port_in({port}, {size})"
                    if size == 1:
                        return [f"SET_LO8(eax, {call}); /* in {insn.op_str} */"]
                    if size == 2:
                        return [f"SET_LO16(eax, {call}); /* in {insn.op_str} */"]
                    return [f"eax = {call}; /* in {insn.op_str} */"]
            if m == "out" and len(regs) == 2:
                dst, src = regs[0], regs[1]
                size = {"al": 1, "ax": 2, "eax": 4}.get(src)
                if size is not None:
                    port = "(uint16_t)(edx & 0xFFFF)" if dst == "dx" else f"(uint16_t)({dst})"
                    val = {1: "eax & 0xFF", 2: "eax & 0xFFFF", 4: "eax"}[size]
                    return [f"recomp_port_out({port}, {size}, {val}); /* out {insn.op_str} */"]
            return [f"/* UNLIFTED {m} {insn.op_str} (unrecognised operands) */"]

        # ── Unhandled ──
        return [f"/* UNLIFTED {m} {insn.op_str} */"]

    # ── MOV family ──

    def _lift_mov(self, insn, ops):
        if nops := len(ops) < 2:
            return [f"/* mov: bad operands */"]
        src = _fmt_operand_read(ops[1])
        out = [_fmt_operand_write(ops[0], src)]
        # `mov ebp, esp` establishes this function's frame. Publish it, because
        # ebp is a per-function local and a callee with no prologue of its own
        # reads its caller's frame through ebp -- MSVC emits those routinely for
        # shared tails (Halo's CRT float formatting is one). Without the
        # publish, such a callee starts from an uninitialised ebp and its
        # [ebp-N] stores land wherever that garbage points.
        if (ops[0].type == "reg" and ops[0].reg == "ebp" and
                ops[1].type == "reg" and ops[1].reg == "esp"):
            out.append("g_ebp = ebp; /* publish frame for frameless callees */")
        return out

    def _lift_movzx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movzx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            if ops[1].mem_size == 1:
                src = f"ZX8({src})"
            elif ops[1].mem_size == 2:
                src = f"ZX16({src})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"ZX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
                src = f"ZX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movsx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movsx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            accessor = _smem_accessor(ops[1].mem_size)
            addr = _fmt_mem(ops[1])
            src = f"(uint32_t)(int32_t){accessor}({addr})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"SX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di"):
                src = f"SX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_lea(self, insn, ops):
        if len(ops) < 2 or ops[1].type != "mem":
            return [f"/* lea: unexpected operands */"]
        addr_expr = _fmt_mem(ops[1])
        return [_fmt_operand_write(ops[0], addr_expr)]

    def _lift_cmpxchg(self, insn, ops):
        """cmpxchg DEST, SRC

            old_dest = DEST; old_eax = EAX
            flags = old_eax - old_dest          (a full cmp, not just ZF)
            if old_eax == old_dest:  DEST = SRC
            else:                    EAX  = old_dest

        Three things this has to get right, all of which the previous
        TODO-stub got wrong by emitting nothing at all:

        The flags come from the comparison *before* the exchange.  Reading
        them afterwards -- which the old jcc handler did, as `(dest == eax)` --
        is wrong on both paths: on the equal path DEST has become SRC, and on
        the not-equal path EAX has become the old DEST, so that expression
        reports equal exactly when the instruction did not exchange.

        Every input is snapshotted before any write, so an operand that
        aliases another (`cmpxchg [eax], eax`, or a memory destination whose
        address register is also the source) still sees pre-instruction
        values.

        Only one of the two writes happens, and the accumulator write is at
        the operand's width -- an 8- or 16-bit cmpxchg leaves the rest of EAX
        alone, which a plain `eax = ...` would not.
        """
        if len(ops) < 2:
            return ["/* cmpxchg: bad operands */"]

        size = _operand_width(ops[0])
        if size is None:
            size = _operand_width(ops[1])
        if size not in self._SNAP_MASK:
            size = 4
        mask = self._SNAP_MASK[size]
        sx = self._SNAP_SX[size]

        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        acc = {1: "eax & 0xFF", 2: "eax & 0xFFFF", 4: "eax"}[size]
        set_acc = {1: "SET_LO8(eax, _cx_d);",
                   2: "SET_LO16(eax, _cx_d);",
                   4: "eax = _cx_d;"}[size]
        write_dst = _fmt_operand_write(ops[0], "_cx_s")

        out = [
            f"{{ uint32_t _cx_d = (uint32_t)({dst}) & {mask};",
            f"  uint32_t _cx_s = (uint32_t)({src}) & {mask};",
            f"  uint32_t _cx_a = (uint32_t)({acc}) & {mask};",
            "  _fa = _cx_a; _fb = _cx_d;",
            f"  _fas = (int32_t){sx}(_fa); _fbs = (int32_t){sx}(_fb);"
            f" /* cmpxchg {dst}, {src} ({size * 8}-bit) */",
        ]
        if self.needs_cf:
            out.append("  _cf = (int)(_fa < _fb);")
        out.append(f"  if (_cx_a == _cx_d) {{ {write_dst} }}")
        out.append(f"  else {{ {set_acc} }} }}")
        return out

    def _lift_xadd(self, insn, ops):
        """Exchange the old destination into SRC and store DEST + SRC.

        Memory is written before the source register changes its address;
        register destinations are written last so identical operands retain
        the sum. All reads and addition flags use pre-instruction snapshots.
        """
        if len(ops) != 2 or ops[1].type != "reg":
            return ["/* xadd: bad operands */"]
        size = _operand_width(ops[0]) or _operand_width(ops[1]) or 4
        mask, sx = self._SNAP_MASK[size], self._SNAP_SX[size]
        dst, src = _fmt_operand_read(ops[0]), _fmt_operand_read(ops[1])
        out = [
            f"{{ uint32_t _xa_d = (uint32_t)({dst}) & {mask};",
            f"  uint32_t _xa_s = (uint32_t)({src}) & {mask};",
            f"  uint32_t _xa_r = (_xa_d + _xa_s) & {mask};",
            "  _fa = _xa_d; _fb = _xa_s;",
            f"  _fas = (int32_t){sx}(_fa); _fbs = (int32_t){sx}(_fb);",
        ]
        if self.needs_cf:
            out.append(f"  _cf = (int)(((uint64_t)_fa + _fb) > {mask});")
        writes = [_fmt_operand_write(ops[0], "_xa_r"),
                  _fmt_operand_write(ops[1], "_xa_d")]
        if ops[0].type == "reg":
            writes.reverse()
        out.extend("  " + write for write in writes)
        out.append("}")
        return out

    def _lift_xchg(self, insn, ops):
        if len(ops) < 2:
            return [f"/* xchg: bad operands */"]
        a = _fmt_operand_read(ops[0])
        b = _fmt_operand_read(ops[1])
        return [
            f"{{ uint32_t _tmp = {a};",
            _fmt_operand_write(ops[0], b),
            _fmt_operand_write(ops[1], "_tmp") + " }",
        ]

    # ── Stack ──

    def _lift_push(self, insn, ops):
        if len(ops) < 1:
            return ["/* push: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [f"PUSH32(esp, {val});"]

    def _lift_pop(self, insn, ops):
        if len(ops) < 1:
            return ["/* pop: no operand */"]
        if ops[0].type == "reg":
            r = ops[0].reg
            # Segment register pop → discard from stack
            if r in ("fs", "gs", "cs", "ds", "es", "ss"):
                return [f"{{ uint32_t _tmp; POP32(esp, _tmp); }} /* pop {r} - segment register */"]
            # Sample esp at each pop in a traced function. An epilogue that ends
            # `mov esp, ebp` restores esp unconditionally, so any drift inside
            # the function is erased before a return-time trace can see it --
            # yet the pops run *before* that restore, so drift is exactly what
            # breaks them. This is the only point the drift is observable.
            if self.trace_exit_name:
                return [f'RECOMP_TRACE_ESP("{self.trace_exit_name}", "pop {r}");',
                        f"POP32(esp, {r});"]
            return [f"POP32(esp, {r});"]
        else:
            return [f"{{ uint32_t _tmp; POP32(esp, _tmp); {_fmt_operand_write(ops[0], '_tmp')} }}"]

    # ── ALU binary operations ──

    def _lift_alu_binop(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        c_op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[m]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # XOR reg, reg → zero
        if m == "xor" and ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            out = [_fmt_operand_write(ops[0], "0") + " /* xor self */"]
            if self.needs_cf:
                out.append("_cf = 0; /* logical op clears CF */")
            return out
        expr = f"{dst} {c_op} {src}"
        out = []
        if self.needs_cf:
            # CF must be computed from the pre-write operands.
            if m == "add":
                w = _operand_width(ops[0]) or 4
                out.append(f"_cf = (int)((((uint64_t)({dst}) + (uint64_t)({src})) >> {w * 8}) & 1);")
            elif m == "sub":
                out.append(f"_cf = (int)((uint32_t)({dst}) < (uint32_t)({src}));")
            else:
                out.append("_cf = 0; /* logical op clears CF */")
        out.append(_fmt_operand_write(ops[0], expr))
        return out

    def _lift_inc_dec(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        val = _fmt_operand_read(ops[0])
        delta = "1"
        op_char = "+" if m == "inc" else "-"
        # For sub-registers (al, cl, etc.), use the SET macro instead of ++
        if ops[0].type == "reg" and ops[0].reg in (
                "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"):
            return [f"{val}{'++' if m == 'inc' else '--'};"]
        else:
            return [_fmt_operand_write(ops[0], f"{val} {op_char} {delta}")]

    def _lift_neg(self, insn, ops, preserve_carry=False):
        if len(ops) < 1:
            return ["/* neg: no operand */"]
        val = _fmt_operand_read(ops[0])
        out = []
        if preserve_carry or self.needs_cf:
            # neg sets CF iff the operand was non-zero (neg/sbb sign-extract).
            out.append(f"_cf = (int)(({val}) != 0);")
        out.append(_fmt_operand_write(ops[0], f"(uint32_t)(-(int32_t){val})"))
        return out

    def _lift_not(self, insn, ops):
        if len(ops) < 1:
            return ["/* not: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"~{val}")]

    def _lift_sbb(self, insn, ops):
        """SBB: subtract with borrow. Common idiom: sbb reg, reg → -CF (0 or -1)."""
        if len(ops) < 2:
            return ["/* sbb: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # sbb reg, reg is a common idiom: result is 0 or 0xFFFFFFFF depending on CF
        if ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "_cf ? 0xFFFFFFFF : 0") + " /* sbb self (CF extend) */"]
        w = (_operand_width(ops[0]) or 4) * 8
        return ["{ uint64_t _t = (uint64_t)(%s) - (uint64_t)(%s) - (uint64_t)_cf;"
                " _cf = (int)((_t >> %d) & 1); %s }  /* sbb */"
                % (dst, src, w, _fmt_operand_write(ops[0], "(uint32_t)_t"))]

    def _lift_adc(self, insn, ops):
        """ADC: add with carry."""
        if len(ops) < 2:
            return ["/* adc: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        w = (_operand_width(ops[0]) or 4) * 8
        return ["{ uint64_t _t = (uint64_t)(%s) + (uint64_t)(%s) + (uint64_t)_cf;"
                " _cf = (int)((_t >> %d) & 1); %s }  /* adc */"
                % (dst, src, w, _fmt_operand_write(ops[0], "(uint32_t)_t"))]

    def _lift_shld(self, insn, ops):
        """SHLD: double-precision shift left."""
        if len(ops) < 3:
            return [f"/* shld: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} << {cnt}) | ({src} >> (32 - {cnt}))") + " /* shld */"]

    def _lift_shrd(self, insn, ops):
        """SHRD: double-precision shift right."""
        if len(ops) < 3:
            return [f"/* shrd: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} >> {cnt}) | ({src} << (32 - {cnt}))") + " /* shrd */"]

    def _lift_imul(self, insn, ops):
        nops = len(ops)
        if nops == 1:
            # One operand: edx:eax = eax * ops[0]
            src = _fmt_operand_read(ops[0])
            return [
                f"{{ int64_t _r = (int64_t)(int32_t)eax * (int64_t)(int32_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif nops == 2:
            # Two operand: dst = dst * src
            dst = _fmt_operand_read(ops[0])
            src = _fmt_operand_read(ops[1])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} * (int32_t){src})")]
        elif nops == 3:
            # Three operand: dst = src1 * imm
            src = _fmt_operand_read(ops[1])
            imm = _fmt_operand_read(ops[2])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){src} * (int32_t){imm})")]
        return ["/* imul: unexpected form */"]

    def _lift_muldiv(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        src = _fmt_operand_read(ops[0])
        if m == "mul":
            return [
                f"{{ uint64_t _r = (uint64_t)eax * (uint64_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif m == "div":
            return [
                f"{{ uint64_t _dividend = ((uint64_t)edx << 32) | eax;",
                f"  eax = (uint32_t)(_dividend / (uint32_t){src});",
                f"  edx = (uint32_t)(_dividend % (uint32_t){src}); }}"
            ]
        elif m == "idiv":
            return [
                f"{{ int64_t _dividend = ((int64_t)(int32_t)edx << 32) | eax;",
                f"  eax = (uint32_t)((int32_t)(_dividend / (int32_t){src}));",
                f"  edx = (uint32_t)((int32_t)(_dividend % (int32_t){src})); }}"
            ]
        # Marked so the build-time audit can count it; a silent comment here
        # is what hid 1949 unlifted MMX instructions behind a working decoder.
        return [f"/* UNLIFTED {m}: unhandled */"]

    def _lift_shift(self, insn, ops, c_op):
        if len(ops) < 2:
            return [f"/* shift: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        out = []
        if self.needs_cf:
            w = (_operand_width(ops[0]) or 4) * 8
            # CF is the last bit shifted out; a zero count leaves CF alone.
            bit = f"({cnt}) - 1" if c_op == ">>" else f"{w} - ({cnt})"
            out.append(f"if ({cnt}) _cf = (int)((({dst}) >> ({bit})) & 1);")
        out.append(_fmt_operand_write(ops[0], f"{dst} {c_op} {cnt}"))
        return out

    def _lift_sar(self, insn, ops):
        if len(ops) < 2:
            return ["/* sar: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        out = []
        if self.needs_cf:
            out.append(f"if ({cnt}) _cf = (int)((({dst}) >> (({cnt}) - 1)) & 1);")
        out.append(_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} >> {cnt})"))
        return out

    def _lift_rotate(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        func = "ROL32" if m == "rol" else "ROR32"
        return [_fmt_operand_write(ops[0], f"{func}({dst}, {cnt})")]

    def _lift_bit_scan(self, ops, mnemonic):
        if len(ops) < 2:
            return [f"/* {mnemonic}: bad operands */"]

        src = _fmt_operand_read(ops[1])
        width = _operand_width(ops[1]) or _operand_width(ops[0]) or 4
        bits = width * 8
        value = (f"(uint32_t)(uint16_t)({src})" if width == 2
                 else f"(uint32_t)({src})")
        if mnemonic == "bsr":
            initial_index = bits - 1
            step = "--_bs_index"
        else:
            initial_index = 0
            step = "++_bs_index"
        write = _fmt_operand_write(ops[0], "_bs_index")
        statements = [
            "{",
            f"    uint32_t _bs_value = {value};",
        ]
        if self.needs_flags:
            statements.append("    _flags = (_bs_value == 0);")
        statements.extend([
            "    if (_bs_value != 0) {",
            f"        uint32_t _bs_index = {initial_index};",
            "        while (((_bs_value >> _bs_index) & 1u) == 0u) "
            f"{step};",
            f"        {write}",
            "    }",
            f"}} /* {mnemonic} */",
        ])
        return statements

    # ── Compare / Test (standalone) ──

    # Widths for the flag snapshot below.
    _SNAP_MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
    _SNAP_SX = {1: "(int8_t)", 2: "(int16_t)", 4: "(int32_t)"}

    def _snapshot_float_flags(self, mnemonic, a, b):
        """Materialise a float compare's EFLAGS into _ah.

        The counterpart of _snapshot_flags. A comiss whose branch is not
        adjacent cannot have its operands re-read at the branch -- an
        EFLAGS-preserving instruction in between may have clobbered them --
        so the result is captured here, while they are still in hand.

        AH is SF:ZF:0:AF:0:PF:1:CF and bit 1 reads back set. comiss leaves SF,
        OF and AF clear, so only ZF (0x40), PF (0x04) and CF (0x01) carry
        information: unordered sets all three, equal sets ZF, less sets CF,
        greater sets none.
        """
        if not self.needs_ah:
            return []
        ty = "double" if mnemonic in ("comisd", "ucomisd") else "float"
        return [
            f"{{ {ty} _ca = ({ty})({a}), _cb = ({ty})({b});",
            "  _ah = (uint8_t)(0x02u | "
            "((_ca != _ca || _cb != _cb) ? 0x45u : "
            "(_ca == _cb) ? 0x40u : "
            "(_ca < _cb) ? 0x01u : 0x00u)); }",
        ]

    def _snapshot_flags(self, insn, ops, kind):
        """Capture a cmp/test's operands where the comparison happens.

        The flags are consumed later by a jcc, which used to re-read the
        operands at that point. Two things go wrong with that. Anything
        between the two can change them - "cmp [esi+ecx*2], -1 / lea esi,
        [esi+ecx*2] / jne" re-read through the already-advanced esi and
        tested the wrong slot. And the comparison lost its width, so a
        16-bit "cmp word ptr [..], -1" became a compare against
        0xFFFFFFFF, which a 16-bit -1 (0xFFFF) never equals - the branch
        then went the same way every time.

        Snapshotting fixes both: operands are read once, at the right
        width, in both a zero- and a sign-extended form so the jcc can
        pick whichever its condition needs.
        """
        size = _operand_width(ops[0])
        if size is None:
            size = _operand_width(ops[1])          # e.g. cmp imm, reg
        if size not in self._SNAP_MASK:
            size = 4
        mask = self._SNAP_MASK[size]
        sx = self._SNAP_SX[size]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        out = [
            f"_fa = (uint32_t)({lhs}) & {mask}; _fb = (uint32_t)({rhs}) & {mask};",
            f"_fas = (int32_t){sx}(_fa); _fbs = (int32_t){sx}(_fb);"
            f" /* {kind} {lhs}, {rhs} ({size*8}-bit) */",
        ]
        # cmp and test set CF too, and something other than a jcc may read it.
        #
        # Snapshotting alone served the fused-branch case and left _cf holding
        # whatever the *previous* arithmetic instruction put there. MSVC's
        # branchless ASCII _stricmp is built on exactly that carry:
        #
        #     sub al, 0x41      ; CF = c < 'A'
        #     cmp al, 0x1a      ; CF = (c-'A') < 26, i.e. "is an uppercase letter"
        #     sbb cl, cl        ; cl = -CF
        #     and cl, 0x20      ; 0x20 for uppercase
        #     add al, cl        ; fold to lowercase
        #
        # With cmp not writing _cf, the sbb read the carry from the sub, so
        # uppercase letters were never folded and _stricmp quietly became
        # case-sensitive. Conker then failed to find "fxPosColTex2Vs" in its
        # resource registry when the title asked for "fxPosColTex2vs", and hit
        # its fatal-error handler.
        #
        # Emitted only when the function actually consumes CF, matching the
        # existing needs_cf gate -- _cf is not declared otherwise.
        if self.needs_cf:
            if kind == "test":
                out.append("_cf = 0; /* test clears CF */")
            else:
                out.append("_cf = (int)(_fa < _fb); /* cmp sets CF (borrow) */")
        return out

    def _lift_cmp(self, insn, ops):
        if len(ops) < 2:
            return ["/* cmp: bad operands */"]
        return self._snapshot_flags(insn, ops, "cmp")

    def _lift_test(self, insn, ops):
        if len(ops) < 2:
            return ["/* test: bad operands */"]
        return self._snapshot_flags(insn, ops, "test")

    # ── Control flow ──

    def _build_call_args(self, target_addr):
        """Build argument list for a function call based on ABI data."""
        abi_info = self.abi_db.get(target_addr, {})
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)

        args = []
        if cc in ("thiscall", "thiscall_cdecl"):
            args.append("(void*)(uintptr_t)ecx")
        for i in range(num_params):
            args.append(f"0 /* a{i+1} */")
        return ", ".join(args)

    # SEH prolog/epilog addresses - these functions modify ebp for their
    # caller.  After calling __SEH_prolog, the caller must read back ebp
    # from g_seh_ebp.  Before returning, __SEH_prolog writes g_seh_ebp.
    #
    # Per-title addresses, detected from the binary by detect_seh_helpers()
    # and assigned to the instance. The class values are only a fallback for
    # callers that construct a Lifter without a function database.
    SEH_PROLOG = frozenset()
    SEH_EPILOG = frozenset()
    SEH_HELPERS = frozenset()

    def _lift_call(self, insn, ops):
        # x86 'call' pushes the address of the following instruction, then jumps.
        # Push that real guest address, not a placeholder: the value is visible
        # to the callee, and plenty of x86 code reads it. __SEH_prolog locates
        # its scope table through [esp], _alloca probes walk back to it, and the
        # `mov eax, [esp]` / `pop eax` idiom for "where was I called from" shows
        # up in any CRT. A zero there is silently wrong until something reads it.
        #
        # 'ret' still only does esp += 4 and returns -- it never consumes the
        # value -- so writing the true address costs nothing at the return side.
        ret_va = insn.end_address
        if insn.call_target:
            lines = []
            # Re-publish this function's frame before every call, not just once
            # at `mov ebp, esp`. g_ebp is "the last frame established anywhere",
            # so a callee that sets up its own frame overwrites it and leaves it
            # stale on return. A frameless helper called afterwards then
            # inherits the wrong frame -- in Halo, sub_001E1BA0 called one
            # function, returned, then called the frameless sub_001DEC07, which
            # inherited a long-dead frame of ~0xA6 and wrote [ebp-0xa2] and
            # [ebp-0xa0] onto Xbox VA 4 and 6: exactly the fs:[4] corruption.
            if self.publishes_ebp:
                lines.append("g_ebp = ebp; /* frame stays current across calls */")
            if insn.call_target in self.manual_functions:
                lines.append(
                    f"PUSH32(esp, 0x{ret_va:08X}u); "
                    f"RECOMP_ICALL_SAFE(0x{insn.call_target:08X}u, "
                    "_icall_esp); "
                    f"/* manual call 0x{insn.call_target:08X} */")
            else:
                name = self._call_target_name(insn.call_target)
                lines.append(
                    f"PUSH32(esp, 0x{ret_va:08X}u); {name}(); "
                    f"/* call 0x{insn.call_target:08X} */")
            # esp immediately after the callee returns. A per-call delta is the
            # only way to attribute a leak to one callee rather than to the
            # function containing them all.
            if self.trace_exit_name:
                lines.append(
                    f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                    f'"after call 0x{insn.call_target:08X}");')
            # The SEH helpers exchange the frame pointer with their caller
            # through g_seh_ebp, because ebp is a C local rather than a global.
            #
            # Publishing before the call is as necessary as reading back after.
            # __SEH_prolog stores the caller's ebp into the new frame, and
            # __SEH_epilog restores esp from it before popping ebx/esi/edi. With
            # only the read-back, g_seh_ebp still held whatever the last *nested*
            # SEH function left there, so the epilog unwound to the wrong frame
            # and restored the callee-saved registers from the wrong stack slots.
            #
            # That corrupts ebx/esi/edi across any SEH function that calls
            # another - which is most of them. In Halo it left esi holding a
            # stack address where the caller had just zeroed it, so an
            # "if (status < 0)" test against esi failed and XapiInitProcess
            # bailed to the dashboard.
            if insn.call_target in self.SEH_HELPERS:
                lines.insert(0, "g_seh_ebp = ebp; /* publish frame to SEH helper */")
                lines.append("ebp = g_seh_ebp; /* read back frame from SEH helper */")
            return lines
        elif len(ops) >= 1:
            target = _fmt_operand_read(ops[0])
            # Mark indirect calls for post-processing by _fixup_icall_esp_save
            #
            # The target is read into a local BEFORE the return-address push.
            # On real x86 the memory operand is computed before the push, so
            # emitting the push first shifts esp by four and every esp-relative
            # target -- `call dword ptr [esp+8]`, the shape MSVC gives a
            # callback invoked through a stack argument -- is read four bytes
            # low and dispatches through the wrong slot.
            return [f"{{ uint32_t _icall_target = {target}; "
                    f"PUSH32(esp, 0x{ret_va:08X}u); "
                    "RECOMP_ICALL_SAFE(_icall_target, _icall_esp); }"
                    " /* indirect call */"]
        return ["/* call: no target */"]

    def _lift_ret(self, insn, ops):
        # x86 'ret' pops return address from stack.
        # 'ret N' also pops N extra bytes (stdcall cleanup).
        # If this function IS __SEH_prolog or __SEH_epilog, bridge ebp
        # so the caller can read back the frame pointer.
        prefix = ""
        if self.func_start in self.SEH_HELPERS:
            prefix = "g_seh_ebp = ebp; "
        # Exit trace, for functions that return with a register the caller
        # relied on holding something else. Entry tracing alone cannot show
        # that: it tells you what went in, never what came back. Emitted at the
        # ret, after the epilogue's pops, so the values are what the caller
        # actually receives.
        if self.trace_exit_name:
            prefix = (f'RECOMP_TRACE_EXIT("{self.trace_exit_name}", '
                      f'0x{self.func_start:08X}); ') + prefix
        if len(ops) >= 1 and ops[0].type == "imm":
            n = ops[0].imm
            return [f"{prefix}esp += {4 + n}; return; /* ret {n} */"]
        return [f"{prefix}esp += 4; return; /* ret */"]

    def _is_external_target(self, addr):
        """Check if a jump target is outside the current function."""
        return not (self.func_start <= addr < self.func_end)

    def _read_jump_table(self, table_va, max_entries=256):
        """Read 32-bit jump table entries from the XBE at a given VA.
        Returns list of target addresses. Stops when an entry is not a
        valid code address or max_entries is reached."""
        if not self.xbe_data:
            return []
        offset = va_to_file_offset(table_va)
        if offset is None:
            return []
        targets = []
        for i in range(max_entries):
            o = offset + i * 4
            if o + 4 > len(self.xbe_data):
                break
            val = struct.unpack_from('<I', self.xbe_data, o)[0]
            if not is_code_address(val):
                break
            targets.append(val)
        return targets

    def _analyze_switch_table(self, ops):
        """Detect if an indirect jmp operand is an intra-function switch table.
        Pattern: jmp [reg*scale + table_base] or jmp [reg + table_base]
        Returns (targets: list[int]) if ALL table entries are within the current
        function, else empty list."""
        if not ops or ops[0].type != "mem":
            return []
        op = ops[0]
        # Need a table base (displacement) and an index register
        if not op.mem_disp or not (op.mem_index or op.mem_base):
            return []
        table_va = op.mem_disp
        targets = self.jump_table_targets.get(table_va)
        if targets is None:
            targets = self._read_jump_table(table_va)
        if not targets:
            return []
        # Check that ALL targets are within the current function
        if all(self.func_start <= t < self.func_end for t in targets):
            return targets
        return []

    def _lift_jmp(self, insn, ops):
        if insn.jump_target:
            if self._is_external_target(insn.jump_target):
                # Tail call - no return address push (reuses current frame's)
                # Bridge ebp so the target function can inherit our frame pointer.
                if insn.jump_target in self.manual_functions:
                    tail = (
                        f"g_seh_ebp = ebp; "
                        f"RECOMP_ITAIL(0x{insn.jump_target:08X}u); return; "
                        f"/* manual tail jmp 0x{insn.jump_target:08X} */"
                    )
                    if self.trace_exit_name:
                        return [
                            f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                            f'"tail 0x{insn.jump_target:08X}");',
                            tail,
                        ]
                    return [tail]
                name = self._call_target_name(insn.jump_target)
                tail = (f"g_seh_ebp = ebp; {name}(); return; "
                        f"/* tail jmp 0x{insn.jump_target:08X} */")
                if self.trace_exit_name:
                    # Tag the exit so a trace says which one was taken. A
                    # function whose paths all look individually balanced can
                    # still leak, and knowing the path is the difference
                    # between measuring and guessing.
                    return [f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                            f'"tail 0x{insn.jump_target:08X}");', tail]
                return [tail]
            return [f"goto loc_{insn.jump_target:08X};"]
        elif len(ops) >= 1:
            # Detect intra-function switch tables (computed gotos)
            switch_targets = self._analyze_switch_table(ops)
            if switch_targets:
                target_expr = _fmt_operand_read(ops[0])
                unique_targets = sorted(set(switch_targets))
                lines = [f"{{ uint32_t _jt = {target_expr}; /* switch: {len(switch_targets)} entries, {len(unique_targets)} targets */"]
                for t in unique_targets:
                    lines.append(f"if (_jt == 0x{t:08X}u) goto loc_{t:08X};")
                lines.append(f"g_seh_ebp = ebp; RECOMP_ITAIL(_jt); return; }}")
                return lines
            target = _fmt_operand_read(ops[0])
            return [f"g_seh_ebp = ebp; RECOMP_ITAIL({target}); return; /* indirect tail jmp */"]
        return ["/* jmp: no target */"]

    def _lift_jcc(self, insn):
        """Standalone conditional jump (no flag-setter tracked)."""
        target = insn.jump_target
        jcc = insn.mnemonic

        # jecxz/jcxz: jump if ecx/cx is zero (not flag-based)
        if jcc in ("jecxz", "jcxz"):
            cond = "ecx == 0" if jcc == "jecxz" else "LO16(ecx) == 0"
            if target:
                if self._is_external_target(target):
                    name = self._call_target_name(target)
                    return [f"if ({cond}) {{ g_seh_ebp = ebp; {name}(); return; }} /* {jcc} */"]
                return [f"if ({cond}) goto loc_{target:08X}; /* {jcc} */"]
            return [f"/* {jcc} - no target */"]

        cond_info = COND_MAP.get(jcc)
        desc = cond_info[2] if cond_info else jcc
        if target:
            if self._is_external_target(target):
                name = self._call_target_name(target)
                return [f"if (_flags /* {jcc}: {desc} */) {{ g_seh_ebp = ebp; {name}(); return; }}"]
            return [f"if (_flags /* {jcc}: {desc} */) goto loc_{target:08X};"]
        return [f"/* {jcc}: {desc} - no target */"]

    # ── SETcc / CMOVcc ──

    def _lift_setcc(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        return [_fmt_operand_write(ops[0], f"_flags /* {m} */")]

    def _lift_cmovcc(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [f"if (_flags /* {m} */) {_fmt_operand_write(ops[0], src)}"]

    # ── String operations ──

    # `rep movs` must be an element-by-element copy, never memcpy or memmove.
    #
    # The buffers are allowed to overlap, and the overlap is the *point*: an
    # LZ77 decompressor emits a run by pointing esi one byte behind edi and
    # letting `rep movsb` replicate it. `rep movsb` with esi = edi-1 and
    # ecx = 11 writes the same byte eleven times.
    #
    # memcpy is undefined on overlap and in practice copies in wide chunks, so
    # it reproduces the *original* eleven bytes instead of replicating one.
    # memmove is no better: it is defined precisely to preserve the original
    # source, which is the opposite of what the instruction does.
    #
    # This is very hard to catch downstream, because the byte *count* is right
    # either way. Conker's PAGELK inflate stayed perfectly in sync -- every
    # block still ended exactly on its output limit -- while silently
    # corrupting every run whose distance was shorter than its length, which
    # then fed a garbage size into the CAFF resource parse.
    def _lift_rep_string(self, insn, m):
        if "movsb" in m:
            return ["if (_df > 0) { uint32_t _i; for (_i = 0; _i < ecx; _i++) "
                    "MEM8(edi + _i) = MEM8(esi + _i); esi += ecx; edi += ecx; }",
                    "else { uint32_t _i; for (_i = 0; _i < ecx; _i++) "
                    "MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }",
                    "ecx = 0; /* rep movsb */"]
        if "movsd" in m:
            return ["if (_df > 0) { uint32_t _i; for (_i = 0; _i < ecx; _i++) "
                    "MEM32(edi + _i*4) = MEM32(esi + _i*4); esi += ecx * 4; edi += ecx * 4; }",
                    "else { uint32_t _i; for (_i = 0; _i < ecx; _i++) "
                    "MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }",
                    "ecx = 0; /* rep movsd */"]
        if "movsw" in m:
            return ["if (_df > 0) { uint32_t _i; for (_i = 0; _i < ecx; _i++) "
                    "MEM16(edi + _i*2) = MEM16(esi + _i*2); esi += ecx * 2; edi += ecx * 2; }",
                    "else { uint32_t _i; for (_i = 0; _i < ecx; _i++) "
                    "MEM16(edi - _i*2) = MEM16(esi - _i*2); esi -= ecx * 2; edi -= ecx * 2; }",
                    "ecx = 0; /* rep movsw */"]
        if "stosb" in m:
            return ["if (_df > 0) { memset((void*)XBOX_PTR(edi), "
                    "(uint8_t)eax, ecx); edi += ecx; }",
                    "else { uint32_t _i; for (_i = 0; _i < ecx; _i++) "
                    "MEM8(edi - _i) = (uint8_t)eax; edi -= ecx; }",
                    "ecx = 0; /* rep stosb */"]
        if "stosd" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }",
                "edi += ecx * 4; ecx = 0; /* rep stosd */"
            ]
        if "stosw" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM16(edi + _i*2) = LO16(eax); }",
                "edi += ecx * 2; ecx = 0; /* rep stosw */"
            ]
        if "cmpsb" in m:
            continue_on_equal = "repne" not in m and "repnz" not in m
            stop_condition = "!_flags" if continue_on_equal else "_flags"
            comparison = ["    _flags = (MEM8(esi) == MEM8(edi));"]
            if self.needs_cf:
                # std::string::compare consumes CMPS carry through SBB after
                # the loop. Equality alone cannot preserve ordered-map lookup.
                comparison = ["    uint8_t _src = MEM8(esi), _dst = MEM8(edi);",
                              "    _flags = (_src == _dst);",
                              "    _cf = (_src < _dst);"]
            return [
                "while (ecx != 0) {",
                *comparison,
                "    esi += _df; edi += _df; ecx--;",
                f"    if ({stop_condition}) break;",
                f"}} /* {m} */",
            ]
        if "scasb" in m:
            continue_on_equal = "repne" not in m and "repnz" not in m
            stop_condition = "!_flags" if continue_on_equal else "_flags"
            return [
                "while (ecx != 0) {",
                "    _flags = (LO8(eax) == MEM8(edi));",
                "    edi += _df; ecx--;",
                f"    if ({stop_condition}) break;",
                f"}} /* {m} */",
            ]
        if "cmpsw" in m or "cmpsd" in m:
            return [f"/* {m} - string compare, ecx iterations */"]
        if "scasw" in m or "scasd" in m:
            return [f"/* {m} - string scan, ecx iterations */"]
        return [f"/* {m} */"]

    def _lift_string_op(self, insn, m):
        if m == "movsb":
            return ["MEM8(edi) = MEM8(esi); esi += _df; edi += _df; /* movsb */"]
        if m == "movsd":
            return ["MEM32(edi) = MEM32(esi); esi += 4*_df; edi += 4*_df; /* movsd */"]
        if m == "stosb":
            return ["MEM8(edi) = LO8(eax); edi += _df; /* stosb */"]
        if m == "stosd":
            return ["MEM32(edi) = eax; edi += 4*_df; /* stosd */"]
        if m == "lodsb":
            return ["SET_LO8(eax, MEM8(esi)); esi += _df; /* lodsb */"]
        if m == "lodsd":
            return ["eax = MEM32(esi); esi += 4*_df; /* lodsd */"]
        if m == "movsw":
            return ["MEM16(edi) = MEM16(esi); esi += 2*_df; edi += 2*_df; /* movsw */"]
        if m == "stosw":
            return ["MEM16(edi) = LO16(eax); edi += 2*_df; /* stosw */"]
        if m == "lodsw":
            return ["SET_LO16(eax, MEM16(esi)); esi += 2*_df; /* lodsw */"]
        return [f"/* {m} */"]

    # ── FPU (x87) ──

    # ── SSE (scalar/packed float) ──

    def _lift_sse(self, insn, m, ops):
        """Translate SSE instructions to C float operations."""
        nops = len(ops)
        if nops < 1:
            return [f"/* {m}: no operands */"]

        def _is_xmm(op):
            return op.type == "reg" and op.reg and op.reg.startswith("xmm")

        def _is_mmx(op):
            return (op.type == "reg" and op.reg and op.reg.startswith("mm")
                    and not op.reg.startswith("xmm"))

        # ── Scalar (lane 0) access ──
        # movss/addss/... genuinely operate on one 32-bit value, so they read
        # and write lane 0 explicitly rather than the whole register.
        def _sse_read(op):
            if _is_xmm(op):
                return f"{op.reg}.f[0]"
            elif op.type == "reg":
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            elif op.type == "imm":
                return _fmt_imm(op.imm)
            return f"/* sse_read? */"

        def _sse_write(op, val):
            if _is_xmm(op):
                return f"{op.reg}.f[0] = {val};"
            elif op.type == "reg":
                return f"{op.reg} = {val};"
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)}) = {val};"
                return f"MEMF({_fmt_mem(op)}) = {val};"
            return f"/* sse_write? */;"

        # ── Packed (128-bit) access ──
        # A whole-register read yields a RecompXmm value; a whole-register
        # write is a statement. Memory goes through the guest translation.
        def _packed_read(op):
            if _is_xmm(op):
                return op.reg
            if op.type == "mem":
                return f"XMM_MEM({_fmt_mem(op)})"
            return None

        def _packed_write(op, val):
            if _is_xmm(op):
                return f"{op.reg} = {val};"
            if op.type == "mem":
                return f"XMM_STORE({_fmt_mem(op)}, {val});"
            return None

        def _packed_binary(helper):
            """dst = helper(dst, src) for a two-operand packed op."""
            if nops < 2:
                return None
            a = _packed_read(ops[0])
            b = _packed_read(ops[1])
            if a is None or b is None:
                return None
            written = _packed_write(ops[0], f"{helper}({a}, {b})")
            if written is None:
                return None
            return [written + f" /* {m} */"]

        # ── Moves ──
        # movaps/movups move all 16 bytes. Treating them like movss was the
        # defect that left every packed value 4 bytes wide.
        if m in ("movaps", "movups", "movapd", "movupd"):
            if nops >= 2:
                src = _packed_read(ops[1])
                if src is not None:
                    written = _packed_write(ops[0], src)
                    if written is not None:
                        return [written + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # movlps/movhps transfer 8 bytes into or out of one half.
        if m in ("movlps", "movhps"):
            half = "LOW" if m == "movlps" else "HIGH"
            if nops >= 2:
                if _is_xmm(ops[0]) and ops[1].type == "mem":
                    return [f"XMM_LOAD_{half}({ops[0].reg}, "
                            f"{_fmt_mem(ops[1])}); /* {m} */"]
                if ops[0].type == "mem" and _is_xmm(ops[1]):
                    return [f"XMM_STORE_{half}({_fmt_mem(ops[0])}, "
                            f"{ops[1].reg}); /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m in ("movlhps", "movhlps"):
            helper = ("XMM_MOVE_LOW_TO_HIGH" if m == "movlhps"
                      else "XMM_MOVE_HIGH_TO_LOW")
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # movss/movsd are scalar. Loading from memory zeroes the upper lanes;
        # a register-to-register move leaves them untouched.
        if m in ("movss", "movsd"):
            if nops >= 2:
                scalar = ("XMM_SCALAR" if m == "movss"
                          else "XMM_SCALAR_DOUBLE")
                if _is_xmm(ops[0]) and ops[1].type == "mem":
                    return [f"{ops[0].reg} = "
                            f"{scalar}({_sse_read(ops[1])}); /* {m} */"]
                src = _sse_read(ops[1])
                return [_sse_write(ops[0], src) + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # movd moves 32 bits without converting. Into an XMM register it also
        # zeroes the upper lanes. The generated code redefines memcpy as a
        # guest-to-guest copy, so the bits are moved through the union instead
        # of taking the address of a host local.
        if m == "movd":
            if nops >= 2:
                if _is_xmm(ops[0]):
                    if _is_xmm(ops[1]):
                        src = f"{ops[1].reg}.u[0]"
                    elif _is_mmx(ops[1]):
                        src = f"(uint32_t){ops[1].reg}"
                    else:
                        src = _fmt_operand_read(ops[1])
                    return [f"{ops[0].reg} = XMM_SCALAR_BITS({src});"
                            " /* movd to xmm */"]
                if _is_xmm(ops[1]):
                    return [f"{_fmt_operand_write(ops[0], ops[1].reg + '.u[0]')}"
                            " /* movd */"]
                if _is_mmx(ops[0]):
                    # movd into an MMX register zeroes the upper half.  This
                    # used to fall through to a plain 32-bit assignment, which
                    # left whatever the previous 64-bit value had there.
                    if _is_mmx(ops[1]):
                        return [f"{ops[0].reg} = mmx_movd_to_mm("
                                f"(uint32_t){ops[1].reg}); /* movd */"]
                    src = _fmt_operand_read(ops[1])
                    return [f"{ops[0].reg} = mmx_movd_to_mm({src});"
                            " /* movd */"]
                if _is_mmx(ops[1]):
                    return [f"{_fmt_operand_write(ops[0], '(uint32_t)' + ops[1].reg)}"
                            " /* movd */"]
                src = _fmt_operand_read(ops[1])
                return [f"{_fmt_operand_write(ops[0], src)} /* movd */"]
            return [f"/* UNLIFTED-MMX movd {insn.op_str} */"]

        # ── Arithmetic ──
        if m in ("addss", "addsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} + {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("subss", "subsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} - {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("mulss", "mulsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} * {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("divss", "divsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} / {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("sqrtss", "sqrtsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})") + f" /* {m} */"]
        if m in ("minss", "minsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} < {b} ? {a} : {b})") + f" /* {m} */"]
        if m in ("maxss", "maxsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} > {b} ? {a} : {b})") + f" /* {m} */"]

        # ── Packed arithmetic ──
        # These used to emit a bare comment, so a matrix concatenation built
        # from shufps + mulps + addps executed as nothing at all.
        if m in ("addps", "subps", "mulps", "divps"):
            helper = {"addps": "XMM_ADD", "subps": "XMM_SUB",
                      "mulps": "XMM_MUL", "divps": "XMM_DIV"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Conversions ──
        if m == "cvtsi2ss":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(float)(int32_t){src}") + " /* cvtsi2ss */"]
        if m in ("cvtss2si", "cvttss2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtsi2sd":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(double)(int32_t){src}") + " /* cvtsi2sd */"]
        if m in ("cvtsd2si", "cvttsd2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtss2sd":
            if nops >= 2:
                return [_sse_write(ops[0], f"(double){_sse_read(ops[1])}") + " /* cvtss2sd */"]
        if m == "cvtsd2ss":
            if nops >= 2:
                return [_sse_write(ops[0], f"(float){_sse_read(ops[1])}") + " /* cvtsd2ss */"]

        # ── Comparison ──
        if m in ("comiss", "comisd", "ucomiss", "ucomisd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                out = [f"/* {m} {a}, {b} - sets EFLAGS */"]
                # When the branch is adjacent, _make_condition reads the
                # operands directly and this comment is the whole story. When
                # it is not, MSVC routes the result through AH -- lahf, then
                # `test ah, 0x44`, then jp/jnp -- so the flags have to be
                # materialised here, at the compare, while the operands are
                # still in hand.
                #
                # AH is SF:ZF:0:AF:0:PF:1:CF, and bit 1 reads back as 1.
                # ucomiss leaves SF, OF and AF clear, so only ZF, PF and CF
                # matter: unordered sets all three, equal sets ZF, less sets
                # CF, greater sets none.
                out.extend(self._snapshot_float_flags(m, a, b))
                return out

        # ── Bitwise ──
        # Done on the integer lanes: these carry sign-mask and select idioms
        # (fabs, negate, blend) that are meaningless as float arithmetic.
        if m in ("xorps", "xorpd"):
            if (nops >= 2 and _is_xmm(ops[0]) and _is_xmm(ops[1])
                    and ops[0].reg == ops[1].reg):
                return [f"{ops[0].reg} = XMM_ZERO(); /* {m} self = zero */"]
            lifted = _packed_binary("XMM_XOR")
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]
        if m in ("andps", "orps", "andnps"):
            helper = {"andps": "XMM_AND", "orps": "XMM_OR",
                      "andnps": "XMM_ANDN"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Packed min/max ──
        if m in ("minps", "maxps"):
            helper = "XMM_MIN" if m == "minps" else "XMM_MAX"
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Reciprocal / rsqrt ──
        if m == "rsqrtss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})") + " /* rsqrtss */"]
        if m == "rcpss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}") + " /* rcpss */"]

        # ── Packed sqrt / reciprocal / rsqrt ──
        # The SSE model here tracks only the low lane as a single float, so
        # these compute the low lane like their scalar ...ss forms rather than
        # all four. That is the same low-lane approximation the packed
        # arithmetic ops (addps/mulps) already use -- but computing the low lane
        # is strictly better than the TODO no-op these used to hit, which left
        # the destination stale and fed garbage into vector normalisation.
        # rsqrtps/sqrtps are the workhorse of 3D vector normalize; Wreckless
        # uses them heavily, Burnout 3 did not, which is why this surfaced now.
        if m == "sqrtps":
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})")
                        + " /* sqrtps (low lane; 4-lane model TODO) */"]
        if m == "rsqrtps":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})")
                        + " /* rsqrtps (low lane; 4-lane model TODO) */"]
        if m == "rcpps":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}")
                        + " /* rcpps (low lane; 4-lane model TODO) */"]

        # ── Packed comparison ──
        if m in ("cmpneqps", "cmpeqps", "cmpltps", "cmpleps"):
            helper = {"cmpeqps": "XMM_CMP_EQ", "cmpltps": "XMM_CMP_LT",
                      "cmpleps": "XMM_CMP_LE", "cmpneqps": "XMM_CMP_NEQ"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Move mask ──
        # This feeds branches, so a hardcoded 0 silently picked one side.
        if m == "movmskps":
            if nops >= 2:
                src = _packed_read(ops[1])
                if src is not None:
                    return [_fmt_operand_write(ops[0], f"XMM_MOVEMASK({src})")
                            + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # ── MMX / integer SIMD ──
        #
        # These five used to return a bare comment here, and this branch sits
        # ahead of the real MMX handler below, so it won every time: 280
        # instructions -- 106 pxor, 95 pand, 57 por, 16 pandn, 6 pcmpgtd --
        # were silently dropped even after the MMX helpers existed and were
        # verified bit-exact against the host CPU.
        #
        # The consequence was not subtle.  sub_004E2F1B clears the XMV
        # coefficient block with `pxor mm0, mm0` followed by sixteen
        # `movq [ecx+N], mm0`; with the pxor gone, mm0 kept a stale value and
        # the "clear" wrote that garbage 64 bits at a time across all 128
        # bytes.  Every block the decoder left sparse then carried leftover
        # data into the IDCT -- which is why the video was macroblock noise
        # while the transform itself was provably correct.
        #
        # Fall through to the MMX handler; every occurrence in this title is
        # an mm,mm or mm,mem form, which it covers.

        # ── Shuffle/unpack ──
        # shufps is the broadcast in every matrix concatenation.
        if m == "shufps":
            if nops >= 3 and ops[2].type == "imm":
                a = _packed_read(ops[0])
                b = _packed_read(ops[1])
                if a is not None and b is not None:
                    written = _packed_write(
                        ops[0],
                        f"XMM_SHUFFLE({a}, {b}, {_fmt_imm(ops[2].imm)})")
                    if written is not None:
                        return [written + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]
        if m in ("unpcklps", "unpckhps"):
            helper = ("XMM_UNPACK_LOW" if m == "unpcklps"
                      else "XMM_UNPACK_HIGH")
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]


        # ── MMX integer ──
        #
        # The XMV pixel decoder is written in MMX; until these were lifted the
        # decoder's bookkeeping ran but produced no pixels, so the boot video
        # was black while every timing metric matched hardware.
        #
        # Each op is emitted as a call to a helper in recomp_mmx.h that takes
        # operand VALUES and returns a value, so `mm0 = mmx_punpcklbw(mm0, mm0)`
        # is correct with no explicit snapshot: the arguments are evaluated
        # before the assignment, and destination-as-source aliasing -- which is
        # everywhere in real MMX -- cannot see a half-written register.
        #
        # None of these write EFLAGS on the real hardware, and none is listed
        # in FLAG_SETTERS, so no flag state is synthesised for them.

        def _mm_read64(op):
            """A 64-bit MMX source: register, memory, or immediate count."""
            if _is_mmx(op):
                return op.reg
            if op.type == "mem":
                return f"MEM64({_fmt_mem(op)})"
            if op.type == "imm":
                return _fmt_imm(op.imm)
            if op.type == "reg":
                return op.reg
            return None

        def _mm_write64(op, value):
            if _is_mmx(op):
                return f"{op.reg} = {value};"
            if op.type == "mem":
                return f"MEM64({_fmt_mem(op)}) = {value};"
            return None

        MMX_BINARY = {
            "paddb", "paddw", "paddd",
            "psubb", "psubw", "psubd",
            "pmullw", "pmaddwd",
            "pand", "pandn", "por", "pxor",
            "punpcklbw", "punpckhbw",
            "punpcklwd", "punpckhwd",
            "punpckldq", "punpckhdq",
            "packuswb", "packsswb", "packssdw",
            "pcmpeqb", "pcmpeqw", "pcmpeqd",
            "pcmpgtb", "pcmpgtw", "pcmpgtd",
            "pavgb", "pminub",
        }
        # Shifts take a count that may be an immediate or a register, but never
        # a 64-bit memory lane the way the binaries do.
        #
        # pshufw is NOT one of these.  It is a three-operand instruction --
        # dest, source, imm8 selector -- and reading only two operands drops
        # the selector and passes the SOURCE as the control, which turns an
        # encoded shuffle into a data-dependent one.  It is handled below.
        MMX_SHIFT = {
            "psllw", "pslld", "psllq",
            "psrlw", "psrld", "psrlq",
            "psraw", "psrad",
        }

        # pshufw mm, mm/m64, imm8 -- selects each of the four result words
        # from the source by a two-bit field of the immediate.  The XMV border
        # padder uses it as a broadcast (imm8 0x00 and 0xFF) to replicate an
        # edge pixel across a whole register.
        if m == "pshufw":
            if nops >= 3 and _is_mmx(ops[0]) and ops[2].type == "imm":
                src = _mm_read64(ops[1])
                if src is not None:
                    return [f"{ops[0].reg} = mmx_pshufw({src},"
                            f" {_fmt_imm(ops[2].imm)}); /* {m} */"]
            return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]

        if m in MMX_BINARY or m in MMX_SHIFT:
            if nops >= 2 and (_is_mmx(ops[0]) or _is_mmx(ops[1])):
                a = _mm_read64(ops[0])
                b = _mm_read64(ops[1])
                if a is not None and b is not None:
                    written = _mm_write64(ops[0], f"mmx_{m}({a}, {b})")
                    if written is not None:
                        return [written + f" /* {m} */"]
            return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]

        # movq moves the whole 64 bits, register-to-register or to/from memory.
        # movntq is the same store with a non-temporal hint, which this model
        # has no cache to honour.
        if m in ("movq", "movntq"):
            if nops >= 2:
                src = _mm_read64(ops[1])
                if src is not None:
                    written = _mm_write64(ops[0], src)
                    if written is not None:
                        return [written + f" /* {m} */"]
            return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]

        # pmovmskb writes a GPR, not an MMX register.
        if m == "pmovmskb":
            if nops >= 2 and _is_mmx(ops[1]) and ops[0].type == "reg":
                return [f"{ops[0].reg} = mmx_pmovmskb({ops[1].reg});"
                        f" /* {m} */"]
            return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]

        if m == "emms":
            return ["mmx_emms(); /* emms */"]

        # movntps stores a whole XMM register -- all sixteen bytes.  It must
        # not be aliased to movntq's 64-bit store, which would drop half of
        # every write silently.
        if m == "movntps":
            if nops >= 2 and _is_xmm(ops[1]) and ops[0].type == "mem":
                return [f"mmx_store128({_fmt_mem(ops[0])}, {ops[1].reg});"
                        f" /* {m} */"]
            if nops >= 2 and _is_xmm(ops[0]) and _is_xmm(ops[1]):
                return [f"{ops[0].reg} = {ops[1].reg}; /* {m} */"]
            return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]

        # cvtpi2ps: two signed 32-bit ints from an MMX register or a 64-bit
        # memory operand become the low two float lanes of the XMM
        # destination; lanes 2 and 3 are preserved.
        if m == "cvtpi2ps":
            if nops >= 2 and _is_xmm(ops[0]):
                if _is_mmx(ops[1]):
                    src = ops[1].reg
                elif ops[1].type == "mem":
                    src = f"MEM64({_fmt_mem(ops[1])})"
                else:
                    return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]
                return [f"{ops[0].reg} = mmx_cvtpi2ps({ops[0].reg}, {src});"
                        f" /* {m} */"]
            return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]

        # cvtps2pi: the low two float lanes of an XMM register or a 64-bit
        # memory operand become two signed 32-bit integers in an MMX register.
        # It rounds by MXCSR, so it is NOT a truncating cast; mmx_f2i also
        # returns the integer-indefinite value rather than saturating.
        if m == "cvtps2pi":
            if nops >= 2 and _is_mmx(ops[0]):
                if _is_xmm(ops[1]):
                    lo = f"{ops[1].reg}.f[0]"
                    hi = f"{ops[1].reg}.f[1]"
                elif ops[1].type == "mem":
                    addr = _fmt_mem(ops[1])
                    lo = f"MEMF({addr})"
                    hi = f"MEMF(({addr}) + 4)"
                else:
                    return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]
                return [f"{ops[0].reg} = mmx_cvtps2pi({lo}, {hi});"
                        f" /* {m} */"]
            return [f"/* UNLIFTED-MMX {m} {insn.op_str} */"]

        return [f"/* SSE: {m} {insn.op_str} */"]

    # ── FPU (x87) ──

    @staticmethod
    def _st_index(reg):
        """FPU register index from a capstone name like 'st(2)' or 'st2'."""
        mm = re.search(r"st\(?(\d+)\)?", reg or "")
        return int(mm.group(1)) if mm else 0

    @staticmethod
    def _st_expr(i):
        """C expression for FPU register st(i) relative to the current top."""
        if i == 0:
            return "fp_top()"
        if i == 1:
            return "fp_st1()"
        return f"g_fp_stack[(g_fp_top + {i}) & 7]"

    def _fcom_rhs(self, ops):
        """The value an fcom-family instruction compares st0 against.

        `fcom`/`fcomp`/`fcompp` with no operand compare st0 with st1. With a
        memory operand they compare st0 with that float/double. With an st(i)
        operand, that register. Returning fp_st1() unconditionally (the old
        behavior) is only right for the no-operand form.
        """
        if not ops:
            return "fp_st1()"
        op = ops[0]
        if getattr(op, "type", None) == "mem":
            if getattr(op, "mem_size", 4) == 8:
                return f"MEMD({_fmt_mem(op)})"
            return f"MEMF({_fmt_mem(op)})"
        if getattr(op, "type", None) == "reg" and op.reg:
            return self._st_expr(self._st_index(op.reg))
        return "fp_st1()"

    def _lift_fpu(self, insn, m, ops):
        # Rotate TOP without changing the physical register contents. Matrix
        # builders deliberately retain the old st(0) and read it as st(7).
        if m == "fincstp":
            return ["g_fp_top = (g_fp_top + 1u) & 7u; /* fincstp */"]
        if m == "fdecstp":
            return ["g_fp_top = (g_fp_top + 7u) & 7u; /* fdecstp */"]
        """Basic FPU instruction translation using double locals."""
        # FPU is complex. We translate common patterns to double operations.
        # Full accuracy would require an x87 stack emulator.

        if m == "fld":
            if len(ops) >= 1:
                if ops[0].type == "mem":
                    if ops[0].mem_size == 4:
                        return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld float */"]
                    elif ops[0].mem_size == 8:
                        return [f"fp_push(MEMD({_fmt_mem(ops[0])})); /* fld double */"]
                    return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld */"]
                if ops[0].type == "reg":
                    # fld st(i) pushes a COPY of st(i). Was a no-op comment,
                    # which silently dropped a stack slot -- sub_00109150 (the
                    # world_to_view builder) duplicates values with fld st(0)
                    # three times. Capture the value first: fp_push mutates
                    # g_fp_top, so passing fp_top() directly would read the
                    # slot after the decrement.
                    src = self._st_expr(self._st_index(ops[0].reg))
                    return [f"{{ double _t = {src}; fp_push(_t); }}"
                            f" /* fld {insn.op_str} */"]
            return [f"/* fld {insn.op_str} */"]

        if m in ("fst", "fstp"):
            # Only fstp pops. fst stores st0 and leaves the stack alone. The old
            # code emitted fp_pop() for BOTH -- fp_pop() is g_fp_top++, a real
            # pop, not a no-op -- so every `fst [mem]` (store WITHOUT pop) shrank
            # the FP stack by one. valid_real_matrix4x3 does `fst [tmp]` before
            # its fabs/fcompare, so the compare ran on an emptied stack slot and
            # rejected orthonormal camera matrices at render_cameras.c:458.
            do_pop = " fp_pop();" if m == "fstp" else ""
            if len(ops) >= 1 and ops[0].type == "mem":
                pop = " fp_pop();" if m == "fstp" else ""
                if ops[0].mem_size == 4:
                    return [f"MEMF({_fmt_mem(ops[0])}) = (float)fp_top();{do_pop} /* {m} */"]
                elif ops[0].mem_size == 8:
                    return [f"MEMD({_fmt_mem(ops[0])}) = fp_top();{do_pop} /* {m} */"]
            # fst/fstp st(i): copy st0 to st(i); fstp then pops. This used to be
            # a bare comment -- a no-op -- which LEAKS the FPU stack. `fstp st(0)`
            # is the common idiom for "pop the value fptan/fsincos just pushed";
            # dropping it left every later float op one slot off. That is why
            # Halo's field-of-view came out 0.
            if len(ops) >= 1 and ops[0].type == "reg" and ops[0].reg:
                mm = re.search(r"st\(?(\d+)\)?", ops[0].reg)
                idx = int(mm.group(1)) if mm else 0
                parts = []
                if idx != 0:   # i==0 is a self-copy; skip, just (maybe) pop
                    dst = "fp_st1()" if idx == 1 else \
                          f"g_fp_stack[(g_fp_top + {idx}) & 7]"
                    parts.append(f"{dst} = fp_top();")
                if m == "fstp":
                    parts.append("fp_pop();")
                body = " ".join(parts) if parts else "(void)0;"
                return [f"{body} /* {m} st({idx}) */"]
            return [f"/* {m} {insn.op_str} */"]

        if m == "fild":
            if len(ops) >= 1 and ops[0].type == "mem":
                smem = _smem_accessor(ops[0].mem_size)
                return [f"fp_push((double){smem}({_fmt_mem(ops[0])})); /* fild */"]
            return [f"/* fild {insn.op_str} */"]

        if m in ("fist", "fistp"):
            if len(ops) >= 1 and ops[0].type == "mem":
                size = ops[0].mem_size
                mem_acc = _smem_accessor(size)
                int_type = {2: "int16_t", 4: "int32_t", 8: "int64_t"}.get(
                    size, "int32_t")
                pop = " fp_pop();" if m == "fistp" else ""
                return [f"{mem_acc}({_fmt_mem(ops[0])}) = "
                        f"({int_type})llrint(fp_top());{pop} /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m in ("fadd", "faddp", "fsub", "fsubp", "fsubr", "fsubrp",
                 "fmul", "fmulp", "fdiv", "fdivp", "fdivr", "fdivrp",
                 "fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            # x87 binary arithmetic, operand-aware. The old handlers hardcoded
            # the no-operand pop form (fp_st1() op= fp_top(); pop) for every
            # variant, so `fmul [mem]`, `fadd st(0),st(0)`, and the reversed
            # fsubr/fdivr were all wrong -- and fsubr/fdivr fell through to a
            # no-op. That corrupted the FPU stack on any float that used a
            # memory or register operand; Halo's field-of-view chain multiplied
            # by a constant with `fmul [k]` and came out 0.
            # The fi* forms are the same operations against a signed integer
            # in memory (fiadd -> fadd, fisubr -> fsubr). They are memory-only
            # and never pop.
            integer_operand = m.startswith("fi")
            base = m[:-1] if m.endswith("p") else m       # strip pop suffix
            if integer_operand:
                base = "f" + base[2:]
            reverse = base.endswith("r")                  # fsubr / fdivr
            core = base[:-1] if reverse else base         # fsub / fdiv / fadd / fmul
            cop = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}[core]
            pops = m.endswith("p")

            def _combine(dst, src):
                if cop in ("+", "*") or not reverse:
                    return f"{dst} = {dst} {cop} {src};"
                return f"{dst} = {src} {cop} {dst};"   # reversed sub/div

            # Memory operand: dst is st0, no pop (memory forms never pop).
            if ops and ops[0].type == "mem":
                if integer_operand:
                    accessor = "SMEM16" if ops[0].mem_size == 2 else "SMEM32"
                    rhs = f"(double){accessor}({_fmt_mem(ops[0])})"
                else:
                    rhs = (f"MEMD({_fmt_mem(ops[0])})" if ops[0].mem_size == 8
                           else f"MEMF({_fmt_mem(ops[0])})")
                return [f"{_combine('fp_top()', rhs)} /* {m} {insn.op_str} */"]

            # Two register operands: fXXX st(i), st(j).
            if len(ops) >= 2 and ops[0].type == "reg" and ops[1].type == "reg":
                di, si = self._st_index(ops[0].reg), self._st_index(ops[1].reg)
                code = _combine(self._st_expr(di), self._st_expr(si))
                if pops:
                    code += " fp_pop();"
                return [f"{code} /* {m} {insn.op_str} */"]

            # One register operand. Capstone reports the pop forms this way too
            # -- `faddp st(1)` comes through as a single operand st(1), NOT as
            # two operands and NOT as the no-operand form. The pop variants put
            # the result in st(i) and pop; only the non-pop variants target st0:
            #   fadd  st(i)  ->  st0  = st0  op st(i)          (no pop)
            #   faddp st(i)  ->  st(i) = st(i) op st0 ; pop
            # Treating faddp st(i) as the no-pop st0 form left the FP stack one
            # slot deep AND wrote the wrong slot, so a normalize's sum of squares
            # double-counted -- a unit vector got length sqrt(2) and Halo's
            # world_to_view basis came out scaled by 1/sqrt(2) (0.707), failing
            # valid_real_matrix4x3.
            if len(ops) >= 1 and ops[0].type == "reg":
                si = self._st_index(ops[0].reg)
                if pops:
                    code = _combine(self._st_expr(si), "fp_top()") + " fp_pop();"
                else:
                    code = _combine("fp_top()", self._st_expr(si))
                return [f"{code} /* {m} {insn.op_str} */"]

            # No operand: the stack pop form, st1 op= st0, pop.
            code = _combine("fp_st1()", "fp_top()") + " fp_pop();"
            return [f"{code} /* {m} */"]
        if m == "fchs":
            return [f"fp_top() = -fp_top(); /* fchs */"]
        if m == "fabs":
            return [f"fp_top() = fabs(fp_top()); /* fabs */"]
        if m == "fsqrt":
            return [f"fp_top() = sqrt(fp_top()); /* fsqrt */"]
        # x87 transcendentals. None of these were implemented, so every one fell
        # through to the unknown-op path and left the FP stack untouched --
        # silently, because an unimplemented FPU op looks exactly like an
        # instruction that only had a side effect on the status word.
        #
        # Halo 2276 alone has 123 fcos, 108 fsin, 31 fpatan, 8 fptan, 5 fyl2x,
        # 3 f2xm1 and 1 fprem. Every camera matrix and rotation in the game goes
        # through them, which is why render_camera_build_frustum asserted on
        # valid_real_matrix4x3: the matrix was built out of tangents that were
        # never computed, so it held whatever had been in those globals before.
        #
        # These are exact-enough mappings onto libm. The 80-bit intermediates of
        # real x87 are not reproduced -- fp_stack is double -- which is the same
        # approximation every other op here already makes.
        if m == "fsin":
            return [f"fp_top() = sin(fp_top()); /* fsin */"]
        if m == "fcos":
            return [f"fp_top() = cos(fp_top()); /* fcos */"]
        if m == "fsincos":
            # Replaces st0 with sin, then pushes cos. Order matters: the push
            # must see the sine already stored.
            return [f"{{ double _a = fp_top(); fp_top() = sin(_a);"
                    f" fp_push(cos(_a)); }} /* fsincos */"]
        if m == "fptan":
            # st0 = tan(st0), then push 1.0. The constant push is not decoration:
            # callers use it as the denominator of a subsequent fdiv.
            return [f"{{ fp_top() = tan(fp_top()); fp_push(1.0); }} /* fptan */"]
        if m == "fpatan":
            # st1 = atan2(st1, st0), pop. Argument order is st1 over st0.
            return [f"{{ fp_st1() = atan2(fp_st1(), fp_top()); fp_pop(); }}"
                    f" /* fpatan */"]
        if m == "fyl2x":
            # st1 = st1 * log2(st0), pop.
            return [f"{{ fp_st1() = fp_st1() * log2(fp_top()); fp_pop(); }}"
                    f" /* fyl2x */"]
        if m == "fyl2xp1":
            return [f"{{ fp_st1() = fp_st1() * log2(fp_top() + 1.0); fp_pop(); }}"
                    f" /* fyl2xp1 */"]
        if m == "f2xm1":
            return [f"fp_top() = exp2(fp_top()) - 1.0; /* f2xm1 */"]
        if m in ("fprem", "fprem1"):
            # Both leave the remainder in st0 and clear C2 to say "complete".
            # fprem truncates toward zero, fprem1 rounds to nearest (IEEE), which
            # is the difference between fmod and remainder.
            fn = "fmod" if m == "fprem" else "remainder"
            return [f"fp_top() = {fn}(fp_top(), fp_st1()); /* {m} */"]
        if m == "fscale":
            return [f"fp_top() = ldexp(fp_top(), (int)fp_st1()); /* fscale */"]
        if m == "frndint":
            return ["fp_top() = RECOMP_FRNDINT(fp_top(), g_fp_control_word); /* frndint */"]
        if m == "fldpi":
            return [f"fp_push(3.14159265358979323846); /* fldpi */"]
        if m == "fldl2e":
            return [f"fp_push(1.44269504088896340736); /* fldl2e */"]
        if m == "fldl2t":
            return [f"fp_push(3.32192809488736234787); /* fldl2t */"]
        if m == "fldlg2":
            return [f"fp_push(0.30102999566398119521); /* fldlg2 */"]
        if m == "fldln2":
            return [f"fp_push(0.69314718055994530942); /* fldln2 */"]
        if m == "ftst":
            return [f"g_fp_cmp = (fp_top() < 0.0) ? -1 : "
                    f"(fp_top() > 0.0) ? 1 : 0; /* ftst */"]
        if m == "fxch":
            # fxch st(i) swaps st0 with st(i); the bare form is st(1). Was
            # hardcoded to st1, so fxch st(2)/st(3)/st(4) (86/7/1 sites in Halo)
            # swapped the wrong slot -- corrupting the FP stack in the camera
            # basis builder that feeds world_to_view.
            # Capstone reports fxch with BOTH operands -- (st(0), st(i)) -- and
            # it is the only x87 form that does; fadd/fcom/fld st(i) all come
            # through with the explicit register alone. Reading ops[0] therefore
            # picked up the implicit st(0) and emitted a swap of st0 with
            # itself, so every `fxch st(i)` was a silent no-op.
            i = (self._st_index(ops[-1].reg)
                 if (ops and ops[-1].type == "reg" and ops[-1].reg) else 1)
            dst = self._st_expr(i)
            return [f"{{ double _t = fp_top(); fp_top() = {dst}; {dst} = _t; }}"
                    f" /* fxch {insn.op_str} */"]
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            # Compare st0 against the operand, not always st1. `fcomp [mem]`
            # compares st0 with the memory value; only the no-operand form
            # compares st0 with st1. Getting this wrong made every float compare
            # against a constant read a garbage st1 -- Halo's camera FOV and
            # world_to_view checks both fed on it.
            rhs = self._fcom_rhs(ops)
            # Pop count is in the mnemonic and was being ignored: fcom/fucom pop
            # nothing, fcomp/fucomp pop once, fcompp/fucompp pop twice. Emitting
            # zero pops for every form leaked a stack slot on each fcomp -- and
            # float compares are everywhere -- so g_fp_top drifted and later fld
            # st(i)/faddp read the wrong slots. valid_real_matrix4x3 calls the
            # leaking per-vector check three times, then its own dot products ran
            # on a drifted stack: an orthonormal matrix failed non-deterministically
            # at render_cameras.c:458. Compare first (rhs may be fp_st1()), then pop.
            npop = 2 if m.endswith("pp") else (1 if m.endswith("p") else 0)
            pops = " fp_pop();" * npop
            return [f"g_fp_cmp = RECOMP_FCMP(fp_top(), {rhs});"
                    f"{pops} /* {m} {insn.op_str} */"]
        if m in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip", "fcomi"):
            # These set EFLAGS directly (CF, ZF, PF) from FPU comparison
            # fcompi/fucompi pop st(0) after comparing; fcomi/fucomi do not
            pops = m.endswith("pi") or m.endswith("ip")
            pop_code = " fp_pop();" if pops else ""
            rhs = self._fcom_rhs(ops)
            out = [f"g_fp_cmp = RECOMP_FCMP(fp_top(), {rhs});"
                   f"{pop_code} /* {m} */"]
            if self.needs_ah:
                # FCOMI/FUCOMI write EFLAGS, unlike FCOM/FUCOM. LAHF must
                # retain their result even after FSTP changes the x87 stack.
                # Conker uses FUCOMIP; FSTP; LAHF; TEST AH,44h to skip an
                # optional zero font-size multiplier. Stale AH erased it.
                out.append("_ah = (uint8_t)(0x02u | (g_fp_cmp == 2 ? 0x45u : "
                           "g_fp_cmp == 0 ? 0x40u : g_fp_cmp < 0 ? 0x01u : 0x00u));")
            return out
        if m == "fnstsw":
            # `fnstsw ax` after an FPU compare is half of the pre-SSE float
            # branch idiom `fcomp; fnstsw ax; test ah, mask; j(p/np/z/nz)`.
            # Put the compare's C3/C2/C0 condition bits into ah so the following
            # test reads a real value instead of stale eax. g_fp_cmp is -1/0/1
            # for st0 </=/> src; the FPU sets C3 on equal (ah bit 6 = 0x40) and
            # C0 on less-than (ah bit 0 = 0x01), C2 only on unordered (NaN),
            # which non-NaN game math does not hit.
            # The status word also carries TOP in bits 11-13, which lands in
            # AH bits 3-5. We model TOP (it is g_fp_top), so emit it: leaving it
            # out made `fnstsw ax` disagree with the hardware on every AH read
            # taken while the stack was non-empty. The exception-flag byte (AL)
            # we do not model and it is zero after masked, clean operations.
            # C3/C2/C0 as the hardware sets them: unordered is C3|C2|C0, and
            # `test ah, 0x44; jp` -- the standard isnan idiom -- reads exactly
            # those two bits. Reporting equal for a NaN compare sends every
            # float classification in a title down the wrong branch.
            status = ("(uint16_t)(((g_fp_top & 7u) << 11) |"
                      " (g_fp_cmp == 2 ? 0x4500u :"
                      " g_fp_cmp < 0 ? 0x0100u :"
                      " g_fp_cmp > 0 ? 0x0000u : 0x4000u))")
            if insn.op_str.strip() in ("ax", "eax"):
                # `fnstsw ax` writes the whole of AX, not just AH.
                return [f"eax = (eax & 0xFFFF0000u) | (uint32_t){status};"
                        " /* fnstsw ax <- fpu status */"]
            if ops and ops[0].type == "mem":
                return [f"MEM16({_fmt_mem(ops[0])}) = {status};"
                        f" /* fnstsw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [_fmt_set_reg(ops[0].reg, status)
                        + f" /* fnstsw {insn.op_str} */"]
            return [f"/* fnstsw {insn.op_str} - no destination operand */"]
        if m == "fnstcw":
            # The CRT reads the control word back to decide whether an
            # exception is masked, so it must be stored, not dropped.
            if ops and ops[0].type == "mem":
                return [f"MEM16({_fmt_mem(ops[0])}) = g_fp_control_word;"
                        f" /* fnstcw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [_fmt_set_reg(ops[0].reg, "g_fp_control_word")
                        + f" /* fnstcw {insn.op_str} */"]
            return [f"/* fnstcw {insn.op_str} - no destination operand */"]
        if m == "fldcw":
            if ops and ops[0].type == "mem":
                return [f"g_fp_control_word = MEM16({_fmt_mem(ops[0])});"
                        f" /* fldcw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [f"g_fp_control_word = (uint16_t)"
                        f"{_fmt_operand_read(ops[0])};"
                        f" /* fldcw {insn.op_str} */"]
            return [f"/* fldcw {insn.op_str} - no source operand */"]
        if m == "fldz":
            return [f"fp_push(0.0); /* fldz */"]
        if m == "fld1":
            return [f"fp_push(1.0); /* fld1 */"]

        return [f"/* FPU: {m} {insn.op_str} */"]


# ── EFLAGS dataflow ───────────────────────────────────────────────────────
#
# A flag state is either None -- unknown, so a consumer falls back to _flags --
# or a tuple of (mnemonic, operands) candidates.  More than one candidate means
# control flow joined and the producers differ; a consumer may still use them,
# but only if every candidate generates the identical condition.
#
# That distinction is the whole point.  A loop head is reached from its
# fallthrough and from its own back edge, and those two rarely end in the same
# instruction.  Conker's inflate has
#
#     loc_004E4448:  sub eax, [ebp+0x20]      ; ZF = (eax == 0)
#     loc_004E4450:  je  loc_004E445E
#     loc_004E4452:  ...
#                    dec eax                  ; ZF = (eax == 0)
#                    jmp loc_004E4450
#
# `sub` and `dec` are different producers and identical here: for `je` both
# emit `(eax == 0)`.  Comparing mnemonics rejects that and leaves the branch
# reading _flags, which is zero for the life of the function -- the loop then
# never exits, and the boot video never decodes.  Comparing the generated
# condition accepts it, and still rejects anything that would actually differ.

def advance_flag_state(insn, state):
    """EFLAGS transfer for one instruction.

    The single source of truth for how flags flow, shared by lift_basic_block
    and by the translator's dataflow pass so the two cannot drift apart.
    """
    m = insn.mnemonic
    # Explicit mnemonic entries take precedence over prefix normalization.
    if (m not in FLAG_SETTERS and m not in _EFLAGS_SETTERS
            and m not in _FLAGS_UNDEFINED and m not in _EFLAGS_PRESERVE):
        m = _norm_mnem(m)
    if m in FLAG_SETTERS or m in _EFLAGS_SETTERS:
        return ((m, list(insn.operands)),)
    if m in _FLAGS_UNDEFINED:
        return None                      # flags undefined afterwards
    if m in _EFLAGS_PRESERVE:
        return state
    if m in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip", "fcomi"):
        return ((m, list(insn.operands)),)   # FPU compare straight to EFLAGS
    if m == "sahf":                      # after fnstsw ax, for FPU compares
        return (("sahf", list(insn.operands)),)
    if m.startswith("f") or m.startswith("cmov"):
        return state                     # FPU, and CMOVcc reads but not writes
    if m.startswith("j"):
        return state                     # jumps do not set flags
    if m.startswith("set"):
        return state                     # SETcc reads but does not write
    if m.startswith("rep"):
        rest = insn.op_str.strip() if hasattr(insn, "op_str") else ""
        if ("cmpsb" in m or "scasb" in m
                or "cmpsb" in rest or "scasb" in rest):
            return ((m, list(insn.operands)),)   # repe cmpsb / repne scasb
        return state                     # rep movs/stos move data, keep flags
    return None                          # unknown: assume clobbered


def normalize_flag_state(state):
    """Accept the legacy (mnemonic, operands) pair as a one-candidate state."""
    if state is None:
        return None
    if len(state) == 2 and isinstance(state[0], str):
        return ((state[0], list(state[1])),)
    return tuple((m, list(ops)) for m, ops in state)


def block_flag_transfer(bb, state):
    """Flag state leaving `bb`, entered with `state`. Emits nothing.

    Stricter than advance_flag_state on one point: a call ends the state.

    `call` sits in _EFLAGS_PRESERVE because the instruction itself leaves
    EFLAGS alone, and within a block that has always been the model.  Across
    blocks it cannot be, because the callee is free to clobber them and this
    is where a producer would otherwise travel an arbitrary distance -- around
    a loop, through every call in the body, and into a branch that has no
    business reading it.  Before the solver existed a loop head's state was
    always unknown, so those branches read _flags and never took their edge;
    resolving them to a real condition made the DSP mixer crash on frame 13.
    Agreement is about knowing the producer, and a callee is not one.
    """
    state = normalize_flag_state(state)
    for insn in bb.instructions:
        if insn.is_call:
            state = None
            continue
        state = advance_flag_state(insn, state)
    return state


def agree(fn, mnemonic, cands):
    """Apply `fn` to every candidate; return its result only if all agree.

    Disagreement, or any candidate the helper cannot resolve, gives None and
    the caller falls back to _flags.  One dissenter is enough to reject.
    """
    if not cands:
        return None
    first = None
    for setter, ops in cands:
        result = fn(mnemonic, setter, ops)
        if result is None:
            return None
        if first is None:
            first = result
        elif result != first:
            return None
    return first


_ZF_SNAPSHOT_SETTERS = frozenset((
    "cmp", "test", "inc", "dec", "add", "sub", "and", "or", "xor", "neg"))


def divergent_zf(cands):
    """Whether known incoming paths require a stored ZF instead of one expression.

    Calls, undefined flags and unsupported producers remain unresolved. Only
    integer operations whose zero result is modelled can use this snapshot.
    """
    return bool(cands and len(cands) > 1
                and all(m in _ZF_SNAPSHOT_SETTERS
                        and _make_condition("je", m, ops) is not None
                        for m, ops in cands)
                and agree(_make_condition, "je", cands) is None)


def stored_zf(cands):
    """INC/DEC results can be overwritten while their flags remain live."""
    return bool(cands and any(m in ("inc", "dec") for m, _ in cands)
                and all(m in _ZF_SNAPSHOT_SETTERS
                        and _make_condition("je", m, ops) is not None
                        for m, ops in cands))


def _snapshot_zf(lifter, insn):
    if not lifter.needs_zf or insn.mnemonic not in _ZF_SNAPSHOT_SETTERS:
        return []
    result = _make_condition("je", insn.mnemonic, insn.operands)
    return [f"_zf = ({result[0]}); /* preserve ZF across writes and joins */"] if result else []


def lift_basic_block(lifter, bb, flag_state=None):
    """
    Lift a basic block to C statements.
    Tracks flags to generate proper conditions for jcc/setcc/cmovcc.

    Args:
        lifter: Lifter instance
        bb: BasicBlock with instructions
        flag_state: tuple of (flag_setter_mnemonic, flag_operands) from
                    a preceding block, or None

    Returns:
        (stmts, flag_state) where stmts is a list of C statement strings
        and flag_state is a tuple for passing to the next block.
    """
    stmts = []
    insns = bb.instructions
    i = 0

    # The flag producers live at this point: None when unknown, otherwise one
    # candidate per joining path.  See advance_flag_state above.
    flag_cands = normalize_flag_state(flag_state)

    while i < len(insns):
        curr = insns[i]

        # Try cmp/test + jcc pattern first (2-instruction match)
        match = try_match_cmp_jcc(insns, i, lifter=lifter)
        if match:
            stmt, consumed = match
            flag_insn = insns[i]
            # The fused form tests the operands inline, but the flags stay live
            # for any later jcc, and that one reads the snapshot. Emit the
            # snapshot here too or those temps are stale - which silently sends
            # every reusing branch the wrong way.
            if flag_insn.mnemonic in ("cmp", "test") and len(flag_insn.operands) >= 2:
                stmts.extend(lifter._snapshot_flags(
                    flag_insn, flag_insn.operands, flag_insn.mnemonic))
            elif (flag_insn.mnemonic in ("comiss", "comisd", "ucomiss",
                                         "ucomisd")
                    and len(flag_insn.operands) >= 2):
                # Same reason as above, for the float compare: fusing consumes
                # the instruction, so nothing else would write _ah, and a
                # second branch reusing these flags would read one that was
                # never set.
                def _sse_txt(op):
                    if op.type == "reg" and op.reg and op.reg.startswith("xmm"):
                        return f"{op.reg}.f[0]"
                    if op.type == "mem":
                        return (f"MEMD({_fmt_mem(op)})" if op.mem_size == 8
                                else f"MEMF({_fmt_mem(op)})")
                    return _fmt_operand_read(op)
                stmts.extend(lifter._snapshot_float_flags(
                    flag_insn.mnemonic,
                    _sse_txt(flag_insn.operands[0]),
                    _sse_txt(flag_insn.operands[1])))
            stmts.extend(_snapshot_zf(lifter, flag_insn))
            stmts.append(stmt)
            flag_cands = ((flag_insn.mnemonic, list(flag_insn.operands)),)
            i += consumed
            continue

        # Handle jecxz/jcxz specially (not flag-based)
        if curr.mnemonic in ("jecxz", "jcxz"):
            results = lifter._lift_jcc(curr)
            stmts.extend(results)
            i += 1
            continue

        # Check if this instruction uses flags (jcc, setcc, cmovcc)
        if curr.is_cond_jump and flag_cands:
            result = agree(_make_condition, curr.mnemonic, flag_cands)
            if (lifter.needs_zf and (stored_zf(flag_cands)
                    or (not result and divergent_zf(flag_cands)))
                    and curr.mnemonic in ("je", "jz", "jne", "jnz")):
                result = ("_zf" if curr.mnemonic in ("je", "jz") else "!_zf",
                          "preserved zero flag")
            if result:
                cond_expr, desc = result
                target = curr.jump_target
                stmt = _emit_cond_goto(
                    cond_expr, curr.mnemonic, desc, target, lifter)
                stmts.append(stmt)
                i += 1
                continue

        if (curr.mnemonic in ("sete", "setne", "setb", "setae", "setbe",
                              "seta", "setl", "setge", "setle", "setg",
                              "sets", "setns")
                and flag_cands and len(curr.operands) >= 1):
            cond = agree(_make_setcc_value, curr.mnemonic, flag_cands)
            if (lifter.needs_zf and (stored_zf(flag_cands)
                    or (not cond and divergent_zf(flag_cands)))
                    and curr.mnemonic in ("sete", "setne")):
                cond = "_zf" if curr.mnemonic == "sete" else "!_zf"
            if cond:
                stmts.append(
                    _fmt_operand_write(curr.operands[0],
                                       f"({cond}) ? 1 : 0")
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        if (curr.mnemonic in ("cmove", "cmovne", "cmovb", "cmovae",
                              "cmovbe", "cmova", "cmovl", "cmovge",
                              "cmovle", "cmovg", "cmovs", "cmovns")
                and flag_cands and len(curr.operands) >= 2):
            cond = agree(_make_cmovcc_cond, curr.mnemonic, flag_cands)
            if (lifter.needs_zf and (stored_zf(flag_cands)
                    or (not cond and divergent_zf(flag_cands)))
                    and curr.mnemonic in ("cmove", "cmovne")):
                cond = "_zf" if curr.mnemonic == "cmove" else "!_zf"
            if cond:
                src = _fmt_operand_read(curr.operands[1])
                stmts.append(
                    f"if ({cond}) "
                    + _fmt_operand_write(curr.operands[0], src)
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        # A zero-count REP comparison preserves its incoming ZF. Capture the
        # known producer before the loop changes ECX/ESI/EDI.
        if "cmpsb" in curr.mnemonic and "rep" in curr.mnemonic and flag_cands:
            incoming_zf = agree(_make_condition, "je", flag_cands)
            if incoming_zf:
                stmts.append(f"_flags = ({incoming_zf[0]}); /* REP preserves ZF when ECX is zero */")

        # NEG sets CF when its operand is nonzero. Preserve that value when
        # a later SBB/ADC consumes it, skipping over EFLAGS-preserving
        # instructions (e.g. neg eax; push edi; sbb eax, eax).
        if curr.mnemonic == "neg":
            j = i + 1
            while (j < len(insns)
                    and insns[j].mnemonic in _EFLAGS_PRESERVE
                    and insns[j].mnemonic != "popfd"
                    and not insns[j].is_branch
                    and not insns[j].is_call
                    and not insns[j].is_ret):
                j += 1
            preserve = (j < len(insns)
                        and insns[j].mnemonic in ("sbb", "adc"))
            results = lifter._lift_neg(
                curr, curr.operands, preserve_carry=preserve)
        else:
            results = lifter.lift_instruction(insns[i])
        stmts.extend(results)
        stmts.extend(_snapshot_zf(lifter, curr))

        # Track flag-setting instructions
        flag_cands = advance_flag_state(curr, flag_cands)

        i += 1

    out_flag_state = flag_cands if flag_cands else None
    return stmts, out_flag_state
