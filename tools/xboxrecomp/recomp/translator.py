"""
Function-level x86 → C translator.

For each function:
1. Read raw bytes from XBE
2. Disassemble with Capstone
3. Build basic blocks
4. Lift each block to C statements
5. Generate a complete C function

Produces compilable C code using recomp_types.h macros.
"""

import bisect
import json
import glob
import os
import struct

# Import the functions, not the VA constants: configure_from_xbe() rebinds those
# at startup, so a by-value import would freeze the fallback layout.
from .config import va_to_file_offset, is_code_address
from .disasm import Disassembler
from .lifter import (Lifter, lift_basic_block, detect_seh_helpers,
                     block_flag_transfer, divergent_zf)


def _icall_caller_cleans(lines, icall_idx):
    """Does the caller pop this indirect call's arguments itself?

    An `add esp, N` immediately after the call is the caller cleaning up, which
    is cdecl; a callee that cleaned up its own arguments would leave nothing to
    do. The distinction decides what esp must be when the target fails to
    resolve, and getting it backwards corrupts the caller rather than merely
    leaking:

      stdcall  the callee pops the return address and the arguments,
               so esp ends at saved_esp             -> RECOMP_ICALL_SAFE
      cdecl    the callee pops only the return address,
               so esp ends at saved_esp - args      -> RECOMP_ICALL_CDECL

    The cleanup sits after the return-address label, because the generated code
    turns the return address into one, so skip the wrapper's closing brace, a
    blank line and at most one label before looking. It must be the *first*
    statement after that: anything else means this is not the matching cleanup.
    """
    import re
    j = icall_idx + 1
    seen_label = False
    while j < len(lines):
        stripped = lines[j].strip()
        if not stripped or stripped == "}":
            j += 1
            continue
        if re.match(r'^loc_[0-9A-Fa-f]+: ;$', stripped):
            if seen_label:
                return False
            seen_label = True
            j += 1
            continue
        return re.match(r'^esp = esp \+ (0x[0-9A-Fa-f]+|\d+);$', stripped) is not None
    return False


def _insert_block_trace(lines):
    """Record the guest register file at the top of every basic block.

    The balance check names the unbalanced return; when the epilogue is shared
    it does not name the block that did the damage. sub_0035E210 has 35 returns
    that all tear down the same 0x134 frame -- the exit index said "#6", and
    the trace said the step into loc_0035E881 was -348 where every other step
    was 0, which is the indirect call whose target leaked.

    It records every register, not just esp, because the question keeps
    arriving in a different one: sub_00076F10 walks a linked list in ebx and
    read it back as 1, and what was needed there was the last block that still
    had a real node.

    Emitted only for functions named by --trace-blocks; compiled away entirely
    unless the build defines RECOMP_ESP_CHECK.
    """
    import re
    out = []
    for line in lines:
        out.append(line)
        m = re.match(r'^loc_([0-9A-Fa-f]+): ;', line)
        if m:
            out.append("    RECOMP_BLOCK_TRACE(0x%su);" % m.group(1))
    return out


# Detection methods that mean "reached by a branch, never by a call".
# Such a function is entered with its caller's frame still live and its
# contract is to tear down a frame it did not build, so g_esp at its `ret`
# is *supposed* to differ from g_esp at its entry -- there is no call
# boundary to measure against. Checking them reported nine functions whose
# only fault was being a switch arm.
#
# code_section_table is deliberately not here: those addresses come from a
# dword in a code section and are as often a vtable slot, which is called
# normally, as a jump-table arm. Without evidence either way the honest
# choice is to keep reporting them.
FRAME_INHERITING_METHODS = frozenset((
    "tail_jump_alias",
    "tail_jump_target",
    "branch_gap_target",
    "jump_table",
))


def _close_fallthrough(lines, name, end_va):
    """Never let a lifted function run off the end of its C body.

    If the last statement is neither a `return` nor a `goto`, control reaches
    the closing brace and the C function returns -- with the guest frame still
    allocated and eax holding whatever was last computed. Nothing reports it;
    the damage surfaces frames away as a shifted stack read.

    In x86 that shape means the function falls through into the code at its end
    address, which is always a detection artefact: a candidate landed
    mid-function and truncated it. Two have now cost a day each --
    sub_0002D710 (a .data dword inside `add esp, 8`) and sub_005459EA (a
    code-section dword inside `mov ecx, 0x10000`, which left the entire D3D
    render-state dispatcher without a teardown and passed 2 to the next
    caller as a device pointer).

    Emitting the fall-through as a tail call makes it behave like the original:
    control continues at the next address. RECOMP_ITAIL rather than a direct
    call so that an end address with no body is *logged* instead of silently
    doing nothing.
    """
    import re
    for line in reversed(lines):
        stripped = line.strip()
        if not stripped or stripped == "}":
            continue
        if stripped.startswith("/*") or stripped.startswith("#"):
            continue
        if re.match(r'^loc_[0-9A-Fa-f]+:\s*;?$', stripped):
            continue
        if "return;" in stripped or re.search(r'\bgoto \w+;', stripped):
            return lines
        break
    lines.append("")
    lines.append(f"    /* Falls off the end of the body: {name} was truncated "
                 f"at 0x{end_va:08X}.")
    lines.append("       The original continues there, so continue there. */")
    lines.append(f"    RECOMP_ITAIL(0x{end_va:08X}u); return;")
    return lines


def _insert_esp_balance_check(lines, name, va, frame_inherited=False):
    """Bracket a lifted function with RECOMP_ESP_ENTER/LEAVE.

    The invariant is checked immediately before the lifted `ret`, where the
    function has finished its pops and stack adjustments but has not yet
    consumed the return address: g_esp must equal what it was at entry. The
    lifter emits that as `esp += 4 + N; return; /* ret N */`, so the check goes
    on the line before.

    Tail calls are deliberately exempt. `name(); return;` and
    `RECOMP_ITAIL(...); return;` hand the frame to the target, which performs
    the real return, so esp is *supposed* to differ here -- checking them would
    report every tail call in the binary and drown the real findings.

    Frame-inheriting entries are exempt too (see
    FRAME_INHERITING_METHODS): an alias, a switch arm or a shared tail starts
    in the middle of another function, so its pops belong to a prologue it does
    not contain -- the caller arranged that stack. They are unbalanced *by
    construction* and would be pure noise here.

    The macros compile to nothing unless RECOMP_ESP_CHECK is defined, so this
    runs on every build without costing anything.
    """
    import re

    if frame_inherited:
        return lines

    # Stack-relocating helpers are exempt, because moving esp is their job.
    # __chkstk is the one that matters: called with a byte count in eax, it
    # probes down a page at a time, points esp at the new block and copies the
    # return address onto it, so it returns esp lower by eax *by contract*.
    # sub_00470E50 is that function, and it reported the largest imbalance in
    # the binary (-4176) while being perfectly correct.
    #
    # The signal is assigning esp from a register. Ordinary code only ever
    # adjusts esp by a constant or through push/pop; a function that points it
    # somewhere computed is relocating the stack deliberately. `mov esp, ebp`
    # is the exception to the exception -- that is just a frame teardown, and
    # excluding it keeps every normal epilogue under the check.
    for line in lines:
        m = re.search(r'\besp = (e[a-z][a-z])\b', line)
        if m and m.group(1) not in ("ebp", "esp"):
            return lines
    out = []
    entered = False
    exit_index = 0
    ret_re = re.compile(r'^\s*esp \+= \d+; return;')
    for line in lines:
        # ENTER goes after the declarations, just before the first label, so
        # the entry esp is sampled before any lifted instruction runs.
        if not entered and re.match(r'^loc_[0-9A-Fa-f]+: ;', line):
            out.append("    RECOMP_ESP_ENTER();")
            out.append("")
            entered = True
        if entered and ret_re.match(line) and 'tail' not in line:
            indent = line[:len(line) - len(line.lstrip())]
            out.append(f'{indent}RECOMP_ESP_LEAVE("{name}", 0x{va:08X}u, '
                       f'{exit_index});')
            exit_index += 1
        out.append(line)
    return out


def _fixup_icall_esp_save(lines):
    """
    Post-process generated C lines to insert _icall_esp save points.

    When RECOMP_ICALL_SAFE is used, we need to save g_esp BEFORE any
    args are pushed so the macro can restore it on lookup failure.

    Scans backwards from each RECOMP_ICALL_SAFE line to find consecutive
    PUSH32 lines (the arg pushes), then inserts a save before the first.
    """
    import re
    result = []
    # Find indices of all ICALL_SAFE lines
    icall_indices = []
    for i, line in enumerate(lines):
        if 'RECOMP_ICALL_SAFE(' in line:
            icall_indices.append(i)

    if not icall_indices:
        return lines  # nothing to do

    # For each ICALL, determine where to insert the save
    insert_before = set()  # map: line_index → True (insert save before this line)
    for icall_idx in icall_indices:
        # The ICALL line itself is "PUSH32(esp, <retva>); RECOMP_ICALL_SAFE(...)"
        # Look backwards for consecutive lines containing PUSH32(esp,
        first_push_idx = icall_idx
        j = icall_idx - 1
        while j >= 0:
            stripped = lines[j].strip()
            # Skip blank lines
            if not stripped:
                j -= 1
                continue
            # A completed direct call ends the argument run, and must be tested
            # before the generic PUSH32 check below: a direct call is emitted as
            # "PUSH32(esp, <retva>); name();" on one line, so it also looks like
            # an argument push. Treating it as one put the _icall_esp save
            # *before* the direct call, and an ICALL that then failed rewound
            # g_esp over a call that had already returned.
            if '/* call 0x' in stripped:
                break
            # Check if this is a PUSH32 line (arg push)
            if stripped.startswith('PUSH32(esp,'):
                first_push_idx = j
                j -= 1
                continue
            # Check if this is a non-push instruction that could be part of
            # arg evaluation (e.g., "eax = MEM32(...);") - these are interleaved
            # with pushes in the x86 code. We need to look past them.
            # Stop at labels, gotos, other control flow, or other ICALL lines.
            if (re.match(r'^loc_[0-9A-Fa-f]+:', stripped) or
                'goto ' in stripped or
                'RECOMP_ICALL' in stripped or
                'return;' in stripped or
                stripped.startswith('if (') or
                stripped.startswith('POP32(')):
                break
            # It's an interleaved computation - skip past it
            j -= 1
            continue

        insert_before.add(first_push_idx)

    # Build result with saves inserted
    for i, line in enumerate(lines):
        if i in insert_before:
            # Determine indentation from the current line
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}{{ uint32_t _icall_esp = g_esp;")
        if ('RECOMP_ICALL_SAFE(' in line
                and _icall_caller_cleans(lines, i)):
            line = line.replace('RECOMP_ICALL_SAFE(', 'RECOMP_ICALL_CDECL(')
        result.append(line)
        if ('RECOMP_ICALL_SAFE(' in line
                or 'RECOMP_ICALL_CDECL(' in line):
            indent = line[:len(line) - len(line.lstrip())]
            result.append(f"{indent}}}")

    return result


# The x87 stack accessors the lifter's output is written against. Module-level
# so tools/conformance emits byte-identical macros: a harness with its own copy
# would still pass after these changed, which is the dangerous direction.
FP_STACK_MACROS = [
    "    #define fp_push(v) do { double _fp_value = (v); \\",
    "        g_fp_top = (g_fp_top + 7u) & 7u; \\",
    "        g_fp_stack[g_fp_top] = _fp_value; } while (0)",
    "    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)",
    "    #define fp_top() g_fp_stack[g_fp_top]",
    "    #define fp_st(i) g_fp_stack[(g_fp_top + (i)) & 7u]",
    "    #define fp_st1() fp_st(1)",
]

FP_STACK_UNDEFS = [
    "    #undef fp_push",
    "    #undef fp_pop",
    "    #undef fp_top",
    "    #undef fp_st",
    "    #undef fp_st1",
]


class FunctionTranslator:
    """Translates individual x86 functions to C source code."""

    def __init__(self, xbe_data, func_db, label_db=None, classification_db=None,
                 abi_db=None, seh_prolog=None, seh_epilog=None,
                 trace_functions=None, trace_block_functions=None):
        """
        xbe_data: bytes - raw XBE file contents
        func_db: dict - addr → function info from functions.json
        label_db: dict - addr → name from labels.json
        classification_db: dict - addr → classification from identified_functions.json
        abi_db: dict - addr → ABI info from abi_functions.json
        seh_prolog/seh_epilog: override the detected SEH helper addresses
        """
        self.xbe_data = xbe_data
        self.func_db = func_db
        self.label_db = label_db or {}
        self.classification_db = classification_db or {}
        self.abi_db = abi_db or {}
        self.trace_functions = set(trace_functions or ())
        self.trace_block_functions = set(trace_block_functions or ())
        self.disasm = Disassembler()
        self.lifter = Lifter(func_db=func_db, label_db=label_db, abi_db=abi_db,
                             xbe_data=xbe_data, seh_prolog=seh_prolog,
                             seh_epilog=seh_epilog)
        self.owned_function_starts = set()
        self.recovered_function_starts = set()
        self._recovered_cfg = {}
        self._ownership_ready = False

    def discover_static_indirect_targets(self):
        """Recover function entries from bounded static callback tables."""
        original_starts = sorted(self.func_db)
        recovered_callers = {}

        for caller, func_info in list(self.func_db.items()):
            end = func_info.get("end", caller)
            raw_bytes = self._read_func_bytes(caller, end)
            if not raw_bytes:
                continue
            instructions = self.disasm.disassemble_function(
                raw_bytes, caller, end)
            for lower, upper in self._find_static_indirect_ranges(instructions):
                targets = self._read_static_callback_table(
                    lower, upper, original_starts)
                if targets is None:
                    continue
                for target in targets:
                    if target not in self.func_db:
                        recovered_callers.setdefault(target, set()).add(caller)

        for target, callers in sorted(recovered_callers.items()):
            next_index = bisect.bisect_right(original_starts, target)
            if next_index == 0 or next_index >= len(original_starts):
                continue
            previous = self.func_db[original_starts[next_index - 1]]
            next_start = original_starts[next_index]
            next_func = self.func_db[next_start]
            if previous.get("end", previous["_addr"]) > target:
                continue
            section = next_func.get("section", "")
            if section in (".rdata", ".data"):
                continue

            raw_bytes = self._read_func_bytes(target, next_start)
            if not raw_bytes:
                continue
            instructions = self.disasm.disassemble_function(
                raw_bytes, target, next_start)
            if not instructions or not any(insn.is_ret for insn in instructions):
                continue

            self.func_db[target] = {
                "_addr": target,
                "start": f"0x{target:08X}",
                "end": next_start,
                "size": next_start - target,
                "name": self.label_db.get(target, f"sub_{target:08X}"),
                "section": section,
                "confidence": 0.9,
                "detection_method": "static_indirect_table",
                "num_instructions": len(instructions),
                "has_prologue": self._func_has_prologue(instructions),
                "calls_to": [],
                "called_by": sorted(callers),
            }
            self.recovered_function_starts.add(target)

        return self.recovered_function_starts

    @staticmethod
    def _find_static_indirect_ranges(instructions, max_bytes=0x10000):
        """Find immediate-backed ranges in functions that call a register."""
        constants = {}
        ranges = set()
        has_indirect_call = False

        for insn in instructions:
            operands = insn.operands
            if (insn.mnemonic == "mov" and len(operands) >= 2
                    and operands[0].type == "reg"):
                destination = operands[0].reg
                source = operands[1]
                if source.type == "imm":
                    constants[destination] = source.imm
                elif source.type == "reg" and source.reg in constants:
                    constants[destination] = constants[source.reg]
                else:
                    constants.pop(destination, None)
            elif (insn.mnemonic == "cmp" and len(operands) >= 2
                    and operands[0].type == "reg"
                    and operands[1].type == "reg"):
                lower = constants.get(operands[0].reg)
                upper = constants.get(operands[1].reg)
                if (lower is not None and upper is not None
                        and lower < upper and lower % 4 == 0
                        and upper % 4 == 0 and upper - lower <= max_bytes):
                    ranges.add((lower, upper))
            elif (insn.is_call and insn.call_target is None
                    and operands and operands[0].type == "reg"):
                has_indirect_call = True

        return sorted(ranges) if has_indirect_call else []

    def _read_static_callback_table(self, lower, upper, original_starts):
        """Validate and return a bounded table of static code pointers."""
        targets = []
        for entry_va in range(lower, upper, 4):
            offset = va_to_file_offset(entry_va)
            if offset is None or offset + 4 > len(self.xbe_data):
                return None
            target = struct.unpack_from('<I', self.xbe_data, offset)[0]
            if target in (0, 0xFFFFFFFF):
                continue

            index = bisect.bisect_right(original_starts, target)
            if index and self.func_db[original_starts[index - 1]].get(
                    "end", original_starts[index - 1]) > target:
                targets.append(target)
                continue
            if index == 0 or index >= len(original_starts):
                return None
            previous = self.func_db[original_starts[index - 1]]
            following = self.func_db[original_starts[index]]
            if (previous.get("section") != following.get("section")
                    or following.get("section") in (".rdata", ".data")
                    or va_to_file_offset(target) is None):
                return None
            targets.append(target)

        return targets if targets else None

    @staticmethod
    def _is_strong_entry(func_info):
        """Return whether an entry has evidence independent of seed recovery."""
        return bool(func_info.get("has_prologue") or func_info.get("called_by"))

    def discover_cfg_ownership(self):
        """Reassign weak seeds reached through a split computed-jump CFG."""
        if self._ownership_ready:
            return
        self._ownership_ready = True

        by_section = {}
        weak_by_section = {}
        for addr, info in self.func_db.items():
            section = info.get("section", "")
            if self._is_strong_entry(info):
                by_section.setdefault(section, []).append(addr)
            else:
                weak_by_section.setdefault(section, []).append(addr)

        for section, strong_starts in by_section.items():
            strong_starts.sort()
            weak_starts = sorted(weak_by_section.get(section, []))
            for index, start in enumerate(strong_starts[:-1]):
                original_end = self.func_db[start].get("end", start)
                upper = strong_starts[index + 1]
                if original_end >= upper:
                    continue

                weak_index = bisect.bisect_left(weak_starts, original_end)
                if (weak_index >= len(weak_starts)
                        or weak_starts[weak_index] >= upper):
                    continue

                raw_prefix = self._read_func_bytes(start, original_end)
                if not raw_prefix:
                    continue
                prefix = self.disasm.disassemble_function(
                    raw_prefix, start, original_end)
                bridges = {
                    insn.jump_target
                    for insn in prefix
                    if (insn.is_cond_jump and insn.jump_target is not None
                        and original_end <= insn.jump_target < upper)
                }
                has_indexed_jump = any(
                    insn.is_jump and insn.jump_target is None
                    and insn.operands and insn.operands[0].type == "mem"
                    and insn.operands[0].mem_index
                    for insn in prefix)
                if not bridges or not has_indexed_jump:
                    continue

                stop_addresses = {
                    addr for addr in self.func_db if start < addr < upper
                }
                recovered = self._recover_cfg(
                    start, upper, bridges, stop_addresses)
                if recovered is None:
                    continue
                instructions, jump_tables, cfg_targets = recovered
                owned = {
                    addr for addr in weak_starts[weak_index:]
                    if addr < upper and addr in cfg_targets
                }
                if not owned:
                    continue

                self.owned_function_starts.update(owned)
                self._recovered_cfg[start] = {
                    "end": max(insn.end_address for insn in instructions),
                    "instructions": instructions,
                    "jump_tables": jump_tables,
                }

    def _recover_cfg(self, start, upper, bridge_targets, stop_addresses):
        """Decode direct CFG edges and local indexed-table destinations."""
        raw_bytes = self._read_func_bytes(start, upper)
        if not raw_bytes:
            return None

        entry_points = {start, *bridge_targets}
        jump_tables = {}
        while True:
            instructions = self.disasm.disassemble_cfg(
                raw_bytes, start, upper, entry_points,
                stop_addresses=stop_addresses)
            changed = False
            for insn in instructions:
                if not insn.is_jump or insn.jump_target is not None:
                    continue
                if not insn.operands or insn.operands[0].type != "mem":
                    continue
                operand = insn.operands[0]
                if not operand.mem_index or operand.mem_base:
                    continue
                table_va = operand.mem_disp
                if not (start <= table_va < upper):
                    continue
                targets = self._read_local_jump_table(table_va, start, upper)
                if not targets:
                    continue
                jump_tables[table_va] = targets
                for target in targets:
                    if target not in entry_points:
                        entry_points.add(target)
                        changed = True
            if not changed:
                cfg_targets = {
                    insn.jump_target
                    for insn in instructions
                    if insn.jump_target is not None
                    and start <= insn.jump_target < upper
                }
                for targets in jump_tables.values():
                    cfg_targets.update(targets)
                return instructions, jump_tables, cfg_targets

    def _read_local_jump_table(self, table_va, lower, upper,
                               max_entries=256):
        """Read the contiguous pointer cluster around an indexed-jump base."""
        def scan(step, first):
            targets = []
            for index in range(first, max_entries + first):
                entry_va = table_va + step * index * 4
                offset = va_to_file_offset(entry_va)
                if offset is None or offset + 4 > len(self.xbe_data):
                    break
                target = struct.unpack_from('<I', self.xbe_data, offset)[0]
                if not (lower <= target < upper):
                    break
                targets.append(target)
            return targets

        backward = scan(-1, 1)
        forward = scan(1, 0)
        if len(backward) + len(forward) < 2:
            return []
        backward.reverse()
        return backward + forward

    def _read_func_bytes(self, start_va, end_va):
        """Read raw bytes for a function from the XBE."""
        offset = va_to_file_offset(start_va)
        if offset is None:
            return None
        size = end_va - start_va
        if offset + size > len(self.xbe_data):
            return None
        return self.xbe_data[offset:offset + size]

    def _determine_calling_convention(self, func_info):
        """Guess calling convention from function properties."""
        name = func_info.get("name", "")
        # thiscall methods have ecx = this
        if "thiscall" in name or func_info.get("calling_convention") == "thiscall":
            return "thiscall"
        return "cdecl"

    def _func_has_prologue(self, instructions):
        """Check if function starts with push ebp; mov ebp, esp."""
        if len(instructions) < 2:
            return False
        return (instructions[0].mnemonic == "push" and
                instructions[0].op_str == "ebp" and
                instructions[1].mnemonic == "mov" and
                instructions[1].op_str == "ebp, esp")

    def translate_function(self, func_addr, func_info):
        """
        Translate a single function to C code.
        Returns a string of C source code, or None on failure.
        """
        start = func_addr
        recovered = self._recovered_cfg.get(start)
        end = recovered["end"] if recovered else func_info.get("end")
        if not end:
            end = start + func_info.get("size", 0)
        if end <= start:
            return None

        name = func_info.get("name", f"sub_{start:08X}")
        size = end - start

        # Read bytes from XBE
        raw_bytes = self._read_func_bytes(start, end)
        if not raw_bytes:
            return None

        # Set function bounds for the lifter
        self.lifter.func_start = start
        self.lifter.func_end = end
        self.lifter.jump_table_targets = (
            recovered["jump_tables"] if recovered else {})

        # Disassemble
        instructions = (recovered["instructions"] if recovered else
                        self.disasm.disassemble_function(raw_bytes, start, end))
        if not instructions:
            return None

        # Collect switch table targets as extra block leaders
        switch_leaders = set()
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                targets = self.lifter._analyze_switch_table(insn.operands)
                for t in targets:
                    if start <= t < end:
                        switch_leaders.add(t)

        # A switch target the decode never produced an instruction for cannot
        # become a block leader, so it gets no label and its `goto` is dropped
        # as dead code -- the case then falls through to an unresolved indirect
        # branch. That happens whenever the jump table sits in .text ahead of
        # the code it points at: decoding the table as instructions leaves the
        # stream misaligned across the first case. Re-decode, telling the
        # disassembler where the real instruction boundaries are.
        if recovered is None:
            missing = switch_leaders - {insn.address for insn in instructions}
            if missing:
                instructions = self.disasm.disassemble_function(
                    raw_bytes, start, end, resync=missing)

        # Build basic blocks
        blocks = self.disasm.build_basic_blocks(
            instructions, start, end,
            extra_leaders=switch_leaders if switch_leaders else None)
        if not blocks:
            return None

        # Get classification and ABI info
        cls_info = self.classification_db.get(start, {})
        category = cls_info.get("category", "unknown")
        module = cls_info.get("module", "")
        source_file = cls_info.get("source_file", "")
        abi_info = self.abi_db.get(start, {})

        # ABI-derived info (kept for comments)
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)
        return_hint = abi_info.get("return_hint", "int_or_void")
        frame_type = abi_info.get("frame_type", "fpo_leaf")
        stack_frame_size = abi_info.get("stack_frame_size", 0)

        # Determine which registers are used
        used_regs = self._find_used_registers(instructions)
        used_xmm = self._find_used_xmm(instructions)
        has_prologue = self._func_has_prologue(instructions)
        has_fpu = any(insn.mnemonic.startswith("f") for insn in instructions)

        # Volatile registers (eax, ecx, edx, esp) are globals - don't declare
        # them as locals. The RECOMP_GENERATED_CODE #define maps register names
        # to the global variables via preprocessor macros.
        volatile_regs = {"eax", "ecx", "edx", "esp"}

        # Ensure ebp tracked if function uses 'leave' (implicit ebp)
        if any(insn.mnemonic == "leave" for insn in instructions):
            used_regs.add("ebp")

        # Guest control leaves the bottom of this function when its last
        # instruction neither returns, jumps, nor traps. A function the lifter
        # split into consecutive pieces continues into the next piece exactly
        # as an unconditional tail jump would, so bridge to it the same way.
        # Only an address that is itself a translated function start is a
        # usable target; anything else is an analysis boundary gap with no
        # callable symbol.
        last_insn = instructions[-1]
        continues_past_end = not (
            last_insn.is_terminator
            or last_insn.mnemonic in ("int3", "ud2", "hlt"))
        fallthrough_target = None
        if (continues_past_end and end in self.func_db
                and end not in self.owned_function_starts):
            fallthrough_target = end

        # Ensure ebp tracked if function has tail jumps (lifter emits
        # g_seh_ebp = ebp before external jmp, external jcc, and indirect jmp,
        # and translate_function emits it before a fallthrough tail call).
        has_tail_jump = any(
            (insn.mnemonic == "jmp" and (
                (insn.jump_target and not (start <= insn.jump_target < end))
                or not insn.jump_target  # indirect jmp
            ))
            or (insn.is_cond_jump and insn.jump_target
                and not (start <= insn.jump_target < end))
            for insn in instructions
        )
        if has_tail_jump or fallthrough_target is not None:
            used_regs.add("ebp")


        # Ensure ebp is declared if the function talks to the SEH helpers: the
        # lifter emits a publish before and a read-back after those calls, both
        # of which name ebp even in a function that otherwise never touches it.
        #
        # These addresses are per-title and detected at startup. They used to be
        # hardcoded to one game's CRT here, so for every other title the forcing
        # silently never fired and the generated C failed to compile with
        # "'ebp': undeclared identifier". Read the lifter's combined set rather
        # than rebuilding one from its two attributes: those hold sets now (a
        # title can link several prologs), and a set of sets matches no call
        # target, which brings the same build failure straight back.
        seh_funcs = self.lifter.SEH_HELPERS
        if seh_funcs and any(insn.call_target in seh_funcs
                             for insn in instructions):
            used_regs.add("ebp")

        # Build call targets list
        call_targets = set()
        for insn in instructions:
            if insn.call_target and is_code_address(insn.call_target):
                call_targets.add(insn.call_target)

        # All translated functions are void(void).
        # Arguments pass via the global simulated stack (push instructions).
        # Return values pass via g_eax (the global eax register).
        ret_type = "void"
        param_str = "void"

        # Generate C code
        lines = []

        # Header comment
        lines.append(f"/**")
        lines.append(f" * {name}")
        lines.append(f" * Original: 0x{start:08X} - 0x{end:08X} ({size} bytes, {len(instructions)} insns)")
        if category != "unknown":
            lines.append(f" * Category: {category}")
        if source_file:
            lines.append(f" * Source: {source_file}")
        lines.append(f" * CC: {cc}, {num_params} params, returns {return_hint}")
        if frame_type == "ebp_frame":
            lines.append(f" * Frame: EBP-based ({stack_frame_size} bytes locals)")
        else:
            lines.append(f" * Frame: {frame_type}")
        lines.append(f" */")

        # Function signature
        lines.append(f"{ret_type} {name}({param_str})")
        lines.append(f"{{")

        # Optional entry trace. Bring-up is mostly "which of these ten init
        # calls does it not come back from", and answering that by overriding
        # a function loses the body you were trying to observe.
        if start in self.trace_functions:
            lines.append(
                f'    RECOMP_TRACE_ENTER("{name}", 0x{start:08X});')
        # Entry tracing shows what went in; it cannot show what came back, and
        # "this function returns with esi wrong" is exactly the question that
        # kept coming up. The lifter emits the matching exit trace at each ret.
        self.lifter.trace_exit_name = name if start in self.trace_functions else None

        # ebp is the only callee-saved register declared as a local.
        # ebx, esi, edi are global via #define macros (g_ebx, g_esi, g_edi)
        # and must NOT be declared locally, otherwise the local shadows
        # the global and cross-function register passing breaks.
        # Volatile registers (eax, ecx, edx, esp) are also global via macros.
        reg_decls = []
        if "ebp" in used_regs:
            reg_decls.append("ebp")
        if reg_decls:
            lines.append(f"    uint32_t {', '.join(reg_decls)};")

        # A function with no `push ebp; mov ebp, esp` prologue that still reads
        # ebp is addressing its *caller's* frame. MSVC emits these for shared
        # tails and helpers; Halo's CRT float formatting (sub_001DEC07) opens
        # with `cmp byte ptr [edx+0xe], 5` and goes straight to [ebp-0xa4].
        #
        # ebp is a per-function local, so without this it starts as garbage and
        # every [ebp-N] store lands wherever that points. In Halo that was the
        # fake TIB at Xbox VA 0: `mov [ebp-0xa2], bx` destroyed fs:[4], and the
        # next TLS lookup faulted, thousands of calls away from the cause.
        #
        # Inherit it instead. Frame-establishing functions publish g_ebp when
        # they execute `mov ebp, esp` (see the lifter), so the value is the
        # nearest enclosing frame -- which is exactly what the hardware ebp
        # would still hold. Deliberately not the same as making ebp global:
        # that also changes save/restore, and a callee that fails to restore
        # then corrupts its caller (tried; esp underflowed inside XapiStartup).
        if "ebp" in used_regs and not self._func_has_prologue(instructions):
            lines.append("    ebp = g_ebp;  /* frameless: caller's frame */")

        # Add _flags variable if function has conditional instructions.
        #
        # `cmps` and `scas` count even with no branch in sight: the lifter
        # expands their rep forms into a loop that tests _flags to decide
        # whether to break, so a function whose only conditional behaviour is
        # a `repe cmpsb` still needs the declaration. Missing it is a build
        # failure ("'_flags': undeclared identifier"), not a silent wrong
        # answer, but it only appears once such a function is detected at all.
        has_conditionals = any(
            insn.is_cond_jump or insn.mnemonic.startswith("set")
            or insn.mnemonic.startswith("cmov")
            or "cmps" in insn.mnemonic or "scas" in insn.mnemonic
            for insn in instructions)
        if has_conditionals:
            lines.append(f"    int _flags = 0; /* fallback flag var */")
        self.lifter.needs_flags = has_conditionals

        # Direction flag. std/cld choose whether a string instruction walks up
        # or down, and dropping it makes a backwards scan run forwards off the
        # end of its buffer. That is what broke the CRT's strrchr
        # (sub_0046EC00): it returned NULL for every path, so the title's
        # content root kept the executable's file name and every resource
        # lookup was built from "D:\default.xbe" instead of "D:\\".
        if any(insn.mnemonic in ("std", "cld")
               or "movs" in insn.mnemonic or "stos" in insn.mnemonic
               or "lods" in insn.mnemonic or "scas" in insn.mnemonic
               or "cmps" in insn.mnemonic
               for insn in instructions):
            lines.append("    int _df = 1; /* direction flag: +1 up, -1 down */")

        # Flag snapshot temporaries: a cmp/test records its operands here,
        # zero- and sign-extended to the compare's own width, so the branch
        # tests what the compare actually saw. Declared whenever a cmp/test
        # exists - the consuming jcc can be in a later basic block, or absent.
        # XADD also emits operand snapshots even without a later branch.
        # cmpxchg records the same snapshot -- its flags are the EAX vs
        # destination comparison -- so it needs these declared too.  It was
        # missing here, which is why the hand-applied cmpxchg sites each
        # needed their declarations added by hand and regeneration broke.
        if any(insn.mnemonic.replace("lock ", "") in ("cmp", "test", "cmpxchg", "xadd")
               for insn in instructions):
            lines.append("    uint32_t _fa = 0, _fb = 0;")
            lines.append("    int32_t _fas = 0, _fbs = 0;")
            lines.append("    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;")
            # Flag snapshot: a cmp/test that is not fused with its jcc records
            # its operands here, zero- and sign-extended to the compare's own
            # width, so the branch tests what the compare saw.


        # Add _cf for carry-dependent instructions (sbb, adc)
        has_carry = any(insn.mnemonic in ("sbb", "adc")
                        for insn in instructions)
        if has_carry:
            lines.append(f"    int _cf = 0; /* carry flag */")
        # Only functions that consume CF pay for producing it: an adc/sbb
        # reading a never-written _cf silently drops every carry, which
        # corrupts multi-word arithmetic (add/adc pairs) and the shr/adc
        # idiom MSVC emits for odd trailing elements.
        self.lifter.needs_cf = has_carry

        # Add _ah for the SSE compare-through-AH idiom.
        #
        # MSVC compiles a float compare whose branch is not adjacent as
        #     ucomiss xmm1, xmm0 ; lahf ; test ah, 0x44 ; jp/jnp
        # so the result travels through AH rather than straight to a jcc. Both
        # halves were comments in the lifter, leaving the branch to test AH
        # from whatever eax happened to hold. sub_00047010 walks a table with
        # `edx += 0x18` until a float matches; with the compare missing it never
        # matched, and it ran off the end of the title's 1MB heap.
        has_ah_flags = any(
            insn.mnemonic in ("lahf", "comiss", "comisd", "ucomiss",
                              "ucomisd")
            for insn in instructions)
        if has_ah_flags:
            lines.append("    uint8_t _ah = 0; /* EFLAGS low byte for lahf */")
        self.lifter.needs_ah = has_ah_flags
        self.lifter.publishes_ebp = self._func_has_prologue(instructions)

        # SSE/MMX register declarations
        if used_xmm:
            xmm_regs = sorted([r for r in used_xmm if r.startswith("xmm")])
            mmx_regs = sorted([r for r in used_xmm if r.startswith("mm")
                               and not r.startswith("xmm")])
            # XMM is architectural state and is declared globally by the
            # runtime, exactly like the GPRs and the x87 stack. Declaring it
            # here would shadow that global with a fresh zeroed local, so a
            # value produced in one block and read in the next - a return
            # value in xmm0, or a spill straddling a branch - would be lost.
            # MMX is architectural state for the same reason XMM is, and
            # is declared globally by the runtime.  Declaring it here shadowed
            # the global with an uninitialised local, so a register set up by
            # a caller -- which the XMV pixel loops rely on -- read garbage.
            _ = mmx_regs

        # The x87 stack is architectural state and survives guest calls. Some
        # detector boundaries also split one original CRT helper into several
        # generated C functions, so function-local storage loses live ST values.
        if has_fpu:
            lines.extend(FP_STACK_MACROS)

        # For fpo_leaf functions that use ebp: initialize from g_seh_ebp.
        # In x86, these functions inherit EBP from their caller (typically
        # via a tail jump that shares the caller's frame). In our C translation,
        # ebp is a local variable that would start uninitialized, causing
        # crashes when the function reads MEM32(ebp + offset). The g_seh_ebp
        # global bridges ebp across function boundaries.
        if frame_type == "fpo_leaf" and "ebp" in used_regs and not has_prologue:
            lines.append(f"    ebp = g_seh_ebp; /* fpo_leaf: inherit caller's frame */")

        lines.append(f"")

        # Generate code for each basic block
        # Create a set of addresses that need labels
        label_addrs = set()
        for bb in blocks:
            for succ in bb.successors:
                label_addrs.add(succ)
        # Also add any jump targets within the function
        for insn in instructions:
            if insn.jump_target and start <= insn.jump_target < end:
                label_addrs.add(insn.jump_target)
        # Add switch table targets (indirect jmp with intra-function table)
        for insn in instructions:
            if insn.mnemonic == "jmp" and not insn.jump_target and insn.operands:
                switch_targets = self.lifter._analyze_switch_table(insn.operands)
                for t in switch_targets:
                    label_addrs.add(t)

        # Which blocks can reach each block. Flag state has to follow control
        # flow, not address order: an optimising compiler routinely lets a jcc
        # consume a `cmp` from a block that is not its immediate predecessor in
        # memory. Threading the state linearly then hands that jcc the flags of
        # whatever instruction happens to sit above it -- silently, and with a
        # perfectly plausible-looking condition.
        preds = {bb.start: set() for bb in blocks}
        unresolved_indirect = False
        for i, bb in enumerate(blocks):
            last = bb.instructions[-1] if bb.instructions else None
            if last is None:
                continue
            if last.jump_target in preds:
                preds[last.jump_target].add(bb.start)
            # An indirect jmp through a switch table reaches every entry in it.
            # These edges were only ever collected for labelling, so the
            # predecessor map said a switch arm had one way in when it had
            # twenty.  Threading flags along that map then resolves a branch
            # from an incomplete vote -- confidently, and wrongly.
            if (last.mnemonic == "jmp" and not last.jump_target
                    and last.operands):
                targets = self.lifter._analyze_switch_table(last.operands)
                if targets:
                    for t in targets:
                        if t in preds:
                            preds[t].add(bb.start)
                else:
                    # An indirect jump we could not resolve can land on any
                    # label in the function, so no block's predecessor list is
                    # trustworthy and flags must not cross a block boundary
                    # here at all.
                    unresolved_indirect = True
            # A conditional jump also falls through; ret and an unconditional
            # jmp do not.
            leaves = last.is_ret or last.mnemonic in ("jmp", "int3", "ud2", "hlt")
            if not leaves and i + 1 < len(blocks):
                preds[blocks[i + 1].start].add(bb.start)

        # Flag state, solved over the CFG rather than threaded in address
        # order.  Walking in address order means a loop head's back edge has
        # not been lifted when the head is reached, so its state was unknown
        # and every branch there fell back to _flags -- a variable that is zero
        # at function entry and never written.  Conker's inflate loop exits on
        # exactly such a branch, so it never exited and the boot video never
        # decoded.
        #
        # Three-valued, solved to a fixed point:
        #
        #   _TOP    not computed yet; contributes nothing to a join
        #   tuple   one candidate producer per joining path
        #   None    unknown -- clobbered, or paths that cannot be reconciled
        #
        # A join takes the union of its predecessors and is poisoned by any
        # predecessor whose state is unknown, so a path that clobbers the flags
        # can never be silently dropped from the vote.  Agreement is then
        # decided per consumer, on the condition each candidate generates
        # rather than on the mnemonic: `sub eax, x` and `dec eax` are different
        # producers that both give `(eax == 0)` for je, and rejecting that pair
        # is what left the loop above spinning.
        _TOP = object()
        out_flags = {bb.start: _TOP for bb in blocks}
        in_flags = {bb.start: None for bb in blocks}
        for _round in range(0 if unresolved_indirect else 64):
            changed = False
            for bb in blocks:
                sources = preds[bb.start]
                if bb.start == start or not sources:
                    incoming = None
                else:
                    incoming = _TOP
                    for p in sources:
                        state = out_flags[p]
                        if state is _TOP:
                            continue          # nothing known from it yet
                        if state is None:
                            incoming = None   # one unknown path poisons the join
                            break
                        if incoming is _TOP:
                            incoming = tuple(state)
                        else:
                            merged = list(incoming)
                            for cand in state:
                                if cand not in merged:
                                    merged.append(cand)
                            incoming = tuple(merged)
                if incoming is _TOP:
                    continue                  # revisit once a predecessor lands
                in_flags[bb.start] = incoming
                result = block_flag_transfer(bb, incoming)
                if result != out_flags[bb.start]:
                    out_flags[bb.start] = result
                    changed = True
            if not changed:
                break
        else:
            # Did not settle in 64 rounds.  Fall back to what this did before
            # the solver existed rather than emit a state that may be missing
            # a candidate.
            in_flags = {bb.start: None for bb in blocks}

        # Different incoming comparisons cannot share a condition expression.
        # Preserve each producer's actual zero flag for those joins instead of
        # reading the never-written generic fallback flag.
        self.lifter.needs_zf = (any(divergent_zf(s) for s in in_flags.values())
                               or any(i.mnemonic in ("inc", "dec")
                                      for bb in blocks for i in bb.instructions))
        if self.lifter.needs_zf:
            lines.append("    int _zf = 0; /* zero flag survives writes and CFG joins */")
        out_state = {}
        for bb in blocks:
            # Emit label if this block is a branch target
            if bb.start in label_addrs or bb.start == start:
                # The trailing ';' is load-bearing: C requires a statement after
                # a label, and a block whose instructions all emit comments only
                # (a lone `cmp`, which just sets flags for the next jcc) would
                # otherwise produce `loc_X:` immediately before `}` and fail to
                # compile. The null statement costs nothing and is always valid.
                lines.append(f"loc_{bb.start:08X}: ;")

                # A block reached from at or after its own address is a loop
                # head, and a loop is where a guest can spend unbounded time
                # without calling into the runtime.  Delivery otherwise only
                # happens where the guest does -- from the MMIO fault handler,
                # in practice -- so a compute-bound loop could never be
                # interrupted at all, and a title waiting inside one on a
                # counter that only an interrupt advances deadlocks outright.
                #
                # A back-edge target is a safe point by construction: the
                # lifted register file holds exactly the guest's state, and no
                # partially-emitted instruction is in flight.  The cost when
                # nothing is pending is a load of a global and an untaken
                # branch.
                if any(p >= bb.start for p in preds[bb.start]):
                    lines.append("    RECOMP_SAFE_POINT();")

            # The solver adds resolutions; it must never remove one.
            #
            # Falling back to _flags is not a safe default -- it is a wrong
            # answer, not an unknown.  _flags is zero at function entry and
            # written by almost nothing, so a branch that reads it takes its
            # not-taken edge for the life of the run.  When the solver brings a
            # back edge into the vote and that edge disagrees, the join
            # correctly reports "unknown", but replacing a resolved condition
            # with _flags is a regression: sub_0035E35C lost a `jne` that way
            # and the DSP mixer crashed on frame 13.
            #
            # So where the join cannot decide, keep what the address-order rule
            # used to conclude.  That is exactly today's behaviour, and the
            # solver is then purely additive.
            incoming = in_flags[bb.start]
            if incoming is None:
                sources = preds[bb.start]
                if bb.start != start and sources and all(
                        p in out_state for p in sources):
                    states = [out_state[p] for p in sources]
                    incoming = states[0]
                    for other in states[1:]:
                        if other != incoming:
                            incoming = None
                            break

            stmts, out_state[bb.start] = lift_basic_block(
                self.lifter, bb, flag_state=incoming)
            for stmt in stmts:
                lines.append(f"    {stmt}")

            lines.append(f"")

        # Continue into the next function when control runs off the bottom.
        if fallthrough_target is not None:
            ft_name = self.lifter._call_target_name(fallthrough_target)
            lines.append(f"    g_seh_ebp = ebp; {ft_name}(); return;"
                         f" /* fallthrough 0x{fallthrough_target:08X} */")
            lines.append(f"")

        # Insert _icall_esp save points before RECOMP_ICALL_SAFE arg pushes.
        # The pattern is: optional PUSH32 args, then
        # PUSH32(esp, <retva>); RECOMP_ICALL_SAFE(...).
        # We insert "uint32_t _icall_esp = g_esp;" before the first arg push.
        lines = _fixup_icall_esp_save(lines)

        # Bracket the function so an unbalanced return names itself instead of
        # corrupting its caller silently. Costs nothing unless the build
        # defines RECOMP_ESP_CHECK.
        lines = _insert_esp_balance_check(
            lines, name, start,
            frame_inherited=(func_info.get("frame_inherited", False)
                             or func_info.get("detection_method")
                             in FRAME_INHERITING_METHODS))
        if start in self.trace_block_functions:
            lines = _insert_block_trace(lines)

        # Validate: comment out goto targets that reference missing labels
        # (dead code after unconditional jumps may reference non-existent labels)
        import re
        defined_labels = set()
        goto_lines = []
        for idx, line in enumerate(lines):
            lbl_match = re.match(r'^(loc_[0-9A-Fa-f]+):', line)
            if lbl_match:
                defined_labels.add(lbl_match.group(1))
            goto_match = re.search(r'goto (loc_[0-9A-Fa-f]+);', line)
            if goto_match:
                goto_lines.append((idx, goto_match.group(1)))
        for idx, target in goto_lines:
            if target not in defined_labels:
                lines[idx] = lines[idx].replace(
                    f"goto {target};",
                    f"(void)0; /* goto {target} - dead code, label not in function */")

        # Ensure labels at end of function have a statement after them.
        # In C, a label must be followed by a statement; a comment alone is not
        # enough.  Walk backwards from the end and if the last real content is a
        # label (with only blank lines / comments after it), insert "(void)0;".
        _last_label_idx = None
        _has_stmt_after = False
        for _ri in range(len(lines) - 1, -1, -1):
            _s = lines[_ri].strip()
            if not _s:
                continue
            if _s.startswith("/*") and _s.endswith("*/"):
                continue
            if re.match(r'^loc_[0-9A-Fa-f]+:', _s):
                _last_label_idx = _ri
                break
            _has_stmt_after = True
            break
        if _last_label_idx is not None and not _has_stmt_after:
            lines.insert(_last_label_idx + 1, "    (void)0;")

        lines = _close_fallthrough(lines, name, end)

        # Undefine FPU macros
        if has_fpu:
            lines.extend(FP_STACK_UNDEFS)

        lines.append(f"}}")
        lines.append(f"")

        return "\n".join(lines)

    def _find_used_registers(self, instructions):
        """Find which 32-bit registers are referenced by any instruction."""
        regs = set()
        reg_map = {
            "eax": "eax", "ax": "eax", "al": "eax", "ah": "eax",
            "ebx": "ebx", "bx": "ebx", "bl": "ebx", "bh": "ebx",
            "ecx": "ecx", "cx": "ecx", "cl": "ecx", "ch": "ecx",
            "edx": "edx", "dx": "edx", "dl": "edx", "dh": "edx",
            "esi": "esi", "si": "esi",
            "edi": "edi", "di": "edi",
            "ebp": "ebp", "bp": "ebp",
            "esp": "esp", "sp": "esp",
        }
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg in reg_map:
                    regs.add(reg_map[op.reg])
                elif op.type == "mem":
                    if op.mem_base and op.mem_base in reg_map:
                        regs.add(reg_map[op.mem_base])
                    if op.mem_index and op.mem_index in reg_map:
                        regs.add(reg_map[op.mem_index])
        return regs

    def _find_used_xmm(self, instructions):
        """Find which XMM and MMX registers are used."""
        regs = set()
        for insn in instructions:
            for op in insn.operands:
                if op.type == "reg" and op.reg:
                    if op.reg.startswith("xmm") or op.reg.startswith("mm"):
                        regs.add(op.reg)
        return regs


class BatchTranslator:
    """Translates multiple functions and writes C source files."""

    def __init__(self, xbe_path, func_json_path, labels_json_path=None,
                 identified_json_path=None, abi_json_path=None,
                 output_dir=None, seh_prolog=None, seh_epilog=None,
                 trace_functions=None, trace_block_functions=None):
        self.xbe_path = xbe_path
        self.output_dir = output_dir or os.path.join(
            os.path.dirname(__file__), "output")

        # Load XBE
        with open(xbe_path, "rb") as f:
            self.xbe_data = f.read()

        # Load function database
        with open(func_json_path, "r") as f:
            func_list = json.load(f)

        self.func_db = {}
        for func in func_list:
            addr = int(func["start"], 16)
            func["_addr"] = addr
            if "end" in func:
                func["end"] = int(func["end"], 16)
            self.func_db[addr] = func

        # Load labels
        self.label_db = {}
        if labels_json_path and os.path.exists(labels_json_path):
            with open(labels_json_path, "r") as f:
                labels = json.load(f)
            for lbl in labels:
                addr = int(lbl["address"], 16)
                self.label_db[addr] = lbl["name"]

        # Load classifications
        self.classification_db = {}
        if identified_json_path and os.path.exists(identified_json_path):
            with open(identified_json_path, "r") as f:
                identified = json.load(f)
            for entry in identified:
                addr = int(entry["start"], 16)
                self.classification_db[addr] = entry

        # Load ABI data
        self.abi_db = {}
        if abi_json_path and os.path.exists(abi_json_path):
            with open(abi_json_path, "r") as f:
                abi_list = json.load(f)
            for entry in abi_list:
                addr = int(entry["address"], 16)
                self.abi_db[addr] = entry

        # Detect the SEH helpers once here rather than per-Lifter, so the
        # result can be reported and overridden from the command line.
        if seh_prolog is None or seh_epilog is None:
            found_prolog, found_epilog = detect_seh_helpers(
                self.func_db, self.xbe_data, verbose=True)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.seh_prolog = seh_prolog
        self.seh_epilog = seh_epilog

        # Create translator
        self.translator = FunctionTranslator(
            self.xbe_data, self.func_db, self.label_db,
            self.classification_db, self.abi_db,
            seh_prolog=seh_prolog, seh_epilog=seh_epilog,
            trace_functions=trace_functions,
            trace_block_functions=trace_block_functions)
        self.translator.discover_static_indirect_targets()
        self.translator.discover_cfg_ownership()

    def get_functions_by_category(self, categories=None, exclude_categories=None):
        """
        Get function addresses filtered by category.
        Returns list of (addr, func_info) tuples.
        """
        result = []
        for addr, func_info in sorted(self.func_db.items()):
            if addr in self.translator.owned_function_starts:
                continue
            cls_info = self.classification_db.get(addr, {})
            cat = cls_info.get("category", "unknown")

            if categories and cat not in categories:
                continue
            if exclude_categories and cat in exclude_categories:
                continue

            result.append((addr, func_info))
        return result

    def _make_declaration(self, addr, name):
        """Generate a function declaration string.
        All translated functions are void(void) - args pass via stack,
        return values via g_eax."""
        return f"void {name}(void)"

    def translate_single(self, addr):
        """Translate a single function by address. Returns C code string."""
        func_info = self.func_db.get(addr)
        if not func_info:
            return None
        return self.translator.translate_function(addr, func_info)

    def translate_batch(self, func_list, output_file=None, max_funcs=None,
                        verbose=False):
        """
        Translate a batch of functions.

        func_list: list of (addr, func_info) tuples
        output_file: path to write combined C output
        max_funcs: limit number of functions
        verbose: print progress

        Returns dict with statistics.
        """
        os.makedirs(self.output_dir, exist_ok=True)

        func_list = [item for item in func_list
                     if item[0] not in self.translator.owned_function_starts]

        if max_funcs:
            func_list = func_list[:max_funcs]

        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
            "total_insns": 0,
        }

        c_chunks = []
        c_chunks.append("/**")
        c_chunks.append(" * Xbox - Mechanically Translated Game Code")
        c_chunks.append(f" * Generated by tools/recomp from original Xbox x86 code.")
        c_chunks.append(f" * Functions: {len(func_list)}")
        c_chunks.append(" */")
        c_chunks.append("")
        c_chunks.append('#define RECOMP_GENERATED_CODE')
        c_chunks.append('#include "recomp_types.h"')
        c_chunks.append('#include <math.h>')
        c_chunks.append("")
        c_chunks.append("/* Forward declarations */")

        # Forward declarations
        for addr, func_info in func_list:
            name = func_info.get("name", f"sub_{addr:08X}")
            decl = self._make_declaration(addr, name)
            c_chunks.append(f"{decl};")
        c_chunks.append("")
        c_chunks.append("/* ═══════════════════════════════════════════════════ */")
        c_chunks.append("")

        # Translate each function
        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 100 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name} at 0x{addr:08X}...")

            code = self.translator.translate_function(addr, func_info)
            if code:
                c_chunks.append(code)
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")

                # Count instructions
                num_insns = func_info.get("num_instructions", 0)
                stats["total_insns"] += num_insns
            else:
                c_chunks.append(f"/* FAILED to translate {name} at 0x{addr:08X} */")
                c_chunks.append(f"void {name}(void) {{ /* translation failed */ }}")
                c_chunks.append("")
                stats["failed"] += 1

        # Write output
        if output_file is None:
            output_file = os.path.join(self.output_dir, "recompiled.c")

        output_text = "\n".join(c_chunks)
        with open(output_file, "w", encoding="utf-8") as f:
            f.write(output_text)

        stats["output_file"] = output_file
        stats["output_size"] = len(output_text)

        return stats

    def translate_by_category(self, categories, output_prefix=None,
                              max_per_file=500, verbose=False):
        """
        Translate functions grouped by category, one file per category.
        Returns dict with per-category stats.
        """
        os.makedirs(self.output_dir, exist_ok=True)
        all_stats = {}

        for cat in categories:
            funcs = self.get_functions_by_category(categories={cat})
            if not funcs:
                continue

            prefix = output_prefix or cat
            out_file = os.path.join(self.output_dir, f"{prefix}.c")

            if verbose:
                print(f"\nCategory: {cat} ({len(funcs)} functions)")

            stats = self.translate_batch(
                funcs, output_file=out_file,
                max_funcs=max_per_file, verbose=verbose)
            all_stats[cat] = stats

        return all_stats

    def translate_batch_split(self, func_list, output_dir, chunk_size=1000,
                              header_name="recomp_funcs.h",
                              prefix="recomp", verbose=False, manual=None):
        """
        Translate functions into multiple .c files + a shared header.

        Generates:
          output_dir/recomp_funcs.h       - forward declarations for all functions
          output_dir/recomp_0000.c        - chunk 0
          output_dir/recomp_0001.c        - chunk 1
          ...
          output_dir/recomp_dispatch.c    - address -> function pointer table

        manual: addresses the project implements by hand. Their bodies are not
        emitted, so the hand-written definition is the one that links, but they
        are still declared and still count as defined for stub purposes. This
        is how a game replaces a recompiled XDK routine (a D3D8 entry point,
        say) with one that drives the host runtime instead of the hardware.
        Direct calls and tail jumps to these addresses use the same manual-first
        lookup as indirect calls, so every call path reaches the override.

        Returns dict with stats and list of generated files.
        """
        import sys

        os.makedirs(output_dir, exist_ok=True)

        func_list = [item for item in func_list
                     if item[0] not in self.translator.owned_function_starts]
        manual = set(manual or ())
        self.translator.lifter.manual_functions = manual
        manual_decls = {}

        # Translate all functions first, collecting results
        translations = []
        stats = {
            "total": len(func_list),
            "translated": 0,
            "failed": 0,
            "total_lines": 0,
        }

        for i, (addr, func_info) in enumerate(func_list):
            name = func_info.get("name", f"sub_{addr:08X}")
            if verbose and (i % 500 == 0 or i == len(func_list) - 1):
                print(f"  [{i+1}/{len(func_list)}] Translating {name}...",
                      file=sys.stderr)

            if addr in manual:
                # Hand-written elsewhere: declare it, emit nothing.
                manual_decls[addr] = name
                continue

            code = self.translator.translate_function(addr, func_info)
            if code:
                translations.append((addr, name, code))
                stats["translated"] += 1
                stats["total_lines"] += code.count("\n")
            else:
                # Stub for failed translations
                stub = f"/* FAILED: {name} at 0x{addr:08X} */\n"
                stub += f"void {name}(void) {{ /* translation failed */ }}\n"
                translations.append((addr, name, stub))
                stats["failed"] += 1

        # Any address called but never defined needs a stub, or the link fails.
        # These are almost all mid-function entry points the function detector
        # did not split out: a call lands a few bytes inside (or just past) a
        # function it already found. Emitting an empty stub keeps the build
        # linking; hitting one at runtime is a silent no-op, so they are
        # reported and written to their own file rather than hidden among the
        # translated chunks.
        defined = {name for _, name, _ in translations}
        defined |= set(manual_decls.values())   # hand-written, but defined
        unresolved = {
            addr: name
            for addr, name in self.translator.lifter.referenced_calls.items()
            if name not in defined
        }
        stats["unresolved_stubs"] = len(unresolved)
        stats["manual_functions"] = len(manual_decls)

        # Generate header with all forward declarations
        header_path = os.path.join(output_dir, header_name)
        header_lines = [
            "/**",
            " * Xbox - Recompiled Function Declarations",
            f" * {stats['translated']} functions, auto-generated by tools/recomp",
            " */",
            "",
            "#ifndef RECOMP_FUNCS_H",
            "#define RECOMP_FUNCS_H",
            "",
            '#include "recomp_types.h"',
            "",
        ]
        for addr, name, _ in translations:
            decl = self._make_declaration(addr, name)
            header_lines.append(f"{decl};")

        if manual_decls:
            header_lines.append("")
            header_lines.append("/* Hand-written overrides (defined by the project) */")
            for addr in sorted(manual_decls):
                header_lines.append(
                    f"void {manual_decls[addr]}(void);  /* 0x{addr:08X} */")

        if unresolved:
            header_lines.append("")
            header_lines.append("/* Unresolved call targets (stubbed) */")
            for addr in sorted(unresolved):
                header_lines.append(f"void {unresolved[addr]}(void);")

        header_lines.extend(["", "#endif /* RECOMP_FUNCS_H */", ""])

        with open(header_path, "w", encoding="utf-8") as f:
            f.write("\n".join(header_lines))

        # Split translations into chunks and write .c files
        generated_files = [header_path]
        chunks = [translations[i:i+chunk_size]
                  for i in range(0, len(translations), chunk_size)]

        # Remove chunk files a previous, larger run left behind. Projects glob
        # gen/*.c into their build, so a stale chunk keeps compiling: it still
        # defines the functions it held last time, and the build fails with a
        # wall of "redefinition; different basic types" pointing at generated
        # code that looks perfectly correct. Nothing else cleans them, and the
        # count only has to shrink once -- which it does the first time a
        # detector fix changes how many functions are found.
        for stale in sorted(glob.glob(os.path.join(output_dir,
                                                   f"{prefix}_[0-9][0-9][0-9][0-9].c"))):
            index = int(os.path.basename(stale)[len(prefix) + 1:-2])
            if index >= len(chunks):
                os.remove(stale)
                if verbose:
                    print(f"  removed stale chunk {os.path.basename(stale)}")

        for ci, chunk in enumerate(chunks):
            c_path = os.path.join(output_dir, f"{prefix}_{ci:04d}.c")
            c_lines = [
                "/**",
                f" * Xbox - Recompiled code chunk {ci}",
                f" * Functions: {len(chunk)} "
                f"(0x{chunk[0][0]:08X} - 0x{chunk[-1][0]:08X})",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                '#include <math.h>',
                "",
            ]
            for addr, name, code in chunk:
                c_lines.append(code)

            with open(c_path, "w", encoding="utf-8") as f:
                f.write("\n".join(c_lines))
            generated_files.append(c_path)

            if verbose:
                print(f"  Wrote {c_path} ({len(chunk)} functions)",
                      file=sys.stderr)

        # Emit the stub bodies for call targets with no definition.
        if unresolved:
            stub_path = os.path.join(output_dir, f"{prefix}_stubs_unresolved.c")
            stub_lines = [
                "/**",
                " * Unresolved call target stubs",
                f" * {len(unresolved)} addresses called by translated code but not",
                " * detected as functions - typically mid-function entry points.",
                " * Auto-generated by tools/recomp.",
                " */",
                "",
                "#define RECOMP_GENERATED_CODE",
                f'#include "{header_name}"',
                "",
            ]
            stub_lines.append(
                "/* Each stub consumes the return address its caller pushed,")
            stub_lines.append(
                " * exactly as a real 'ret' would. An empty body leaves esp 4 bytes")
            stub_lines.append(
                " * low, and the caller then reads every subsequent stack slot off by")
            stub_lines.append(
                " * one - which surfaces far from here, as corrupted callee-saved")
            stub_lines.append(
                " * registers or a garbage local. Args are not popped: the callee's")
            stub_lines.append(
                " * stdcall byte count is unknown, and cdecl is the safer guess. */")
            stub_lines.append("")
            for addr in sorted(unresolved):
                stub_lines.append(
                    f"void {unresolved[addr]}(void) {{ g_esp += 4; "
                    f"/* 0x{addr:08X}: not detected */ }}"
                )
            stub_lines.append("")

            with open(stub_path, "w", encoding="utf-8") as f:
                f.write("\n".join(stub_lines))
            generated_files.append(stub_path)

            if verbose:
                print(f"  Wrote {stub_path} ({len(unresolved)} stubs)",
                      file=sys.stderr)

        # Generate dispatch table
        dispatch_path = os.path.join(output_dir, f"{prefix}_dispatch.c")
        self._write_dispatch_table(translations, dispatch_path, header_name)
        generated_files.append(dispatch_path)

        stats["files"] = generated_files
        stats["num_chunks"] = len(chunks)
        stats["chunk_size"] = chunk_size
        return stats

    def _write_dispatch_table(self, translations, output_path, header_name):
        """
        Generate a dispatch table mapping Xbox VA -> function pointer.

        Uses a sorted array + binary search for O(log n) lookup.
        """
        lines = [
            "/**",
            " * Xbox - Recompiled Function Dispatch Table",
            f" * Maps {len(translations)} Xbox VAs to translated function pointers.",
            " * Auto-generated by tools/recomp",
            " */",
            "",
            "#define RECOMP_DISPATCH_H",
            f'#include "{header_name}"',
            '#include <stddef.h>',
            '#include <stdlib.h>',
            "",
            "/* Generic function pointer type */",
            "typedef void (*recomp_func_t)(void);",
            "",
            "typedef struct {",
            "    uint32_t xbox_va;",
            "    recomp_func_t func;",
            "} recomp_entry_t;",
            "",
            f"static const recomp_entry_t g_recomp_table[] = {{",
        ]

        for addr, name, _ in translations:
            lines.append(f"    {{ 0x{addr:08X}u, (recomp_func_t){name} }},")

        addrs = [addr for addr, _, _ in translations]
        flat_base = min(addrs) if addrs else 0
        flat_span = (max(addrs) - flat_base + 1) if addrs else 0

        lines.extend([
            "};",
            "",
            f"static const size_t g_recomp_table_size = "
            f"{len(translations)};",
            "",
            "/* ----------------------------------------------------------------",
            " * Flat, directly-indexed dispatch.",
            " *",
            " * Microsoft's recompiler resolves an indirect branch with a single",
            " * `jmp qword ptr [r9 + r8*8]` -- one indexed load off a table keyed",
            " * by guest address, no compare and no miss path. This is that, in C.",
            " *",
            " * The binary search below is still here and still correct. It runs",
            " * ~log2(n) iterations per indirect call, which for this title is",
            f" * about {max(1, len(translations).bit_length())} branches every time the game calls through a",
            " * vtable. The flat table turns that into a bounds check and a load.",
            " *",
            " * Costs 8 bytes per byte of guest code span. Allocated with calloc so",
            " * the untouched middle stays uncommitted rather than resident.",
            " *",
            " * recomp_dispatch_init() is optional by design: if it is never called,",
            " * or the allocation fails, recomp_lookup silently keeps using the",
            " * binary search. Nothing else in the program has to know. That is also",
            " * why this does not pre-fill manual overrides -- they cannot be",
            " * enumerated portably, so RECOMP_ICALL still consults",
            " * recomp_lookup_manual first and this only replaces the search it used",
            " * to fall through to. Behaviour is identical by construction.",
            " * ---------------------------------------------------------------- */",
            "",
            f"static const uint32_t g_flat_base = 0x{flat_base:08X}u;",
            f"static const uint32_t g_flat_span = 0x{flat_span:08X}u;",
            "static recomp_func_t *g_flat_table = NULL;",
            "",
            "int recomp_dispatch_init(void)",
            "{",
            "    size_t i;",
            "    if (g_flat_table) return 1;          /* already built */",
            "    if (!g_flat_span) return 0;",
            "    g_flat_table = (recomp_func_t *)calloc(g_flat_span,",
            "                                           sizeof(recomp_func_t));",
            "    if (!g_flat_table) return 0;         /* keep the binary search */",
            "    for (i = 0; i < g_recomp_table_size; i++) {",
            "        g_flat_table[g_recomp_table[i].xbox_va - g_flat_base] =",
            "            g_recomp_table[i].func;",
            "    }",
            "    return 1;",
            "}",
            "",
            "size_t recomp_dispatch_flat_bytes(void)",
            "{",
            "    return g_flat_table ? (size_t)g_flat_span * sizeof(recomp_func_t) : 0;",
            "}",
            "",
            "/* Flat index when built, binary search otherwise. */",
            "recomp_func_t recomp_lookup(uint32_t xbox_va)",
            "{",
            "    size_t lo, hi;",
            "    if (g_flat_table) {",
            "        uint32_t off = xbox_va - g_flat_base;",
            "        /* Unsigned: a VA below the base wraps to a huge offset and is",
            "         * rejected by the same compare, so no separate lower bound. */",
            "        return (off < g_flat_span) ? g_flat_table[off] : NULL;",
            "    }",
            "    lo = 0; hi = g_recomp_table_size;",
            "    while (lo < hi) {",
            "        size_t mid = lo + (hi - lo) / 2;",
            "        if (g_recomp_table[mid].xbox_va < xbox_va)",
            "            lo = mid + 1;",
            "        else if (g_recomp_table[mid].xbox_va > xbox_va)",
            "            hi = mid;",
            "        else",
            "            return g_recomp_table[mid].func;",
            "    }",
            "    return NULL;",
            "}",
            "",
            "/* Get the number of registered functions */",
            "size_t recomp_get_count(void)",
            "{",
            "    return g_recomp_table_size;",
            "}",
            "",
            "/* Call all registered functions (for bulk testing) */",
            "size_t recomp_call_all(void)",
            "{",
            "    size_t i;",
            "    for (i = 0; i < g_recomp_table_size; i++) {",
            "        g_recomp_table[i].func();",
            "    }",
            "    return g_recomp_table_size;",
            "}",
            "",
        ])

        with open(output_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
