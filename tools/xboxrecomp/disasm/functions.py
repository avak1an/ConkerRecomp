"""
Function boundary detection for the disassembler.

Implements multi-pass function detection with confidence scoring:
1. Known addresses (entry point)
2. Standard prologues (push ebp; mov ebp, esp)
3. CC padding boundaries (CC run after ret)
4. Call targets (destinations of call instructions)
5. Cross-validation and overlap resolution
"""

import bisect
import os
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from . import config
from .engine import DisasmEngine, Instruction
from .loader import BinaryImage, SectionInfo
from .xrefs import XRefTracker
from .labels import LabelManager, Label, LabelType


@dataclass
class Function:
    """A detected function with boundaries and metadata."""
    start: int
    end: int           # Address after last instruction
    name: str
    section: str = ""
    confidence: float = 0.0
    detection_method: str = ""

    # Call graph data
    calls_to: List[int] = field(default_factory=list)     # Functions this calls
    called_by: List[int] = field(default_factory=list)     # Functions that call this

    # Instruction stats
    num_instructions: int = 0
    has_prologue: bool = False

    # Some other function's last instruction falls through into this one, so
    # it is entered with that function's frame live and its `ret` is not
    # measured against a call boundary. See _mark_fallthrough_entries.
    frame_inherited: bool = False

    @property
    def size(self) -> int:
        return self.end - self.start

    def to_dict(self) -> dict:
        return {
            "start": f"0x{self.start:08X}",
            "end": f"0x{self.end:08X}",
            "size": self.size,
            "name": self.name,
            "section": self.section,
            "confidence": self.confidence,
            "detection_method": self.detection_method,
            "num_instructions": self.num_instructions,
            "has_prologue": self.has_prologue,
            "frame_inherited": self.frame_inherited,
            "calls_to": [f"0x{a:08X}" for a in self.calls_to],
            "called_by": [f"0x{a:08X}" for a in self.called_by],
        }


# `jmp dword ptr [reg*4 + 0xADDR]` and the scale-less `jmp dword ptr [0xADDR]`.
_JUMP_TABLE_RE = re.compile(
    r"\[\s*(?:e[a-z]{2}\s*\*\s*4\s*\+\s*)?(0x[0-9a-fA-F]+)\s*\]")

class FunctionDetector:
    """
    Multi-pass function boundary detector.

    Identifies function start addresses through multiple heuristics,
    then determines function boundaries by following instruction flow
    until the next function or a terminal instruction.
    """

    def __init__(self, engine: DisasmEngine, image: BinaryImage,
                 xrefs: XRefTracker, labels: LabelManager):
        self.engine = engine
        self.image = image
        self.xrefs = xrefs
        self.labels = labels

        # Candidate function starts: address -> (confidence, method)
        self._candidates: Dict[int, Tuple[float, str]] = {}

        # Final function list
        self.functions: Dict[int, Function] = {}

        # Tail-jump targets landing inside another function: addr -> that
        # function's end. Kept out of self._candidates so they cannot truncate
        # the function they land in.
        self._alias_entries: Dict[int, int] = {}

        # Indirect-call targets observed at run time. Held separately and
        # resolved once the extents are final: whether one is a function start
        # or a mid-function entry is a question only the finished bodies can
        # answer, and getting it wrong in the truncating direction is worse
        # than not seeding it at all.
        self._observed_targets: Set[int] = set()

    def add_configured_functions(self, addresses):
        """Decode explicit entry points even when a linear sweep missed them."""
        for addr in addresses:
            if self.engine.get_instruction(addr) is None:
                self.engine.decode_at(addr)
            if self.engine.get_instruction(addr) is None:
                raise ValueError(f"Cannot decode configured function at {addr:08X}")
            self._add_candidate(addr, 0.95, "configured_entry")

    def detect_all(self, sections: Optional[List[SectionInfo]] = None) -> int:
        """
        Run all detection passes and build the function database.

        Args:
            sections: Sections to analyze. If None, uses all executable sections.

        Returns:
            Number of functions detected.
        """
        if sections is None:
            sections = self.image.get_code_sections()

        # Pass 1: Known addresses
        self._pass_known_addresses()

        # Pass 2: Prologue patterns
        for sec in sections:
            self._pass_prologues(sec)

        # Pass 3: CC padding boundaries
        for sec in sections:
            self._pass_cc_boundaries(sec)

        # Pass 4: Call targets
        self._pass_call_targets(sections)

        # Pass 5: Build functions from candidates
        self._build_functions(sections)

        # Pass 6: Tail-jump targets. A function reached only by "jmp" and never
        # by "call" is invisible to every pass above, so it is emitted as a stub
        # that returns without unwinding the frame its jumping caller built --
        # silently corrupting the simulated stack for everything upstream. Halo
        # had 127 of these; one of them (the CRT two-arg error handler) leaked
        # 0x24 bytes per call and turned a 6-iteration init loop into 21,938
        # allocations that exhausted the heap.
        #
        # Needs the bodies from pass 5 to tell a tail jump from an ordinary
        # intra-function branch, so it runs after and rebuilds. Iterate: a newly
        # found function can itself tail-jump somewhere new.
        for _round in range(8):
            before = len(self._candidates)
            if not self._pass_tail_jump_targets(sections):
                break
            print(f"  tail-jump pass {_round}: "
                  f"+{len(self._candidates) - before} standalone, "
                  f"{len(self._alias_entries)} aliases")
            self.functions.clear()
            self._build_functions(sections)

        # Pass 7: address-taken code that no function claimed.  Runs after the
        # tail-jump rounds have settled so "unclaimed" reflects final extents.
        if config.DETECT_ADDRESS_TAKEN_GAPS:
            if self._pass_address_taken_gaps(sections):
                self.functions.clear()
                self._build_functions(sections)

        # Pass 8: mid-function entry points that some *other* function
        # branches into. Pass 6 covers this too, but only for unconditional
        # jumps, only against the bodies as they stood mid-detection, and it
        # skips any address already sitting in _candidates -- including ones
        # that never became a function. Conker left 566 of these: MSVC's
        # optimiser routinely lets several paths converge on a shared tail,
        # and D3D's SetRenderState dispatcher is built out of them.
        #
        # Each became a `g_esp += 4` stub, which returns without running the
        # epilogue it jumped to. sub_00545A56 is the pops-and-ret 8 tail of a
        # jump-table dispatcher: nine paths reach it, none of them unwound,
        # so esi came back holding a device field instead of the D3D device
        # pointer and the caller dereferenced 8.
        #
        # Runs last, against final extents, so "lands inside a function" is
        # settled. Conditional branches count: a `jne` into another
        # function's tail is translated as a tail call exactly like a `jmp`.
        if self._pass_cross_function_entries():
            print(f"  cross-function entries: "
                  f"{len(self._alias_entries)} aliases total")

        # Pass 8b: branch targets in unclaimed code. Iterates -- a gap that
        # becomes a function can branch into another gap, and the switch
        # default blocks that motivated this pass sit in runs.
        _gap_total = 0
        for _round in range(8):
            _gap_new = self._pass_branch_gap_targets(sections)
            if not _gap_new:
                break
            _gap_total += _gap_new
        if _gap_total:
            print(f"  branch targets in gaps: {_gap_total} new functions")

        # Pass 8c: switch arms. After 8b so the gap functions it created are
        # part of "inside a function", and before 9 so the runtime feedback has
        # less to discover.
        _arm_new, _arm_alias = self._pass_jump_table_arms(sections)
        if _arm_new or _arm_alias:
            print(f"  jump-table arms: {_arm_new} new functions, "
                  f"{_arm_alias} aliased into existing ones")

        # Pass 8d: entries the aliases themselves need. Iterates: an alias
        # created here starts even earlier in the container, so its own body
        # can reach further back again.
        _body_total = 0
        for _round in range(8):
            _body_new = self._pass_alias_body_entries()
            if not _body_new:
                break
            _body_total += _body_new
        if _body_total:
            print(f"  alias body entries: {_body_total} more aliases")

        # Pass 9: indirect-branch targets observed at run time. Last, so the
        # start-or-mid-function decision is made against final extents.
        _icall_new, _icall_alias = self._pass_observed_targets(sections)
        if _icall_new or _icall_alias:
            print(f"  observed icall targets: {_icall_new} new functions, "
                  f"{_icall_alias} aliased into existing ones")

        # Last: extents are final, so "falls through into" is settled.
        _ft = self._mark_fallthrough_entries()
        if _ft:
            print(f"  fall-through entries: {_ft} functions inherit a frame")

        self._build_alias_entries()

        # Populate call graph
        self._build_call_graph()

        return len(self.functions)

    def add_observed_targets(self, addrs) -> None:
        """Record indirect-branch targets seen at run time (see
        tools/recomp/icall_feedback.py). Resolved in _pass_observed_targets."""
        for addr in addrs:
            self._observed_targets.add(int(addr))

    def _add_candidate(self, addr: int, confidence: float, method: str) -> None:
        """Add a function start candidate, keeping highest confidence."""
        existing = self._candidates.get(addr)
        if existing is None or confidence > existing[0]:
            self._candidates[addr] = (confidence, method)

    def _pass_address_taken_gaps(self, sections: List[SectionInfo]) -> int:
        """
        Pass 7: promote address-taken code sitting in an unclaimed gap.

        An indirect entry point -- a thread start routine, a callback, a vtable
        slot -- is only ever referenced by having its address loaded as an
        immediate (`push offset fn`).  No prologue, CC boundary, call target or
        tail jump marks it, so every pass above misses it and it ends up inside
        no function at all.  The recompiler then has no dispatch-table entry and
        the indirect call fails at runtime.

        Conker: sub_0042F075 is the start routine handed to
        PsCreateSystemThreadEx.  Without this the boot died immediately with
        "real start routine 0x0042F075 not found in dispatch table".

        Restricted to addresses no detected function already covers.  An
        immediate pointing into the middle of a real function is a jump-table
        entry, a switch label or a data reference, and splitting the function
        there would truncate it -- the defect _find_function_end exists to
        avoid.

        Set XBOXRECOMP_DEBUG_ADDR to a comma-separated list of hex addresses to
        have each decision for them printed -- these rejections are otherwise
        entirely silent, which is what made them expensive to find.

        Returns the number of candidates added.
        """
        exec_ranges = [(s.virtual_addr, s.virtual_addr + s.virtual_size)
                       for s in sections if s.executable]

        bounds = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bounds]

        def claimed(addr: int) -> bool:
            i = bisect.bisect_right(starts, addr) - 1
            return i >= 0 and bounds[i][0] <= addr < bounds[i][1]

        seen: Set[int] = set()
        added = 0

        watch = set()
        for tok in os.environ.get("XBOXRECOMP_DEBUG_ADDR", "").split(","):
            tok = tok.strip()
            if tok:
                try:
                    watch.add(int(tok, 16))
                except ValueError:
                    pass

        def covering_insn(target):
            """The decoded instruction that strictly contains `target`, if any.

            x86 instructions are at most 15 bytes, so a short walk back finds
            it or proves there is none."""
            for back in range(1, 16):
                insn = self.engine.get_instruction(target - back)
                if insn is not None and target < insn.end_address:
                    return insn
            return None

        def consider(target, method, realign=True):
            """Promote `target` if it is unclaimed code. Returns 1 if added."""
            def note(why):
                if target in watch:
                    print(f"  [debug] {target:#010x} via {method}: {why}")
            if target is None:
                return 0
            if target in seen:
                note("already considered (an earlier source rejected it)")
                return 0
            seen.add(target)
            if not any(lo <= target < hi for lo, hi in exec_ranges):
                note("outside every executable section")
                return 0
            # Must land exactly on an instruction boundary; a value that points
            # mid-instruction is data that merely looks like an address.
            # The sweep can leave a whole run out of phase after walking
            # through a jump table, so realign there before giving up --
            # decode_at rewrites only the out-of-phase run.
            if self.engine.get_instruction(target) is None:
                # decode_at leaves overlapping decodes in place, so realigning
                # on an address the sweep already covered does not correct a
                # phase error -- it manufactures a second boundary inside a
                # perfectly good instruction. That is only safe when something
                # independent says the address is code.
                #
                # A bare dword is not that, in a data section or a code one.
                # 0x0002D82D is the second byte of `add esp, 8` at 0x0002D82C
                # and some unrelated .data word holds that value; realigning
                # split sub_0002D710 in half, so the first half fell off the
                # end of its C body and leaked 60 bytes into its caller, five
                # frames below sub_002A96E0.  0x00545A00 is the second byte of
                # `mov ecx, 0x10000` at 0x005459FF and some code-section word
                # holds *that*; it split sub_005459EA, and because that
                # function is the last arm of the D3D render-state dispatcher
                # the entire tail-call chain returned with no teardown at all.
                #
                # The adjacency run that guards the code-section scan is not
                # enough on its own: it argues the *table* is real, not that
                # this entry points at an instruction.
                if not realign:
                    inside = covering_insn(target)
                    if inside is not None:
                        note(f"mid-instruction inside {inside.address:#010x}, "
                             f"and a data dword is not evidence enough to "
                             f"realign the sweep")
                        return 0
                self.engine.decode_at(target)
                if self.engine.get_instruction(target) is None:
                    note("not on an instruction boundary, even after realigning")
                    return 0
            if target in self._candidates:
                note("already a candidate")
                return 0
            # An address inside a detected function is a jump-table entry, a
            # switch label or a data reference, and splitting there would
            # truncate the function -- the defect _find_function_end exists to
            # avoid.
            #
            # Splitting at an address whose predecessor is a `ret` looks safe
            # and is not: _find_function_end deliberately walks past a `ret`
            # to take in an out-of-line tail (see test_out_of_line_tail_is_
            # included), so a coincidental .data dword landing just after an
            # interior `ret` cuts a real function in half. Tried; it truncated
            # enough of the CRT to fault 46 lines into the boot.
            if claimed(target):
                note("inside an existing function")
                return 0
            note("promoted")
            self._add_candidate(target, config.CONFIDENCE_ADDRESS_TAKEN, method)
            return 1

        # Addresses the code loads as immediates: `push offset fn`.
        # Snapshot first: consider() can realign the sweep, which adds to the
        # instruction map while we are walking it.
        for insn in list(self.engine.instructions.values()):
            added += consider(getattr(insn, "imm_ref", None), "address_taken")

        # Addresses that only ever exist in data: vtables, jump tables built by
        # the compiler, and hand-written function-pointer arrays.  A C++ title
        # reaches most of its virtual methods this way and never names them in
        # an instruction, so the immediate scan above cannot see them.
        #
        # Conker's CRT heap constructor calls through one of these
        # (0x0046E7CC), and with no function there the call failed, the heap
        # was never created, and every later allocation walked a NULL heap.
        #
        # The same unclaimed-gap rule keeps this safe: a dword that points into
        # the middle of a real function is data, and is left alone.
        # Jump tables: `jmp dword ptr [reg*4 + 0xADDR]`.  MSVC parks the table
        # in .text beside the code, so the data scan below cannot see it, and
        # the sweep decodes its bytes as instructions so the targets are not
        # instruction boundaries either.  Read the table directly.
        #
        # Conker's CRT heap constructor dispatches through one of these; with
        # its arms missing the call failed, the heap was never created, and
        # every later allocation walked a NULL heap.
        for value in self._jump_table_entries(exec_ranges):
            added += consider(value, "jump_table")

        if config.DETECT_DATA_POINTER_GAPS:
            # sections here is the code-section list, so walk the image's
            # own section table to reach the data ones.
            #
            # Select them by *not* being a code section, never by the
            # executable flag: Xbox linkers mark nearly every section
            # executable, .data and .rdata included (which is why
            # get_code_sections excludes those by name instead). Testing the
            # flag here skipped .data and .rdata and left this scan walking
            # only bitmaps and image blobs -- so the one thing it exists to
            # find, a function pointer that appears nowhere but in data, was
            # exactly what it could not see. Conker keeps its 1,441-entry C++
            # constructor table and its CRT float-init pointer in .data.
            code_secs = {id(s) for s in self.image.get_code_sections()}
            scanned = []
            for sec in self.image.sections:
                if id(sec) in code_secs:
                    continue
                data = self.image.get_section_data(sec)
                if not data:
                    scanned.append(f"{sec.name}:NO-DATA")
                    continue
                scanned.append(f"{sec.name}:{len(data)}")
                base = sec.virtual_addr
                for off in range(0, len(data) - 3, 4):
                    value = int.from_bytes(data[off:off + 4], "little")
                    if value < 0x00010000:
                        continue
                    added += consider(value, "data_pointer",
                                      realign=False)
            if watch:
                print(f"  [debug] data sections scanned: {', '.join(scanned) or 'NONE'}")

            # Read-only data merged into a code section.  The Xbox linker maps
            # the XDK libraries (XPP, XNET, XONLINE, D3D, ...) as CODE, tables
            # and all, so their vtables and dispatch tables sit among the
            # instructions where neither the scan above (which skips code
            # sections) nor the jump-table scan (which only follows `jmp
            # [table]`, not `call [table]`) can see them.  Conker reaches
            # 0x00558C9D only through such a table at XPP:0x0055850C; the call
            # failed, returned 0, and the caller then wrote 0xCC through the
            # null result into the fake TIB at guest 0x250.
            #
            # Scanning every dword of a code section would promote instruction
            # bytes that happen to read as an address, so require a *run*: a
            # pointer whose neighbour is also one. Real tables hold several in
            # a row; a coincidence in code almost never does, and the maths
            # agrees -- roughly 0.03% of random dwords land in the executable
            # range, so adjacent pairs are ~1e-7 per position, well under one
            # expected false pair across the whole image.
            def in_exec(v):
                return any(lo <= v < hi for lo, hi in exec_ranges)

            for sec in self.image.get_code_sections():
                data = self.image.get_section_data(sec)
                if not data:
                    continue
                words = [int.from_bytes(data[o:o + 4], "little")
                         for o in range(0, len(data) - 3, 4)]
                looks = [in_exec(w) for w in words]
                for i, w in enumerate(words):
                    if not looks[i]:
                        continue
                    if (i and looks[i - 1]) or (i + 1 < len(looks) and looks[i + 1]):
                        added += consider(w, "code_section_table",
                                          realign=False)

        if added:
            print(f"  address-taken pass: +{added} indirect entry points")
        return added

    def _pass_known_addresses(self) -> None:
        """Pass 1: Add known function addresses."""
        # Entry point
        self._add_candidate(
            self.image.entry_point,
            config.CONFIDENCE_KNOWN,
            "entry_point"
        )

    def _pass_prologues(self, section: SectionInfo) -> None:
        """
        Pass 2: Scan for standard function prologues.

        Looks for: push ebp (0x55); mov ebp, esp (0x8BEC or 0x89E5)
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0
        while i < len(data) - 2:
            # Check for push ebp; mov ebp, esp
            if data[i] == 0x55:
                if (i + 2 < len(data) and
                        data[i + 1] == 0x8B and data[i + 2] == 0xEC):
                    addr = va_start + i
                    # Verify this address has a decoded instruction
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue"
                        )
                    i += 3
                    continue
                elif (i + 2 < len(data) and
                      data[i + 1] == 0x89 and data[i + 2] == 0xE5):
                    addr = va_start + i
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue_alt"
                        )
                    i += 3
                    continue
            i += 1

    def _pass_cc_boundaries(self, section: SectionInfo) -> None:
        """
        Pass 3: Find function boundaries at CC padding.

        Pattern: ret instruction, followed by one or more 0xCC bytes,
        followed by the start of the next function.
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0

        while i < len(data):
            # Look for CC padding runs
            if data[i] == config.CC_PADDING:
                cc_start = i
                while i < len(data) and data[i] == config.CC_PADDING:
                    i += 1

                cc_run_length = i - cc_start

                if cc_run_length >= config.MIN_CC_RUN and i < len(data):
                    # Check if instruction before CC run was a ret
                    before_addr = va_start + cc_start
                    # Look for a ret instruction ending right at the CC run
                    found_ret = False
                    for check_offset in range(1, 4):  # ret can be 1-3 bytes
                        check_addr = before_addr - check_offset
                        insn = self.engine.get_instruction(check_addr)
                        if insn and insn.is_ret and insn.end_address == before_addr:
                            found_ret = True
                            break

                    if found_ret:
                        next_addr = va_start + i
                        if next_addr in self.engine.instructions:
                            self._add_candidate(
                                next_addr,
                                config.CONFIDENCE_CC_BOUNDARY,
                                "cc_boundary"
                            )
            else:
                i += 1

    def _pass_call_targets(self, sections: List[SectionInfo]) -> None:
        """
        Pass 4: Add destinations of direct call instructions as function starts.
        """
        # Build set of valid code ranges
        code_ranges = set()
        for sec in sections:
            for addr in range(sec.virtual_addr,
                              sec.virtual_addr + sec.virtual_size):
                code_ranges.add(addr)

        call_targets = self.engine.get_call_targets()
        realigned = unaligned = 0
        for target in call_targets:
            section = self.image.get_section_at_va(target)
            if not (section and section.executable):
                continue
            # A direct call to an executable address is the strongest evidence
            # of a function start there is -- stronger than a prologue match,
            # which is a guess about bytes. It used to be discarded whenever the
            # linear sweep had stepped over that exact address, which happens
            # wherever the sweep passes through data and comes out of phase.
            # Decode there instead of dropping the candidate; see
            # Engine.decode_at. On Halo 2276 this is 46 functions that were
            # becoming `g_esp += 4` no-op stubs.
            if target not in self.engine.instructions:
                # Manufacturing an instruction here is creating evidence, not
                # reading it, so require corroboration. A call operand decoded
                # out of data produces a plausible-looking in-section address
                # that is almost never aligned; a real MSVC function start
                # almost always is. Targets that already decoded are untouched,
                # whatever their alignment -- that is pre-existing behaviour.
                if target % config.CALL_TARGET_REALIGN_ALIGNMENT:
                    unaligned += 1
                    continue
                if self.engine.decode_at(target):
                    realigned += 1
                else:
                    continue  # genuinely undecodable: not code
            self._add_candidate(
                target,
                config.CONFIDENCE_CALL_TARGET,
                "call_target"
            )
        if realigned or unaligned:
            print(f"  Realigned {realigned} call targets the sweep stepped over"
                  f" ({unaligned} rejected as unaligned)")

    def _pass_tail_jump_targets(self, sections: List[SectionInfo]) -> bool:
        """
        Pass 6: Add the target of every unconditional jmp that leaves the body
        of the function containing it.

        Returns True if any new candidate was added.
        """
        bodies = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bodies]
        added = False

        for insn in self.engine.instructions.values():
            if not insn.is_jump or insn.is_cond_jump:
                continue
            target = insn.jump_target
            if target is None or target in self._candidates:
                continue
            if target not in self.engine.instructions:
                continue

            # The jump is a tail jump only if it leaves its own function.
            i = bisect.bisect_right(starts, insn.address) - 1
            if i < 0:
                continue
            body_start, body_end = bodies[i]
            if insn.address >= body_end:
                continue            # not inside any known function
            if body_start <= target < body_end:
                continue            # ordinary intra-function branch

            section = self.image.get_section_at_va(target)
            if section is None or not section.executable:
                continue

            # Does the target land inside some *other* function? The CRT does
            # this constantly -- _startOneArgErrorHandling jumps into the middle
            # of _startTwoArgErrorHandling to share its tail. Registering that
            # address as an ordinary candidate would truncate the function it
            # lands in, breaking the very code it wanted to reach. Record it as
            # an alias entry instead: same end address, translated separately.
            j = bisect.bisect_right(starts, target) - 1
            if j >= 0 and bodies[j][0] < target < bodies[j][1]:
                if target not in self._alias_entries:
                    self._alias_entries[target] = bodies[j][1]
                    added = True
                continue

            self._add_candidate(target, config.CONFIDENCE_TAIL_JUMP,
                                "tail_jump_target")
            added = True

        return added

    def _pass_cross_function_entries(self) -> int:
        """
        Pass 8: alias every branch target that lands strictly inside a
        function other than the one the branch came from.

        Returns the number of new alias entries.
        """
        bodies = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bodies]
        added = 0

        for insn in self.engine.instructions.values():
            if not insn.is_branch:
                continue
            target = insn.jump_target
            if target is None or target in self.functions:
                continue
            if target in self._alias_entries:
                continue
            if target not in self.engine.instructions:
                continue

            # Which function does the branch live in?
            i = bisect.bisect_right(starts, insn.address) - 1
            if i < 0:
                continue
            src_start, src_end = bodies[i]
            if insn.address >= src_end:
                continue            # in a gap, not inside a function

            # Which function does it land in?
            j = bisect.bisect_right(starts, target) - 1
            if j < 0:
                continue
            tgt_start, tgt_end = bodies[j]
            if not tgt_start < target < tgt_end:
                continue            # a function start, or a gap
            if tgt_start == src_start:
                continue            # ordinary intra-function branch

            section = self.image.get_section_at_va(target)
            if section is None or not section.executable:
                continue

            self._alias_entries[target] = tgt_end
            added += 1

        return added

    def _pass_alias_body_entries(self) -> int:
        """
        Pass 8d: backward branches out of an alias body.

        An alias entry at A inside a function [S, E) is translated as its own
        body covering [A, E). Every branch in that range to a target in [S, A)
        is an ordinary intra-function jump *for the container* -- so no earlier
        pass considers it an entry point -- but it leaves the alias body, and
        the translator emits it as a call to a function that does not exist.
        Another `g_esp += 4` stub, and aliasing is what created the demand:
        pass 8c added 4,796 alias bodies and 232 new stubs along with them.

        Aliases do not move extents, so this only ever adds more aliases and is
        safe to iterate to a fixpoint.

        Returns the number of new alias entries.
        """
        if not self._alias_entries:
            return 0

        bodies = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bodies]
        added = 0

        for alias_start, alias_end in sorted(self._alias_entries.items()):
            i = bisect.bisect_right(starts, alias_start) - 1
            if i < 0:
                continue
            container_start, container_end = bodies[i]
            if not container_start < alias_start < container_end:
                continue

            for insn in self.engine.get_instructions_in_range(alias_start,
                                                              alias_end):
                if not insn.is_branch:
                    continue
                target = insn.jump_target
                if target is None or target in self.functions:
                    continue
                if target in self._alias_entries:
                    continue
                if not container_start < target < alias_start:
                    continue
                if target not in self.engine.instructions:
                    continue
                section = self.image.get_section_at_va(target)
                if section is None or not section.executable:
                    continue
                self._alias_entries[target] = container_end
                added += 1

        return added

    def _mark_fallthrough_entries(self) -> int:
        """
        Mark functions that some other function falls through into.

        If a function's last instruction is neither a `ret` nor an
        unconditional `jmp`, control runs on into whatever starts at its end
        address. That successor is therefore entered with the predecessor's
        frame still live -- it did not allocate the frame it tears down, and
        its `ret` is not measured against a call boundary. The stack-balance
        checker has to know, or it reports the function for doing its job.

        sub_00545A58 is the whole D3D render-state dispatcher's shared tail,
        reached by falling out of sub_00545A56. It was the last remaining
        `[ESP]` report at +200, and it was never a defect.

        Detection method alone cannot say this: 0x00545A58 is
        `code_section_table`, and those are as often ordinary called vtable
        slots as they are shared tails.

        Returns the number of functions marked.
        """
        marked = 0
        for func in self.functions.values():
            insns = self.engine.get_instructions_in_range(func.start, func.end)
            if not insns:
                continue
            last = insns[-1]
            if last.is_ret or (last.is_jump and not last.is_cond_jump):
                continue        # control leaves; nothing falls through
            successor = self.functions.get(func.end)
            if successor is None or successor.frame_inherited:
                continue
            successor.frame_inherited = True
            marked += 1
        return marked

    def _jump_table_entries(self, exec_ranges):
        """Every arm of every `jmp dword ptr [reg*4 + 0xADDR]` table.

        MSVC parks switch tables in .text beside the code, so the data-section
        scan cannot see them, and the sweep decodes their bytes as instructions
        so the arms are not instruction boundaries either. Read the table
        directly.
        """
        for insn in list(self.engine.instructions.values()):
            if not insn.is_jump or insn.jump_target is not None:
                continue
            # The engine leaves memory_ref unset for a scaled-index operand
            # with no base register, which is exactly the shape a switch
            # dispatch takes, so read the displacement from the operand text.
            table = getattr(insn, "memory_ref", None)
            if table is None:
                m = _JUMP_TABLE_RE.search(insn.op_str or "")
                if not m:
                    continue
                table = int(m.group(1), 16)
            sec = self.image.get_section_at_va(table)
            if sec is None or not sec.executable:
                continue
            data = self.image.get_section_data(sec)
            if not data:
                continue
            off = table - sec.virtual_addr
            # Stop at the first entry that is not code: that is the end of the
            # table, and reading past it would invent functions from whatever
            # follows.
            for _entry in range(config.MAX_JUMP_TABLE_ENTRIES):
                if off < 0 or off + 4 > len(data):
                    break
                value = int.from_bytes(data[off:off + 4], "little")
                if not any(lo <= value < hi for lo, hi in exec_ranges):
                    break
                yield value
                off += 4

    def _pass_jump_table_arms(self, sections: List[SectionInfo]) -> Tuple[int, int]:
        """
        Pass 8c: give every switch arm a body, including the ones inside a
        function.

        Pass 7 already reads these tables, but it can only *promote* an arm,
        and promoting one that lands inside a function would truncate the very
        code the jump is trying to reach -- so it drops those silently. Most
        arms land inside a function: a switch belongs to the function that
        contains it. So the common case was being discarded.

        An arm with no body is an unresolved `RECOMP_ITAIL`, which does
        `g_esp += 4; g_eax = 0` and returns -- abandoning the frame the
        dispatching function allocated for it to tear down, and returning 0 as
        though the call had succeeded. sub_00079050 allocates 0x8C and
        dispatches through the table at 0x0007B148, whose six arms all live
        inside it; 0x000790C3 leaked 164 bytes into sub_00076F10, whose linked
        list walk then read a node pointer of 1 and dereferenced 0xCCCCCCCC.

        Resolved through the same helper as passes 8b and 9, so an arm inside a
        body becomes an alias entry and one in a gap becomes a function start.

        Returns (new candidates, new aliases).
        """
        exec_ranges = [(sec.virtual_addr, sec.virtual_addr + sec.virtual_size)
                       for sec in sections if sec.executable]
        arms = set(self._jump_table_entries(exec_ranges))
        if not arms:
            return 0, 0
        return self._resolve_extra_entries(arms, sections, "jump_table_arm")

    def _pass_branch_gap_targets(self, sections: List[SectionInfo]) -> int:
        """
        Pass 8b: branch targets that leave their function and land in a gap.

        Pass 6 covers this for unconditional jumps only; pass 8 covers
        conditional ones only when they land *inside* another function. A
        conditional branch into unclaimed code falls through both, and the
        switch default block is exactly that shape: MSVC emits

            ja  default          ; sub_00378020, operand out of range
            jmp [table+eax*4]
          default:               ; 12 bytes, then the jump table
            ...

        Nothing calls `default`, and nothing jumps to it unconditionally, so it
        stayed unclaimed and was emitted as a `g_esp += 4` stub. Conker's
        printf engine reaches it through sub_00378233, which had already
        allocated a 0x150 frame for the shared body to tear down -- the stub
        returned without tearing it down, so every %-conversion that took the
        default path leaked 336 bytes into the caller's stack.

        Returns the number of new function starts.
        """
        found = set()
        bodies = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bodies]

        for insn in self.engine.instructions.values():
            if not insn.is_branch:
                continue            # calls are pass 4's job
            target = insn.jump_target
            if target is None or target in self.functions:
                continue
            if target in self._alias_entries:
                continue
            if target not in self.engine.instructions:
                continue

            # The branch has to live inside a known function, or we have no
            # frame to be wrong about and no evidence it is a real entry.
            i = bisect.bisect_right(starts, insn.address) - 1
            if i < 0 or insn.address >= bodies[i][1]:
                continue

            # Only gaps: a target inside a body is pass 8's alias case, and an
            # intra-function branch is not an entry at all.
            j = bisect.bisect_right(starts, target) - 1
            if j >= 0 and bodies[j][0] <= target < bodies[j][1]:
                continue

            section = self.image.get_section_at_va(target)
            if section is None or not section.executable:
                continue

            found.add(target)

        if not found:
            return 0

        added, _aliased = self._resolve_extra_entries(found, sections,
                                                      "branch_gap_target")
        return added

    def _pass_observed_targets(self, sections: List[SectionInfo]) -> Tuple[int, int]:
        """
        Pass 9: resolve indirect-branch targets observed at run time.

        Static analysis cannot see where a vtable call goes; running the title
        can, and tools/recomp/icall_feedback.py persists what it saw. But an
        observed target is not automatically a function *start*. MSVC lets
        several entry points share one body, so a good share of them land in
        the middle of a function that was detected perfectly well -- Conker
        calls 0x00287E24, which is 0x24 bytes into sub_00287E00.

        Seeding those through --seed-functions, which is what icall_feedback
        used to recommend, registers them as candidates, and a candidate
        truncates the function containing it at that address. That breaks the
        very code the call was trying to reach, and swaps a stub for a
        half-function -- a worse failure, because it looks like it worked.

        Deciding it here is the point: this runs after the extents are final,
        so "inside a function" is a settled question. Targets outside every
        body become candidates and the bodies are rebuilt; targets inside one
        become alias entries, which give the address its own callable body
        without moving anybody's end.

        Returns (new candidates, new aliases).
        """
        return self._resolve_extra_entries(self._observed_targets, sections,
                                           "icall_observed")

    def _resolve_extra_entries(self, targets, sections: List[SectionInfo],
                               method: str) -> Tuple[int, int]:
        """
        Turn a set of extra entry addresses into functions or alias entries.

        Shared by the runtime-observed pass and the branch-gap pass: both have
        a bag of addresses that code reaches but no pass claimed, and both need
        the same start-or-mid-function decision made against final extents.

        Returns (new candidates, new aliases).
        """
        if not targets:
            return 0, 0

        def _decodable(target: int) -> bool:
            if target not in self.engine.instructions:
                # Same rule as the call-target pass: manufacturing an
                # instruction is creating evidence, so require alignment.
                if target % config.CALL_TARGET_REALIGN_ALIGNMENT:
                    return False
                if not self.engine.decode_at(target):
                    return False
            section = self.image.get_section_at_va(target)
            return section is not None and section.executable

        def _bodies():
            b = sorted((f.start, f.end) for f in self.functions.values())
            return b, [x[0] for x in b]

        bodies, starts = _bodies()
        pending = []
        added = 0

        # First the ones that belong to no function: those are real starts, and
        # they must land before any alias end is computed, because creating
        # them moves the ends.
        for target in sorted(targets):
            if target in self.functions or target in self._alias_entries:
                continue
            if not _decodable(target):
                continue
            i = bisect.bisect_right(starts, target) - 1
            if i >= 0 and bodies[i][0] < target < bodies[i][1]:
                pending.append(target)
                continue
            self._add_candidate(target, config.CONFIDENCE_CALL_TARGET,
                                method)
            added += 1

        if added:
            self.functions.clear()
            self._build_functions(sections)
            bodies, starts = _bodies()

            # Those new starts may have shortened a function an earlier pass
            # already aliased into. Re-anchor every alias to the body it now
            # sits in, or drop it if it no longer sits inside one.
            for addr in list(self._alias_entries):
                j = bisect.bisect_right(starts, addr) - 1
                if j >= 0 and bodies[j][0] < addr < bodies[j][1]:
                    self._alias_entries[addr] = bodies[j][1]
                else:
                    del self._alias_entries[addr]

        aliased = 0
        for target in pending:
            if target in self.functions or target in self._alias_entries:
                continue
            j = bisect.bisect_right(starts, target) - 1
            if j >= 0 and bodies[j][0] < target < bodies[j][1]:
                self._alias_entries[target] = bodies[j][1]
                aliased += 1

        return added, aliased

    def _build_alias_entries(self) -> None:
        """
        Emit a Function for each tail-jump target that lands inside another
        function, running from the target to that function's end.

        The overlap is deliberate: the translator produces a second body for the
        shared tail, which costs a little code size and makes the entry point
        callable. The alternative -- a stub that returns immediately -- silently
        skips the epilogue and leaks the caller's frame.
        """
        for addr, end in sorted(self._alias_entries.items()):
            if addr in self.functions:
                continue
            insns = self.engine.get_instructions_in_range(addr, end)
            if not insns:
                continue
            section = self.image.get_section_at_va(addr)
            sec_name = section.name if section else ""
            label = self.labels.get(addr)
            name = label.name if label else f"sub_{addr:08X}"
            if not label:
                self.labels.auto_name_function(
                    addr, sec_name, config.CONFIDENCE_TAIL_JUMP)
            self.functions[addr] = Function(
                start=addr,
                end=end,
                name=name,
                section=sec_name,
                confidence=config.CONFIDENCE_TAIL_JUMP,
                detection_method="tail_jump_alias",
                num_instructions=len(insns),
                has_prologue=False,
            )

    def _build_functions(self, sections: List[SectionInfo]) -> None:
        """
        Pass 5: Build Function objects from candidates.

        Determines function boundaries by finding the extent of each
        function (up to the next function start or unreachable point).
        """
        # Sort candidates by address
        sorted_starts = sorted(self._candidates.keys())
        if not sorted_starts:
            return

        # Build section boundary lookup
        sec_ranges = {}
        for sec in sections:
            sec_ranges[sec.name] = (sec.virtual_addr,
                                    sec.virtual_addr + sec.virtual_size)

        # Create functions
        for idx, start_addr in enumerate(sorted_starts):
            confidence, method = self._candidates[start_addr]

            # Determine section
            section = self.image.get_section_at_va(start_addr)
            sec_name = section.name if section else ""

            # Determine end address:
            # Walk instructions until we hit the next function start,
            # leave the section, or reach an unconditional terminator
            # with no fall-through.
            if idx + 1 < len(sorted_starts):
                next_func = sorted_starts[idx + 1]
            else:
                next_func = None

            # Section end boundary
            sec_end = None
            if section:
                sec_end = section.virtual_addr + section.virtual_size

            end_addr = self._find_function_end(start_addr, next_func, sec_end)

            # Count instructions
            insns = self.engine.get_instructions_in_range(start_addr, end_addr)
            num_insns = len(insns)

            if num_insns == 0:
                continue

            # Check for prologue
            first_insn = self.engine.get_instruction(start_addr)
            has_prologue = (first_insn is not None and
                            first_insn.mnemonic == "push" and
                            first_insn.op_str == "ebp")

            # Get or create name
            label = self.labels.get(start_addr)
            if label:
                name = label.name
            else:
                name = f"sub_{start_addr:08X}"
                self.labels.auto_name_function(
                    start_addr, sec_name, confidence)

            func = Function(
                start=start_addr,
                end=end_addr,
                name=name,
                section=sec_name,
                confidence=confidence,
                detection_method=method,
                num_instructions=num_insns,
                has_prologue=has_prologue,
            )
            self.functions[start_addr] = func

    def _find_function_end(self, start: int, next_func: Optional[int],
                           sec_end: Optional[int]) -> int:
        """
        Determine where a function ends.

        Walks forward from start, tracking the furthest reachable point
        through fall-through and internal jumps.
        """
        max_addr = start   # exclusive end of the code decoded so far
        max_target = start  # highest branch target that must be *inside* it
        addr = start

        # Upper bound
        upper = sec_end if sec_end else start + 0x100000
        if next_func and next_func < upper:
            upper = next_func

        while addr < upper:
            insn = self.engine.get_instruction(addr)
            if insn is None:
                break

            end = insn.end_address
            if end > max_addr:
                max_addr = end

            # Track internal forward jumps to extend function bounds.
            #
            # Unconditional jumps count too, not just conditional ones. A body
            # ending in "jmp <forward>" - the tail of an if/else, or a jump
            # over an interleaved block - otherwise hit the break below with
            # max_addr still short of the target, truncating the function
            # mid-body. Everything past the cut then looked like separate code,
            # and the function's own jump targets became calls to empty stubs.
            #
            # `upper` is already clamped to the next known function start, so a
            # target inside these bounds is internal rather than a tail call.
            # is_jump and is_cond_jump are mutually exclusive; is_branch is both.
            if insn.is_branch and insn.jump_target is not None:
                target = insn.jump_target
                if start <= target < upper and target > max_target:
                    # This jump goes forward within bounds, extend
                    max_target = target

            if insn.is_ret or (insn.is_jump and not insn.is_cond_jump):
                # Stop only once we have decoded *past* every internal branch
                # target. A target is an address that must be inside the
                # function, so landing exactly on it is not coverage -- the
                # instruction there still has to be decoded. Using the target
                # as an exclusive end cut functions off at their own
                # out-of-line tail: MSVC routinely emits "jmp <backward>" and
                # then parks a conditional branch's target after it. Halo's
                # get_edge_vertex ended at the branch target, so the tail was
                # lifted as a separate function and the jump to it became a
                # tail call that returned without running the epilogue --
                # leaking the whole 28-byte frame on every call.
                if insn.end_address > max_target:
                    break
                # There might be more code after (jumped over)
                addr = insn.end_address
                continue

            addr = insn.end_address

        return max_addr

    def _build_call_graph(self) -> None:
        """Populate calls_to and called_by for all functions."""
        func_starts = set(self.functions.keys())

        for func in self.functions.values():
            insns = self.engine.get_instructions_in_range(func.start, func.end)
            callees = set()
            for insn in insns:
                if insn.call_target is not None:
                    callees.add(insn.call_target)

            func.calls_to = sorted(callees)

            for callee_addr in callees:
                callee = self.functions.get(callee_addr)
                if callee is not None:
                    callee.called_by.append(func.start)

        # Sort called_by lists
        for func in self.functions.values():
            func.called_by = sorted(set(func.called_by))

    def get_function_at(self, addr: int) -> Optional[Function]:
        """Get the function containing an address."""
        # First check direct match
        if addr in self.functions:
            return self.functions[addr]
        # Search for containing function
        for func in self.functions.values():
            if func.start <= addr < func.end:
                return func
        return None

    def get_functions_in_section(self, section_name: str) -> List[Function]:
        """Get all functions in a section, sorted by address."""
        return sorted(
            [f for f in self.functions.values() if f.section == section_name],
            key=lambda f: f.start
        )

    def summary(self) -> dict:
        """Return summary statistics."""
        by_method: Dict[str, int] = {}
        by_section: Dict[str, int] = {}
        total_insns = 0
        with_prologue = 0

        for func in self.functions.values():
            by_method[func.detection_method] = by_method.get(
                func.detection_method, 0) + 1
            by_section[func.section] = by_section.get(func.section, 0) + 1
            total_insns += func.num_instructions
            if func.has_prologue:
                with_prologue += 1

        return {
            "total_functions": len(self.functions),
            "total_instructions_in_functions": total_insns,
            "with_prologue": with_prologue,
            "by_detection_method": by_method,
            "by_section": by_section,
        }
