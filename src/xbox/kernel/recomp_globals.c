/**
 * Global register state + ICALL trace storage.
 * recomp_types.h declares these as `extern` - they must be defined
 * exactly once somewhere in the link. This is that place.
 *
 * Upstream keeps this storage in xbox_memory_layout.c; this tree moved it here
 * so the layout file only owns the mapping.  The definitions must still match
 * recomp_types.h exactly, RECOMP_TLS included: the v0.6.0 runtime makes the
 * simulated CPU state thread-local, so a value survives across basic blocks
 * and cannot be clobbered by another thread.
 */

#include "recomp_types.h"
/* getenv returns char*.  Without this header C treats it as returning
 * int, which on x64 truncates the pointer to 32 bits and sign-extends
 * it -- a wild address that only faults once something dereferences
 * the result rather than just testing it against NULL. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

/* SEH frame pointer bridge (see recomp_types.h). */
RECOMP_TLS uint32_t g_seh_ebp = 0;

/* Last frame established by `mov ebp, esp`. Read by frameless functions that
 * address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* x87 stack. The reset default for the control word masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS double   g_fp_stack[8];
RECOMP_TLS int      g_fp_top = 0;
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int      g_fp_cmp = 0;

/* SSE. 128 bits of architectural state each, per-thread like the rest. */
/* MMX is architectural state like the GPRs: a value produced in one lifted
 * function and read in another has to survive the call. */
RECOMP_TLS uint64_t g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS uint64_t g_mm4, g_mm5, g_mm6, g_mm7;
/* MXCSR rounding control; x86 resets to round-to-nearest-even. */
RECOMP_TLS uint32_t g_mmx_rc;

/* Opt-in frame-decoder capture for offline execution of the original x86.
 * Snapshots are never loaded back into the running title. */
unsigned long long g_xmv_decode_calls;
void recomp_xmv_step_capture(int after)
{
    static const char *prefix;
    static int initialized, active;
    static unsigned calls, wanted;
    if (!after) ++g_xmv_decode_calls;
    if (!initialized) {
        initialized=1;
        prefix=getenv("CONKER_XMV_STEP_CAPTURE");
        const char *number=getenv("CONKER_XMV_STEP_CALL");
        wanted=number ? (unsigned)strtoul(number,NULL,0) : 1u;
    }
    if (!prefix) return;
    if (!after) active=(++calls==wanted);
    if (!active) return;
    char path[512];
    snprintf(path,sizeof(path),"%s.%s.regs",prefix,after?"after":"before");
    FILE *f=fopen(path,"wb");
    if (!f) return;
    uint32_t regs[8]={g_eax,g_ecx,g_edx,g_ebx,g_esp,g_ebp,g_esi,g_edi};
    uint64_t mm[8]={g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7};
    RecompXmm xmm[8]={g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7};
    fwrite(regs,sizeof(regs),1,f);fwrite(mm,sizeof(mm),1,f);fwrite(xmm,sizeof(xmm),1,f);fclose(f);
    snprintf(path,sizeof(path),"%s.%s.ram",prefix,after?"after":"before");
    f=fopen(path,"wb");
    if (!f) return;
    static const unsigned char zero[0x10000];
    fwrite(zero,sizeof(zero),1,f);
    fwrite((const void*)(uintptr_t)(g_xbox_mem_offset+0x10000),0x03FF0000,1,f);
    fclose(f);
    fprintf(stderr,"[XMV-STEP] %s call=%u esp=%08X saved=%s\n",after?"after":"before",calls,g_esp,path);
    if (after) active=0;
}

RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

void recomp_icall_skip_log(uint32_t va)
{
    static unsigned logged;

    if (logged++ < 24u)
        fprintf(stderr, "[ICALL] SKIPPED out-of-range target 0x%08X (icall #%llu)\n",
                va, (unsigned long long)g_icall_count);
}

/* TEMPORARY diagnostic: esp/esi across one call site.
 *
 * sub_004E2801 decrements the decoder's slice counter through esi immediately
 * after calling sub_004E5450, and that write never lands on the decoder
 * object even though the call happens thousands of times.  esi is callee-saved
 * and sub_004E5450 saves and restores it correctly, so the question is whether
 * esp comes back where it went in. */
void recomp_probe_call(const char *tag, unsigned esp_v, unsigned esi_v)
{
    static unsigned logged;
    static unsigned before_esp, before_esi;

    if (tag[0] == 'b') {
        before_esp = esp_v;
        before_esi = esi_v;
        return;
    }
    if (logged++ < 20u) {
        fprintf(stderr,
                "[CALLPROBE] esp %08X -> %08X (delta %+d)   "
                "esi %08X -> %08X%s\n",
                before_esp, esp_v, (int)(esp_v - before_esp),
                before_esi, esi_v,
                before_esi == esi_v ? "" : "   *** esi CHANGED ***");
        fflush(stderr);
    }
}

/* TEMPORARY: the three conditions that end the decoder's slice loop.
 *
 *   continue only while   budget > limit
 *                   and   [esi+0xF8] == 0xFFFFFFFF
 *                   and   slices != 0
 *
 * The loop stops after three slices where hardware runs twelve to fourteen,
 * so exactly one of these is going the wrong way. */
void recomp_probe_loopexit(unsigned budget, unsigned limit,
                           unsigned f8, unsigned slices)
{
    static unsigned logged;

    if (logged++ < 24u) {
        const char *why = (budget <= limit) ? "budget <= limit -> EXIT"
                        : (f8 != 0xFFFFFFFFu) ? "F8 != FFFFFFFF -> EXIT"
                        : (slices != 0) ? "loop again"
                        : "slices == 0 -> EXIT (frame done)";
        fprintf(stderr, "[LOOPEXIT] budget=%08X limit=%08X F8=%08X slices=%u : %s\n",
                budget, limit, f8, slices, why);
        fflush(stderr);
    }
}

/* TEMPORARY: the division that decides whether another slice is decoded.
 *
 *   quotient = ((([esi+0xC8] + eax) << 16) + 0xFFFF) / [esi+0xBC]
 *
 * and the loop continues only while quotient <= the slice length.  The high
 * dword of the numerator is assembled with an adc, so a wrong carry there
 * would shift the quotient and stop the loop early. */
static unsigned g_div_c8, g_div_edi, g_div_bc, g_div_hi, g_div_lo;

void recomp_probe_div_in(unsigned c8, unsigned edi_v, unsigned bc,
                         unsigned num_hi, unsigned num_lo)
{
    g_div_c8 = c8; g_div_edi = edi_v; g_div_bc = bc;
    g_div_hi = num_hi; g_div_lo = num_lo;
}

void recomp_probe_div_out(unsigned quotient, unsigned limit)
{
    static unsigned logged;
    unsigned long long num = ((unsigned long long)g_div_hi << 32) | g_div_lo;
    unsigned long long want = g_div_bc ? (num / g_div_bc) : 0ull;

    if (logged++ < 20u) {
        fprintf(stderr,
                "[DIV] C8=%08X edi=%08X  num=%016llX / %08X = %08X"
                "  (native %016llX%s)  limit=%08X -> %s\n",
                g_div_c8, g_div_edi, num, g_div_bc, quotient, want,
                (unsigned)want == quotient ? " ok" : " MISMATCH",
                limit, quotient > limit ? "EXIT" : "continue");
        fflush(stderr);
    }
}

/* TEMPORARY: the slice length in the caller's own argument slot.
 *
 * sub_004E2801 stores (len & 0x1FFFF) * 4 + 4 -- never less than 4 -- into
 * MEM32(ebp + 8), and the loop's continue test later compares against it.  By
 * the time it is read it is 0.  sub_004E5450 is an fpo_leaf callee that
 * inherits this frame, so the question is whether it lands on this slot. */
void recomp_probe_frame(const char *tag, unsigned ebp_v, unsigned slot)
{
    static unsigned logged;

    if (logged++ < 30u) {
        fprintf(stderr, "[FRAME] %-6s ebp=%08X  [ebp+8]=%08X%s\n",
                tag, ebp_v, slot,
                (slot == 0u && tag[0] != 's') ? "   *** ZERO ***" : "");
        fflush(stderr);
    }
}

/* TEMPORARY: the decode budget's two terms.
 *
 *   limit = [0x5499E8]+0x1DE8  minus  [esi+0xB8]
 *
 * a counter now against the same counter when the frame started -- a
 * vsync-paced budget.  If the counter never advances the budget is always
 * zero and no slice is ever decoded. */
void recomp_probe_budget(unsigned now, unsigned start)
{
    static unsigned logged, first_now, seen;

    if (!seen) { first_now = now; seen = 1; }
    if (logged++ < 24u) {
        fprintf(stderr, "[BUDGET] counter=%08X start=%08X  limit=%d%s\n",
                now, start, (int)(now - start),
                now == first_now ? "   (counter has not moved)" : "");
        fflush(stderr);
    }
}

/* TEMPORARY: the parity gate on the presentation counter.
 *
 * sub_0053D4D0 increments [obj+0x1DE8] only when (counter ^ arg) & 1 == 0.
 * The counter is what the XMV decoder budgets its slices against, and it has
 * moved exactly once in an entire run while this function was called 3000
 * times -- so the argument's low bit is not what the title expects. */
void recomp_probe_parity(unsigned counter, unsigned arg)
{
    static unsigned logged, incremented, skipped;

    if (((counter ^ arg) & 1u) == 0u) ++incremented; else ++skipped;
    if (logged++ < 16u || (logged % 500u) == 0u) {
        fprintf(stderr, "[PARITY] counter=%08X arg=%08X -> %s"
                "   (incremented %u, skipped %u)\n",
                counter, arg,
                ((counter ^ arg) & 1u) == 0u ? "INCREMENT" : "skip",
                incremented, skipped);
        fflush(stderr);
    }
}

/* TEMPORARY: the present path that feeds the decoder's budget counter.
 *
 * sub_0053D6C0 selects a back buffer with [dev+0x2478] & 1, then calls
 * sub_0053D4D0 only when [dev+0x1DE0] is non-zero.  That callee increments
 * [dev+0x1DE8] -- the counter the XMV decoder budgets slices against -- but
 * only when the counter's low bit matches its argument.  Logged together so
 * the producer of that parity can be seen rather than guessed at. */
void recomp_probe_present(unsigned dev, unsigned gate, unsigned bufidx,
                          unsigned counter)
{
    static unsigned logged, calls, gated_out;
    static unsigned first_idx, seen;

    ++calls;
    if (!gate) ++gated_out;
    if (!seen) { first_idx = bufidx; seen = 1; }
    if (logged++ < 20u || (logged % 200u) == 0u) {
        fprintf(stderr, "[PRESENT] dev=%08X  bufidx=%08X (bit0=%u)  "
                "gate[1DE0]=%08X  counter=%08X  %s   (calls %u, gated out %u)%s\n",
                dev, bufidx, bufidx & 1u, gate, counter,
                gate ? "-> counter path" : "-> SKIPPED",
                calls, gated_out,
                bufidx == first_idx ? "   [bufidx has not changed]" : "");
        fflush(stderr);
    }
}

/* TEMPORARY: where the producer chain stops.
 *
 * main loop -> sub_002A6D90 -> sub_0053D8B0 -> sub_0053E5D0 -> sub_00541881,
 * and only the last of those raises the pending flag the presentation counter
 * needs.  Counting each link says which one stops being reached. */
void recomp_probe_chain(unsigned which)
{
    static unsigned counts[4];
    static unsigned logged;

    if (which < 4u)
        counts[which]++;
    ++logged;
    if (logged <= 12u || (logged % 400u) == 0u) {
        fprintf(stderr, "[CHAIN] 2A6D90=%u  D8B0=%u  E5D0=%u  41881=%u\n",
                counts[3], counts[0], counts[1], counts[2]);
        fflush(stderr);
    }
}

/* TEMPORARY: sub_00540F70, which on hardware increments the presentation
 * counter at 0x00540FA9 -- thousands of times, one at a time.  Ours writes
 * that counter only three times in a run, from a different function, so the
 * question is whether we call this at all. */
void recomp_probe_f70(unsigned counter)
{
    static unsigned calls, logged;

    ++calls;
    if (logged++ < 8u || (calls % 500u) == 0u) {
        fprintf(stderr, "[F70] sub_00540F70 entered, calls=%u  counter=%u\n",
                calls, counter);
        fflush(stderr);
    }
}

/* TEMPORARY: sub_00540DC0, which on hardware calls the counter's incrementer
 * 15,750 times a second.  It has no direct caller in the generated code, so it
 * is reached through a function pointer -- if at all. */
void recomp_probe_dc0(void)
{
    static unsigned calls, logged;

    ++calls;
    if (logged++ < 6u || (calls % 2000u) == 0u) {
        fprintf(stderr, "[DC0] sub_00540DC0 entered, calls=%u\n", calls);
        fflush(stderr);
    }
}

/* TEMPORARY: the service routine's gating checks, in the order it makes them.
 *
 * sub_00540CA0 is entered far more often than it queues its DPC, so most
 * entries are taking an early exit.  edi is the NV2A register base, so
 * +0x100 is PMC_INTR_0 and +0x140 is PMC_INTR_EN_0; the rest are fields of
 * the service context. */
void recomp_probe_isr(unsigned ctx, unsigned a0, unsigned intr_en,
                      unsigned intr_0, unsigned a4, unsigned f1b4,
                      unsigned p8)
{
    static unsigned calls, bail_a0, bail_en, skip_a4, no_pcrtc, ok;

    ++calls;
    if (a0 == 0u) ++bail_a0;
    else if (intr_en == 0u) ++bail_en;
    else if (a4 != 0u) ++skip_a4;
    else if ((intr_0 & 0x01000000u) == 0u) ++no_pcrtc;
    else ++ok;

    if (calls <= 6u || (calls % 50u) == 0u) {
        fprintf(stderr, "[ISR] ctx=%08X +A0=%08X PMC_EN=%08X PMC_INTR=%08X "
                "+A4=%08X +1B4=%08X +8=%08X(&C0000000=%08X %s) | calls=%u "
                "bailA0=%u bailEN=%u skipA4=%u noPCRTC=%u reached=%u\n",
                ctx, a0, intr_en, intr_0, a4, f1b4, p8, p8 & 0xC0000000u,
                (p8 & 0xC0000000u) == 0x80000000u ? "guard PASSES" : "eax=0",
                calls, bail_a0, bail_en, skip_a4, no_pcrtc, ok);
        fflush(stderr);
    }
}

/* TEMPORARY: the open's spin loop.
 *
 * sub_002520D0 calls the XMV update until its status is 1 or 2, or until esi
 * falls below ebx.  Status 3 means "still working".  With vblank delivery on,
 * the guest never leaves this loop and the frame count flatlines. */
void recomp_probe_openspin(unsigned status, unsigned esi_v, unsigned ebx_v)
{
    static unsigned long long iters;
    static unsigned last_status = 0xFFFFFFFFu, last_esi = 0xFFFFFFFFu;

    ++iters;
    if (iters <= 6ull || status != last_status || esi_v != last_esi
        || (iters % 20000ull) == 0ull) {
        fprintf(stderr, "[OPENSPIN] iter=%llu status=%u esi=%08X ebx=%08X%s\n",
                iters, status, esi_v, ebx_v,
                (status == 1u || status == 2u) ? "  -> would EXIT"
                : (int)esi_v < (int)ebx_v ? "  -> would EXIT (esi<ebx)" : "");
        fflush(stderr);
        last_status = status; last_esi = esi_v;
    }
}

/* TEMPORARY: does the status out-parameter the caller passes resolve to the
 * same address the loop later reads?  Statically both are esp0+0x1C, but that
 * depends on the stack being balanced across two calls. */
static unsigned g_outptr_pushed;

void recomp_probe_outptr_push(unsigned ptr)
{
    static unsigned logged;
    g_outptr_pushed = ptr;
    if (logged++ < 4u)
        fprintf(stderr, "[OUTPTR] caller passes status slot at %08X\n", ptr);
}

void recomp_probe_outptr_read(unsigned addr, unsigned value)
{
    static unsigned logged;
    if (logged++ < 8u) {
        fprintf(stderr, "[OUTPTR] loop reads %08X = %u   passed=%08X  %s\n",
                addr, value, g_outptr_pushed,
                addr == g_outptr_pushed ? "SAME ADDRESS"
                                        : "*** DIFFERENT ADDRESS ***");
        fflush(stderr);
    }
}

/* TEMPORARY: the out-pointer as the callee sees it.
 *
 * The caller passes the status slot as argument 3 and later reads it back at
 * the same address; the callee writes 3 into MEM32(ebp+0x10) as its first
 * action.  The slot nonetheless reads 0 forever, so either these two addresses
 * differ or the callee never reaches the write. */
void recomp_probe_callee_outptr(unsigned ptr, unsigned obj)
{
    static unsigned logged;

    if (logged++ < 8u) {
        fprintf(stderr, "[CALLEE] sub_004E2801 sees out-ptr=%08X obj=%08X\n",
                ptr, obj);
        fflush(stderr);
    }
}

/* TEMPORARY: the conditions that route sub_002520D0 into its spin loop.
 *
 * The loop calls the XMV update until the status is 1 or 2.  Status 0 means
 * "this call did nothing", which the caller has no case for, so the loop never
 * exits.  Baseline never enters the loop at all, so the divergence is here,
 * not in the update. */
void recomp_probe_route(unsigned c50, unsigned c54, unsigned c4c, unsigned entered)
{
    static unsigned long long calls;
    static unsigned last = 0xFFFFFFFFu;

    ++calls;
    if (calls <= 8ull || entered != last || (calls % 100000ull) == 0ull) {
        fprintf(stderr, "[ROUTE] call=%llu  849C50=%08X 849C54=%08X 849C4C=%08X"
                "  -> %s\n",
                calls, c50, c54, c4c,
                entered ? "ENTERS SPIN LOOP" : "skips");
        fflush(stderr);
        last = entered;
    }
}

/* TEMPORARY: the update's return value at the loop's first guard. */
void recomp_probe_firstguard(int ret, int limit)
{
    static unsigned long long calls, exits, falls;
    static int last = 0x7FFFFFFF;

    ++calls;
    if (ret < limit) ++exits; else ++falls;
    if (calls <= 8ull || ret != last || (calls % 200000ull) == 0ull) {
        fprintf(stderr, "[GUARD1] call=%llu update-returned=%d (0x%08X) limit=%d -> %s"
                "   exits=%llu falls-through=%llu\n",
                calls, ret, (unsigned)ret, limit,
                ret < limit ? "EXIT loop" : "fall through to status test",
                exits, falls);
        fflush(stderr);
        last = ret;
    }
}

/* TEMPORARY: does sub_004E2801 return to its caller at all?
 *
 * The baseline reaches the loop head but never the guard 6 lines later, so one
 * of the two calls between them does not come back.  This sits immediately
 * after the first of them. */
void recomp_probe_returned(unsigned which, int ret)
{
    static unsigned long long a, b;

    if (which == 0u) ++a; else ++b;
    if ((which == 0u && a <= 6ull) || (which == 1u && b <= 6ull)
        || ((a + b) % 200000ull) == 0ull) {
        fprintf(stderr, "[RET] %s returned (eax=%d)   after_4E2801=%llu after_2A37B0=%llu\n",
                which == 0u ? "sub_004E2801" : "sub_002A37B0", ret, a, b);
        fflush(stderr);
    }
}

/* TEMPORARY: iterations per entry to sub_002520D0's poll loop.
 *
 * Both configurations take the same path with the same operands.  The baseline
 * goes round twice and leaves; with vblank on it goes round for ever.  So the
 * loop is polling for a state transition that stops happening -- and the
 * iteration count per entry is the thing to measure first. */
static unsigned long long g_loop_iters, g_loop_entries;

void recomp_probe_loop_iter(void)
{
    ++g_loop_iters;
}

void recomp_probe_loop_exit(void)
{
    static unsigned long long reported;

    ++g_loop_entries;
    if (reported++ < 12ull || (g_loop_entries % 500ull) == 0ull) {
        fprintf(stderr, "[LOOPN] entry #%llu took %llu iteration(s)\n",
                g_loop_entries, g_loop_iters);
        fflush(stderr);
    }
    g_loop_iters = 0;
}

/* TEMPORARY: the three guards standing between sub_004E2801 and the only path
 * that sets a real status.
 *
 *   (A) [esi+0x40] == 0            -> bail, no status written
 *   (B) [esi+0x18] & 1             -> shortcut straight to the work
 *   (C) [ebp-20] > [ebp+8]         -> bail, no status written
 *
 * Bailing leaves the 3 written on entry, which the exit downgrades to 0 --
 * the "this call did nothing" the caller cannot handle. */
void recomp_probe_guards(unsigned width, unsigned flags,
                         unsigned budget, unsigned slicelen)
{
    static unsigned long long calls;
    static unsigned last_outcome = 0xFFFFFFFFu;
    unsigned outcome;

    if (width == 0u)                 outcome = 0;   /* A bails */
    else if (flags & 1u)             outcome = 1;   /* B shortcuts to work */
    else if (budget > slicelen)      outcome = 2;   /* C bails */
    else                             outcome = 3;   /* falls into the work */

    ++calls;
    if (calls <= 10ull || outcome != last_outcome || (calls % 100000ull) == 0ull) {
        static const char *names[4] = {
            "A bails (width==0)", "B shortcut -> work",
            "C bails (budget > slicelen)", "falls through -> work" };
        fprintf(stderr, "[GUARDS] call=%llu  width=%08X flags=%02X budget=%08X "
                "slicelen=%08X  -> %s\n",
                calls, width, flags & 0xFFu, budget, slicelen, names[outcome]);
        fflush(stderr);
        last_outcome = outcome;
    }
}

/* TEMPORARY: which basic blocks of sub_004E2801 execute.
 *
 * One probe per label, one pass, both configurations.  The update bails
 * somewhere between its entry and loc_004E2CE4 when the presentation counter
 * is advancing, and single-stepping that by one probe per run was costing a
 * turn per instruction. */
/* TEMPORARY: the 64-bit field at [esi+0xB0]/[esi+0xB4].
 *
 * Block coverage says the whole tail of sub_004E2801 -- the budget divide, the
 * three guards, sub_004E39A0 and the status-1 write -- is skipped under vblank
 * and taken 1,176 times in the baseline.  The branch that decides it ORs these
 * two halves together and skips when the result is zero. */
void recomp_probe_b0(unsigned lo, unsigned hi)
{
    static unsigned long long calls, zero, nonzero;
    static unsigned last_lo = 0xFFFFFFFFu, last_hi = 0xFFFFFFFFu;

    ++calls;
    if ((lo | hi) == 0u) ++zero; else ++nonzero;
    if (calls <= 10ull || lo != last_lo || hi != last_hi
        || (calls % 100000ull) == 0ull) {
        fprintf(stderr, "[B0] call=%llu  +B0=%08X +B4=%08X (64-bit %llu)  -> %s"
                "   zero=%llu nonzero=%llu\n",
                calls, lo, hi, ((unsigned long long)hi << 32) | lo,
                (lo | hi) == 0u ? "SKIP the tail" : "take the tail",
                zero, nonzero);
        fflush(stderr);
        last_lo = lo; last_hi = hi;
    }
}

/* TEMPORARY: the presentation status returned by sub_00539C60.
 *
 * That function fills an 8-byte out-parameter at [ebp-36]/[ebp-32] of
 * sub_004E2801:
 *
 *     out[4] = MEM32(dev + 0x1DE8);                 // presentation counter
 *     if ((MEM32(dev + 0x1DDC) & 0x1200000) == 0) { out[0] = 3; return; }
 *     out[0] = (MEM32(dev + 0x1DF8) == 0) ? 2 : 1;
 *
 * out[0] is what the guard at loc_004E2C70 tests against 1, and status 1 is
 * what stops the rdtsc at loc_004E2C76 from ever seeding [esi+0xB0].  Block
 * coverage says that guard sends every one of 72.7 million calls to the skip
 * under vblank, and the baseline reaches it only once.  Nothing in the lifted
 * title writes +0x1DF8, so measure the three inputs rather than assume them. */
static unsigned long long g_ps_calls, g_ps_1, g_ps_2, g_ps_3;
static unsigned g_ps_flags_seen, g_ps_pend_seen;

void recomp_probe_pstat(unsigned dev, unsigned flags, unsigned pending,
                        unsigned status, unsigned counter)
{
    int changed;

    ++g_ps_calls;
    if (status == 1u) ++g_ps_1; else if (status == 2u) ++g_ps_2; else ++g_ps_3;

    changed = (flags != g_ps_flags_seen) || (pending != g_ps_pend_seen);
    if (g_ps_calls <= 12ull || changed) {
        fprintf(stderr, "[PSTAT] call=%llu dev=%08X +1DDC=%08X (&1200000=%08X) "
                "+1DF8=%08X +1DE8=%08X -> status=%u%s" "\n",
                g_ps_calls, dev, flags, flags & 0x1200000u, pending, counter,
                status, status == 1u ? "  (BLOCKS the rdtsc)" : "");
        fflush(stderr);
        g_ps_flags_seen = flags; g_ps_pend_seen = pending;
    }
}

void recomp_dump_pstat(void)
{
    fprintf(stderr, "[PSTAT] total=%llu  status1=%llu status2=%llu status3=%llu"
            "  last +1DDC=%08X +1DF8=%08X" "\n",
            g_ps_calls, g_ps_1, g_ps_2, g_ps_3,
            g_ps_flags_seen, g_ps_pend_seen);
    fflush(stderr);
}

/* TEMPORARY: slices remaining at loc_004E2C6B, the first of the two guards. */
static unsigned long long g_sl_calls, g_sl_zero;

void recomp_probe_slices(unsigned slices, unsigned cmpval)
{
    ++g_sl_calls;
    if (slices == 0u) ++g_sl_zero;
    if (g_sl_calls <= 12ull || (g_sl_calls % 5000000ull) == 0ull) {
        fprintf(stderr, "[SLICE] call=%llu remaining=%u  zero=%llu nonzero=%llu" "\n",
                g_sl_calls, slices, g_sl_zero, g_sl_calls - g_sl_zero);
        fflush(stderr);
    }
}

/* TEMPORARY: which row of the mode table at 0x005466B0 was selected. */
void recomp_probe_mode(unsigned entry, unsigned value, unsigned avpack)
{
    fprintf(stderr, "[MODE] avpack=%08X -> entry 0x%08X (index %d) value=%08X"
            "  bit21=%u bit24=%u  &1200000=%08X\n",
            avpack, entry, (int)((entry - 0x005466B0u) / 12u), value,
            (value >> 21) & 1u, (value >> 24) & 1u, value & 0x1200000u);
    fflush(stderr);
}

/* TEMPORARY: is the XMV update still being pumped after the first burst?
 *
 * The decoder object stops advancing about six seconds in -- +08 (read
 * position) freezes and +3C (slices remaining) sits at zero -- while the guest
 * keeps running.  Two possibilities: the caller stops calling sub_004E2801, or
 * it keeps calling and the update reports it has nothing to do.  Uncapped, so
 * this cannot be misread as a capped log the way several earlier ones were.
 *
 * Entry writes 3 to the status out-parameter, so the value read at exit is
 * what this call decided.  esi is the object, [ebp+0x10] the status pointer.
 */
static unsigned long long g_xmv_calls;
static unsigned g_xmv_obj, g_xmv_status_ptr;
static unsigned g_xmv_last_read, g_xmv_last_slices;
static unsigned long long g_xmv_frozen_since;   /* call # read pos last moved */
static int g_xmv_reported_freeze;

void recomp_probe_xmv_enter(unsigned obj, unsigned status_ptr)
{
    g_xmv_obj = obj;
    g_xmv_status_ptr = status_ptr;
    ++g_xmv_calls;
}

void recomp_probe_xmv_exit(void)
{
    unsigned rd, slices, p60, p64, p68, status;

    if (g_xmv_obj == 0u)
        return;
    rd     = MEM32(g_xmv_obj + 0x08);
    slices = MEM32(g_xmv_obj + 0x3C);
    p60    = MEM32(g_xmv_obj + 0x60);
    p64    = MEM32(g_xmv_obj + 0x64);
    p68    = MEM32(g_xmv_obj + 0x68);
    status = g_xmv_status_ptr ? MEM32(g_xmv_status_ptr) : 0xFFFFFFFFu;

    if (rd != g_xmv_last_read) {
        g_xmv_frozen_since = g_xmv_calls;
        g_xmv_reported_freeze = 0;
    }

    /* Every call for the first 40, then every state change, then the moment
     * the read position has been still for 200 calls. */
    if (g_xmv_calls <= 40ull
        || rd != g_xmv_last_read || slices != g_xmv_last_slices
        || (!g_xmv_reported_freeze
            && g_xmv_calls - g_xmv_frozen_since == 200ull)) {
        if (g_xmv_calls - g_xmv_frozen_since == 200ull)
            g_xmv_reported_freeze = 1;
        fprintf(stderr, "[XMV] call=%llu status=%u  +08=%08X +3C=%u  "
                "+60=%08X +64=%08X +68=%08X%s" "\n",
                g_xmv_calls, status, rd, slices, p60, p64, p68,
                (g_xmv_calls - g_xmv_frozen_since == 200ull)
                    ? "   <- read position still for 200 calls" : "");
        fflush(stderr);
    }
    g_xmv_last_read = rd;
    g_xmv_last_slices = slices;
}

void recomp_dump_xmv(void)
{
    fprintf(stderr, "[XMV] total calls=%llu  last +08=%08X +3C=%u  "
            "read position last moved at call %llu" "\n",
            g_xmv_calls, g_xmv_last_read, g_xmv_last_slices,
            g_xmv_frozen_since);
    fflush(stderr);
}

/* TEMPORARY: the completed-frame publish, sub_004E2801 loc_004E2DA7..DB9.
 *
 *     loc_004E2DA7:  if ([esi+0x74] == 0) goto loc_004E2E6B;   bail
 *     loc_004E2DB0:  if ([esi+0x6C] != 0) goto loc_004E2E6B;   bail
 *     loc_004E2DB9:  [esi+0x6C] = [esi+0x68];  [esi+0x68] = 0; publish
 *
 * The pipeline freezes with +64 and +68 holding buffers and +60/+6C empty, so
 * the second guard passes and the suspect is +0x74.  Counting all three tells
 * us which one stops it, without touching either field. */
static unsigned long long g_pub_hits[3];
static unsigned g_pub_last74 = 0xFFFFFFFFu;

void recomp_probe_pub(unsigned which, unsigned obj)
{
    unsigned v74, v6c, v68;

    if (which < 3u) ++g_pub_hits[which];
    if (which != 0u) return;

    v74 = MEM32(obj + 0x74);
    v6c = MEM32(obj + 0x6C);
    v68 = MEM32(obj + 0x68);
    if (v74 != g_pub_last74 || g_pub_hits[0] <= 8ull) {
        fprintf(stderr, "[PUB] reach=%llu +74=%08X +6C=%08X +68=%08X -> %s" "\n",
                g_pub_hits[0], v74, v6c, v68,
                v74 == 0u ? "BAIL on +74"
                          : (v6c != 0u ? "BAIL on +6C" : "publish"));
        fflush(stderr);
        g_pub_last74 = v74;
    }
}

void recomp_dump_pub(void)
{
    fprintf(stderr, "[PUB] reached DA7=%llu  passed 74-guard (DB0)=%llu  "
            "published (DB9)=%llu   last +74=%08X" "\n",
            g_pub_hits[0], g_pub_hits[1], g_pub_hits[2], g_pub_last74);
    fflush(stderr);
}

/* TEMPORARY: the packet-status loop that gates the completed-frame publish.
 *
 *     loc_004E2D78:  call sub_004C5248()          the DSP/APU side
 *     loc_004E2D7D:  [esi+0x74] = 1               publish enabled
 *     loc_004E2D91:  if (*ecx == 0x8000000A) goto loc_004E2DA4
 *     loc_004E2DA4:  [esi+0x74] = 0               publish disabled again
 *
 * ecx walks [esi+0x48] entries starting at [esi+0x148].  0x8000000A is
 * E_PENDING, so a frame is held back while an associated packet has not
 * retired.  Our APU is a passthrough stub, which is the obvious suspect. */
static unsigned long long g_pend_hits, g_pend_pending;
static unsigned g_pend_last = 0xFFFFFFFFu;

void recomp_probe_pend(unsigned entry, unsigned value, unsigned count)
{
    ++g_pend_hits;
    if (value == 0x8000000Au) ++g_pend_pending;
    if (value != g_pend_last || g_pend_hits <= 6ull) {
        fprintf(stderr, "[PEND] hit=%llu entry=%08X *entry=%08X count=%u  %s" "\n",
                g_pend_hits, entry, value, count,
                value == 0x8000000Au ? "E_PENDING -> publish disabled" : "");
        fflush(stderr);
        g_pend_last = value;
    }
}

void recomp_dump_pend(void)
{
    fprintf(stderr, "[PEND] loop hits=%llu  E_PENDING=%llu  last=%08X" "\n",
            g_pend_hits, g_pend_pending, g_pend_last);
    fflush(stderr);
}

/* TEMPORARY: sub_004CD89F sets bit 0 of the APU IEN -- GINTSTS, the global
 * interrupt enable.  Without it update_irq can never assert and vector 49
 * never fires, which is where the audio packet retirement stops.  On hardware
 * the audio ISR runs 607 times in a few seconds, so this must be reached with
 * a non-zero argument there. */
void recomp_probe_ien(unsigned enable, unsigned cur)
{
    static unsigned n;
    if (n < 12u) {
        ++n;
        fprintf(stderr, "[IEN] sub_004CD89F(enable=%u) current IEN=%08X" "\n",
                enable, cur);
        fflush(stderr);
    }
}

/* TEMPORARY: the gate on enabling the APU global interrupt.
 *
 * sub_004C86AC: KeInitializeInterrupt, then KeConnectInterrupt; a zero return
 * makes edi E_FAIL, and a negative edi skips
 *
 *     [0xFE801000] = 0xFFFFFFFF;      ack every ISTS bit
 *     sub_004CD89F(1);                set IEN bit 0, GINTSTS
 *
 * Without that the APU can never assert, vector 49 never fires, the audio ISR
 * never runs, and the packet the XMV update waits on never retires.  On
 * hardware that ISR runs 607 times in a few seconds. */
void recomp_probe_audioinit(unsigned edi_v, unsigned eax_v)
{
    static unsigned n;
    if (n < 8u) {
        ++n;
        fprintf(stderr, "[AUDIOINIT] edi=%08X (%s)  eax=%08X -> %s" "\n",
                edi_v, ((int)edi_v < 0) ? "negative, ENABLE SKIPPED" : "ok",
                eax_v, ((int)edi_v < 0) ? "no interrupt" : "enables GINTSTS");
        fflush(stderr);
    }
}

/* TEMPORARY: the branch that turns the whole APU interrupt setup away.
 *
 *     loc_004C86AC:  ecx = this + 8; sub_004CDFED();
 *     loc_004C86BD:  edi = eax; test edi, edi; jl loc_004C8780
 *
 * Block coverage says sub_004C86AC is entered once, reaches loc_004C86BD, and
 * jumps straight to the exit at loc_004C8780 -- so the enable at loc_004C874D
 * and every block between are never executed.  This is the operand. */
void recomp_probe_audiogate(unsigned ret)
{
    static unsigned n;
    if (n < 8u) {
        ++n;
        fprintf(stderr, "[AUDIOGATE] sub_004CDFED returned %08X -> %s" "\n",
                ret, ((int)ret < 0) ? "negative, whole init ABANDONED"
                                    : "ok, init continues");
        fflush(stderr);
    }
}

/* TEMPORARY: what the audio ISR at 0x0055FB7A actually does.
 *
 *     ecx = [context];  eax = [ecx+0x10] IEN;  edx = [ecx+0x0C] ISTS
 *     edx &= eax;  if (!edx) leave -- not ours
 *     if (edx & 0x20) -> loc_0055FBB0, the notification branch
 *     loc_0055FBE3    -> indirect call, queues the DPC that retires packets
 *     loc_0055FC09    -> the not-ours exit
 *
 * It runs 6 times in 42 seconds here against 607 in a few on a console, and
 * the packets it should retire stay at E_PENDING. */
static unsigned long long g_aisr[4];

void recomp_probe_aisr(unsigned which, unsigned base, unsigned pend, unsigned ien)
{
    static unsigned n;
    if (which < 4u) ++g_aisr[which];
    if (which == 0u && n < 10u) {
        ++n;
        fprintf(stderr, "[AISR] entry base=%08X  ISTS&IEN=%08X  IEN=%08X" "\n",
                base, pend, ien);
        fflush(stderr);
    }
    if (which == 2u && n < 14u) {
        ++n;
        fprintf(stderr, "[AISR]   reached the DPC-queue call" "\n");
        fflush(stderr);
    }
}

void recomp_dump_aisr(void)
{
    fprintf(stderr, "[AISR] entered=%llu  notify-branch=%llu  dpc-queue=%llu  "
            "not-ours-exit=%llu" "\n",
            g_aisr[0], g_aisr[1], g_aisr[2], g_aisr[3]);
    fflush(stderr);
}

/* TEMPORARY: does the DirectSound service loop run?
 *
 * Hardware retires the audio packet at 0x0055945C, reached from sub_004C7FFF
 * via loc_004C3C39 -- the guest's own service routine, called synchronously by
 * the XMV update through sub_004C5248 in the same call that then publishes the
 * frame.  Not an interrupt, not the DSP asynchronously.
 *
 *     loc_004C3C21:  if (MEM32(0x4E1668) != 0) goto loc_004C3C39;   skip
 *     loc_004C3C2D:  sub_004C7FFF();                                retire
 */
void recomp_probe_svc(unsigned flag)
{
    static unsigned long long calls, skipped;
    static unsigned last = 0xFFFFFFFFu;

    ++calls;
    if (flag != 0u) ++skipped;
    if (calls <= 6ull || flag != last || (calls % 500ull) == 0ull) {
        fprintf(stderr, "[SVC] call=%llu [0x4E1668]=%08X -> %s   "
                "skipped=%llu of %llu" "\n",
                calls, flag, flag ? "SKIP sub_004C7FFF" : "run it",
                skipped, calls);
        fflush(stderr);
        last = flag;
    }
}

/* TEMPORARY: what decides a DirectSound packet is finished.
 *
 * On a console the status is cleared by sub_0055943E, reached through
 * sub_004C7FFF -> sub_004C8E98 -> sub_004C8E22 -> sub_004C8C02.  sub_004C8E98
 * retires every pending packet in the list at [esi+0xB8] unconditionally, so
 * the decision is whether anything calls it.  Three places do, and one of them
 * gates on a hardware byte:
 *
 *     loc_004C9161:  eax = [esi+0x80];
 *                    if ((MEM8(eax + 0x0B) & 0x40) == 0) skip the retire
 *
 * +0x0B is the NABM channel control byte.  This says whether that guard is
 * reached and what it reads. */
static unsigned long long g_ret[5];

void recomp_probe_retire(unsigned which, unsigned ptr, unsigned byte)
{
    static unsigned n;

    if (which < 5u) ++g_ret[which];
    /* which==4 is the flush.  Report the distinct lists it walks: the XMV
     * packet that never retires lives around 0x03F0E76D, so if no flush
     * ever names a list near there, it belongs to an object that nothing
     * services. */
    if (which == 4u) {
        static unsigned seen[8], nseen, shown;
        unsigned i;
        for (i = 0; i < nseen; i++)
            if (seen[i] == ptr) break;
        if (i == nseen && nseen < 8u) {
            seen[nseen++] = ptr;
            if (shown < 8u) {
                ++shown;
                fprintf(stderr, "[RETIRE] flush list #%u at %08X head=%08X" "\n",
                        nseen, ptr, byte);
                fflush(stderr);
            }
        }
    }
    if (which == 0u && n < 10u) {
        ++n;
        fprintf(stderr, "[RETIRE] guard: [esi+0x80]=%08X  [+0x0B]=%02X  "
                "& 0x40 = %02X -> %s" "\n",
                ptr, byte, byte & 0x40u,
                (byte & 0x40u) ? "RETIRE" : "skip");
        fflush(stderr);
    }
}

void recomp_dump_retire(void)
{
    fprintf(stderr, "[RETIRE] guard reached=%llu  retire calls: A=%llu B=%llu "
            "C=%llu  flush reached=%llu" "\n",
            g_ret[0], g_ret[1], g_ret[2], g_ret[3], g_ret[4]);
    fflush(stderr);
}

/* TEMPORARY: which DirectSound buffer owns the packet the video waits on.
 *
 * sub_004C9358(this=ecx, packet=[ebp+8]) sets the packet to E_PENDING and
 * queues it on the object in esi.  The flush walks four objects whose lists
 * sit at 012E96F4, 012E9A1C, 012E9D44 and 012D4104 -- so the objects are
 * those minus 0xB8 -- and all four are empty.  The packets the XMV polls are
 * at 03F0E7xx.  This prints the owner so the two sets can be compared. */
void recomp_probe_owner(unsigned owner, unsigned packet, unsigned listhead)
{
    static unsigned seen[8], n, shown;
    unsigned i;

    for (i = 0; i < n; i++)
        if (seen[i] == owner) break;
    if (i == n && n < 8u) seen[n++] = owner;
    if (shown < 10u) {
        ++shown;
        fprintf(stderr, "[OWNER] buffer=%08X list=%08X head=%08X packet=%08X" "\n",
                owner, owner + 0xB8u, listhead, packet);
        fflush(stderr);
    }
}

/* TEMPORARY: the list head at the END of sub_004C9358.
 *
 * At entry the completion list is empty, which proves nothing -- the linking
 * would happen later in the call.  This samples the same head on the way out,
 * with the returned HRESULT and the flags byte at +0x12 whose bit 5 decides
 * whether sub_004C90A0 (the service kick) is called at all on the way past.
 *
 * head == list address means the circular list is still empty. */
void recomp_probe_owner2(unsigned owner, unsigned head, unsigned ret,
                         unsigned flags)
{
    static unsigned shown;
    static unsigned long long calls, empty;

    ++calls;
    if (head == owner + 0xA8u) ++empty;
    if (shown < 10u) {
        ++shown;
        fprintf(stderr, "[SUBMIT-END] buffer=%08X head=%08X %s  hr=%08X  "
                "flags=%02X kick=%s" "\n",
                owner, head,
                head == owner + 0xA8u ? "EMPTY" : "linked",
                ret, flags, (flags & 0x20u) ? "skipped" : "called");
        fflush(stderr);
    }
    if ((calls % 200ull) == 0ull) {
        fprintf(stderr, "[SUBMIT-END] %llu submits, %llu left the list empty" "\n",
                calls, empty);
        fflush(stderr);
    }
}

/* TEMPORARY: is the packet ever actually queued?
 *
 *     loc_004C9370:  if (MEM16(esi+0x12) & 0x2000) goto loc_004C9395;
 *     loc_004C9395:  sub_004CAE82(); if (hr < 0) return hr;
 *     loc_004C93A3:  take the lock, allocate a packet from [esi+0xB0]
 *     loc_004C93C2:  copy the 24-byte context in, set the packet status,
 *                    lock the buffer pages
 *
 * The completion list at [esi+0xB8] is empty on the way out of every submit,
 * so either this path is not taken or the packet is built and never linked. */
unsigned long long g_queue_stage[4];

void recomp_probe_queue(unsigned stage)
{
    /* Reported from the periodic dump, not on a modulo of its own call
     * count: submissions happen a handful of times in a run, so "every
     * 200 calls" printed nothing and read as "never taken". */
    if (stage < 4u) ++g_queue_stage[stage];
}

void recomp_dump_queue(void)
{
    fprintf(stderr, "[QUEUE] gate-taken=%llu alloc=%llu fill=%llu" "\n",
            g_queue_stage[1], g_queue_stage[2], g_queue_stage[3]);
    fflush(stderr);
}

/* TEMPORARY: does the packet stay linked at +0xA8, and does anything service it?
 *
 * sub_004C9358 inserts the packet into the circular list at [buffer+0xA8].
 * sub_004C8E22, the flush, walks [buffer+0xB8] -- a different list, which is
 * why every earlier "empty" reading was meaningless.  sub_004C8FB0 is the
 * function that touches +0xA8 and also calls the flush, and its retire call
 * site fired zero times, so it is the candidate servicer.
 *
 * head == buffer + 0xA8 means the circular list is empty. */
static unsigned g_a8_buffer;
static unsigned long long g_svc8[7];

void recomp_probe_svc8(unsigned which, unsigned self)
{
    if (which < 7u) ++g_svc8[which];
    if (which == 0u && self) g_a8_buffer = self;
}

void recomp_dump_a8(void)
{
    fprintf(stderr, "[SVC8] sub_004C8FB0 entered=%llu  call sites: "
            "1=%llu 2=%llu 3=%llu 4=%llu 5=%llu 6=%llu" "\n",
            g_svc8[0], g_svc8[1], g_svc8[2], g_svc8[3],
            g_svc8[4], g_svc8[5], g_svc8[6]);
    if (g_a8_buffer) {
        unsigned head = MEM32(g_a8_buffer + 0xA8u);
        fprintf(stderr, "[SVC8] buffer=%08X  +0xA8 head=%08X -> %s" "\n",
                g_a8_buffer, head,
                head == g_a8_buffer + 0xA8u ? "EMPTY"
                                            : "still linked");
    }
    fflush(stderr);
}

/* TEMPORARY: the +0xA8 head immediately after the insert and before the
 * kick at loc_004C9417, to tell "never linked" from "linked then drained". */
void recomp_probe_a8mid(unsigned owner, unsigned head)
{
    static unsigned shown;
    if (shown < 8u) {
        ++shown;
        fprintf(stderr, "[A8-MID] buffer=%08X head=%08X -> %s" "\n",
                owner, head,
                head == owner + 0xA8u ? "EMPTY" : "LINKED");
        fflush(stderr);
    }
}

/* TEMPORARY: object identity from buffer construction to the retire guard.
 *
 * [buffer+0x80] holds an object passed INTO the buffer constructor --
 * sub_004CAE38 is only an AddRef, it takes the object as its argument and
 * hands it straight back.  Two constructors do this, sub_004C8CD7 (vtable
 * 0x5860A0) and sub_004CBEFB (vtable 0x586104).
 *
 * The guard at loc_004C9161 tests bit 6 of the byte at +0x0B of that object,
 * which is bit 30 of its dword at +0x08.  Printing the whole dword at both
 * ends, keyed on the actual pointer values rather than on offsets, says
 * whether it was ever non-zero and which object each buffer really holds. */
void recomp_probe_ident(unsigned which, unsigned buffer, unsigned obj,
                        unsigned dword8)
{
    static unsigned nctor, nguard;
    const char *what = (which == 2u) ? "guard"
                     : (which == 0u) ? "ctor 5860A0" : "ctor 586104";

    if (which == 2u) {
        if (nguard >= 6u) return;
        ++nguard;
    } else {
        if (nctor >= 8u) return;
        ++nctor;
    }
    fprintf(stderr, "[IDENT] %-11s buffer=%08X obj=%08X  [obj+8]=%08X  "
            "bit30=%u" "\n",
            what, buffer, obj, dword8, (dword8 >> 30) & 1u);
    fflush(stderr);
}

/* TEMPORARY: the caps word that becomes [obj+0x08].
 *
 * sub_004C61B6 assigns it wholesale from its argument, and sub_004C644A
 * takes that argument straight from the descriptor: push [esi], where esi is
 * the descriptor passed down unchanged from sub_004C65E1 -- the
 * CreateSoundBuffer path.  So the caps word is the descriptor field the
 * title supplies, and bit 30 is either in it or it is not.
 *
 * A console has 0x40000000 in [obj+0x08] on all 28 buffers; we have zero. */
void recomp_probe_caps(unsigned desc, unsigned f0, unsigned f8, unsigned f14)
{
    static unsigned n;
    if (n < 12u) {
        ++n;
        fprintf(stderr, "[CAPS] desc=%08X  [desc]=%08X bit30=%u  "
                "[desc+8]=%08X [desc+0x14]=%08X" "\n",
                desc, f0, (f0 >> 30) & 1u, f8, f14);
        fflush(stderr);
    }
}

/* TEMPORARY: which of the two CreateSoundBuffer call sites is used.
 *
 *   0x004C67DA  returns into sub_004C67B9, a thin ret-16 forwarder
 *   0x004C6A1D  returns into sub_004C69EA
 *
 * The descriptor a console passes lives on the heap at 0xD005DA6C with bit 30
 * set; ours is a stack temporary at 0x011FED54 with the field zero.  Different
 * storage and different contents points at a different caller, and there are
 * exactly two to choose between. */
static unsigned long long g_site[2];

void recomp_probe_site(unsigned which, unsigned ret)
{
    static unsigned shown;
    if (which < 2u) ++g_site[which];
    if (shown < 8u) {
        ++shown;
        fprintf(stderr, "[SITE] %s  returned=%08X" "\n",
                which == 0u ? "A 0x004C67DA" : "B 0x004C6A1D", ret);
        fflush(stderr);
    }
}

void recomp_dump_site(void)
{
    fprintf(stderr, "[SITE] A(0x004C67DA)=%llu  B(0x004C6A1D)=%llu" "\n",
            g_site[0], g_site[1]);
    fflush(stderr);
}

/* TEMPORARY: which parameter of sub_004C69EA is the descriptor.
 *
 * Both systems create their buffers through site B, so the caller is the
 * same and the descriptor differs in how it is obtained.  Ours is the stack
 * address 0x011FED54; whichever of these arguments equals it is the one to
 * follow upward. */
void recomp_probe_args(unsigned a0, unsigned a1, unsigned a2, unsigned framep)
{
    static unsigned n;
    if (n < 8u) {
        ++n;
        fprintf(stderr, "[ARGS] esi=%08X [ebp+8]=%08X [ebp+C]=%08X  ebp=%08X" "\n",
                a0, a1, a2, framep);
        fflush(stderr);
    }
}

/* TEMPORARY: which producer supplies the descriptor.
 *
 * The parameter trace ended in a literal: sub_00251F70 does xor ebx,ebx once
 * and passes ebx as the caps word at both of its call sites, so the XMV path
 * hands DirectSound a hard zero.  A console cannot be getting 0x40000000 down
 * that path, and its descriptor is on the heap at 0xD005DA6C rather than in a
 * stack frame -- so it must be arriving from the OTHER caller of
 * sub_004C69EA.  Earlier instrumentation covered the callers of sub_004C65E1,
 * one level too low to see this.
 *
 *   producer 0  return 0x00437461, in recomp_0018
 *   producer 1  return 0x004E27B4, the XMV path
 *
 * The descriptor is the first stacked argument at the call. */
static unsigned long long g_prod[2];

void recomp_probe_prod(unsigned which, unsigned desc, unsigned caps)
{
    static unsigned shown;
    if (which < 2u) ++g_prod[which];
    if (shown < 10u) {
        ++shown;
        fprintf(stderr, "[PROD] %s  desc=%08X  caps=%08X bit30=%u" "\n",
                which == 0u ? "0x00437461" : "0x004E27B4 (XMV)",
                desc, caps, (caps >> 30) & 1u);
        fflush(stderr);
    }
}

void recomp_dump_prod(void)
{
    fprintf(stderr, "[PROD] 0x00437461=%llu  0x004E27B4(XMV)=%llu" "\n",
            g_prod[0], g_prod[1]);
    fflush(stderr);
}

/* TEMPORARY: is the missing sound-buffer producer reached at all?
 *
 * sub_00437339 creates 23 buffers on a console and none here.  Its three call
 * sites are unguarded -- each sits at the top of a thin wrapper that calls it
 * immediately with different constants:
 *
 *   sub_00437479  -> sub_00437339(arg, 8, 2)
 *   sub_004375D3  -> sub_00437339(arg, 1, 2)
 *   sub_004376E9  -> sub_00437339(arg, 1, 3)
 *
 * So there is no branch guarding the call and the question is only whether
 * the wrappers are entered.  If none are, the divergence is above them. */
static unsigned long long g_wrap[4];

void recomp_probe_wrap(unsigned which)
{
    if (which < 4u) ++g_wrap[which];
}

void recomp_dump_wrap(void)
{
    fprintf(stderr, "[WRAP] 00437479=%llu  004375D3=%llu  004376E9=%llu  "
            "producer 00437339=%llu" "\n",
            g_wrap[0], g_wrap[1], g_wrap[2], g_wrap[3]);
    fflush(stderr);
}

/* TEMPORARY: the sound-bank loaders above the missing producer.
 *
 * None of the three wrappers is entered here, so the divergence is above
 * them.  Each has exactly one caller, and each call sits in a loop body
 * behind the same shape of guard -- a signed jl on a counter:
 *
 *   sub_00434D3D  loop body loc_00434DE9 -> sub_00437479
 *   sub_004347C9  loop body loc_00434879 -> sub_004375D3
 *   sub_004355E9  loop body loc_00435699 -> sub_004376E9
 *
 * Entry counts say whether the loaders run at all; body counts say whether
 * they run and iterate zero times, which are different failures. */
static unsigned long long g_load[6];

void recomp_probe_load(unsigned which)
{
    if (which < 6u) ++g_load[which];
}

void recomp_dump_load(void)
{
    fprintf(stderr, "[LOAD] entries: 00434D3D=%llu 004347C9=%llu 004355E9=%llu" "\n",
            g_load[0], g_load[1], g_load[2]);
    fprintf(stderr, "[LOAD] bodies:  00434DE9=%llu 00434879=%llu 00435699=%llu" "\n",
            g_load[3], g_load[4], g_load[5]);
    fflush(stderr);
}

/* TEMPORARY: the two upstream failures behind the 23 missing sound buffers.
 *
 * Task 1 -- sub_00434D3D is never called.  Its single call site at
 * loc_0043551B is UNGUARDED, so the question is whether its enclosing
 * function sub_00435442 runs and reaches that block.
 *
 * Task 2 -- the other two loaders are not iterating an empty collection.
 * They fail before the loop:
 *
 *     esi = ebx + 0x10;
 *     sub_004C69A3(...);              creation call, out-slot at esi
 *     if (MEM32(esi) != 0) continue;
 *     MEM32(ebp-4) = 0x8007000E;      E_OUTOFMEMORY
 *     goto skip;
 *
 * So [ebp-4] is not a counter at that point, it is an HRESULT, and the loop
 * guard reads it as negative.  What matters is what sub_004C69A3 leaves in
 * the out-slot. */
static unsigned long long g_up[5];
static unsigned g_up_val[5];

void recomp_probe_up(unsigned which, unsigned val)
{
    if (which < 5u) { ++g_up[which]; if (val) g_up_val[which] = val; }
}

void recomp_dump_up(void)
{
    fprintf(stderr, "[UP] task1: sub_00435442 entry=%llu  loc_0043551B=%llu" "\n",
            g_up[0], g_up[1]);
    fprintf(stderr, "[UP] task2: sub_004C69A3 entered=%llu   out-slot A=%llu "
            "(last %08X)  out-slot B=%llu (last %08X)" "\n",
            g_up[4], g_up[2], g_up_val[2], g_up[3], g_up_val[3]);
    fflush(stderr);
}

/* TEMPORARY: why the sound singleton at 0x75F11C is unusable.
 *
 * sub_00435442 reads its count as MEM32(MEM32(0x75F11C) + 4) and
 * sub_00433DAB reads its array as MEM32(MEM32(0x75F11C) + 0x14), so both
 * upstream failures are the same object.  That object is filled by a
 * 9-dword rep movsd in sub_0043454E from a descriptor built out of
 * literals in sub_0027B080:
 *
 *     desc[0] = 0xF                 -> [S+4]     the count (15!)
 *     desc[1] = 2                   -> [S+8]
 *     desc[4] = MEM32(0x849E78)     -> [S+0x14]  the array
 *
 * desc[0] is a hard literal, so a zero count means S itself is null --
 * which sub_0043463C does at loc_0043469E when the init fails.  These
 * counters say which of created / destroyed / null-descriptor it is. */
static unsigned long long g_sing[57];
static unsigned g_sing_seq[57];
static unsigned g_sing_clock;
static unsigned g_sing_a[57], g_sing_b[57];

void recomp_probe_sing(unsigned which, unsigned a, unsigned b)
{
    if (which < 57u) {
        if (g_sing[which] == 0u) g_sing_seq[which] = ++g_sing_clock;
        ++g_sing[which];
        g_sing_a[which] = a;
        g_sing_b[which] = b;
    }
}

void recomp_dump_sing(void)
{
    static const char *const nm[57] = {
        "00 sub_0027B080 entry",
        "01 sub_0043463C entry     desc / desc[0] (want 0000000F)",
        "02   0x80 alloc           eax",
        "03 sub_0043454E copied    [S+4] (want F) / [S+14]",
        "04   sub_00436C0E ret     eax / [0x75F138]",
        "05   0x7C alloc           eax / count (want 2)",
        "06   sub_00433E03 ret     eax / [0x561B8C]",
        "07 sub_0043454E exit      HRESULT",
        "08 *** SINGLETON CLEARED ***",
        "09 sub_0043463C exit      HRESULT / [0x75F11C]",
        "10 sub_00435442 entry     [0x75F11C]",
        "11   loc_0043549C         count / [0x75F11C]",
        "12   loader A accessor    eax / [0x75F11C]",
        "13 sub_00433E03 entry     arg2 / vtable",
        "14   slot probe           index / slot",
        "15   Initialize ret       eax",
        "16   loc_00433E2F         [S+3C] / [S+8]",
        "17   slot already taken",
        "18 STORE readback        eax / [0x75F11C]",
        "19 before sub_0043454E   [0x75F11C] / eax",
        "20 after  sub_0043454E   eax / [0x75F11C]",
        "21 *** DESTRUCTOR sub_0043411E ***  [0x75F11C]",
        "22 sub_0028A170 entry     (the producer path)",
        "23 after sub_004C5271     eax / [esp+18]",
        "24 after sub_0027AF20     [0x849E78] / [0x77A5A4]",
        "25 sub_004C5271 entry     arg0 / arg1",
        "26 sub_004C5271 gate      [0x4E1CF0] / flag arg",
        "27   -> DSERR 88780032 early-out",
        "28   -> CreateFile path   name",
        "29   handle from open     eax",
        "30   size query           eax",
        "31   DSad alloc           eax",
        "32   after read           eax / [ebp-4]",
        "33   into sub_004C3AA9    [0x4E1CF0] / outptr",
        "34   after sub_004C3AA9   eax / *outptr",
        "35   exit funnel          esi",
        "36 sub_00437339 entry     arg1 / arg2 (2 fails, 3 works)",
        "37   alloc arg1*8         eax",
        "38   alloc arg2*320       eax",
        "39   alloc 0x280          eax",
        "40   sub_004C69EA ret     eax / arg2",
        "41 sub_00437339 exit      HRESULT",
        "42 5D3 alloc 4/8017      eax",
        "43 5D3 sub_004387CA      eax",
        "44 5D3 alloc 0xB0        eax",
        "45 5D3 sub_00437D8F      eax",
        "46 5D3 alloc 0xA         eax",
        "47 5D3 -> E_OUTOFMEMORY",
        "48 5D3 exit HRESULT      [ebp+8]",
        "49 sub_00439390 entry     handle / arg1",
        "50   -> 0x181A null handle",
        "51   alloc 0x2C           eax",
        "52   sub_0043E3B0         eax",
        "53   sub_0043E2E0         eax",
        "54   alloc 9              eax",
        "55   sub_00439440         eax",
        "56   -> 0x1770 FAILURE EXIT",
    };
    unsigned i;
    fprintf(stderr, "[SING] ---- sound singleton 0x75F11C ----" "\n");
    for (i = 0; i < 57u; ++i)
        fprintf(stderr, "[SING] seq=%-3u %-44s n=%-6llu a=%08X b=%08X" "\n",
                g_sing_seq[i], nm[i], g_sing[i],
                g_sing_a[i], g_sing_b[i]);
    fflush(stderr);
}

/* cpuid.
 *
 * The lifter emits cpuid as a bare comment while the surrounding decode is
 * emitted normally, so eax keeps whatever leaf number it was loaded with and
 * every field decoded from it is wrong.  sub_00449DE0 decodes the family as
 * (1 >> 8) & 0xF = 0, sub_00449E60 requires 6, and the 0x1775 it returns
 * becomes an E_OUTOFMEMORY that stops two of the three sound-bank loaders
 * after one iteration.
 *
 * Reporting the host CPU would be wrong: the title is asking what console it
 * is running on.  The answer is a Pentium III Coppermine -- family 6, model 8,
 * stepping 10, MMX and SSE, no SSE2.
 */
void recomp_cpuid(uint32_t leaf)
{
    switch (leaf) {
    case 0u:
        g_eax = 2u;                  /* highest supported leaf */
        g_ebx = 0x756E6547u;         /* "Genu" */
        g_edx = 0x49656E69u;         /* "ineI" */
        g_ecx = 0x6C65746Eu;         /* "ntel" */
        break;
    case 1u:
        g_eax = 0x0000068Au;         /* family 6, model 8, stepping 10 */
        g_ebx = 0x00000000u;
        g_ecx = 0x00000000u;
        g_edx = 0x0383F9FFu;         /* MMX (23) and SSE (25) set, SSE2 clear */
        break;
    case 2u:
        g_eax = 0x03020101u;         /* cache descriptors */
        g_ebx = 0x00000000u;
        g_ecx = 0x00000000u;
        g_edx = 0x0C040843u;
        break;
    default:
        g_eax = g_ebx = g_ecx = g_edx = 0u;
        break;
    }
}

/* TEMPORARY: why decode stops after the first published frame.
 *
 * sub_004E2801 is called ~1200 times but the compressed-stream read
 * position at +0x08 stops advancing after call 3.  This records, for the
 * first few calls, the whole decoder state on entry, the status written to
 * the out-parameter on exit, and which of the 118 basic blocks each call
 * reached -- so the first block taken on an advancing call and skipped on
 * the next can be read off directly rather than guessed at. */
#define E28_BLOCKS 118u
#define E28_CALLS  8u
static const char *const g_e28_name[E28_BLOCKS] = { "004E2801","004E2823","004E2838","004E2841","004E2846","004E284F","004E2858","004E2863","004E2871","004E2879","004E2884","004E28EE","004E28F4","004E290F","004E2914","004E2925","004E292D","004E2934","004E2963","004E296E","004E2971","004E2985","004E298D","004E299C","004E29A1","004E29B0","004E29B8","004E29C1","004E29E2","004E29E5","004E29F0","004E29F9","004E2A24","004E2A29","004E2A2D","004E2A30","004E2A3A","004E2A46","004E2A59","004E2ABD","004E2ACD","004E2AD6","004E2ADB","004E2B10","004E2B1E","004E2B27","004E2B42","004E2B4A","004E2B58","004E2B81","004E2B89","004E2B93","004E2B9B","004E2BF3","004E2BFB","004E2C09","004E2C36","004E2C3E","004E2C47","004E2C50","004E2C5D","004E2C62","004E2C6B","004E2C70","004E2C76","004E2C82","004E2C92","004E2C9F","004E2CA4","004E2CAF","004E2CB7","004E2CBF","004E2CCA","004E2CCF","004E2CD7","004E2CDF","004E2CE4","004E2CE9","004E2CEF","004E2CF7","004E2D00","004E2D11","004E2D13","004E2D1C","004E2D21","004E2D28","004E2D2D","004E2D5E","004E2D6A","004E2D73","004E2D78","004E2D7D","004E2D8B","004E2D91","004E2D99","004E2DA2","004E2DA4","004E2DA7","004E2DB0","004E2DB9","004E2DE7","004E2DEC","004E2DF2","004E2DF7","004E2DFC","004E2E10","004E2E1A","004E2E25","004E2E34","004E2E38","004E2E3E","004E2E43","004E2E58","004E2E67","004E2E6B","004E2E73","004E2E75","004E2E77" };
static unsigned char g_e28_seen[E28_CALLS][E28_BLOCKS];
static unsigned g_e28_call;            /* 1-based; 0 = none yet */
static unsigned long long g_e28_total;
static uint32_t g_e28_obj[E28_CALLS];
static uint32_t g_e28_fld[E28_CALLS][8];
static uint32_t g_e28_status[E28_CALLS];

void recomp_probe_e28b(unsigned id)
{
    if (id == 0u) {                    /* function entry: a new call */
        ++g_e28_total;
        if (g_e28_total <= (unsigned long long)E28_CALLS) {
            g_e28_call = (unsigned)g_e28_total;
            memset(g_e28_seen[g_e28_call - 1u], 0, E28_BLOCKS);
        } else {
            g_e28_call = 0u;
        }
    }
    if (g_e28_call != 0u && id < E28_BLOCKS)
        g_e28_seen[g_e28_call - 1u][id] = 1u;
}

void recomp_probe_e28_enter(uint32_t obj, uint32_t outp)
{
    static const uint32_t off[8] = { 0x08u, 0x3Cu, 0x50u, 0x60u,
                                     0x64u, 0x68u, 0x6Cu, 0x74u };
    unsigned i;
    (void)outp;
    if (g_e28_call == 0u) return;
    g_e28_obj[g_e28_call - 1u] = obj;
    for (i = 0; i < 8u; ++i)
        g_e28_fld[g_e28_call - 1u][i] = obj ? MEM32(obj + off[i]) : 0u;
}

void recomp_probe_e28_exit(uint32_t outp)
{
    if (g_e28_call == 0u) return;
    g_e28_status[g_e28_call - 1u] = outp ? MEM32(outp) : 0xFFFFFFFFu;
}

void recomp_dump_e28(void)
{
    unsigned c, i, n = (unsigned)(g_e28_total < (unsigned long long)E28_CALLS
                                  ? g_e28_total : (unsigned long long)E28_CALLS);
    if (n == 0u) return;
    fprintf(stderr, "[E28] %llu calls; state on entry, status on exit" "\n",
            g_e28_total);
    fprintf(stderr, "[E28] call obj      +08      +3C  +50 +60 +64 +68 +6C +74  status  blocks" "\n");
    for (c = 0; c < n; ++c) {
        unsigned hit = 0;
        for (i = 0; i < E28_BLOCKS; ++i) hit += g_e28_seen[c][i];
        fprintf(stderr, "[E28]  %2u  %08X %08X %4u %4u %3u %3u %3u %3u %3u  %08X  %u" "\n",
                c + 1u, g_e28_obj[c], g_e28_fld[c][0], g_e28_fld[c][1],
                g_e28_fld[c][2], g_e28_fld[c][3], g_e28_fld[c][4],
                g_e28_fld[c][5], g_e28_fld[c][6], g_e28_fld[c][7],
                g_e28_status[c], hit);
    }
    /* Consecutive-call coverage diff.  The first block one call reaches and
     * the next does not is the divergence point. */
    for (c = 0; c + 1u < n; ++c) {
        int first = -1;
        for (i = 0; i < E28_BLOCKS; ++i) {
            if (g_e28_seen[c][i] && !g_e28_seen[c + 1u][i]) { first = (int)i; break; }
        }
        if (first < 0) {
            fprintf(stderr, "[E28] call %u -> %u: no block lost" "\n", c + 1u, c + 2u);
            continue;
        }
        fprintf(stderr, "[E28] call %u -> %u: FIRST BLOCK LOST = %s (block %d)" "\n",
                c + 1u, c + 2u, g_e28_name[first], first);
        fprintf(stderr, "[E28]   lost:");
        for (i = 0; i < E28_BLOCKS; ++i)
            if (g_e28_seen[c][i] && !g_e28_seen[c + 1u][i])
                fprintf(stderr, " %s", g_e28_name[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "[E28]   gained:");
        for (i = 0; i < E28_BLOCKS; ++i)
            if (!g_e28_seen[c][i] && g_e28_seen[c + 1u][i])
                fprintf(stderr, " %s", g_e28_name[i]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* Is the presentation clock advancing?
 *
 * loc_004E2B27 computes elapsed = [dev+0x1DE8] - [esi+0xB8], where the
 * first term comes from sub_00539C60 and is the display field counter, not
 * a wall clock.  That elapsed value is what eventually resets [esi+0xF8] to
 * -1 at loc_004E2B9B and lets the next compressed packet be consumed.  If
 * the counter never moves, the deadline never passes and decode stops after
 * the packet that is already in flight. */
static unsigned long long g_e28clk_n;
static uint32_t g_e28clk_first_ctr, g_e28clk_last_ctr;
static uint32_t g_e28clk_min_ctr, g_e28clk_max_ctr;
static uint32_t g_e28clk_base, g_e28clk_e8, g_e28clk_f8;

void recomp_probe_e28clk(uint32_t counter, uint32_t base,
                         uint32_t e8, uint32_t f8)
{
    if (g_e28clk_n == 0ull) {
        g_e28clk_first_ctr = counter;
        g_e28clk_min_ctr = g_e28clk_max_ctr = counter;
    }
    if (counter < g_e28clk_min_ctr) g_e28clk_min_ctr = counter;
    if (counter > g_e28clk_max_ctr) g_e28clk_max_ctr = counter;
    g_e28clk_last_ctr = counter;
    g_e28clk_base = base;
    g_e28clk_e8 = e8;
    g_e28clk_f8 = f8;
    ++g_e28clk_n;
}

void recomp_dump_e28clk(void)
{
    if (g_e28clk_n == 0ull) return;
    fprintf(stderr, "[E28CLK] n=%llu  field counter first=%u last=%u min=%u max=%u  %s" "\n",
            g_e28clk_n, g_e28clk_first_ctr, g_e28clk_last_ctr,
            g_e28clk_min_ctr, g_e28clk_max_ctr,
            g_e28clk_max_ctr == g_e28clk_min_ctr ? "FROZEN" : "advancing");
    fprintf(stderr, "[E28CLK] base [+B8]=%u  elapsed=%d  [+E8]=%08X  [+F8]=%08X" "\n",
            g_e28clk_base, (int)(g_e28clk_last_ctr - g_e28clk_base),
            g_e28clk_e8, g_e28clk_f8);
    fflush(stderr);
}

/* TEMPORARY: why the next compressed packet never becomes available.
 *
 * With the presentation clock running, decode advances and then stops with
 * [decoder+0x3C] == 0 and [decoder+0xF8] == 0xFFFFFFFF -- the decoder wants
 * a packet and the current chunk is exhausted.  A chunk is refilled at
 * loc_004E2ABD, which sets the read position at +0x08 and the packet count
 * at +0x3C from bits 23..30 of a chunk header.  Reaching it depends on two
 * branches and nothing else:
 *
 *   loc_004E29E5   if ([esi+0x60] == 0) skip the refill
 *   loc_004E29F0   if ([esi+0x64] != 0) skip the refill
 *
 * This records the full field set every time that decision is made, keeps
 * the last one that went on to refill, and keeps the first one that did not
 * while the decoder was actually starved.  Comparing the two says which of
 * the two buffer pointers changed and which way. */
#define RF_FIELDS 20u
static const uint32_t g_rf_off[RF_FIELDS] = { 0x08u, 0x1Cu, 0x24u, 0x2Cu, 0x3Cu, 0x50u, 0x54u, 0x58u, 0x60u, 0x64u, 0x68u, 0x6Cu, 0x74u, 0xB0u, 0xB4u, 0xB8u, 0xC8u, 0xE4u, 0xE8u, 0xF8u };
static const char *const g_rf_name[RF_FIELDS] = { "+08", "+1C", "+24", "+2C", "+3C", "+50", "+54", "+58", "+60", "+64", "+68", "+6C", "+74", "+B0", "+B4", "+B8", "+C8", "+E4", "+E8", "+F8" };

static uint32_t g_rf_cur[RF_FIELDS];    /* the decision in progress */
static uint32_t g_rf_ok[RF_FIELDS];     /* last decision that refilled */
static uint32_t g_rf_bad[RF_FIELDS];    /* first starved decision that did not */
static uint32_t g_rf_post[RF_FIELDS];   /* state just after that last refill */
static unsigned long long g_rf_decisions, g_rf_refills, g_rf_starved;
static int g_rf_pending, g_rf_have_bad;

static void rf_snap(uint32_t obj, uint32_t *dst)
{
    unsigned i;
    for (i = 0; i < RF_FIELDS; ++i)
        dst[i] = obj ? MEM32(obj + g_rf_off[i]) : 0u;
}

void recomp_probe_rf(unsigned site, uint32_t obj)
{
    unsigned i;

    if (site == 0u) {
        /* Close out the previous decision before starting a new one. */
        if (g_rf_pending && !g_rf_have_bad &&
            g_rf_cur[4] == 0u && g_rf_cur[19] == 0xFFFFFFFFu) {
            /* +0x3C == 0 and +0xF8 == -1: starved, and it did not refill. */
            ++g_rf_starved;
            for (i = 0; i < RF_FIELDS; ++i) g_rf_bad[i] = g_rf_cur[i];
            g_rf_have_bad = 1;
        }
        ++g_rf_decisions;
        rf_snap(obj, g_rf_cur);
        g_rf_pending = 1;
        return;
    }

    /* site 1: the refill actually ran. */
    ++g_rf_refills;
    for (i = 0; i < RF_FIELDS; ++i) g_rf_ok[i] = g_rf_cur[i];
    rf_snap(obj, g_rf_post);
    g_rf_pending = 0;
}

void recomp_dump_rf(void)
{
    unsigned i;
    if (g_rf_decisions == 0ull) return;
    fprintf(stderr, "[RF] decisions=%llu refills=%llu starved-skips=%llu" "\n",
            g_rf_decisions, g_rf_refills, g_rf_starved);
    if (!g_rf_have_bad) {
        fprintf(stderr, "[RF] no starved skip recorded" "\n");
    }
    fprintf(stderr, "[RF] field   last-refill  after-it   first-starved  changed" "\n");
    for (i = 0; i < RF_FIELDS; ++i) {
        int diff = g_rf_have_bad && g_rf_ok[i] != g_rf_bad[i];
        fprintf(stderr, "[RF]  %-5s  %08X     %08X   %08X       %s" "\n",
                g_rf_name[i], g_rf_ok[i], g_rf_post[i],
                g_rf_have_bad ? g_rf_bad[i] : 0u, diff ? "<<<" : "");
    }
    fflush(stderr);
}

/* TEMPORARY: which packet blocks publication.
 *
 * loc_004E2DA4 is the only path that clears [decoder+0x74], and it is
 * reached from one place: the scan at loc_004E2D91 finding a status of
 * 0x8000000A (E_PENDING) in the array at [decoder+0x148], which holds
 * [decoder+0x48] entries.  The parallel array at [decoder+0x134] holds the
 * owning DirectSound object for each entry -- the submit path at
 * loc_004E2971 takes obj = [[esi+0x134] + i*4] and calls [obj_vtable+0x10]
 * -- and [decoder+0x13C] holds a per-entry submitted flag.
 *
 * Recorded per entry index so the responsible slot is named rather than
 * inferred, with the single successful publication kept for comparison. */
#define PK_MAX 16u

static unsigned long long g_pk_scan, g_pk_clear, g_pk_pub;
static unsigned long long g_pk_clear_idx[PK_MAX];
static unsigned long long g_pk_seen_idx[PK_MAX];
static uint32_t g_pk_status_seen[PK_MAX];

struct pk_snap {
    int valid;
    uint32_t idx, entry, status, count;
    uint32_t obj, objvt, obj80, cls, flag;
    uint32_t f68, f6c, f74;
    uint32_t arr[PK_MAX];
    uint32_t owner[PK_MAX];
};
static struct pk_snap g_pk_bad, g_pk_good;

static void pk_fill(struct pk_snap *d, uint32_t esi_v, uint32_t idx,
                    uint32_t entry)
{
    uint32_t objs = MEM32(esi_v + 0x134);
    uint32_t flags = MEM32(esi_v + 0x13C);
    uint32_t base = MEM32(esi_v + 0x148);
    unsigned i, n;

    d->valid = 1;
    d->idx = idx;
    d->entry = entry;
    d->status = entry ? MEM32(entry) : 0u;
    d->count = MEM32(esi_v + 0x48);
    d->obj = objs ? MEM32(objs + idx * 4u) : 0u;
    d->objvt = d->obj ? MEM32(d->obj) : 0u;
    d->obj80 = d->obj ? MEM32(d->obj + 0x80u) : 0u;
    d->cls = d->obj80 ? MEM32(d->obj80 + 8u) : 0u;
    d->flag = flags ? MEM32(flags + idx * 4u) : 0u;
    d->f68 = MEM32(esi_v + 0x68);
    d->f6c = MEM32(esi_v + 0x6C);
    d->f74 = MEM32(esi_v + 0x74);
    n = d->count < PK_MAX ? d->count : PK_MAX;
    for (i = 0; i < PK_MAX; ++i) {
        d->arr[i] = (base && i < n) ? MEM32(base + i * 4u) : 0xDEADDEADu;
        d->owner[i] = (objs && i < n) ? MEM32(objs + i * 4u) : 0u;
    }
}

void recomp_probe_pk(unsigned site, uint32_t esi_v, uint32_t idx,
                     uint32_t entry)
{
    if (site == 0u) {
        ++g_pk_scan;
        if (idx < PK_MAX) {
            ++g_pk_seen_idx[idx];
            if (entry) g_pk_status_seen[idx] = MEM32(entry);
        }
        return;
    }
    if (site == 1u) {
        ++g_pk_clear;
        if (idx < PK_MAX) ++g_pk_clear_idx[idx];
        if (!g_pk_bad.valid) pk_fill(&g_pk_bad, esi_v, idx, entry);
        return;
    }
    ++g_pk_pub;
    if (!g_pk_good.valid) pk_fill(&g_pk_good, esi_v, 0u, 0u);
}

static void pk_show(const char *tag, const struct pk_snap *d)
{
    unsigned i;
    if (!d->valid) { fprintf(stderr, "[PK] %s: never happened" "\n", tag); return; }
    fprintf(stderr, "[PK] %s: idx=%u entry=%08X status=%08X count=%u" "\n",
            tag, d->idx, d->entry, d->status, d->count);
    fprintf(stderr, "[PK]   owner obj=%08X vtable=%08X [obj+80]=%08X [[obj+80]+8]=%08X flag=%u" "\n",
            d->obj, d->objvt, d->obj80, d->cls, d->flag);
    fprintf(stderr, "[PK]   decoder +68=%08X +6C=%08X +74=%08X" "\n",
            d->f68, d->f6c, d->f74);
    fprintf(stderr, "[PK]   status array:");
    for (i = 0; i < PK_MAX && d->arr[i] != 0xDEADDEADu; ++i)
        fprintf(stderr, " [%u]=%08X", i, d->arr[i]);
    fprintf(stderr, "\n");
    fprintf(stderr, "[PK]   owners      :");
    for (i = 0; i < PK_MAX && d->arr[i] != 0xDEADDEADu; ++i)
        fprintf(stderr, " [%u]=%08X", i, d->owner[i]);
    fprintf(stderr, "\n");
}

void recomp_dump_pk(void)
{
    unsigned i;
    if (g_pk_scan == 0ull) return;
    fprintf(stderr, "[PK] scans=%llu clears=%llu publications=%llu" "\n",
            g_pk_scan, g_pk_clear, g_pk_pub);
    fprintf(stderr, "[PK] per index  seen / cleared / last status:" "\n");
    for (i = 0; i < PK_MAX; ++i)
        if (g_pk_seen_idx[i])
            fprintf(stderr, "[PK]   [%2u] seen=%-8llu cleared=%-8llu last=%08X" "\n",
                    i, g_pk_seen_idx[i], g_pk_clear_idx[i], g_pk_status_seen[i]);
    pk_show("FIRST FAILED publication", &g_pk_bad);
    pk_show("THE ONE SUCCESS        ", &g_pk_good);
    fflush(stderr);
}

/* TEMPORARY: one class-0x586038 packet, from submission onward.
 *
 * The decoder submits at loc_004E2971 with obj = [[esi+0x134] + i*4] and
 * calls [vtable+0x10].  This records the object, its vtable, the resolved
 * submit method, the whole vtable and the descriptor at ebp-60 that carries
 * the status slot -- so the class is followed by identity rather than by
 * offsets borrowed from the 5860A0 / 586104 buffer classes. */
static int g_s38_done;
static uint32_t g_s38_obj, g_s38_voice;

void recomp_probe_sub38(uint32_t obj, uint32_t vtable, uint32_t desc,
                        uint32_t byteidx)
{
    unsigned k;
    if (g_s38_done) return;
    g_s38_done = 1;
    fprintf(stderr, "[S38] submit obj=%08X vtable=%08X idx=%u" "\n",
            obj, vtable, byteidx / 4u);
    fprintf(stderr, "[S38]   [vtable+0x10] = %08X   <- the submit method" "\n",
            vtable ? MEM32(vtable + 0x10) : 0u);
    fprintf(stderr, "[S38]   vtable:");
    for (k = 0; k < 16u; ++k)
        fprintf(stderr, " +%02X=%08X", k * 4u,
                vtable ? MEM32(vtable + k * 4u) : 0u);
    fprintf(stderr, "\n");
    g_s38_obj = obj;
    g_s38_voice = MEM32(obj + 0x24);
    fprintf(stderr, "[S38]   inner voice [obj+0x24] = %08X" "\n",
            g_s38_voice);
    fprintf(stderr, "[S38]   descriptor at %08X:", desc);
    for (k = 0; k < 5u; ++k)
        fprintf(stderr, " [%u]=%08X", k, MEM32(desc + k * 4u));
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Does OUR voice ever reach the retire guard, and with what classification?
 *
 * sub_004C90A0 gates the packet flush on bit 30 of [[esi+0x80]+8].  esi
 * there is a voice object.  The voice that carries the XMV packets is
 * [obj+0x24] of the class-0x586038 object the decoder submits to, captured
 * at submit time -- so this compares by identity and never assumes the
 * 0x586038 object and the voice share a layout. */
static unsigned long long g_g38_all, g_g38_ours, g_g38_ours_pass;
static uint32_t g_g38_ours_cls, g_g38_ours_clsobj;
static uint32_t g_g38_other_cls[4]; static uint32_t g_g38_other_obj[4];
static unsigned g_g38_others;

void recomp_probe_gate38(uint32_t voice, uint32_t clsobj)
{
    uint32_t cls = clsobj ? MEM32(clsobj + 8) : 0u;
    ++g_g38_all;
    if (g_s38_voice != 0u && voice == g_s38_voice) {
        ++g_g38_ours;
        g_g38_ours_clsobj = clsobj;
        g_g38_ours_cls = cls;
        if (cls & 0x40000000u) ++g_g38_ours_pass;
    } else if (g_g38_others < 4u) {
        g_g38_other_obj[g_g38_others] = voice;
        g_g38_other_cls[g_g38_others] = cls;
        ++g_g38_others;
    }
}

void recomp_dump_gate38(void)
{
    unsigned i;
    if (g_s38_voice == 0u) { fprintf(stderr, "[G38] no class-586038 submit seen" "\n"); return; }
    fprintf(stderr, "[G38] our obj=%08X voice=%08X" "\n", g_s38_obj, g_s38_voice);
    fprintf(stderr, "[G38] retire guard: total=%llu  ours=%llu  ours-passed-bit30=%llu" "\n",
            g_g38_all, g_g38_ours, g_g38_ours_pass);
    if (g_g38_ours)
        fprintf(stderr, "[G38]   ours: [voice+0x80]=%08X  [.. +8]=%08X  bit30=%u" "\n",
                g_g38_ours_clsobj, g_g38_ours_cls,
                (g_g38_ours_cls >> 30) & 1u);
    else
        fprintf(stderr, "[G38]   ours NEVER reached the retire guard" "\n");
    for (i = 0; i < g_g38_others; ++i)
        fprintf(stderr, "[G38]   other voice=%08X cls=%08X bit30=%u" "\n",
                g_g38_other_obj[i], g_g38_other_cls[i],
                (g_g38_other_cls[i] >> 30) & 1u);
    fflush(stderr);
}

/* Is the class-0x586038 caps word ever set, or set and then lost?
 *
 * A console shows bit 30 set in [[voice+0x80]+8]; the recomp shows 0 at the
 * retire guard.  Sampling the object the moment sub_004C69EA returns it
 * separates "never written" from "written then cleared". */
static unsigned g_mk38_n;
void recomp_probe_mk38(uint32_t hr, uint32_t obj)
{
    uint32_t voice, clsobj;
    if (g_mk38_n >= 4u) return;
    ++g_mk38_n;
    voice  = obj ? MEM32(obj + 0x24) : 0u;
    clsobj = voice ? MEM32(voice + 0x80) : 0u;
    fprintf(stderr, "[MK38] #%u hr=%08X obj=%08X vtable=%08X  [obj+0x38]=%08X [obj+0x40]=%08X" "\n",
            g_mk38_n, hr, obj, obj ? MEM32(obj) : 0u,
            obj ? MEM32(obj + 0x38) : 0u, obj ? MEM32(obj + 0x40) : 0u);
    fprintf(stderr, "[MK38]    voice=%08X [voice+0x80]=%08X caps=%08X bit30=%u" "\n",
            voice, clsobj, clsobj ? MEM32(clsobj + 8) : 0u,
            clsobj ? ((MEM32(clsobj + 8) >> 30) & 1u) : 0u);
    fflush(stderr);
}

/* TEMPORARY: where the class-0x586038 caps word should get bit 30.
 *
 * Construction, established by following identity rather than offsets:
 *
 *   sub_004C65E1  allocates the 0x28-byte owner, runs sub_004C5D27 (which
 *                 only stores the two vtables) then sub_004C6483
 *   sub_004C6483  allocates a 0xC8 object, stamps [it]=0x586064 [it+4]=1,
 *                 stores it at [owner+0x20], then calls sub_004C644A,
 *                 allocates the voice and calls sub_004C8CD7
 *   sub_004C8CD7  sets [voice] = 0x5860A0 and [voice+0x80] = the object
 *                 sub_004CAE38 hands back -- which is that same 0xC8 object
 *
 * So the caps field the retire guard reads, [[voice+0x80]+8], is reachable
 * as [[owner+0x20]+8] from the moment the 0xC8 object exists, well before
 * the voice does.  That is what makes a before/after diff possible across
 * every call in the path.
 *
 * The owner is only 0x28 bytes, so anything at owner+0x30 or +0x38 is a
 * neighbouring allocation, not a field -- the earlier reading of an
 * "embedded" class object at +0x38 was that coincidence. */
#define CAPS38_SITES 7u

static const char *const g_caps38_name[CAPS38_SITES] = {
    "0 after 0xC8 alloc      (before sub_004C644A)",
    "1 after sub_004C644A    (the caps computation)",
    "2 after 0x198 alloc",
    "3 after sub_004C8CD7    (the voice ctor)",
    "4 after [owner+0x24]=voice",
    "5 after sub_004C882A",
    "6 after sub_004C5671    (end)",
};
#define CAPS38_ROWS 48u
static uint32_t g_caps38_owner[CAPS38_ROWS];
static uint32_t g_caps38_val[CAPS38_ROWS][CAPS38_SITES];
static unsigned char g_caps38_hit[CAPS38_ROWS][CAPS38_SITES];
static unsigned g_caps38_rows;

void recomp_probe_caps38(unsigned site, uint32_t owner)
{
    uint32_t capsobj, caps;
    unsigned r;

    if (site >= CAPS38_SITES) return;
    for (r = 0; r < g_caps38_rows; ++r)
        if (g_caps38_owner[r] == owner) break;
    if (r == g_caps38_rows) {
        if (g_caps38_rows >= CAPS38_ROWS) return;
        g_caps38_owner[r] = owner;
        ++g_caps38_rows;
    }
    capsobj = owner ? MEM32(owner + 0x20) : 0u;
    caps = capsobj ? MEM32(capsobj + 8) : 0xFFFFFFFFu;
    g_caps38_val[r][site] = caps;
    g_caps38_hit[r][site] = 1u;
}

void recomp_dump_caps38(void)
{
    unsigned r, i;
    if (g_caps38_rows == 0u) return;
    fprintf(stderr, "[C38] [[owner+0x20]+8] through the construction path (FFFFFFFF = no object yet)" "\n");
    for (i = 0; i < CAPS38_SITES; ++i)
        fprintf(stderr, "[C38]   site %s" "\n", g_caps38_name[i]);
    for (r = 0; r < g_caps38_rows; ++r) {
        fprintf(stderr, "[C38] owner=%08X:", g_caps38_owner[r]);
        for (i = 0; i < CAPS38_SITES; ++i) {
            if (g_caps38_hit[r][i])
                fprintf(stderr, "  [%u]=%08X", i, g_caps38_val[r][i]);
            else
                fprintf(stderr, "  [%u]=--------", i);
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* TEMPORARY: is the scan reading the generation just submitted?
 *
 * The decoder keeps two 4-slot rings of status arrays:
 *
 *   lane A   +0x140 -> +0x144 -> +0x148 -> +0x14C -> back to +0x140
 *   lane B   +0x150 -> +0x154 -> +0x158 -> +0x15C -> back to +0x150
 *
 * Submission builds a descriptor whose [desc+0x8] is [0x150]+i and whose
 * [desc+0xC] is [0x140]+i, and sub_00559420 then writes 0 to the first and
 * E_PENDING to the second.  The publication scan reads [0x148]+i.  So the
 * design only works if the array sitting at +0x148 is an older generation
 * than the one at +0x140.  Rotations happen in four separate blocks, and
 * this brackets every one of them plus both ends. */
#define ROT_SITES 10u
#define ROT_LOG   24u

static const char *const g_rot_name[ROT_SITES] = {
    "read-completion  BEFORE  (0x14C -> 0x140)",
    "read-completion  AFTER",
    "SUBMIT           marks [0x140]+i, clears [0x150]+i",
    "refill rotation  BEFORE  (0x140 -> 0x144)",
    "refill rotation  AFTER",
    "middle rotation  BEFORE  (0x144 -> 0x148)",
    "middle rotation  AFTER",
    "SCAN             reads [0x148]+i",
    "publication rot  BEFORE  (0x148 -> 0x14C)",
    "publication rot  AFTER",
};

struct rot_ev {
    unsigned site;
    uint32_t idx;
    uint32_t p[8];      /* 0x140 0x144 0x148 0x14C 0x150 0x154 0x158 0x15C */
    uint32_t a140[4];   /* first four statuses in the +0x140 array */
    uint32_t a148[4];   /* ... and in the +0x148 array */
};
static struct rot_ev g_rot[ROT_LOG];
static unsigned g_rot_n;
static unsigned long long g_rot_hits[ROT_SITES];

void recomp_probe_rot(unsigned site, uint32_t dec, uint32_t idx)
{
    static const uint32_t off[8] = { 0x140u, 0x144u, 0x148u, 0x14Cu,
                                     0x150u, 0x154u, 0x158u, 0x15Cu };
    struct rot_ev *e;
    unsigned i;

    if (site < ROT_SITES) ++g_rot_hits[site];
    if (g_rot_n >= ROT_LOG || dec == 0u) return;
    e = &g_rot[g_rot_n++];
    e->site = site;
    e->idx = idx;
    for (i = 0; i < 8u; ++i) e->p[i] = MEM32(dec + off[i]);
    for (i = 0; i < 4u; ++i) {
        e->a140[i] = e->p[0] ? MEM32(e->p[0] + i * 4u) : 0xDEADDEADu;
        e->a148[i] = e->p[2] ? MEM32(e->p[2] + i * 4u) : 0xDEADDEADu;
    }
}

void recomp_dump_rot(void)
{
    unsigned k, i;
    if (g_rot_n == 0u) return;
    fprintf(stderr, "[ROT] site hit counts:" "\n");
    for (i = 0; i < ROT_SITES; ++i)
        fprintf(stderr, "[ROT]   %u %-42s n=%llu" "\n",
                i, g_rot_name[i], g_rot_hits[i]);
    fprintf(stderr, "[ROT] first %u events, pointers then array contents:" "\n",
            g_rot_n);
    for (k = 0; k < g_rot_n; ++k) {
        struct rot_ev *e = &g_rot[k];
        fprintf(stderr, "[ROT] %2u %-42s idx=%u" "\n", k, g_rot_name[e->site],
                e->idx / 4u);
        fprintf(stderr, "[ROT]      A 140=%08X 144=%08X 148=%08X 14C=%08X" "\n",
                e->p[0], e->p[1], e->p[2], e->p[3]);
        fprintf(stderr, "[ROT]      B 150=%08X 154=%08X 158=%08X 15C=%08X" "\n",
                e->p[4], e->p[5], e->p[6], e->p[7]);
        fprintf(stderr, "[ROT]      [140]:");
        for (i = 0; i < 4u; ++i) fprintf(stderr, " %08X", e->a140[i]);
        fprintf(stderr, "   [148]:");
        for (i = 0; i < 4u; ++i) fprintf(stderr, " %08X", e->a148[i]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* TEMPORARY: which caller of sub_004C8FB0 runs.
 *
 * sub_004C8FB0 walks the packet list at [voice+0xA8] -- the list
 * sub_004C9358 links every submitted packet node into -- and the retire
 * probe has shown it reaching zero calls in every run so far.  A console
 * keeps submitting indefinitely, so something there completes packets and
 * it is not sub_004C8E98, which is gated on a caps bit that is clear on
 * both systems.
 *
 * Sites 0..5 are the six call sites, by return address.  Sites 6..11 are
 * the entries of the same six functions, so a caller that runs but diverts
 * before the call is distinguishable from one that never runs at all.
 *
 * The list at +0xA8 is circular: head->next == head means empty. */
#define S8_N 12u
static const char *const g_s8_name[S8_N] = {
    "call  0x004C447B  in sub_004C4445",
    "call  0x004C45FA  in sub_004C45A9",
    "call  0x004C909F  in sub_004C9090",
    "call  0x004C91C1  in sub_004C9184",
    "call  0x004C922F  in sub_004C920C",
    "call  0x004C9280  in sub_004C9269",
    "entry sub_004C4445",
    "entry sub_004C45A9",
    "entry sub_004C9090",
    "entry sub_004C9184",
    "entry sub_004C920C",
    "entry sub_004C9269",
};
static unsigned long long g_s8_hits[S8_N];
static uint32_t g_s8_voice[S8_N], g_s8_head[S8_N];
static unsigned char g_s8_empty[S8_N];

void recomp_probe_s8(unsigned site, uint32_t voice)
{
    if (site >= S8_N) return;
    ++g_s8_hits[site];
    if (voice) {
        uint32_t head = MEM32(voice + 0xA8);
        g_s8_voice[site] = voice;
        g_s8_head[site] = head;
        g_s8_empty[site] = (head == voice + 0xA8u) ? 1u : 0u;
    }
}

void recomp_dump_s8(void)
{
    unsigned i, any = 0;
    for (i = 0; i < S8_N; ++i) if (g_s8_hits[i]) any = 1;
    fprintf(stderr, "[S8] sub_004C8FB0 callers%s" "\n",
            any ? ":" : ": none of the six ever runs");
    for (i = 0; i < S8_N; ++i) {
        if (i == 6u)
            fprintf(stderr, "[S8]   -- caller entries --" "\n");
        fprintf(stderr, "[S8]   %-34s n=%-8llu", g_s8_name[i], g_s8_hits[i]);
        if (g_s8_voice[i])
            fprintf(stderr, "  voice=%08X [+A8]=%08X %s",
                    g_s8_voice[i], g_s8_head[i],
                    g_s8_empty[i] ? "EMPTY" : "has packets");
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* Which event codes reach sub_004C94B4.
 *
 * It dispatches on its single argument, and code 3 is the only arm that
 * reaches sub_004C9090 -> sub_004C8FB0, the walker over the packet list at
 * [voice+0xA8].  None of the six callers of sub_004C8FB0 is entered here at
 * all, so the question is which codes do arrive and whether 3 ever does. */
static unsigned long long g_disp[16], g_disp_other;
void recomp_probe_disp(uint32_t code)
{
    if (code < 16u) ++g_disp[code]; else ++g_disp_other;
}
void recomp_dump_disp(void)
{
    unsigned i; unsigned long long tot = g_disp_other;
    for (i = 0; i < 16u; ++i) tot += g_disp[i];
    if (tot == 0ull) return;
    fprintf(stderr, "[DISP] sub_004C94B4 event codes (3 = the packet walker):" "\n");
    for (i = 0; i < 16u; ++i)
        if (g_disp[i])
            fprintf(stderr, "[DISP]   code %-2u n=%llu%s" "\n", i, g_disp[i],
                    i == 3u ? "   <- reaches sub_004C8FB0" : "");
    if (g_disp_other) fprintf(stderr, "[DISP]   other n=%llu" "\n", g_disp_other);
    if (g_disp[3] == 0ull)
        fprintf(stderr, "[DISP]   code 3 NEVER ARRIVES" "\n");
    fflush(stderr);
}

/* The routine packet-completion path, inside sub_004C90A0:
 *
 *   loc_004C90CC   sub_004C87B9([voice+0x68], i)   has buffer i finished?
 *   loc_004C90D7   if the answer is 0, nothing is completed
 *   loc_004C90DB   sub_004C8EED -> sub_004C8E22, which writes the status 0
 *
 * sub_004C8FB0 is the flush, not this; a console called it twice in a few
 * seconds, once from a destructor.  This is the per-buffer path, and it is
 * gated on a playback-position test rather than on any caps bit. */
#define DN_N 6u
static const char *const g_dn_name[DN_N] = {
    "0 loop head        voice / index",
    "1 sub_004C87B9 ret -- is the buffer done playing",
    "2 COMPLETION taken voice / index",
    "3 sub_004C8EED entry",
    "4 sub_004C8E22 entry  (writes the status 0)",
    "5 sub_004C87B9 entry",
};
static unsigned long long g_dn[DN_N];
static uint32_t g_dn_x[DN_N], g_dn_y[DN_N];
static unsigned long long g_dn_ret0, g_dn_retnz;
void recomp_probe_done(unsigned site, uint32_t x, uint32_t y)
{
    if (site >= DN_N) return;
    ++g_dn[site];
    g_dn_x[site] = x; g_dn_y[site] = y;
    if (site == 1u) { if (x) ++g_dn_retnz; else ++g_dn_ret0; }
}
void recomp_dump_done(void)
{
    { extern unsigned g_apu_fenaddr, g_apu_notify_last_addr,
                      g_apu_notify_last_voice;
      uint32_t V = g_dn_x[0];
      { extern unsigned g_vt_guest_handle;
        if (V) g_vt_guest_handle = (unsigned)MEM16(V + 0xC); }
      uint32_t arr = V ? MEM32(V + 0x68) : 0u;
      unsigned nv = V ? MEM8(V + 0x64) : 0u, i;
      fprintf(stderr, "[DONE] guest voice=%08X  [V+0x68]=%08X  handles(%u):",
              V, arr, nv);
      for (i = 0; i < nv && i < 8u; ++i)
          fprintf(stderr, " %u", (unsigned)MEM16(V + 0xC + i * 2u));
      fprintf(stderr, "\n");
      fprintf(stderr, "[DONE] apu FENADDR=%08X  last notify addr=%08X voice=%u",
              g_apu_fenaddr, g_apu_notify_last_addr, g_apu_notify_last_voice);
      if (g_apu_fenaddr && nv)
          fprintf(stderr, "  expected block for handle %u = %08X",
                  (unsigned)MEM16(V + 0xC),
                  g_apu_fenaddr + 16u * (2u + (unsigned)MEM16(V + 0xC) * 4u));
      fprintf(stderr, "\n"); }
    unsigned i;
    if (g_dn[5] == 0ull && g_dn[0] == 0ull) {
        fprintf(stderr, "[DONE] the completion loop never runs" "\n");
        return;
    }
    for (i = 0; i < DN_N; ++i)
        fprintf(stderr, "[DONE] %-42s n=%-8llu last %08X %08X" "\n",
                g_dn_name[i], g_dn[i], g_dn_x[i], g_dn_y[i]);
    fprintf(stderr, "[DONE] sub_004C87B9 said DONE %llu times, NOT DONE %llu" "\n",
            g_dn_retnz, g_dn_ret0);
    fflush(stderr);
}

/* What the guest actually sees when it polls.
 *
 * The APU notifies voice 96 fifty-five times in a minute, yet
 * sub_004C87B9 answered NOT DONE on all sixteen polls.  This records the
 * index polled and the byte read, so a wrong index and a genuinely unset
 * byte can be told apart. */
static unsigned long long g_poll_n, g_poll_byte[8];
static uint32_t g_poll_last_idx, g_poll_last_base, g_poll_last_byte;
static unsigned long long g_poll_idx_hist[8];
unsigned g_vt_guest_handle;    /* the APU handle the guest voice owns */
void recomp_probe_poll(uint32_t idx, uint32_t base, uint32_t byte)
{
    ++g_poll_n;
    g_poll_last_idx = idx; g_poll_last_base = base; g_poll_last_byte = byte;
    if (idx < 8u) ++g_poll_idx_hist[idx];
    if (byte == 0x80u) ++g_poll_byte[0];
    else if (byte == 0x01u) ++g_poll_byte[1];
    else if (byte == 0u) ++g_poll_byte[2];
    else ++g_poll_byte[3];
}
void recomp_dump_poll(void)
{
    unsigned i;
    if (g_poll_n == 0ull) return;
    fprintf(stderr, "[POLL] n=%llu  last idx=%u base=%08X byte=%02X" "\n",
            g_poll_n, g_poll_last_idx, g_poll_last_base, g_poll_last_byte);
    fprintf(stderr, "[POLL] byte seen: 0x80=%llu  0x01=%llu  0x00=%llu  other=%llu" "\n",
            g_poll_byte[0], g_poll_byte[1], g_poll_byte[2], g_poll_byte[3]);
    fprintf(stderr, "[POLL] index polled:");
    for (i = 0; i < 8u; ++i) if (g_poll_idx_hist[i])
        fprintf(stderr, " [%u]=%llu", i, g_poll_idx_hist[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Who re-arms the notifier byte to 0x80.
 *
 * The APU writes 0x01 to the same byte the guest polls -- verified by
 * address -- 56 times a minute, yet every one of the guest polls reads
 * 0x80.  Something puts it back.  These are the only two writers. */
static unsigned long long g_arm[2];
static uint32_t g_arm_last[2];
void recomp_probe_arm(unsigned which, uint32_t entry)
{
    if (which < 2u) { ++g_arm[which]; g_arm_last[which] = entry + 0xFu; }
}
void recomp_dump_arm(void)
{
    if (g_arm[0] == 0ull && g_arm[1] == 0ull) return;
    fprintf(stderr, "[ARM] sub_004C8B2D per-entry=%llu (last byte %08X)  "
            "sub_004C9CA8 bulk=%llu (last byte %08X)" "\n",
            g_arm[0], g_arm_last[0], g_arm[1], g_arm_last[1]);
    fflush(stderr);
}

/* TEMPORARY: one ordered log of everything that touches the notifier byte.
 *
 * Counts alone showed 46 re-arm sweeps against 55 notifies and 16 polls all
 * reading 0x80, which is consistent with a race but does not prove the
 * ordering.  This records the events in sequence, with the APU frame and
 * the IRQ assert count at each, so "re-armed before the guest could look"
 * can be read off directly rather than inferred from rates.
 *
 * Only events on the guest voice notifier block are kept, so the log stays
 * short enough to read. */
#define EV_MAX 64u

enum { EV_ARM_SWEEP = 0, EV_ARM_ENTRY, EV_NOTIFY, EV_POLL, EV_IRQ };
static const char *const g_ev_kind[5] = {
    "re-arm sweep ", "re-arm entry ", "APU NOTIFY   ", "guest POLL   ",
    "IRQ delivered" };

struct ev_rec {
    unsigned kind, site, handle, index;
    uint32_t addr, before, after;
    unsigned long long seq, frame, irq;
};
static struct ev_rec g_ev[EV_MAX];
static unsigned g_ev_n;
static unsigned long long g_ev_seq;
static unsigned long long g_ev_drop;
static unsigned long long g_ev_kindn[5];
static unsigned long long g_ev_notify_on_block, g_ev_poll_saw_one;
static uint32_t g_ev_notify_last;
static unsigned long long g_ev_first_notify, g_ev_last_notify,
                          g_ev_first_poll, g_ev_last_poll;

/* The notifier block of the voice we care about, published by the poll. */
uint32_t g_ev_block;
unsigned g_ev_sweep_site;

void recomp_ev(unsigned kind, unsigned site, unsigned handle,
               unsigned index, uint32_t addr, uint32_t before, uint32_t after)
{
    extern unsigned long long g_apu_irq_asserts;
    extern unsigned long long mcpx_apu_frame(void);
    struct ev_rec *e;

    /* Only this voice's block, and only once the poll has identified it.
     * The ring keeps the most recent events so the steady state is visible
     * rather than the init burst. */
    if (kind != 3u && g_ev_block == 0u) return;
    if (addr && g_ev_block &&
        (addr < g_ev_block || addr >= g_ev_block + 64u))
        return;
    ++g_ev_seq;
    if (kind < 5u) ++g_ev_kindn[kind];
    if (kind == 2u) {          /* a notify that landed on this block */
        ++g_ev_notify_on_block;
        g_ev_notify_last = addr;
    }
    if (kind == 3u && before == 0x01u) ++g_ev_poll_saw_one;
    { extern unsigned long long mcpx_apu_frame(void);
      unsigned long long f = mcpx_apu_frame();
      if (kind == 2u) { if (!g_ev_first_notify) g_ev_first_notify = f;
                        g_ev_last_notify = f; }
      if (kind == 3u) { if (!g_ev_first_poll) g_ev_first_poll = f;
                        g_ev_last_poll = f; } }
    if (g_ev_n < EV_MAX) {
        e = &g_ev[g_ev_n++];
    } else {
        ++g_ev_drop;
        memmove(&g_ev[0], &g_ev[1], sizeof(g_ev[0]) * (EV_MAX - 1u));
        e = &g_ev[EV_MAX - 1u];
    }
    e->kind = kind; e->site = site; e->handle = handle; e->index = index;
    e->addr = addr; e->before = before; e->after = after;
    e->seq = g_ev_seq;
    e->frame = mcpx_apu_frame();
    e->irq = g_apu_irq_asserts;
}

void recomp_dump_ev(void)
{
    unsigned i;
    if (g_ev_n == 0u) return;
    fprintf(stderr, "[EV] ON THIS BLOCK: sweeps=%llu entry-rearms=%llu notifies=%llu polls=%llu (polls that saw 0x01: %llu, last notify addr %08X)" "\n",
            g_ev_kindn[0], g_ev_kindn[1], g_ev_notify_on_block,
            g_ev_kindn[3], g_ev_poll_saw_one, g_ev_notify_last);
    fprintf(stderr, "[EV] frames: notify first=%llu last=%llu   poll first=%llu last=%llu" "\n",
            g_ev_first_notify, g_ev_last_notify, g_ev_first_poll, g_ev_last_poll);
    fprintf(stderr, "[EV] block=%08X  %u events (%llu more dropped)" "\n",
            g_ev_block, g_ev_n, g_ev_drop);
    fprintf(stderr, "[EV]  seq  frame   irq  event         site hnd idx  addr     before after" "\n");
    for (i = 0; i < g_ev_n; ++i) {
        struct ev_rec *e = &g_ev[i];
        fprintf(stderr, "[EV] %4llu %6llu %5llu  %s  %2u %3u %3u  %08X   %02X    %02X" "\n",
                e->seq, e->frame, e->irq, g_ev_kind[e->kind < 5u ? e->kind : 3u],
                e->site, e->handle, e->index, e->addr, e->before, e->after);
    }
    fflush(stderr);
}

/* Who drives sub_004C90A0, the function holding the completion poll.
 *
 * A console runs it 3226 times against 16 here, and everything below it is
 * verified working, so this is the last link.  One probe at the entry,
 * bucketed by the return address on the stack -- the same shape as the gdb
 * side, so the two are directly comparable.
 *
 *   0x004C91D3  in sub_004C9184   vtable-only, never entered here
 *   0x004C941E  in sub_004C9358   the packet enqueue
 *   0x004C9449  in sub_004C9426   dispatcher code 5
 */
static unsigned long long g_pc[4];
static uint32_t g_pc_voice[4], g_pc_other_ret;
void recomp_probe_pc(uint32_t ret, uint32_t voice)
{
    unsigned i = 3u;
    if (ret == 0x004C91D3u) i = 0u;
    else if (ret == 0x004C941Eu) i = 1u;
    else if (ret == 0x004C9449u) i = 2u;
    else g_pc_other_ret = ret;
    ++g_pc[i];
    g_pc_voice[i] = voice;
}
void recomp_dump_pc(void)
{
    if (g_pc[0] + g_pc[1] + g_pc[2] + g_pc[3] == 0ull) return;
    fprintf(stderr, "[PC] sub_004C90A0 callers  (hardware total 3226)" "\n");
    fprintf(stderr, "[PC]   0x004C91D3 sub_004C9184 = %-8llu voice=%08X" "\n",
            g_pc[0], g_pc_voice[0]);
    fprintf(stderr, "[PC]   0x004C941E sub_004C9358 = %-8llu voice=%08X" "\n",
            g_pc[1], g_pc_voice[1]);
    fprintf(stderr, "[PC]   0x004C9449 sub_004C9426 = %-8llu voice=%08X" "\n",
            g_pc[2], g_pc_voice[2]);
    if (g_pc[3])
        fprintf(stderr, "[PC]   other ret=%08X = %llu" "\n",
                g_pc_other_ret, g_pc[3]);
    fflush(stderr);
}

/* The DSOUND service chain, from DPC registration down to the poll.
 *
 * A console drives the poll from two callers roughly equally; the whole
 * 004C9184 half comes from a single site, 0x004C7CD3, inside sub_004C7CAC,
 * which walks [esi+0x488] and calls [vtable+0x18] on each member.  That
 * loop is reached from sub_004C8424 <- sub_004C84C0, and sub_004C84C0 is
 * not called anywhere: it is registered as a DPC deferred routine by
 * sub_004C86AC via KeInitializeDpc.  So the entire service half of the
 * poll rate is DPC-driven. */
static unsigned long long g_ch2[5];
static uint32_t g_ch2_this[5];
static const char *const g_ch2_name[5] = {
    "0 sub_004C86AC  KeInitializeDpc(&[esi+0x4C8], sub_004C84C0, esi)",
    "1 sub_004C84C0  THE DPC ROUTINE",
    "2 sub_004C8424",
    "3 sub_004C7CAC  the service loop over [esi+0x488]",
    "4 sub_004C9184  [voice vtable+0x18] -> sub_004C90A0 (the poll)",
};
void recomp_probe_chain2(unsigned i, uint32_t self)
{
    if (i < 5u) { ++g_ch2[i]; g_ch2_this[i] = self; }
}
void recomp_dump_chain2(void)
{
    unsigned i;
    for (i = 0; i < 5u; ++i)
        fprintf(stderr, "[CH2] %-62s n=%-8llu this=%08X" "\n",
                g_ch2_name[i], g_ch2[i], g_ch2_this[i]);
    fflush(stderr);
}

/* The first branch of the audio ISR.
 *
 * sub_004CE5AD reads the ACI global status at 0xFEC00130, masks with 0x51
 * -- bits 0, 4 and 6, the per-channel interrupt status -- and returns "not
 * mine" when the result is zero, without queueing its DPC.  The ISR is
 * entered 7115 times a run, so this single value decides everything below
 * it. */
static unsigned long long g_aci_n, g_aci_zero, g_aci_nz;
static uint32_t g_aci_last, g_aci_seen_or;
void recomp_probe_aci(uint32_t status, uint32_t masked)
{
    ++g_aci_n;
    g_aci_last = status;
    g_aci_seen_or |= status;
    if (masked) ++g_aci_nz; else ++g_aci_zero;
}
void recomp_dump_aci(void)
{
    if (g_aci_n == 0ull) return;
    fprintf(stderr, "[ACIS] ISR status reads=%llu  masked!=0: %llu  masked==0: %llu" "\n",
            g_aci_n, g_aci_nz, g_aci_zero);
    fprintf(stderr, "[ACIS] last 0xFEC00130 = %08X   union of all reads = %08X"
            "   (needs any of bits 0/4/6 = 0x51)" "\n",
            g_aci_last, g_aci_seen_or);
    fflush(stderr);
}

/* Every ACI register write below the global registers.
 *
 * A console has NABM channel 1 -- PCM-Out, the source of global-status
 * bit 6 -- programmed and running:
 *
 *     ch  BDBAR     CIV LVI  SR   PICB  PIV CR  running
 *      1  01f52000    0   0  0003 0000    1 1d   1
 *
 * while every channel here reads cr=00 bdbar=00000000.  So either the title
 * never issues those writes on the recomp, or the model drops them.  This
 * records which offsets are written at all. */
static unsigned long long g_aciw[0x130];
static uint32_t g_aciw_last[0x130];
static unsigned long long g_aciw_total;
void recomp_aci_wlog(uint32_t off, uint32_t val, uint32_t old)
{
    (void)old;
    if (off >= 0x130u) return;
    ++g_aciw_total;
    ++g_aciw[off];
    g_aciw_last[off] = val;
}
void recomp_dump_aciw(void)
{
    unsigned o, ch;
    if (g_aciw_total == 0ull) { fprintf(stderr, "[ACIW] no ACI writes below 0x130" "\n"); return; }
    fprintf(stderr, "[ACIW] %llu writes; offsets touched:" "\n", g_aciw_total);
    for (o = 0; o < 0x130u; ++o)
        if (g_aciw[o])
            fprintf(stderr, "[ACIW]   +%03X n=%-6llu last=%02X%s" "\n",
                    o, g_aciw[o], g_aciw_last[o],
                    (o >= 0x110u && o < 0x120u) ? "   <- PCM-Out channel 1" : "");
    for (ch = 0; ch < 3u; ++ch) {
        uint32_t b = 0x100u + ch * 0x10u;
        fprintf(stderr, "[ACIW]   ch%u BDBAR bytes n=%llu/%llu/%llu/%llu  CR n=%llu last=%02X" "\n",
                ch, g_aciw[b], g_aciw[b+1u], g_aciw[b+2u], g_aciw[b+3u],
                g_aciw[b+0x0Bu], g_aciw_last[b+0x0Bu]);
    }
    fflush(stderr);
}

/* Inside the audio ISR, after the global-status gate was fixed.
 *
 * GLOB_STA now carries bit 6 transiently and the ISR proceeds 192 times in
 * a run, but the DSOUND DPC is still not queued, so a later guard stops it:
 *
 *   loc_004CE64D  test byte [this+8], 1 ; jne skip
 *   loc_004CE655  test byte [ebp-4], 3  ; je  skip
 *
 * [ebp-4] starts at 0x80000000 and gains 1<<device for each device whose
 * channel SR shows BCIS, so bit 0 or 1 must be set to reach the insert. */
#define ISR2_N 6u
static const char *const g_isr2_name[ISR2_N] = {
    "0 past the GLOB_STA gate   status / this",
    "1 per-device iteration     edi / device",
    "2 BCIS seen for a device   chan SR / mask",
    "3 at the DPC guards        [this+8] / [ebp-4]",
    "4 past [ebp-4] & 3         [ebp-4]",
    "5 KeInsertQueueDpc CALLED  this",
};
static unsigned long long g_isr2[ISR2_N];
static uint32_t g_isr2_x[ISR2_N], g_isr2_y[ISR2_N];
void recomp_probe_isr2(unsigned i, uint32_t x, uint32_t y)
{
    if (i < ISR2_N) { ++g_isr2[i]; g_isr2_x[i] = x; g_isr2_y[i] = y; }
}
void recomp_dump_isr2(void)
{
    unsigned i;
    if (g_isr2[0] == 0ull) return;
    for (i = 0; i < ISR2_N; ++i)
        fprintf(stderr, "[ISR2] %-38s n=%-8llu %08X %08X" "\n",
                g_isr2_name[i], g_isr2[i], g_isr2_x[i], g_isr2_y[i]);
    fflush(stderr);
}

/* TEMPORARY: the inline audio service callback, sub_004CE556.
 *
 * The DPC guard was a false lead.  [this+8] has exactly one writer --
 * sub_004CE7C1 storing its dword argument -- and its only caller,
 * sub_004CD7E6, pushes the literal 1.  So bit 0 is always set, the title
 * never asks for DPC mode, KeInitializeDpc at loc_004CE80F is skipped and
 * the KeInsertQueueDpc at loc_004CE65B is unreachable by design.  The ISR
 * services the stream inline instead, and it already does that 103 times
 * a run.
 *
 * So the question is what sub_004CE556 does once it is in.  It can leave
 * three ways:
 *   [stream+0x24] == 0   nothing queued -- returns before doing anything
 *   [stream+0x10] == 0   no completion callback registered
 *   otherwise            calls (*[stream+0x10])([stream+0x14])
 * That last call is the one that should reach DSOUND and sub_004C7CAC.
 *
 * sub_004CE466(mode, callback, context) is what fills +0x0C/+0x10/+0x14,
 * and sub_004CE156 / sub_004CE2BB are what raise and lower the +0x24
 * queued count, so whichever of the three it is names the next step. */
#define CB_N 4u
static const char *const g_cb_name[CB_N] = {
    "0 entered sub_004CE556      ",
    "1 past queued-count == 0    ",
    "2 reached the callback test ",
    "3 CALLBACK INVOKED          ",
};
static unsigned long long g_cb_hits[CB_N];
static uint32_t g_cb_stream[CB_N];

/* Last-seen stream state, and the union of every queued count observed, so
 * a count that is briefly non-zero is not hidden by the final sample. */
static uint32_t g_cb_cnt_max, g_cb_cnt_last, g_cb_idx, g_cb_bd;
static uint32_t g_cb_fn, g_cb_ctx, g_cb_mode, g_cb_dev;
static unsigned long long g_cb_cnt_zero, g_cb_fn_zero;

unsigned long long recomp_mode_isr_counts(unsigned which)
{
    switch (which) {
    case 0: return g_isr2[0];
    case 1: return (unsigned long long)g_isr2_x[3];
    case 2: return g_isr2[5];
    }
    return 0ull;
}

void recomp_probe_cb(unsigned site, uint32_t stream)
{
    uint32_t cnt, fn;
    if (site >= CB_N) return;
    ++g_cb_hits[site];
    if (!stream) return;
    g_cb_stream[site] = stream;
    cnt = MEM8(stream + 0x24);
    fn  = MEM32(stream + 0x10);
    g_cb_cnt_last = cnt;
    if (cnt > g_cb_cnt_max) g_cb_cnt_max = cnt;
    if (site == 0u && cnt == 0u) ++g_cb_cnt_zero;
    if (site == 2u && fn == 0u)  ++g_cb_fn_zero;
    g_cb_idx  = MEM8(stream + 0x25);
    g_cb_bd   = MEM32(stream + 0x18);
    g_cb_fn   = fn;
    { extern uint32_t g_reg_ever_nonzero; g_reg_ever_nonzero |= fn; }
    g_cb_ctx  = MEM32(stream + 0x14);
    g_cb_mode = MEM32(stream + 0x0C);
    g_cb_dev  = MEM32(stream + 0x08);
}

void recomp_dump_cb(void)
{
    unsigned i;
    if (g_cb_hits[0] == 0ull) {
        fprintf(stderr, "[CB] sub_004CE556 never entered" "\n");
        return;
    }
    for (i = 0; i < CB_N; ++i)
        fprintf(stderr, "[CB] %s n=%-8llu stream=%08X" "\n",
                g_cb_name[i], g_cb_hits[i], g_cb_stream[i]);
    fprintf(stderr, "[CB] queued count: last=%u max-ever=%u   entered with 0 queued: %llu" "\n",
            g_cb_cnt_last, g_cb_cnt_max, g_cb_cnt_zero);
    fprintf(stderr, "[CB] stream: +0C mode=%08X +08 dev=%08X +18 bd=%08X +25 idx=%u" "\n",
            g_cb_mode, g_cb_dev, g_cb_bd, g_cb_idx);
    fprintf(stderr, "[CB] callback: +10 fn=%08X +14 ctx=%08X   reached test with fn==0: %llu" "\n",
            g_cb_fn, g_cb_ctx, g_cb_fn_zero);
    fflush(stderr);
}

/* TEMPORARY: mode selection on the audio device, in the same shape as the
 * xmvmodeshow gdb command, so the two sides can be read side by side.
 *
 * [this+8] bit 0 chooses inline servicing over the DPC.  Statically it has
 * exactly one writer -- sub_004CE7C1 storing its dword argument -- and one
 * caller, sub_004CD7E6, which pushes a literal 1.  This records the value
 * actually stored, whether KeInitializeDpc was reached (it is skipped when
 * bit 0 is set, so the DPC is never even built), and which arm the ISR
 * takes, so the claim is measured here rather than only read off the
 * disassembly. */
static unsigned long long g_mode_ctor, g_mode_dpcinit, g_mode_inline;
static uint32_t g_mode_this, g_mode_arg = 0xFFFFFFFFu;

void recomp_probe_amode(unsigned site, uint32_t a, uint32_t b)
{
    switch (site) {
    case 0: ++g_mode_ctor; g_mode_this = a; g_mode_arg = b; break;
    case 1: ++g_mode_dpcinit; break;
    case 2: ++g_mode_inline;  break;
    default: break;
    }
}

void recomp_dump_amode(void)
{
    extern unsigned long long recomp_mode_isr_counts(unsigned);
    if (g_mode_ctor == 0ull) {
        fprintf(stderr, "[MODE] sub_004CE7C1 never ran" "\n");
        return;
    }
    fprintf(stderr, "[MODE] sub_004CE7C1 (the only writer of [this+8])" "\n");
    fprintf(stderr, "[MODE]   calls=%llu  this=%08X  argument=%u" "\n",
            g_mode_ctor, g_mode_this, g_mode_arg);
    fprintf(stderr, "[MODE]   KeInitializeDpc reached=%llu   (0 = bit 0 set, DPC never built)" "\n", g_mode_dpcinit);
    fprintf(stderr, "[MODE] sub_004CE5AD (the ISR)" "\n");
    fprintf(stderr, "[MODE]   entries past the GLOB_STA gate=%llu" "\n",
            recomp_mode_isr_counts(0));
    fprintf(stderr, "[MODE]   [this+8] seen at the guards=%08X" "\n",
            (uint32_t)recomp_mode_isr_counts(1));
    fprintf(stderr, "[MODE]   inline arm taken=%llu   KeInsertQueueDpc taken=%llu" "\n",
            g_mode_inline, recomp_mode_isr_counts(2));
    fflush(stderr);
}

/* TEMPORARY: stream-open registration, sub_004CE466(mode, callback, ctx).
 *
 * sub_004CE556 reaches its completion-callback test 40 times a run and
 * finds [stream+0x10] null every time, and one hardware sample says the
 * same.  sub_004CE466 is what fills +0x0C/+0x10/+0x14/+0x18, and it has
 * exactly two call sites, both in sub_004CD7E6, both pushing three zeroes.
 * This confirms that at runtime and keeps object identity, so the stream
 * registered can be matched against the stream serviced.
 *
 * ever_nonzero is the answer to "set correctly and cleared later": it is
 * the union of every +0x10 this ever saw, at entry, at exit, and at each
 * service call.  If it stays 0 the pointer was never installed at all, and
 * there is no clearing writer to go looking for. */
#define REG_MAX 8u

struct reg_rec {
    uint32_t caller, stream, mode, cb, ctx;
    uint32_t before[4], after[4];
    unsigned long long hits;
};
static struct reg_rec g_reg[REG_MAX];
static unsigned g_reg_n;
static unsigned long long g_reg_calls;
uint32_t g_reg_ever_nonzero;      /* union of every [stream+0x10] seen */
static uint32_t g_reg_pending;    /* the stream inside the current call  */

static struct reg_rec *reg_find(uint32_t stream, uint32_t caller)
{
    unsigned i;
    for (i = 0; i < g_reg_n; ++i)
        if (g_reg[i].stream == stream && g_reg[i].caller == caller)
            return &g_reg[i];
    if (g_reg_n >= REG_MAX)
        return 0;
    g_reg[g_reg_n].stream = stream;
    g_reg[g_reg_n].caller = caller;
    return &g_reg[g_reg_n++];
}

void recomp_probe_reg_enter(uint32_t stream, uint32_t caller, uint32_t mode,
                            uint32_t cb, uint32_t ctx)
{
    struct reg_rec *r = reg_find(stream, caller);
    ++g_reg_calls;
    g_reg_pending = stream;
    if (!r) return;
    ++r->hits;
    r->mode = mode; r->cb = cb; r->ctx = ctx;
    r->before[0] = MEM32(stream + 0x0C);
    r->before[1] = MEM32(stream + 0x10);
    r->before[2] = MEM32(stream + 0x14);
    r->before[3] = MEM32(stream + 0x18);
    g_reg_ever_nonzero |= r->before[1];
}

void recomp_probe_reg_exit(uint32_t stream)
{
    unsigned i;
    if (!stream) stream = g_reg_pending;
    for (i = 0; i < g_reg_n; ++i) {
        if (g_reg[i].stream != stream) continue;
        g_reg[i].after[0] = MEM32(stream + 0x0C);
        g_reg[i].after[1] = MEM32(stream + 0x10);
        g_reg[i].after[2] = MEM32(stream + 0x14);
        g_reg[i].after[3] = MEM32(stream + 0x18);
        g_reg_ever_nonzero |= g_reg[i].after[1];
    }
}

void recomp_dump_reg(void)
{
    unsigned i;
    if (g_reg_calls == 0ull) {
        fprintf(stderr, "[REG] sub_004CE466 never called" "\n");
        return;
    }
    fprintf(stderr, "[REG] sub_004CE466 called %llu times, %u distinct (stream,caller)" "\n", g_reg_calls, g_reg_n);
    for (i = 0; i < g_reg_n; ++i) {
        struct reg_rec *r = &g_reg[i];
        fprintf(stderr, "[REG]  caller=%08X stream=%08X n=%llu  args: mode=%08X cb=%08X ctx=%08X" "\n",
                r->caller, r->stream, r->hits, r->mode, r->cb, r->ctx);
        fprintf(stderr, "[REG]    before  +0C=%08X +10=%08X +14=%08X +18=%08X" "\n",
                r->before[0], r->before[1], r->before[2], r->before[3]);
        fprintf(stderr, "[REG]    after   +0C=%08X +10=%08X +14=%08X +18=%08X" "\n",
                r->after[0], r->after[1], r->after[2], r->after[3]);
    }
    fprintf(stderr, "[REG] union of every [stream+0x10] ever seen = %08X  %s" "\n",
            g_reg_ever_nonzero,
            g_reg_ever_nonzero ? "-- it WAS set at some point; look for the clearing writer"
                               : "-- never non-zero, so nothing cleared it");
    fflush(stderr);
}

/* TEMPORARY: the DSOUND service pass and the loop that drives it.
 *
 * sub_004C7CAC has exactly one call site in the title -- return address
 * 0x004C844C, inside sub_004C8424 -- so the caller bucket is known before
 * the run; what matters is how often it fires and what gates it.
 *
 * sub_004C8424 is a worker loop, not a callback:
 *
 *   loc_004C843C  test [this+0x4C0],0x40 ; je skip
 *                 call sub_004C7CAC          <- the service pass
 *                 [this+0x4C0] = 0
 *   loc_004C8453  KeSynchronizeExecution(0x4E1D48, 0x004C7D25, this)
 *   loc_004C8458  test [this+0x4C0],1 ; jne loop
 *
 * so both the loop continuing (bit 0) and the service pass happening
 * (bit 6) depend on sub_004C7D25 ORing flags into [this+0x4C0].  That
 * routine takes the pending word at [this+0x4B8] with a cmpxchg.
 *
 * Inside the pass, three circular lists at +0x488/+0x490/+0x498 are walked;
 * each entry is a member at (entry - 0x4C) and the call is [vtable+0x18].
 */
#define SV_BUCKETS 4u

static unsigned long long g_sv_enter, g_sv_iter, g_sv_ret;
static unsigned long long g_sv_9184;      /* [vtable+0x18] == sub_004C9184 */
static uint32_t g_sv_caller[SV_BUCKETS];
static unsigned long long g_sv_caller_n[SV_BUCKETS];
static uint32_t g_sv_this, g_sv_head, g_sv_member, g_sv_vtable, g_sv_target;
static unsigned g_sv_empty[3];            /* per-list: seen empty */

/* the interrupt above the loop, and the two functions below the pass */
static unsigned long long g_isr_enter, g_isr_pending, g_isr_queue;
static unsigned long long g_isr_dpc, g_svc_9184, g_svc_90A0;
static uint32_t g_isr_ists, g_isr_this;
static unsigned long long g_sync_icall;
static uint32_t g_sync_target;

/* the loop above it */
static unsigned long long g_lp_enter, g_lp_top, g_lp_call, g_lp_after;
static unsigned long long g_lp_sync, g_lp_syncrt;
static uint32_t g_lp_4C0_top, g_lp_4C0_after, g_lp_4C0_ever, g_lp_4B8;

void recomp_probe_sv(unsigned site, uint32_t a, uint32_t b)
{
    unsigned i;
    switch (site) {
    case 0:                       /* sub_004C7CAC entry: a=this b=caller */
        ++g_sv_enter; g_sv_this = a;
        g_sv_head = MEM32(a + 0x488);
        for (i = 0; i < SV_BUCKETS; ++i) {
            if (g_sv_caller[i] == b || g_sv_caller[i] == 0u) {
                g_sv_caller[i] = b; ++g_sv_caller_n[i]; break;
            }
        }
        break;
    case 1: {                     /* about to call: a=list entry b=list# */
        uint32_t member = a - 0x4Cu;
        uint32_t vt = MEM32(member);
        ++g_sv_iter;
        g_sv_member = member; g_sv_vtable = vt;
        g_sv_target = vt ? MEM32(vt + 0x18) : 0u;
        if (g_sv_target == 0x004C9184u) ++g_sv_9184;
        break;
    }
    case 2: ++g_sv_ret; break;    /* returned to 0x004C7CD3 */
    case 3: if (a < 3u) g_sv_empty[a] = 1u; break;
    case 4: ++g_lp_enter; break;  /* sub_004C8424 entry */
    case 5: ++g_lp_top;   g_lp_4C0_top = a;   g_lp_4C0_ever |= a; break;
    case 6: ++g_lp_call;  break;
    case 7: ++g_lp_after; g_lp_4C0_after = a; g_lp_4C0_ever |= a;
            g_lp_4B8 = b; break;
    case 8: ++g_lp_sync;   break;
    case 9: ++g_lp_syncrt; break;
    case 10: ++g_isr_enter; break;
    case 11: ++g_isr_pending; g_isr_ists |= a; g_isr_this = b; break;
    case 12: ++g_isr_queue; break;
    case 13: ++g_isr_dpc;   break;
    case 14: ++g_svc_9184;  break;
    case 15: ++g_svc_90A0;  break;
    case 16: ++g_sync_icall; g_sync_target = a; break;
    default: break;
    }
}

void recomp_dump_sv(void)
{
    unsigned i;
    fprintf(stderr, "[SV] sub_004C8604 DSOUND ISR (vector 53): entered=%llu  past ISTS&1=%llu" "\n",
            g_isr_enter, g_isr_pending);
    fprintf(stderr, "[SV]   union of APU ISTS seen=%08X  this=%08X  KeInsertQueueDpc reached=%llu" "\n",
            g_isr_ists, g_isr_this, g_isr_queue);
    fprintf(stderr, "[SV]   KeSynchronizeExecution icall n=%llu  target=[0x56120C]=%08X" "\n", g_sync_icall, g_sync_target);
    fprintf(stderr, "[SV] sub_004C84C0 DPC: entered=%llu" "\n", g_isr_dpc);
    fprintf(stderr, "[SV] sub_004C8424 loop: entered=%llu  top=%llu  service-call=%llu  after-sync=%llu" "\n",
            g_lp_enter, g_lp_top, g_lp_call, g_lp_after);
    fprintf(stderr, "[SV]   KeSynchronizeExecution calls=%llu   sub_004C7D25 entries=%llu" "\n", g_lp_sync, g_lp_syncrt);
    fprintf(stderr, "[SV]   [this+0x4C0] at top=%08X after sync=%08X  union ever=%08X   [this+0x4B8]=%08X" "\n",
            g_lp_4C0_top, g_lp_4C0_after, g_lp_4C0_ever, g_lp_4B8);
    fprintf(stderr, "[SV] sub_004C7CAC entered=%llu  this=%08X  [this+0x488]=%08X%s" "\n",
            g_sv_enter, g_sv_this, g_sv_head,
            (g_sv_head && g_sv_head == g_sv_this + 0x488u)
                ? "  (EMPTY: head points at itself)" : "");
    for (i = 0; i < SV_BUCKETS; ++i)
        if (g_sv_caller[i])
            fprintf(stderr, "[SV]   caller %08X  n=%llu" "\n",
                    g_sv_caller[i], g_sv_caller_n[i]);
    fprintf(stderr, "[SV]   iterations reaching 0x004C7CD3 = %llu  (dispatched=%llu)" "\n", g_sv_ret, g_sv_iter);
    fprintf(stderr, "[SV]   via [vtable+0x18] -> sub_004C9184 = %llu" "\n",
            g_sv_9184);
    fprintf(stderr, "[SV] sub_004C9184 entered=%llu   sub_004C90A0 entered=%llu" "\n", g_svc_9184, g_svc_90A0);
    fprintf(stderr, "[SV]   last member=%08X vtable=%08X [vtable+0x18]=%08X" "\n",
            g_sv_member, g_sv_vtable, g_sv_target);
    fflush(stderr);
}

/* TEMPORARY: why [decoder+0x60] is never repopulated.
 *
 * The swap works -- loc_004E29F9 moves +0x60 into +0x64 and zeroes +0x60 --
 * so this follows only the path that puts a new buffer back, which is one
 * chain of gates at the top of sub_004E2801:
 *
 *   entry         cmp [esi+6C],0 ; je  out     gate 1  nothing pending
 *   loc_004E2838  cmp [esi+60],0 ; jne out     gate 2  already full
 *   loc_004E2841  cmp [esi+50],0 ; jne read    gate 3  -> the read
 *   loc_004E2846  cmp [esi+68],0 ; je  out     gate 4
 *   loc_004E284F  cmp [esi+74],0 ; je  out     gate 5
 *   loc_004E2858  MEM32(eax) = 2 ; EOF
 *   loc_004E2863  icall [esi+1C](ctx, &buf, &len)     the read callback
 *   loc_004E2871  cmp eax,0 ; jl  fail
 *   loc_004E2879  cmp [ebp-4],0 ; je out       gate 6  no buffer returned
 *   loc_004E2884  [esi+60] = buf, [esi+6C] = 0        THE WRITE
 *
 * Note the write also clears +0x6C, which is gate 1 on the next call -- so
 * whatever sets +0x6C again is part of the answer.
 *
 * Two full snapshots are kept: the last call that reached the write, and
 * the first later call that passed gate 2 (so +0x60 really was empty) and
 * still did not reach it.  Comparing those two is the whole question. */
#define RFL_SITES 11u

struct rfl_snap {
    unsigned long long call;
    uint32_t dec;
    uint32_t f50, f58, f5C, f60, f64, f68, f6C, f74, f78, fF8;
    uint32_t p[8];              /* +0x140 .. +0x15C */
    uint32_t a[4][4];           /* the four status arrays they point at */
    uint32_t cb_fn, cb_ctx, cb_ret, cb_buf, cb_len;
    unsigned last_site;
};

static const char *const g_rfl_name[RFL_SITES] = {
    "0  sub_004E2801 entry            ",
    "1  gate1 [+6C] pending           ",
    "2  gate2 [+60] empty             ",
    "3  gate3 [+50] -> do the read    ",
    "4  gate4 [+68]                   ",
    "5  gate5 [+74]                   ",
    "6  EOF: status 2                 ",
    "7  read callback INVOKED         ",
    "8  read callback returned        ",
    "9  gate6 buffer returned non-null",
    "10 THE WRITE [+60] = buffer      ",
};
static unsigned long long g_rfl_hits[RFL_SITES];
static uint32_t g_rfl_last[RFL_SITES];

static struct rfl_snap g_rfl_ok, g_rfl_bad, g_rfl_cur;
static int g_rfl_have_ok, g_rfl_have_bad;
static unsigned long long g_rfl_calls;
static int g_rfl_cur_gate2, g_rfl_cur_wrote;

static void rfl_capture(struct rfl_snap *s, uint32_t dec)
{
    static const uint32_t off[8] = { 0x140u, 0x144u, 0x148u, 0x14Cu,
                                     0x150u, 0x154u, 0x158u, 0x15Cu };
    unsigned i, j;
    if (!dec) return;
    s->call = g_rfl_calls; s->dec = dec;
    s->f50 = MEM32(dec + 0x50); s->f58 = MEM32(dec + 0x58);
    s->f5C = MEM32(dec + 0x5C); s->f60 = MEM32(dec + 0x60);
    s->f64 = MEM32(dec + 0x64); s->f68 = MEM32(dec + 0x68);
    s->f6C = MEM32(dec + 0x6C); s->f74 = MEM32(dec + 0x74);
    s->f78 = MEM8(dec + 0x78);  s->fF8 = MEM32(dec + 0xF8);
    for (i = 0; i < 8u; ++i) s->p[i] = MEM32(dec + off[i]);
    for (i = 0; i < 4u; ++i)
        for (j = 0; j < 4u; ++j)
            s->a[i][j] = s->p[i] ? MEM32(s->p[i] + j * 4u) : 0xDEADDEADu;
}

unsigned long long recomp_rfl_call(void) { return g_rfl_calls; }

void recomp_probe_rfl(unsigned site, uint32_t dec, uint32_t a, uint32_t b)
{
    if (site >= RFL_SITES) return;
    ++g_rfl_hits[site];
    g_rfl_last[site] = a;

    if (site == 0u) {
        /* Judge the call that just finished, then start a new one. */
        if (g_rfl_calls && g_rfl_have_ok && !g_rfl_have_bad &&
            g_rfl_cur_gate2 && !g_rfl_cur_wrote) {
            g_rfl_bad = g_rfl_cur;
            g_rfl_bad.last_site = 0u;
            g_rfl_have_bad = 1;
        }
        ++g_rfl_calls;
        g_rfl_cur_gate2 = 0; g_rfl_cur_wrote = 0;
        memset(&g_rfl_cur, 0, sizeof(g_rfl_cur));
        rfl_capture(&g_rfl_cur, dec);
        return;
    }
    g_rfl_cur.last_site = site;
    if (site == 2u && a == 0u) g_rfl_cur_gate2 = 1;
    if (site == 7u) { g_rfl_cur.cb_fn = a; g_rfl_cur.cb_ctx = b; }
    if (site == 8u) { g_rfl_cur.cb_ret = a; g_rfl_cur.cb_buf = b; }
    if (site == 9u) { g_rfl_cur.cb_len = b; }
    if (site == 10u) {
        g_rfl_cur_wrote = 1;
        g_rfl_ok = g_rfl_cur;
        g_rfl_ok.cb_buf = a;
        g_rfl_ok.last_site = 10u;
        g_rfl_have_ok = 1;
    }
}

static void rfl_print(const char *tag, const struct rfl_snap *s)
{
    unsigned i;
    fprintf(stderr, "[RFL] %s  update call %llu   decoder=%08X   deepest site reached=%u" "\n", tag, s->call, s->dec, s->last_site);
    fprintf(stderr, "[RFL]    +50=%08X +58=%08X +5C=%08X  +60=%08X +64=%08X" "\n", s->f50, s->f58, s->f5C, s->f60, s->f64);
    fprintf(stderr, "[RFL]    +68=%08X +6C=%08X +74=%08X +78=%02X +F8=%08X" "\n", s->f68, s->f6C, s->f74, s->f78, s->fF8);
    fprintf(stderr, "[RFL]    ptrs 140=%08X 144=%08X 148=%08X 14C=%08X" "\n",
            s->p[0], s->p[1], s->p[2], s->p[3]);
    fprintf(stderr, "[RFL]         150=%08X 154=%08X 158=%08X 15C=%08X" "\n",
            s->p[4], s->p[5], s->p[6], s->p[7]);
    for (i = 0; i < 4u; ++i)
        fprintf(stderr, "[RFL]    status[%u] @%08X: %08X %08X %08X %08X" "\n",
                i, s->p[i], s->a[i][0], s->a[i][1], s->a[i][2], s->a[i][3]);
    fprintf(stderr, "[RFL]    read cb fn=%08X ctx=%08X -> ret=%08X buf=%08X len=%08X" "\n",
            s->cb_fn, s->cb_ctx, s->cb_ret, s->cb_buf, s->cb_len);
}

void recomp_dump_rfl(void)
{
    unsigned i;
    if (g_rfl_calls == 0ull) return;
    fprintf(stderr, "[RFL] sub_004E2801 calls=%llu   refill path site counts:"
            "\n", g_rfl_calls);
    for (i = 0; i < RFL_SITES; ++i)
        fprintf(stderr, "[RFL]   %s n=%-8llu last=%08X" "\n",
                g_rfl_name[i], g_rfl_hits[i], g_rfl_last[i]);
    if (g_rfl_have_ok)
        rfl_print("LAST SUCCESSFUL WRITE ", &g_rfl_ok);
    else
        fprintf(stderr, "[RFL] no successful write to +0x60 was seen" "\n");
    if (g_rfl_have_bad)
        rfl_print("FIRST MISSED CHANCE   ", &g_rfl_bad);
    else
        fprintf(stderr, "[RFL] no later call passed gate 2 without writing" "\n");
    fflush(stderr);
}

/* TEMPORARY: provenance of [decoder+0x74], the publication gate.
 *
 * +0x74 has three writers, all inside sub_004E2801, and the block layout
 * decides which one the gate at loc_004E2DA7 ends up reading:
 *
 *   loc_004E2D2D   [+0x74] = 0      writer 1, on the [+0x68]==0 arm
 *   loc_004E2D6A   cmp [+0x68],0 ; je out
 *   loc_004E2D73   cmp [+0x74],0 ; jne loc_004E2DB0   already flagged
 *   loc_004E2D78   call sub_004C5248
 *   loc_004E2D7D   [+0x74] = 1      writer 2, unconditional
 *                  cmp [+0x48],0 ; jbe loc_004E2DA7   no packets to scan
 *   loc_004E2D8B   ecx = [+0x148]                     the status array
 *   loc_004E2D91   cmp [ecx],0x8000000A ; je loc_004E2DA4   <- E_PENDING
 *   loc_004E2DA2   scanned clean -> gate sees 1
 *   loc_004E2DA4   [+0x74] = 0      writer 3
 *   loc_004E2DA7   cmp [+0x74],0 ; je out             THE GATE
 *
 * So writer 2 always sets it and the only thing that takes it away again
 * is the scan finding a packet still at E_PENDING.  This records which
 * writer spoke last, the blocks visited, and the statuses the scan read.
 */
#define P74_SITES 11u
#define P74_SCAN  8u

static const char *const g_p74_name[P74_SITES] = {
    " 0 loc_004E2D2D  [+68]==0 arm      ",
    " 1 WRITER 1  [+74] = 0             ",
    " 2 loc_004E2D6A  [+68] recheck     ",
    " 3 loc_004E2D73  [+74] already set?",
    " 4 loc_004E2D78  call sub_004C5248 ",
    " 5 WRITER 2  [+74] = 1             ",
    " 6 loc_004E2D8B  scan setup        ",
    " 7 loc_004E2D91  scan iteration    ",
    " 8 loc_004E2DA2  scan found none   ",
    " 9 WRITER 3  [+74] = 0  (E_PENDING)",
    "10 loc_004E2DA7  THE GATE          ",
};
static unsigned long long g_p74_hits[P74_SITES];

struct p74_rec {
    unsigned long long call;
    unsigned writer;            /* 1, 2, 3 -- or 0 for none this call */
    unsigned blocks;            /* bitmask of sites visited */
    uint32_t f74, f68, f6C, f48, arr;
    uint32_t scan[P74_SCAN];
    int match;                  /* scan index that was E_PENDING, else -1 */
    unsigned scanned;
};
static struct p74_rec g_p74_cur, g_p74_ok, g_p74_bad;
static int g_p74_have_ok, g_p74_have_bad;
static unsigned long long g_p74_gate_pass, g_p74_gate_fail;

void recomp_probe_p74(unsigned site, uint32_t dec, uint32_t a, uint32_t b)
{
    extern unsigned long long recomp_rfl_call(void);
    if (site >= P74_SITES) return;
    ++g_p74_hits[site];

    /* A new pass through this region starts at whichever of 2D2D or 2D73
     * is reached first; both are entered once per update call. */
    if (site == 0u || (site == 3u && (g_p74_cur.blocks & 1u) == 0u)) {
        memset(&g_p74_cur, 0, sizeof(g_p74_cur));
        g_p74_cur.match = -1;
        g_p74_cur.call = recomp_rfl_call();
    }
    g_p74_cur.blocks |= 1u << site;

    if (site == 1u) g_p74_cur.writer = 1u;
    if (site == 5u) g_p74_cur.writer = 2u;
    if (site == 9u) { g_p74_cur.writer = 3u; g_p74_cur.match = (int)a; }
    if (site == 7u) {
        if (g_p74_cur.scanned < P74_SCAN)
            g_p74_cur.scan[g_p74_cur.scanned] = b;
        ++g_p74_cur.scanned;
    }
    if (site == 10u && dec) {
        g_p74_cur.f74 = MEM32(dec + 0x74);
        g_p74_cur.f68 = MEM32(dec + 0x68);
        g_p74_cur.f6C = MEM32(dec + 0x6C);
        g_p74_cur.f48 = MEM32(dec + 0x48);
        g_p74_cur.arr = MEM32(dec + 0x148);
        if (g_p74_cur.f74 != 0u) {
            ++g_p74_gate_pass;
            g_p74_ok = g_p74_cur; g_p74_have_ok = 1;
            /* deliberately not clearing have_bad: the interesting record is
             * the first failure after the first success, and the region stops
             * being entered at all once [+0x68] sticks at 0, so a later reset
             * would leave nothing to compare. */
        } else {
            ++g_p74_gate_fail;
            if (g_p74_have_ok && !g_p74_have_bad) {
                g_p74_bad = g_p74_cur; g_p74_have_bad = 1;
            }
        }
    }
}

static void p74_print(const char *tag, const struct p74_rec *r)
{
    unsigned i;
    fprintf(stderr, "[P74] %s update call %llu   last writer = %u   [+74]=%08X" "\n", tag, r->call, r->writer, r->f74);
    fprintf(stderr, "[P74]    [+68]=%08X [+6C]=%08X   packet count [+48]=%u   array [+148]=%08X" "\n", r->f68, r->f6C, r->f48, r->arr);
    fprintf(stderr, "[P74]    blocks visited:");
    for (i = 0; i < P74_SITES; ++i)
        if (r->blocks & (1u << i)) fprintf(stderr, " %u", i);
    fprintf(stderr, "\n");
    fprintf(stderr, "[P74]    scan: %u entries read", r->scanned);
    for (i = 0; i < r->scanned && i < P74_SCAN; ++i)
        fprintf(stderr, " %08X", r->scan[i]);
    if (r->match >= 0)
        fprintf(stderr, "   <- index %d was E_PENDING", r->match);
    fprintf(stderr, "\n");
}

void recomp_dump_p74(void)
{
    unsigned i;
    if (g_p74_hits[10] == 0ull) return;
    fprintf(stderr, "[P74] gate at loc_004E2DA7: passed=%llu  failed=%llu" "\n",
            g_p74_gate_pass, g_p74_gate_fail);
    for (i = 0; i < P74_SITES; ++i)
        fprintf(stderr, "[P74]   %s n=%llu" "\n", g_p74_name[i], g_p74_hits[i]);
    if (g_p74_have_ok)  p74_print("PUBLISHED         ", &g_p74_ok);
    if (g_p74_have_bad) p74_print("FIRST LATER FAILURE", &g_p74_bad);
    fflush(stderr);
}

/* TEMPORARY: the four status slots in the array at [decoder+0x148].
 *
 * The publication gate is cleared whenever the scan at loc_004E2D91 finds
 * 0x8000000A, and it matches on slot 0 in 119 of 125 passes.  This follows
 * the slots themselves: when each is submitted, when it retires, how long
 * it stays pending, and how many publication scans go past while it is.
 *
 * Submissions are hooked directly at sub_00559420/loc_00559435, which is
 * the generic "MEM32([ecx+0xC]) = E_PENDING" helper, so the owning object
 * comes with them.  Retirements have no single writer to hook -- the value
 * is cleared through the completion path -- so both edges are also picked
 * up by diffing the four slots at every observation point.  A transition
 * seen by the diff and not by the hook still gets counted, it just has no
 * owner attached.
 */
#define SLOT_N 4u
#define SLOT_PENDING 0x8000000Au

struct slot_st {
    unsigned long long subs, rets;
    unsigned long long sub_call, sub_frame;   /* when it went pending    */
    unsigned long long life_calls, life_frames, max_call_life;
    unsigned long long scans_while_pending;
    uint32_t owner, owner_voice, last_val;
    int pending;
};
static struct slot_st g_slot[SLOT_N];
static uint32_t g_slot_arr;              /* the array currently scanned */
static unsigned long long g_slot_scans, g_slot_samples, g_slot_arr_changes;
static unsigned long long g_slot_hook_subs;

static void slot_sample(void)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    unsigned long long call, frame;
    unsigned i;

    if (!g_slot_arr) return;
    ++g_slot_samples;
    call = recomp_rfl_call();
    frame = mcpx_apu_frame();
    for (i = 0; i < SLOT_N; ++i) {
        uint32_t v = MEM32(g_slot_arr + i * 4u);
        struct slot_st *s = &g_slot[i];
        if (v == s->last_val) continue;
        if (v == SLOT_PENDING && !s->pending) {
            s->pending = 1; ++s->subs;
            s->sub_call = call; s->sub_frame = frame;
        } else if (v != SLOT_PENDING && s->pending) {
            unsigned long long dc = call - s->sub_call;
            { extern void recomp_own_retire(uint32_t, unsigned long long,
                                            unsigned long long);
              extern int recomp_cmp_was_recycle(uint32_t);
              (void)recomp_cmp_was_recycle(g_slot_arr + i * 4u);
              recomp_own_retire(g_slot_arr + i * 4u, call, frame); }
            s->pending = 0; ++s->rets;
            s->life_calls += dc;
            s->life_frames += frame - s->sub_frame;
            if (dc > s->max_call_life) s->max_call_life = dc;
        }
        s->last_val = v;
    }
}

void recomp_probe_slot(unsigned site, uint32_t a, uint32_t b)
{
    unsigned i;
    switch (site) {
    case 0:                       /* the scan is about to walk array a */
        if (a != g_slot_arr) {
            ++g_slot_arr_changes;
            g_slot_arr = a;
            for (i = 0; i < SLOT_N; ++i)
                g_slot[i].last_val = a ? MEM32(a + i * 4u) : 0u;
        }
        slot_sample();
        break;
    case 1:                       /* submission: a = status addr, b = owner */
        ++g_slot_hook_subs;
        if (g_slot_arr && a >= g_slot_arr && a < g_slot_arr + SLOT_N * 4u) {
            unsigned k = (a - g_slot_arr) / 4u;
            g_slot[k].owner = b;
            g_slot[k].owner_voice = b ? MEM32(b + 0x24) : 0u;
        }
        slot_sample();
        break;
    case 2:                       /* a general observation point */
        slot_sample();
        break;
    case 3:                       /* one publication-scan step, a = index */
        ++g_slot_scans;
        slot_sample();
        for (i = 0; i < SLOT_N; ++i)
            if (g_slot[i].pending) ++g_slot[i].scans_while_pending;
        break;
    default: break;
    }
}

void recomp_dump_slot(void)
{
    extern unsigned long long recomp_rfl_call(void);
    unsigned i;
    if (g_slot_samples == 0ull) return;
    fprintf(stderr, "[SLOT] array [+0x148]=%08X   samples=%llu  scans=%llu  array changed %llu times" "\n",
            g_slot_arr, g_slot_samples, g_slot_scans, g_slot_arr_changes);
    fprintf(stderr, "[SLOT] submissions seen at the hook: %llu" "\n",
            g_slot_hook_subs);
    for (i = 0; i < SLOT_N; ++i) {
        struct slot_st *s = &g_slot[i];
        fprintf(stderr, "[SLOT] slot %u @%08X  now=%08X %s" "\n",
                i, g_slot_arr + i * 4u, s->last_val,
                s->pending ? "PENDING" : "free");
        fprintf(stderr, "[SLOT]    submissions=%llu  retirements=%llu  outstanding=%lld" "\n",
                s->subs, s->rets, (long long)s->subs - (long long)s->rets);
        if (s->rets)
            fprintf(stderr, "[SLOT]    lifetime: avg %llu update calls / %llu APU frames   max %llu calls" "\n",
                    s->life_calls / s->rets, s->life_frames / s->rets,
                    s->max_call_life);
        else
            fprintf(stderr, "[SLOT]    lifetime: never retired" "\n");
        fprintf(stderr, "[SLOT]    publication scans while pending=%llu   owner=%08X voice=%08X" "\n",
                s->scans_while_pending, s->owner, s->owner_voice);
        if (s->pending)
            fprintf(stderr, "[SLOT]    pending since update call %llu (now %llu, age %llu)" "\n",
                    s->sub_call, recomp_rfl_call(),
                    recomp_rfl_call() - s->sub_call);
    }
    fflush(stderr);
}

/* TEMPORARY: packet ownership for the decoder status slots.
 *
 * sub_00559420(desc) sets MEM32([desc+0xC]) = E_PENDING, and desc is a
 * caller-owned descriptor -- on the stack at the DirectSound call site --
 * so nothing inside that helper identifies the owner.  It has exactly two
 * call sites, and both hand over the real object:
 *
 *   sub_004C9358  ecx = the DirectSound object, desc = [ebp+8]
 *   sub_00559E51  esi = desc, ebx = the object, edi = the node just built
 *
 * Records are keyed by the exact status-slot address taken from
 * [desc+0xC], never by offset similarity, and each address carries its own
 * generation counter.  Retirement is closed out by the sampler when that
 * same address goes back to 0, so a generation is followed from submission
 * to completion or to nothing. */
#define OWN_MAX 24u
#define OWN_ADDRS 8u

struct own_rec {
    uint32_t addr, obj, vtable, inner, inner_vt, node;
    unsigned site, gen;
    unsigned long long sub_call, sub_frame, ret_call, ret_frame;
    int retired;
};
static struct own_rec g_own[OWN_MAX];
static unsigned g_own_n;
uint32_t g_own_addr[OWN_ADDRS];
static unsigned g_own_gen[OWN_ADDRS];
unsigned g_own_addrs;
static unsigned long long g_own_subs, g_own_lost;

static unsigned own_gen_for(uint32_t addr)
{
    unsigned i;
    for (i = 0; i < g_own_addrs; ++i)
        if (g_own_addr[i] == addr) return ++g_own_gen[i];
    if (g_own_addrs < OWN_ADDRS) {
        g_own_addr[g_own_addrs] = addr;
        g_own_gen[g_own_addrs] = 1u;
        ++g_own_addrs;
        return 1u;
    }
    return 0u;
}

/* Close the newest open record for this address. */
void recomp_own_retire(uint32_t addr, unsigned long long call,
                       unsigned long long frame)
{
    unsigned i;
    for (i = g_own_n; i-- > 0; ) {
        if (g_own[i].addr == addr && !g_own[i].retired) {
            g_own[i].retired = 1;
            g_own[i].ret_call = call;
            g_own[i].ret_frame = frame;
            return;
        }
    }
}

void recomp_probe_sub(unsigned site, uint32_t desc, uint32_t obj,
                      uint32_t node)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    struct own_rec *r;
    uint32_t addr;

    ++g_own_subs;
    addr = desc ? MEM32(desc + 0x0C) : 0u;
    if (!addr) return;
    if (g_own_n >= OWN_MAX) { ++g_own_lost; return; }
    r = &g_own[g_own_n++];
    r->addr = addr;
    r->obj = obj;
    r->vtable = obj ? MEM32(obj) : 0u;
    r->inner = obj ? MEM32(obj + 0x24) : 0u;
    r->inner_vt = r->inner ? MEM32(r->inner) : 0u;
    r->node = node;
    r->site = site;
    r->gen = own_gen_for(addr);
    r->sub_call = recomp_rfl_call();
    r->sub_frame = mcpx_apu_frame();
    { extern void recomp_cmp_watch(uint32_t); recomp_cmp_watch(obj); }
}

uint32_t recomp_owner_of_slot(uint32_t slot)
{
    unsigned i;
    for (i = 0; i < g_own_n; ++i)
        if (g_own[i].addr == slot) return g_own[i].obj;
    return 0u;
}

void recomp_dump_own(void)
{
    extern unsigned long long recomp_rfl_call(void);
    unsigned i;
    if (g_own_n == 0u) {
        fprintf(stderr, "[OWN] no submissions recorded" "\n");
        return;
    }
    fprintf(stderr, "[OWN] %u submissions recorded (%llu seen, %llu past the table), %u distinct status addresses" "\n",
            g_own_n, g_own_subs, g_own_lost, g_own_addrs);
    fprintf(stderr, "[OWN]  gen  status     object    vtable    inner     inner_vt  node      site  sub@call/frame   retire@call/frame" "\n");
    for (i = 0; i < g_own_n; ++i) {
        struct own_rec *r = &g_own[i];
        fprintf(stderr, "[OWN]  %-3u  %08X  %08X  %08X  %08X  %08X  %08X  %-4u  %6llu/%-8llu ", r->gen, r->addr, r->obj, r->vtable, r->inner,
                r->inner_vt, r->node, r->site, r->sub_call, r->sub_frame);
        if (r->retired)
            fprintf(stderr, "%6llu/%-8llu  (lived %llu calls)" "\n",
                    r->ret_call, r->ret_frame, r->ret_call - r->sub_call);
        else
            fprintf(stderr, "STILL PENDING after %llu calls" "\n",
                    recomp_rfl_call() - r->sub_call);
    }
    fflush(stderr);
}

/* TEMPORARY: does any genuine completion ever service the XMV-owned
 * DirectSound objects?
 *
 * The four owners are not hardcoded -- they are whichever objects the
 * ownership probe saw submit into the decoder status arrays, so identity
 * comes from the trace rather than from an address that changes per run.
 *
 * "Genuine" is defined by exclusion.  sub_00559420 begins by clearing a
 * previous status through [desc+8] before setting the new one through
 * [desc+0xC], and that recycle is what produced the two apparent
 * retirements of generation 1.  That clear is hooked directly, so any
 * other 8000000A -> 0 transition on a watched address is a real one.
 *
 * The service loop walks three circular lists at [dsobj+0x488/0x490/0x498]
 * and calls [vtable+0x18] on the member at (entry - 0x4C).  Recording every
 * distinct member it visits answers whether these four are in the loop at
 * all, and their own link words say whether they are linked anywhere. */
#define CMP_OBJS 8u
#define CMP_MEMBERS 24u

static uint32_t g_cmp_watch[CMP_OBJS];
static unsigned g_cmp_watch_n;
static unsigned long long g_cmp_watch_serviced[CMP_OBJS];

static uint32_t g_cmp_member[CMP_MEMBERS];
static uint32_t g_cmp_member_vt[CMP_MEMBERS];
static uint32_t g_cmp_member_tgt[CMP_MEMBERS];
static unsigned long long g_cmp_member_n[CMP_MEMBERS];
static unsigned g_cmp_members;
static unsigned long long g_cmp_dispatches, g_cmp_member_over;

static uint32_t g_cmp_recycle_addr;      /* cleared by sub_00559420 */
static unsigned long long g_cmp_recycles, g_cmp_genuine;
static uint32_t g_cmp_genuine_addr, g_cmp_genuine_obj;

/* Called by the ownership probe for every object it sees submit. */
void recomp_cmp_watch(uint32_t obj)
{
    unsigned i;
    if (!obj) return;
    for (i = 0; i < g_cmp_watch_n; ++i)
        if (g_cmp_watch[i] == obj) return;
    if (g_cmp_watch_n < CMP_OBJS) g_cmp_watch[g_cmp_watch_n++] = obj;
}

/* The sampler asks whether a 8000000A -> 0 it just saw was the recycle. */
int recomp_cmp_was_recycle(uint32_t addr)
{
    if (addr && addr == g_cmp_recycle_addr) {
        g_cmp_recycle_addr = 0u;
        ++g_cmp_recycles;
        return 1;
    }
    ++g_cmp_genuine;
    g_cmp_genuine_addr = addr;
    return 0;
}

int recomp_cmp_is_watched(uint32_t obj)
{
    unsigned i;
    for (i = 0; i < g_cmp_watch_n; ++i)
        if (g_cmp_watch[i] == obj) return 1;
    return 0;
}

void recomp_probe_cmp(unsigned site, uint32_t a, uint32_t b)
{
    unsigned i;
    if (site == 0u) {                 /* sub_00559420 recycle clear */
        g_cmp_recycle_addr = a;
        return;
    }
    if (site == 1u) {                 /* service dispatch: a = list entry */
        uint32_t member = a - 0x4Cu;
        uint32_t vt = member ? MEM32(member) : 0u;
        ++g_cmp_dispatches;
        for (i = 0; i < g_cmp_watch_n; ++i)
            if (g_cmp_watch[i] == member) ++g_cmp_watch_serviced[i];
        for (i = 0; i < g_cmp_members; ++i)
            if (g_cmp_member[i] == member) { ++g_cmp_member_n[i]; return; }
        if (g_cmp_members >= CMP_MEMBERS) { ++g_cmp_member_over; return; }
        g_cmp_member[g_cmp_members] = member;
        g_cmp_member_vt[g_cmp_members] = vt;
        g_cmp_member_tgt[g_cmp_members] = vt ? MEM32(vt + 0x18) : 0u;
        g_cmp_member_n[g_cmp_members] = 1u;
        ++g_cmp_members;
        return;
    }
}

void recomp_dump_cmp(void)
{
    unsigned i;
    if (g_cmp_dispatches == 0ull && g_cmp_watch_n == 0u) return;
    fprintf(stderr, "[CMP] status clears: recycle (sub_00559420)=%llu   genuine=%llu  last genuine addr=%08X" "\n",
            g_cmp_recycles, g_cmp_genuine, g_cmp_genuine_addr);
    fprintf(stderr, "[CMP] XMV-owning objects seen submitting: %u" "\n",
            g_cmp_watch_n);
    for (i = 0; i < g_cmp_watch_n; ++i) {
        uint32_t o = g_cmp_watch[i];
        fprintf(stderr, "[CMP]   owner %08X  vtable=%08X  serviced by the loop %llu times" "\n",
                o, MEM32(o), g_cmp_watch_serviced[i]);
        fprintf(stderr, "[CMP]     link node at +0x4C: next=%08X prev=%08X   [+0xA8]=%08X" "\n",
                MEM32(o + 0x4Cu), MEM32(o + 0x50u), MEM32(o + 0xA8u));
    }
    fprintf(stderr, "[CMP] service dispatches=%llu over %u distinct members%s" "\n", g_cmp_dispatches, g_cmp_members,
            g_cmp_member_over ? " (table full, some omitted)" : "");
    for (i = 0; i < g_cmp_members; ++i)
        fprintf(stderr, "[CMP]   member %08X vtable=%08X [vt+0x18]=%08X n=%llu" "\n",
                g_cmp_member[i], g_cmp_member_vt[i], g_cmp_member_tgt[i],
                g_cmp_member_n[i]);
    fflush(stderr);
}

/* TEMPORARY: per-node retirement, and whether it touches the decoder slot.
 *
 * Paths B and C both end in sub_004C8E98(this), which calls
 * sub_004C8E22(this, this+0xB8, 0x8000000A) -- note the list is +0xB8, not
 * the +0xA8 the service loop walks -- and that walks the list calling
 * sub_004C8C02(this, node, code) per node.  That is the per-packet
 * retirement, so it is hooked at its entry where ecx is the object and the
 * first stack argument is the node.
 *
 * sub_004C9358 builds the node by copying six dwords of the descriptor to
 * node+8, so the descriptor status pointer at [desc+0xC] lands at
 * [node+0x14].  That is the value compared against the decoder status
 * slots -- matched by exact address against the ones the ownership probe
 * recorded, never by offset. */
#define ND_LOG 12u

unsigned g_nd_path;          /* 2 = path B, 3 = path C, 0 = unknown */

struct nd_rec {
    unsigned path;
    uint32_t obj, node, statusptr, nodestat, slotval;
    int matched;
    unsigned long long call, frame;
};
static struct nd_rec g_nd[ND_LOG];
static unsigned g_nd_n;
static unsigned long long g_nd_total, g_nd_matched, g_nd_null;
static unsigned long long g_nd_by_path[4];
static unsigned long long g_nd_watched;   /* nodes owned by an XMV owner */

extern uint32_t g_own_addr[];
extern unsigned g_own_addrs;

static int nd_is_slot(uint32_t p)
{
    unsigned i;
    if (!p) return 0;
    for (i = 0; i < g_own_addrs; ++i)
        if (g_own_addr[i] == p) return 1;
    return 0;
}

void recomp_probe_node(uint32_t obj, uint32_t node)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    extern int recomp_cmp_is_watched(uint32_t);
    uint32_t sp, ns;
    int watched, matched;

    if (!node) return;
    ++g_nd_total;
    if (g_nd_path < 4u) ++g_nd_by_path[g_nd_path];
    sp = MEM32(node + 0x14);
    ns = MEM32(node + 0x24);
    matched = nd_is_slot(sp);
    watched = recomp_cmp_is_watched(obj);
    if (matched) ++g_nd_matched;
    if (!sp) ++g_nd_null;
    if (watched) ++g_nd_watched;

    /* Keep the interesting ones: anything owned by an XMV object, or whose
     * status pointer is one of the decoder slots. */
    if ((watched || matched) && g_nd_n < ND_LOG) {
        struct nd_rec *r = &g_nd[g_nd_n++];
        r->path = g_nd_path; r->obj = obj; r->node = node;
        r->statusptr = sp; r->nodestat = ns;
        r->slotval = sp ? MEM32(sp) : 0u;
        r->matched = matched;
        r->call = recomp_rfl_call();
        r->frame = mcpx_apu_frame();
    }
}

void recomp_dump_node(void)
{
    unsigned i;
    if (g_nd_total == 0ull) {
        fprintf(stderr, "[ND] sub_004C8C02 never ran" "\n");
        return;
    }
    fprintf(stderr, "[ND] per-node retirements=%llu   path B=%llu C=%llu unknown=%llu" "\n",
            g_nd_total, g_nd_by_path[2], g_nd_by_path[3], g_nd_by_path[0]);
    fprintf(stderr, "[ND]   [node+0x14] is a decoder slot: %llu   is null: %llu   owned by an XMV object: %llu" "\n",
            g_nd_matched, g_nd_null, g_nd_watched);
    if (g_nd_n == 0u) {
        fprintf(stderr, "[ND]   no retirement was owned by an XMV object or carried a decoder slot pointer" "\n");
    }
    for (i = 0; i < g_nd_n; ++i) {
        struct nd_rec *r = &g_nd[i];
        fprintf(stderr, "[ND]   path %c obj=%08X node=%08X  [node+0x14]=%08X %s  [node+0x24]=%08X  slot now=%08X  call %llu frame %llu" "\n",
                r->path == 2u ? (char)66 : (r->path == 3u ? (char)67 : (char)63),
                r->obj, r->node, r->statusptr,
                r->matched ? "== a decoder slot" : "not a decoder slot",
                r->nodestat, r->slotval, r->call, r->frame);
    }
    fflush(stderr);
}

/* TEMPORARY: every write to a decoder status slot, in order.
 *
 * Two stores can touch these words and both are hooked at the instruction:
 *
 *   sub_00559420 / loc_00559435   MEM32(eax) = 0x8000000A     SUBMIT
 *   sub_0055943E / loc_0055945A   MEM32(ecx) = edx            RETIRE
 *
 * The open question this settles: the per-node retirement probe showed all
 * 24 XMV packets retiring with a correct slot pointer and a value of 0 to
 * write, while the slot history showed only 8 ever reaching 0.  If a
 * SUBMIT follows the RETIRE inside the same XMV update call, the
 * once-per-update sampler cannot see the cleared state and both
 * observations are true at once.  The old value is read immediately before
 * each store, so the timeline is exact rather than inferred. */
#define SE_LOG 64u

struct se_ev {
    unsigned kind;              /* 0 = SUBMIT, 1 = RETIRE */
    uint32_t slot, oldv, newv, owner;
    unsigned long long call, frame, seq;
};
static struct se_ev g_se[SE_LOG];
static unsigned g_se_n;
static unsigned long long g_se_seq, g_se_dropped;
static unsigned long long g_se_subs, g_se_rets;
static unsigned long long g_se_same_call;   /* RETIRE then SUBMIT, one call */

/* The address -> owner mapping the ownership probe already established. */
extern uint32_t g_own_addr[];
extern unsigned g_own_addrs;
uint32_t recomp_owner_of_slot(uint32_t slot);

static int se_is_slot(uint32_t p)
{
    unsigned i;
    for (i = 0; i < g_own_addrs; ++i)
        if (g_own_addr[i] == p) return 1;
    return 0;
}

void recomp_probe_se(unsigned kind, uint32_t slot, uint32_t newv)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    struct se_ev *e;
    uint32_t oldv;

    if (!slot || !se_is_slot(slot)) return;
    oldv = MEM32(slot);            /* read before the store */
    ++g_se_seq;
    if (kind == 0u) ++g_se_subs; else ++g_se_rets;

    /* A submit landing on the same slot in the same update call as the
     * retire that just cleared it is exactly the blind spot. */
    if (kind == 0u && g_se_n) {
        unsigned k;
        for (k = g_se_n; k-- > 0; ) {
            if (g_se[k].slot != slot) continue;
            if (g_se[k].kind == 1u &&
                g_se[k].call == recomp_rfl_call())
                ++g_se_same_call;
            break;
        }
    }

    if (g_se_n >= SE_LOG) { ++g_se_dropped; return; }
    e = &g_se[g_se_n++];
    e->kind = kind; e->slot = slot; e->oldv = oldv; e->newv = newv;
    e->owner = recomp_owner_of_slot(slot);
    e->call = recomp_rfl_call();
    e->frame = mcpx_apu_frame();
    e->seq = g_se_seq;
    { extern void recomp_tl_slot(unsigned, uint32_t, uint32_t);
      recomp_tl_slot(kind, slot, e->owner); }
}

void recomp_dump_se(void)
{
    unsigned i, k;
    if (g_se_seq == 0ull) {
        fprintf(stderr, "[SE] no writes to a decoder status slot" "\n");
        return;
    }
    fprintf(stderr, "[SE] slot writes: SUBMIT=%llu  RETIRE=%llu  (%llu logged, %llu dropped)" "\n",
            g_se_subs, g_se_rets, (unsigned long long)g_se_n, g_se_dropped);
    fprintf(stderr, "[SE] SUBMIT landing in the same update call as the RETIRE before it: %llu" "\n", g_se_same_call);

    /* chronological, grouped per slot */
    for (k = 0; k < g_own_addrs; ++k) {
        uint32_t slot = g_own_addr[k];
        int any = 0;
        for (i = 0; i < g_se_n; ++i) if (g_se[i].slot == slot) any = 1;
        if (!any) continue;
        fprintf(stderr, "[SE] --- slot %08X  (owner %08X)  now=%08X" "\n",
                slot, recomp_owner_of_slot(slot), MEM32(slot));
        for (i = 0; i < g_se_n; ++i) {
            struct se_ev *e = &g_se[i];
            if (e->slot != slot) continue;
            fprintf(stderr, "[SE]   #%-4llu %s  %08X -> %08X   update call %llu   APU frame %llu" "\n",
                    e->seq, e->kind == 0u ? "SUBMIT" : "RETIRE",
                    e->oldv, e->newv, e->call, e->frame);
        }
    }
    fflush(stderr);
}

/* TEMPORARY: one merged timeline across the guest and the APU.
 *
 * The packet cycle is batch-synchronised -- all four owners submit and
 * retire together -- so a single ordered log is enough to see which
 * interval grows, without having to join slot addresses to notifier
 * addresses per packet.
 *
 * Voice identity is recovered dynamically: the guest polls a notifier byte
 * at [ [this] + idx*16 + 0xF ], and the APU writes that same address in
 * set_notify_status, where the voice number is in hand.  Matching the two
 * by exact address gives the handle without hardcoding it.  The APU side
 * is filtered to addresses the guest actually polls, or the log would be
 * thousands of unrelated voices.
 */
#define TL_LOG   96u
#define TL_ADDRS 16u

enum { TL_SUBMIT = 0, TL_RETIRE, TL_APU_DONE, TL_POLL_DONE };
static const char *const g_tl_kind[4] = {
    "SUBMIT   ", "RETIRE   ", "APU DONE ", "POLL DONE" };

struct tl_ev {
    unsigned kind, voice, notifier;
    uint32_t key, aux;
    unsigned long long call, frame, seq;
};
static struct tl_ev g_tl[TL_LOG];
static unsigned g_tl_n;
static unsigned long long g_tl_seq, g_tl_dropped;

static uint32_t g_tl_polled[TL_ADDRS];
static unsigned g_tl_polled_n;
unsigned g_tl_voice_of[TL_ADDRS];
static unsigned char g_tl_done_seen[TL_ADDRS];

void recomp_iv_note(unsigned kind);
void recomp_iv_mark(unsigned kind, unsigned long long call,
                    unsigned long long frame);

static int tl_addr_index(uint32_t addr)
{
    unsigned i;
    for (i = 0; i < g_tl_polled_n; ++i)
        if (g_tl_polled[i] == addr) return (int)i;
    return -1;
}

static void tl_add(unsigned kind, uint32_t key, uint32_t aux,
                   unsigned voice, unsigned notifier)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    struct tl_ev *e;
    /* Nothing before the decoder starts: the pre-video audio fills the
     * log many times over and the question is about the later
     * generations.  A ring keeps the most recent window. */
    if (recomp_rfl_call() == 0ull) return;
    ++g_tl_seq;
    if (g_tl_n < TL_LOG) {
        e = &g_tl[g_tl_n++];
    } else {
        ++g_tl_dropped;
        e = &g_tl[(g_tl_seq - 1ull) % TL_LOG];
    }
    e->kind = kind; e->key = key; e->aux = aux;
    e->voice = voice; e->notifier = notifier;
    e->call = recomp_rfl_call();
    e->frame = mcpx_apu_frame();
    e->seq = g_tl_seq;
}

/* from the guest: every sub_004C87B9 observation */
void recomp_tl_poll(uint32_t addr, unsigned byte)
{
    int k;
    if (!addr) return;
    k = tl_addr_index(addr);
    if (k < 0) {
        if (g_tl_polled_n >= TL_ADDRS) return;
        k = (int)g_tl_polled_n++;
        g_tl_polled[k] = addr;
    }
    if (byte == 1u) {
        if (!g_tl_done_seen[k]) {         /* first sighting of this DONE */
            g_tl_done_seen[k] = 1u;
            tl_add(TL_POLL_DONE, addr, byte, g_tl_voice_of[k], 0u);
            { extern void recomp_gen_poll(void); recomp_gen_poll(); }
            recomp_iv_note(0u);
        }
    } else {
        g_tl_done_seen[k] = 0u;           /* re-armed, next DONE is new */
    }
}

/* from the APU: only for addresses the guest polls */
void recomp_tl_apu_done(uint32_t addr, unsigned voice, unsigned notifier)
{
    int k = tl_addr_index(addr);
    if (k < 0) return;
    g_tl_voice_of[k] = voice;
    { extern void recomp_gen_notify(unsigned); recomp_gen_notify(voice); }
    recomp_iv_note(1u);
    tl_add(TL_APU_DONE, addr, 0u, voice, notifier);
}

void recomp_tl_slot(unsigned kind, uint32_t slot, uint32_t owner)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    static unsigned long long last_marked_call = 0xFFFFFFFFull;
    static unsigned last_marked_kind = 99u;
    /* One record per batch.  All eight slots move in the same update call, so
     * collapse on (call, kind) rather than on the slot address, which
     * alternates between the two arrays and defeated the first attempt. */
    unsigned long long c = recomp_rfl_call();
    if (c != last_marked_call || kind != last_marked_kind) {
        last_marked_call = c; last_marked_kind = kind;
        recomp_iv_mark(kind, c, mcpx_apu_frame());
        { extern void recomp_gen_submit(void);
          extern void recomp_gen_retire(void);
          if (kind == 0u) recomp_gen_submit(); else recomp_gen_retire(); }
        { extern void recomp_sg_submit(void);
          extern void recomp_sg_retire(void);
          if (kind == 0u) recomp_sg_submit(); else recomp_sg_retire(); }
        { extern void recomp_pg_submit(unsigned long long, unsigned long long);
          extern void recomp_pg_retire(unsigned long long, unsigned long long);
          if (kind == 0u) recomp_pg_submit(c, mcpx_apu_frame());
          else recomp_pg_retire(c, mcpx_apu_frame()); }
    }
    tl_add(kind ? TL_RETIRE : TL_SUBMIT, slot, owner, 0u, 0u);
}

void recomp_dump_tl(void)
{
    unsigned i;
    unsigned long long prev_frame = 0ull;
    if (g_tl_n == 0u) { fprintf(stderr, "[TL] no events" "\n"); return; }
    fprintf(stderr, "[TL] %u events (%llu dropped).  Notifier addresses the guest polls: %u" "\n",
            g_tl_n, g_tl_dropped, g_tl_polled_n);
    for (i = 0; i < g_tl_polled_n; ++i)
        fprintf(stderr, "[TL]   notifier %08X -> APU voice %u" "\n",
                g_tl_polled[i], g_tl_voice_of[i]);
    fprintf(stderr, "[TL]  seq  event      key       aux       voice/nfy  update call   APU frame   d(frame)" "\n");
    for (i = 0; i < g_tl_n; ++i) {
        unsigned j = (g_tl_dropped && g_tl_n == TL_LOG)
                     ? (unsigned)((g_tl_seq + i) % TL_LOG) : i;
        struct tl_ev *e = &g_tl[j];
        fprintf(stderr, "[TL] %4llu  %s  %08X  %08X  %3u/%-3u    %8llu    %8llu   %+lld" "\n",
                e->seq, g_tl_kind[e->kind & 3u], e->key, e->aux,
                e->voice, e->notifier, e->call, e->frame,
                (long long)e->frame - (long long)prev_frame);
        prev_frame = e->frame;
    }
    fflush(stderr);
}


/* TEMPORARY: the packet cycle measured as intervals, not as a log.
 *
 * The raw timeline drowned: the notifier cycle runs every ~5 APU frames while
 * packets retire every few hundred update calls, so 834k events buried the 52
 * that matter.  What the question needs is the ratio, so count the fast events
 * between the slow ones.
 *
 * The first timeline already settled two of the five intervals: APU DONE ->
 * POLL DONE measured 1-4 APU frames throughout, so notifier delivery and the
 * guest's observation of it are not what grows.
 */
static unsigned long long g_iv_polls, g_iv_apudones;
static unsigned long long g_iv_last_call, g_iv_last_frame;
static unsigned g_iv_n;
struct iv_rec {
    unsigned long long call, frame, dcall, dframe, polls, apudones;
    unsigned kind;
};
static struct iv_rec g_iv[32];

void recomp_iv_note(unsigned kind)          /* 0 = poll done, 1 = apu done */
{
    if (kind == 0u) ++g_iv_polls; else ++g_iv_apudones;
}

void recomp_iv_mark(unsigned kind, unsigned long long call,
                    unsigned long long frame)
{
    struct iv_rec *r;
    if (g_iv_n >= 32u) return;
    r = &g_iv[g_iv_n++];
    r->kind = kind;
    r->call = call; r->frame = frame;
    r->dcall = call - g_iv_last_call;
    r->dframe = frame - g_iv_last_frame;
    r->polls = g_iv_polls;
    r->apudones = g_iv_apudones;
    g_iv_last_call = call; g_iv_last_frame = frame;
    g_iv_polls = 0ull; g_iv_apudones = 0ull;
}

void recomp_dump_iv(void)
{
    unsigned i;
    if (g_iv_n == 0u) return;
    fprintf(stderr, "[IV] packet cycle, each row measured since the row above" "\n");
    fprintf(stderr, "[IV]  event   update call  APU frame   d(call)  d(frame)"
            "   notifier DONEs seen   guest POLL-DONEs" "\n");
    for (i = 0; i < g_iv_n; ++i) {
        struct iv_rec *r = &g_iv[i];
        fprintf(stderr, "[IV]  %s  %10llu  %9llu  %7llu  %8llu   %14llu   %14llu" "\n",
                r->kind == 0u ? "SUBMIT" : "RETIRE",
                r->call, r->frame, r->dcall, r->dframe,
                r->apudones, r->polls);
    }
    fflush(stderr);
}

/* TEMPORARY: per-generation packet lifetime, broken into intervals.
 *
 * The hard APU stall is fixed and throughput is sustained, but packet
 * lifetime measured in APU frames still roughly doubles per generation:
 * 1616, 2512, 4264, 7640.  The growth is now in audio time, so it has to
 * come from the voice state, not from scheduling.
 *
 * The tracked voices are whichever ones the notifier-address map resolved
 * (recomp_tl_apu_done fills it), never a hardcoded handle.  The APU
 * publishes its per-voice state; this samples it at SUBMIT and watches it
 * per APU frame for the transitions that split the lifetime up.
 */
#define GEN_MAX 10u
#define GEN_VOICES 4u

extern unsigned g_av_cbo[], g_av_ebo[], g_av_lbo[], g_av_ba[];
extern unsigned g_av_pitch[], g_av_fmt[], g_av_ssl_index[], g_av_ssl_seg[];
extern unsigned g_av_count0[], g_av_count1[], g_av_base0[], g_av_base1[];
extern unsigned g_av_flags[], g_av_spb[], g_av_csz[];
extern unsigned long long g_av_seen[];

struct gen_voice {
    unsigned voice, cbo, ebo, lbo, ba, pitch, fmt, ssl_index, ssl_seg;
    unsigned count0, count1, base0, base1, flags, spb, csz;
};

struct gen_rec {
    unsigned gen;
    unsigned long long f_submit, f_active, f_cbo_move, f_cbo_end;
    unsigned long long f_notify, f_poll, f_retire;
    unsigned long long c_submit, c_retire;
    struct gen_voice v[GEN_VOICES];
    unsigned nv;
    unsigned max_qdepth;
    unsigned cbo_at_start, cbo_seen_max;
    int open;
};
static struct gen_rec g_gen[GEN_MAX];
static unsigned g_gen_n;
static struct gen_rec *g_gen_cur;

/* The voices the notifier map resolved, deduplicated. */
static unsigned g_gen_voice[GEN_VOICES];
static unsigned g_gen_voices;
extern unsigned g_tl_voice_of[];

static void gen_learn_voice(unsigned v)
{
    unsigned i;
    if (v == 0u) return;
    for (i = 0; i < g_gen_voices; ++i)
        if (g_gen_voice[i] == v) return;
    if (g_gen_voices < GEN_VOICES) g_gen_voice[g_gen_voices++] = v;
}

void recomp_gen_voice_seen(unsigned v) { gen_learn_voice(v); }

static void gen_snapshot(struct gen_rec *r)
{
    unsigned i;
    r->nv = g_gen_voices;
    for (i = 0; i < g_gen_voices && i < GEN_VOICES; ++i) {
        unsigned v = g_gen_voice[i];
        struct gen_voice *gv = &r->v[i];
        gv->voice = v;
        gv->cbo = g_av_cbo[v]; gv->ebo = g_av_ebo[v]; gv->lbo = g_av_lbo[v];
        gv->ba = g_av_ba[v];   gv->pitch = g_av_pitch[v];
        gv->fmt = g_av_fmt[v]; gv->ssl_index = g_av_ssl_index[v];
        gv->ssl_seg = g_av_ssl_seg[v];
        gv->count0 = g_av_count0[v]; gv->count1 = g_av_count1[v];
        gv->base0 = g_av_base0[v];   gv->base1 = g_av_base1[v];
        gv->flags = g_av_flags[v];
        gv->spb = g_av_spb[v];       gv->csz = g_av_csz[v];
    }
}

void recomp_gen_submit(void)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    struct gen_rec *r;
    unsigned i;
    for (i = 0; i < GEN_VOICES; ++i) gen_learn_voice(g_tl_voice_of[i]);
    if (g_gen_n >= GEN_MAX) return;
    r = &g_gen[g_gen_n];
    memset(r, 0, sizeof(*r));
    r->gen = ++g_gen_n;
    r->f_submit = mcpx_apu_frame();
    r->c_submit = recomp_rfl_call();
    r->open = 1;
    gen_snapshot(r);
    r->cbo_at_start = r->nv ? r->v[0].cbo : 0u;
    r->cbo_seen_max = r->cbo_at_start;
    g_gen_cur = r;
}

void recomp_gen_retire(void)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    if (!g_gen_cur || !g_gen_cur->open) return;
    g_gen_cur->f_retire = mcpx_apu_frame();
    g_gen_cur->c_retire = recomp_rfl_call();
    g_gen_cur->open = 0;
}

void recomp_gen_notify(unsigned voice)
{
    extern unsigned long long mcpx_apu_frame(void);
    gen_learn_voice(voice);
    if (g_gen_cur && g_gen_cur->open && g_gen_cur->f_notify == 0ull)
        g_gen_cur->f_notify = mcpx_apu_frame();
}

void recomp_gen_poll(void)
{
    extern unsigned long long mcpx_apu_frame(void);
    if (g_gen_cur && g_gen_cur->open && g_gen_cur->f_poll == 0ull)
        g_gen_cur->f_poll = mcpx_apu_frame();
}

/* One call per APU frame, from the frame loop. */
void recomp_gen_tick(void)
{
    extern unsigned long long mcpx_apu_frame(void);
    struct gen_rec *r = g_gen_cur;
    unsigned v, cbo, ebo;
    if (!r || !r->open || r->nv == 0u) return;
    v = r->v[0].voice;
    if (v == 0u) return;
    cbo = g_av_cbo[v]; ebo = g_av_ebo[v];
    if (g_av_seen[v] && r->f_active == 0ull)
        r->f_active = mcpx_apu_frame();
    if (cbo != r->cbo_at_start && r->f_cbo_move == 0ull)
        r->f_cbo_move = mcpx_apu_frame();
    if (cbo > r->cbo_seen_max) r->cbo_seen_max = cbo;
    if (ebo && cbo >= ebo && r->f_cbo_end == 0ull)
        r->f_cbo_end = mcpx_apu_frame();
}

static unsigned long long gen_d(unsigned long long a, unsigned long long b)
{
    return (a && b && a >= b) ? a - b : 0ull;
}

void recomp_dump_gen(void)
{
    unsigned i, k;
    if (g_gen_n == 0u) return;
    fprintf(stderr, "[GEN] tracked APU voices:");
    for (i = 0; i < g_gen_voices; ++i) fprintf(stderr, " %u", g_gen_voice[i]);
    fprintf(stderr, "\n");
    fprintf(stderr, "[GEN] intervals in APU frames" "\n");
    fprintf(stderr, "[GEN] gen  submit_frame  total  sub->active  active->cbo_move  cbo_move->seg_end  seg_end->notify  notify->poll  poll->retire   calls" "\n");
    for (i = 0; i < g_gen_n; ++i) {
        struct gen_rec *r = &g_gen[i];
        fprintf(stderr, "[GEN] %3u  %12llu  %5llu  %11llu  %16llu  %17llu  %15llu  %12llu  %12llu   %llu%s" "\n",
                r->gen, r->f_submit, gen_d(r->f_retire, r->f_submit),
                gen_d(r->f_active, r->f_submit),
                gen_d(r->f_cbo_move, r->f_active),
                gen_d(r->f_cbo_end, r->f_cbo_move),
                gen_d(r->f_notify, r->f_cbo_end),
                gen_d(r->f_poll, r->f_notify),
                gen_d(r->f_retire, r->f_poll),
                gen_d(r->c_retire, r->c_submit),
                r->open ? "  (still open)" : "");
    }
    fprintf(stderr, "[GEN] voice state captured at each SUBMIT" "\n");
    for (i = 0; i < g_gen_n; ++i) {
        struct gen_rec *r = &g_gen[i];
        fprintf(stderr, "[GEN] gen %u  cbo_at_start=%u cbo_max_seen=%u" "\n",
                r->gen, r->cbo_at_start, r->cbo_seen_max);
        for (k = 0; k < r->nv; ++k) {
            struct gen_voice *gv = &r->v[k];
            fprintf(stderr, "[GEN]    v%-3u cbo=%-8u ebo=%-8u ebo-cbo=%-8d lbo=%-8u ba=%08X" "\n",
                    gv->voice, gv->cbo, gv->ebo,
                    (int)gv->ebo - (int)gv->cbo, gv->lbo, gv->ba);
            fprintf(stderr, "[GEN]         ssl idx=%u seg=%u  count0=%u count1=%u  base0=%u base1=%u" "\n",
                    gv->ssl_index, gv->ssl_seg, gv->count0, gv->count1,
                    gv->base0, gv->base1);
            fprintf(stderr, "[GEN]         pitch=%u fmt=%08X spb=%u csz=%u flags=%c%c%c%c" "\n",
                    gv->pitch, gv->fmt, gv->spb, gv->csz,
                    (gv->flags & 1u) ? 76 : 45, (gv->flags & 2u) ? 83 : 45,
                    (gv->flags & 4u) ? 80 : 45, (gv->flags & 8u) ? 122 : 45);
        }
    }
    fflush(stderr);
}

/* TEMPORARY: what happens between sub_004C87B9 saying DONE and the packet
 * actually retiring, for the XMV owners only.
 *
 * The APU side is flat -- segment end to notify is 0 frames and notify to
 * the guest seeing DONE is 2-7 -- while poll to retire doubles every
 * generation: 592, 712, 1661, 2672, 4948, 8968 APU frames.  So the growth
 * is in this stretch of guest code.
 *
 * sub_004C90A0(this) is the walker:
 *   loc_004C90C2  edi = [this+0x190]          rotating entry cursor
 *   loc_004C90CC  sub_004C87B9(this+0x68, edi)   DONE for this entry?
 *                 !done -> leave the inner loop
 *   loc_004C90DB  sub_004C8EED(this, edi, 0)     handle the completion
 *   loc_004C9161  [[this+0x80]+0xB] & 0x40 -> sub_004C8E98   retire path B
 *
 * and the XMV packets were measured retiring through path C
 * (sub_004C94B4), not B, so the gate at loc_004C9161 is not what releases
 * them.  These counters say which of the five shapes it is, by counting the
 * work done in the gap rather than logging every event. */
#define SG_OWNERS 4u
#define SG_GENS   10u

extern int recomp_cmp_is_watched(uint32_t);

/* cumulative, across all XMV owners */
static unsigned long long g_sg_pass;        /* sub_004C90A0 entries      */
static unsigned long long g_sg_done;        /* sub_004C87B9 said DONE    */
static unsigned long long g_sg_eed;         /* sub_004C8EED calls        */
static unsigned long long g_sg_gateB_seen, g_sg_gateB_pass;
static unsigned long long g_sg_e98;         /* sub_004C8E98 calls        */
static unsigned g_sg_last_owner, g_sg_last_entry, g_sg_last_cursor;
static unsigned g_sg_listlen;

struct sg_gen {
    unsigned long long pass_at_done, pass_at_retire;
    unsigned long long done_at_done, done_at_retire;
    unsigned long long eed_at_done, eed_at_retire;
    unsigned long long gB_seen_at_done, gB_seen_at_retire;
    unsigned long long gB_pass_at_done, gB_pass_at_retire;
    unsigned long long e98_at_done, e98_at_retire;
    unsigned len_at_done, len_at_retire;
    unsigned owner, entry, cursor;
    int have_done;
};
static struct sg_gen g_sg[SG_GENS];
static unsigned g_sg_n;
static struct sg_gen *g_sg_cur;

/* Walk the circular list at [owner+off], capped so a corrupt link cannot
 * hang the probe. */
static unsigned sg_list_len(uint32_t owner, uint32_t off)
{
    uint32_t head, p;
    unsigned n = 0;
    if (!owner) return 0u;
    head = owner + off;
    p = MEM32(head);
    while (p && p != head && n < 4096u) { ++n; p = MEM32(p); }
    return n;
}

void recomp_probe_sg(unsigned site, uint32_t owner, uint32_t a)
{
    if (!recomp_cmp_is_watched(owner)) return;
    switch (site) {
    case 0:                        /* sub_004C90A0 entry */
        ++g_sg_pass;
        g_sg_last_owner = owner;
        g_sg_last_cursor = MEM32(owner + 0x190);
        break;
    case 1:                        /* sub_004C87B9 said DONE, a = entry */
        ++g_sg_done;
        g_sg_last_entry = a;
        g_sg_listlen = sg_list_len(owner, 0xB8u);
        if (g_sg_cur && !g_sg_cur->have_done) {
            g_sg_cur->have_done = 1;
            g_sg_cur->owner = owner;
            g_sg_cur->entry = a;
            g_sg_cur->cursor = g_sg_last_cursor;
            g_sg_cur->pass_at_done = g_sg_pass;
            g_sg_cur->done_at_done = g_sg_done;
            g_sg_cur->eed_at_done = g_sg_eed;
            g_sg_cur->gB_seen_at_done = g_sg_gateB_seen;
            g_sg_cur->gB_pass_at_done = g_sg_gateB_pass;
            g_sg_cur->e98_at_done = g_sg_e98;
            g_sg_cur->len_at_done = g_sg_listlen;
        }
        break;
    case 2: ++g_sg_eed; break;                    /* sub_004C8EED */
    case 3:                                       /* the loc_004C9161 gate */
        ++g_sg_gateB_seen;
        if (a & 0x40u) ++g_sg_gateB_pass;
        break;
    case 4: ++g_sg_e98; break;                    /* sub_004C8E98 */
    default: break;
    }
}

void recomp_sg_submit(void)
{
    if (g_sg_n >= SG_GENS) { g_sg_cur = 0; return; }
    g_sg_cur = &g_sg[g_sg_n++];
    memset(g_sg_cur, 0, sizeof(*g_sg_cur));
}

void recomp_sg_retire(void)
{
    if (!g_sg_cur) return;
    g_sg_cur->pass_at_retire = g_sg_pass;
    g_sg_cur->done_at_retire = g_sg_done;
    g_sg_cur->eed_at_retire = g_sg_eed;
    g_sg_cur->gB_seen_at_retire = g_sg_gateB_seen;
    g_sg_cur->gB_pass_at_retire = g_sg_gateB_pass;
    g_sg_cur->e98_at_retire = g_sg_e98;
    g_sg_cur->len_at_retire = g_sg_last_owner
                              ? sg_list_len(g_sg_last_owner, 0xB8u) : 0u;
    g_sg_cur = 0;
}

void recomp_dump_sg(void)
{
    unsigned i;
    if (g_sg_n == 0u) return;
    fprintf(stderr, "[SG] between the guest seeing DONE and the packet retiring, for XMV owners only" "\n");
    fprintf(stderr, "[SG] gen  owner     entry cursor | service passes  DONE observations  sub_004C8EED  gateB seen/passed  sub_004C8E98 | [+B8] len at DONE -> at RETIRE" "\n");
    for (i = 0; i < g_sg_n; ++i) {
        struct sg_gen *g = &g_sg[i];
        if (!g->have_done) continue;
        fprintf(stderr, "[SG] %3u  %08X  %5u %6u | %14llu  %17llu  %12llu  %8llu/%-8llu %12llu | %6u -> %u" "\n",
                i + 1u, g->owner, g->entry, g->cursor,
                g->pass_at_retire - g->pass_at_done,
                g->done_at_retire - g->done_at_done,
                g->eed_at_retire - g->eed_at_done,
                g->gB_seen_at_retire - g->gB_seen_at_done,
                g->gB_pass_at_retire - g->gB_pass_at_done,
                g->e98_at_retire - g->e98_at_done,
                g->len_at_done, g->len_at_retire);
    }
    fflush(stderr);
}

/* TEMPORARY: why the notifier entry is never re-armed to 0x80.
 *
 * sub_004C87B9 keeps returning DONE for the same entry -- 3, 3, 643, 3103,
 * 6559, 11591 observations per packet -- and sub_004C8EED is called once
 * per DONE.  So either the re-arm is never requested, or it is requested
 * and does not land.
 *
 * sub_004C8EED(this, entry, 0) starts with
 *     ebx = this + (entry + 9) * 16
 *     if (MEM32(ebx) == 0) goto out
 * so an empty per-entry slot makes it a no-op, and a no-op cannot re-arm
 * anything.  Site 0 records exactly that.  Sites 1 and 2 are the two
 * writers of the state byte, recording old and new so a write that lands
 * on the wrong entry is visible as an address mismatch.  Site 3 counts the
 * vtable+0x20 dispatch codes, which is what reaches path C. */
#define RA_CODES 8u

extern int recomp_cmp_is_watched(uint32_t);

static unsigned long long g_ra_eed_calls, g_ra_eed_empty, g_ra_eed_work;
static unsigned long long g_ra_w[2];          /* the two byte writers */
static unsigned long long g_ra_w_towatched[2];
static uint32_t g_ra_last_addr[2], g_ra_last_old[2], g_ra_last_new[2];
static unsigned long long g_ra_code[RA_CODES];
static uint32_t g_ra_eed_slot, g_ra_eed_owner, g_ra_eed_entry;

/* the notifier block of the watched owners, learned from the poll */
extern uint32_t g_ev_block;

void recomp_probe_ra(unsigned site, uint32_t a, uint32_t b)
{
    switch (site) {
    case 0:                    /* sub_004C8EED: a = this, b = slot ptr */
        if (!recomp_cmp_is_watched(a)) return;
        ++g_ra_eed_calls;
        g_ra_eed_owner = a;
        g_ra_eed_slot = b ? MEM32(b) : 0u;
        if (g_ra_eed_slot == 0u) ++g_ra_eed_empty; else ++g_ra_eed_work;
        break;
    case 1:                    /* writer sub_004C8B2D, a = byte address */
    case 2: {                  /* writer sub_004C9CA8, a = byte address */
        unsigned k = site - 1u;
        ++g_ra_w[k];
        g_ra_last_addr[k] = a;
        g_ra_last_old[k] = a ? MEM8(a) : 0u;
        g_ra_last_new[k] = 0x80u;
        if (g_ev_block && a >= g_ev_block && a < g_ev_block + 64u)
            ++g_ra_w_towatched[k];
        break;
    }
    case 3:                    /* vtable+0x20 dispatch, a = code */
        if (a < RA_CODES) ++g_ra_code[a];
        break;
    default: break;
    }
}

void recomp_dump_ra(void)
{
    unsigned i;
    if (g_ra_eed_calls == 0ull && g_ra_w[0] == 0ull && g_ra_w[1] == 0ull)
        return;
    fprintf(stderr, "[RA] sub_004C8EED on XMV owners: calls=%llu  slot EMPTY (no-op)=%llu  slot occupied=%llu" "\n",
            g_ra_eed_calls, g_ra_eed_empty, g_ra_eed_work);
    fprintf(stderr, "[RA]   last: owner=%08X entry=%u slot=%08X" "\n",
            g_ra_eed_owner, g_ra_eed_entry, g_ra_eed_slot);
    fprintf(stderr, "[RA] notifier state-byte writers (both write 0x80):" "\n");
    fprintf(stderr, "[RA]   sub_004C8B2D per-entry  n=%-8llu to a watched block=%-8llu  last addr=%08X old=%02X new=%02X" "\n",
            g_ra_w[0], g_ra_w_towatched[0], g_ra_last_addr[0],
            g_ra_last_old[0], g_ra_last_new[0]);
    fprintf(stderr, "[RA]   sub_004C9CA8 bulk       n=%-8llu to a watched block=%-8llu  last addr=%08X old=%02X new=%02X" "\n",
            g_ra_w[1], g_ra_w_towatched[1], g_ra_last_addr[1],
            g_ra_last_old[1], g_ra_last_new[1]);
    fprintf(stderr, "[RA]   watched notifier block = %08X" "\n", g_ev_block);
    fprintf(stderr, "[RA] vtable+0x20 dispatch codes (path C is code 2):" "\n");
    for (i = 0; i < RA_CODES; ++i)
        if (g_ra_code[i])
            fprintf(stderr, "[RA]   code %u  n=%llu%s" "\n", i, g_ra_code[i],
                    i == 2u ? "   <- reaches sub_004C8E98, the retire" : "");
    fflush(stderr);
}

/* TEMPORARY: who dispatches vtable+0x20 code 2, the event that retires an
 * XMV packet.
 *
 * sub_004C94B4 is the handler, so the caller is simply the return address
 * on its stack -- one probe at its entry gets the caller, the code and the
 * object at once, instead of hooking eleven call sites and guessing which
 * register still holds `this` at each.
 *
 * 516 code-2 dispatches against 21288 notifier re-arms, and the packet
 * interval doubles per generation, so what matters is the spacing between
 * consecutive code-2 events and whatever gates them. */
#define C2_SITES 8u
#define C2_LOG  24u

extern int recomp_cmp_is_watched(uint32_t);

static uint32_t g_c2_site[C2_SITES];
static unsigned long long g_c2_site_n[C2_SITES];
static unsigned long long g_c2_site_n2[C2_SITES];   /* code 2 only */
static unsigned g_c2_sites;

struct c2_ev {
    uint32_t caller, owner;
    unsigned long long frame, call, d_frame, d_call;
};
static struct c2_ev g_c2[C2_LOG];
static unsigned g_c2_n;
static unsigned long long g_c2_last_frame, g_c2_last_call, g_c2_total;

void recomp_probe_c2(uint32_t owner, uint32_t code, uint32_t caller)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    unsigned i;
    if (!recomp_cmp_is_watched(owner)) return;

    for (i = 0; i < g_c2_sites; ++i)
        if (g_c2_site[i] == caller) break;
    if (i == g_c2_sites && g_c2_sites < C2_SITES) {
        g_c2_site[g_c2_sites] = caller;
        i = g_c2_sites++;
    }
    if (i < C2_SITES) {
        ++g_c2_site_n[i];
        if (code == 2u) ++g_c2_site_n2[i];
    }
    if (code != 2u) return;

    ++g_c2_total;
    { extern void recomp_probe_pg(unsigned); recomp_probe_pg(3u); }
    {
        struct c2_ev *e;
        if (g_c2_n < C2_LOG) e = &g_c2[g_c2_n++];
        else e = &g_c2[(unsigned)(g_c2_total % C2_LOG)];
        e->caller = caller; e->owner = owner;
        e->frame = mcpx_apu_frame();
        e->call = recomp_rfl_call();
        e->d_frame = g_c2_last_frame ? e->frame - g_c2_last_frame : 0ull;
        e->d_call = g_c2_last_call ? e->call - g_c2_last_call : 0ull;
    }
    g_c2_last_frame = mcpx_apu_frame();
    g_c2_last_call = recomp_rfl_call();
}

void recomp_dump_c2(void)
{
    unsigned i;
    if (g_c2_sites == 0u) return;
    fprintf(stderr, "[C2] vtable+0x20 callers, XMV owners only" "\n");
    for (i = 0; i < g_c2_sites; ++i)
        fprintf(stderr, "[C2]   caller %08X  all codes n=%-8llu  code 2 n=%llu" "\n",
                g_c2_site[i], g_c2_site_n[i], g_c2_site_n2[i]);
    fprintf(stderr, "[C2] code-2 events: %llu total, first %u shown" "\n",
            g_c2_total, g_c2_n);
    fprintf(stderr, "[C2]   #  caller     owner      APU frame   d(frame)  update call   d(call)" "\n");
    for (i = 0; i < g_c2_n; ++i) {
        struct c2_ev *e = &g_c2[i];
        fprintf(stderr, "[C2]  %2u  %08X   %08X   %9llu  %9llu   %9llu %9llu" "\n",
                i, e->caller, e->owner, e->frame, e->d_frame,
                e->call, e->d_call);
    }
    fflush(stderr);
}

/* TEMPORARY: the gate on event dispatch.
 *
 * sub_004C3C1B is the only caller of the node dispatcher sub_004C7FFF, and it
 * only dispatches when the global at 0x4E1668 is zero:
 *
 *     sub_004C38BE()                    acquire, AL = did we take it
 *     if (MEM32(0x4E1668) != 0) goto skip
 *     ecx = [node+0xC]; sub_004C7FFF()  dispatch
 *
 * So a non-zero value there defers the event rather than delivering it.  This
 * counts dispatched against deferred and tracks the value, which is the
 * "counter/timer/state that gates code 2" the question asks for.
 */
unsigned long long g_g68_calls, g_g68_dispatched, g_g68_deferred;
static uint32_t g_g68_max, g_g68_last;
static unsigned long long g_g68_hist[8];

void recomp_probe_g68(uint32_t v)
{
    ++g_g68_calls;
    g_g68_last = v;
    if (v > g_g68_max) g_g68_max = v;
    if (v < 8u) ++g_g68_hist[v];
    if (v == 0u) ++g_g68_dispatched; else ++g_g68_deferred;
}

void recomp_dump_g68(void)
{
    unsigned i;
    if (g_g68_calls == 0ull) return;
    fprintf(stderr, "[G68] sub_004C3C1B calls=%llu  dispatched (gate==0)=%llu  "
            "deferred (gate!=0)=%llu" "\n",
            g_g68_calls, g_g68_dispatched, g_g68_deferred);
    fprintf(stderr, "[G68]   gate [0x4E1668] last=%u  max ever=%u" "\n",
            g_g68_last, g_g68_max);
    fprintf(stderr, "[G68]   value histogram:");
    for (i = 0; i < 8u; ++i)
        if (g_g68_hist[i]) fprintf(stderr, "  %u:%llu", i, g_g68_hist[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* TEMPORARY: per-generation publication/drain timing, in fixed slots.
 *
 * The previous C2 log was a ring, so it wrapped and its last rows printed
 * out of order -- fine for identifying the caller, useless for measuring
 * whether the interval grows.  One slot per generation, never overwritten,
 * so the early and late generations sit side by side.
 *
 * The chain being timed:
 *   sub_004E2801 publication attempt -> sub_004C5248
 *     -> sub_004C3C1B  (drains the event queue, gate always clear)
 *       -> sub_004C7FFF -> vtable+0x24 code 2 -> sub_004C8E98 -> RETIRE
 *   and publication itself succeeds at loc_004E2DB9.
 */
#define PG_GENS 12u

struct pg_rec {
    unsigned long long submit_call, submit_frame;
    unsigned long long pub1_call, pub1_frame;      /* first sub_004C5248 */
    unsigned long long c2_call, c2_frame;          /* first code-2 batch */
    unsigned long long retire_call, retire_frame;
    unsigned long long nextpub_call, nextpub_frame;
    unsigned long long pubs_before_c2;             /* 5248 calls before it */
    unsigned long long pubs, drains, c2s;
    int open;
};
static struct pg_rec g_pg[PG_GENS];
static unsigned g_pg_n;
static struct pg_rec *g_pg_cur;

/* nodes drained per sub_004C3C1B call */
static unsigned long long g_pg_drain_hist[8];
static unsigned long long g_pg_drain_over;
static unsigned g_pg_drain_open, g_pg_drain_nodes;
static unsigned long long g_pg_drains_total, g_pg_c2_total, g_pg_pubs_total;

void recomp_pg_submit(unsigned long long call, unsigned long long frame)
{
    if (g_pg_n >= PG_GENS) { g_pg_cur = 0; return; }
    g_pg_cur = &g_pg[g_pg_n++];
    memset(g_pg_cur, 0, sizeof(*g_pg_cur));
    g_pg_cur->submit_call = call;
    g_pg_cur->submit_frame = frame;
    g_pg_cur->open = 1;
    { extern void recomp_pd_gen(unsigned); recomp_pd_gen(g_pg_n); }
    { extern void recomp_dc_gen(unsigned); recomp_dc_gen(g_pg_n); }
    { extern void recomp_ck_gen(unsigned); recomp_ck_gen(g_pg_n); }
}

void recomp_pg_retire(unsigned long long call, unsigned long long frame)
{
    if (!g_pg_cur || !g_pg_cur->open) return;
    g_pg_cur->retire_call = call;
    g_pg_cur->retire_frame = frame;
    g_pg_cur->open = 2;      /* closed, but still watching for next pub */
}

void recomp_probe_pg(unsigned site)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    struct pg_rec *r = g_pg_cur;

    switch (site) {
    case 0:                              /* sub_004C5248 entry */
        ++g_pg_pubs_total;
        if (r && r->open) {
            ++r->pubs;
            if (r->pub1_call == 0ull) {
                r->pub1_call = recomp_rfl_call();
                r->pub1_frame = mcpx_apu_frame();
            }
            if (r->c2_call == 0ull) ++r->pubs_before_c2;
        }
        break;
    case 1:                              /* sub_004C3C1B entry */
        ++g_pg_drains_total;
        g_pg_drain_open = 1u;
        g_pg_drain_nodes = 0u;
        if (r && r->open) ++r->drains;
        break;
    case 2:                              /* sub_004C3C1B exit */
        if (g_pg_drain_open) {
            if (g_pg_drain_nodes < 8u) ++g_pg_drain_hist[g_pg_drain_nodes];
            else ++g_pg_drain_over;
            g_pg_drain_open = 0u;
        }
        break;
    case 3:                              /* one code-2 dispatch */
        ++g_pg_c2_total;
        ++g_pg_drain_nodes;
        if (r && r->open) {
            ++r->c2s;
            if (r->c2_call == 0ull) {
                r->c2_call = recomp_rfl_call();
                r->c2_frame = mcpx_apu_frame();
            }
        }
        break;
    case 4:                              /* publication succeeded */
        if (r && r->open == 2 && r->nextpub_call == 0ull) {
            r->nextpub_call = recomp_rfl_call();
            r->nextpub_frame = mcpx_apu_frame();
            r->open = 0;
        }
        break;
    default: break;
    }
}

static unsigned long long pg_d(unsigned long long a, unsigned long long b)
{
    return (a && b && a >= b) ? a - b : 0ull;
}

void recomp_dump_pg(void)
{
    unsigned i;
    if (g_pg_n == 0u) return;
    fprintf(stderr, "[PG] totals: sub_004C5248=%llu  sub_004C3C1B=%llu  code-2 events=%llu" "\n",
            g_pg_pubs_total, g_pg_drains_total, g_pg_c2_total);
    fprintf(stderr, "[PG] nodes drained per sub_004C3C1B call:");
    for (i = 0; i < 8u; ++i)
        if (g_pg_drain_hist[i])
            fprintf(stderr, "  %u:%llu", i, g_pg_drain_hist[i]);
    if (g_pg_drain_over) fprintf(stderr, "  8+:%llu", g_pg_drain_over);
    fprintf(stderr, "\n");
    fprintf(stderr, "[PG] gen | submit call/frame | first 5248 call/frame  d | 5248 calls before c2 | code-2 call/frame  d | retire call/frame  d | next pub call/frame  d | 5248 3C1B c2" "\n");
    for (i = 0; i < g_pg_n; ++i) {
        struct pg_rec *r = &g_pg[i];
        fprintf(stderr, "[PG] %3u | %6llu/%-7llu | %6llu/%-7llu %5llu | %20llu | %6llu/%-7llu %5llu | %6llu/%-7llu %5llu | %6llu/%-7llu %5llu | %4llu %4llu %3llu" "\n",
                i + 1u, r->submit_call, r->submit_frame,
                r->pub1_call, r->pub1_frame,
                pg_d(r->pub1_frame, r->submit_frame),
                r->pubs_before_c2,
                r->c2_call, r->c2_frame, pg_d(r->c2_frame, r->pub1_frame),
                r->retire_call, r->retire_frame,
                pg_d(r->retire_frame, r->c2_frame),
                r->nextpub_call, r->nextpub_frame,
                pg_d(r->nextpub_frame, r->retire_frame),
                r->pubs, r->drains, r->c2s);
    }
    fflush(stderr);
}

/* TEMPORARY: the producer of [decoder+0x68], per generation.
 *
 * The ring is four slots and they cycle:
 *   +0x6C staged -> +0x60 current -> +0x64 previous -> +0x68 ready
 * with the publication path moving +0x68 back to +0x6C.
 *
 * +0x68 is filled from +0x64 at loc_004E2D2D behind three gates:
 *   loc_004E2D1C  [+0x3C] == 0     a countdown
 *   loc_004E2D21  [+0x64] != 0
 *   loc_004E2D28  [+0x68] == 0
 *
 * If the countdown at +0x3C is reloaded with a larger value each generation
 * that is a growing branch countdown; if instead the block is simply reached
 * less often, the growth is upstream of it.  Counting both distinguishes them.
 */
#define PD_GENS 12u

struct pd_rec {
    unsigned long long reached, gateA_fail, gateB_fail, gateC_fail, produced;
    unsigned f3c_max, f3c_last, f3c_at_produce;
    uint32_t buf_produced;
};
static struct pd_rec g_pd[PD_GENS];
static unsigned g_pd_gen;

void recomp_pd_gen(unsigned gen)
{
    if (gen < PD_GENS) g_pd_gen = gen;
}

void recomp_probe_pd(unsigned site, uint32_t a, uint32_t b)
{
    struct pd_rec *r;
    if (g_pd_gen >= PD_GENS) return;
    r = &g_pd[g_pd_gen];
    switch (site) {
    case 0:
        ++r->reached;
        r->f3c_last = a;
        if (a > r->f3c_max) r->f3c_max = a;
        break;
    case 1: ++r->gateA_fail; break;
    case 2: ++r->gateB_fail; break;
    case 3: ++r->gateC_fail; break;
    case 4:
        ++r->produced;
        r->buf_produced = a;
        r->f3c_at_produce = b;
        break;
    default: break;
    }
}

void recomp_dump_pd(void)
{
    unsigned i, any = 0;
    for (i = 0; i < PD_GENS; ++i) if (g_pd[i].reached) any = 1;
    if (!any) return;
    fprintf(stderr, "[PD] producer of [decoder+0x68], per generation" "\n");
    fprintf(stderr, "[PD] gen | loc_004E2D1C reached | gateA [+3C]!=0 | "
            "gateB [+64]==0 | gateC [+68]!=0 | produced | +3C max/last | "
            "buffer" "\n");
    for (i = 0; i < PD_GENS; ++i) {
        struct pd_rec *r = &g_pd[i];
        if (!r->reached) continue;
        fprintf(stderr, "[PD] %3u | %20llu | %14llu | %14llu | %14llu | "
                "%8llu | %6u/%-5u | %08X" "\n",
                i, r->reached, r->gateA_fail, r->gateB_fail, r->gateC_fail,
                r->produced, r->f3c_max, r->f3c_last, r->buf_produced);
    }
    fflush(stderr);
}

/* TEMPORARY: the [+0x3C] decrement and the deadline that gates it.
 *
 *   reload      loc_004E2ABD  [+0x3C] = (hdr >> 23) & 0xFF     slices in packet
 *   guard 1     loc_004E2ACD  [+0xF8] != 0xFFFFFFFF -> skip    deadline pending
 *   guard 2     loc_004E2AD6  [+0x3C] == 0 -> skip
 *               loc_004E2ADB  [+0xF8] = (slice_hdr >> 17) + [+0xE4]
 *   decrement   loc_004E2B10  [+0x3C] -= 1
 *   reset       [+0xF8] = 0xFFFFFFFF ; [+0xE4] = old [+0xF8]
 *
 * so the time base at +0xE4 accumulates: each deadline becomes the base for
 * the next.  An earlier field diff already had recomp at +0xE4 = 0x898 where
 * hardware was 0x3A5, so this is the quantity to watch per generation.
 */
#define DC_GENS 12u

struct dc_rec {
    unsigned long long g1_seen, g1_blocked, g2_blocked, decrements, resets;
    uint32_t e4_first, e4_last, f8_last, pts_last, c3_reload;
};
static struct dc_rec g_dc[DC_GENS];
static unsigned g_dc_gen;

void recomp_dc_gen(unsigned gen) { if (gen < DC_GENS) g_dc_gen = gen; }

void recomp_probe_dc(unsigned site, uint32_t a, uint32_t b)
{
    struct dc_rec *r;
    if (g_dc_gen >= DC_GENS) return;
    r = &g_dc[g_dc_gen];
    switch (site) {
    case 0:                       /* guard 1, a = [+0xF8] */
        ++r->g1_seen;
        if (a != 0xFFFFFFFFu) ++r->g1_blocked;
        break;
    case 1: ++r->g2_blocked; break;             /* [+0x3C] == 0 */
    case 2:                       /* the decrement, a = +0xE4, b = +0xF8 */
        ++r->decrements;
        if (!r->e4_first) r->e4_first = a;
        r->e4_last = a; r->f8_last = b;
        break;
    case 3:                       /* reset: a = old +0xF8 (the new base) */
        ++r->resets;
        r->pts_last = a;
        break;
    case 4: r->c3_reload = a; break;            /* [+0x3C] reload value */
    default: break;
    }
}

void recomp_dump_dc(void)
{
    unsigned i, any = 0;
    for (i = 0; i < DC_GENS; ++i) if (g_dc[i].g1_seen) any = 1;
    if (!any) return;
    fprintf(stderr, "[DC] the [+0x3C] decrement and its deadline gate" "\n");
    fprintf(stderr, "[DC] gen | guard1 seen | blocked by [+F8]!=FFFFFFFF | "
            "blocked by [+3C]==0 | decrements | resets | +3C reload | "
            "+E4 first/last | +F8 last" "\n");
    for (i = 0; i < DC_GENS; ++i) {
        struct dc_rec *r = &g_dc[i];
        if (!r->g1_seen) continue;
        fprintf(stderr, "[DC] %3u | %11llu | %25llu | %19llu | %10llu | "
                "%6llu | %10u | %08X/%08X | %08X" "\n",
                i, r->g1_seen, r->g1_blocked, r->g2_blocked, r->decrements,
                r->resets, r->c3_reload, r->e4_first, r->e4_last, r->f8_last);
    }
    fflush(stderr);
}

/* TEMPORARY: the presentation clock against the deadline.
 *
 * The decoder's deadline test at loc_004E2B27 is
 *
 *     elapsed = [dev+0x1DE8] - [decoder+0xB8]
 *     needed  = (([+0xC8] + [+0xF8]) * 65536 + 0xFFFF) / [+0xBC]
 *     if (needed > elapsed) skip, else present and clear [+0xF8]
 *
 * so the clock is sub_00539C60's out[1], which is [dev+0x1DE8] -- a
 * presentation/field counter, not a wall clock.  That counter is incremented
 * in sub_0053D6C0 only when ([dev+0x1DE8] ^ arg) & 1 == 0, a parity gate.
 * Recording both the comparison and the gate says whether the deadline runs
 * away or the clock stands still.
 */
#define CK_GENS 12u

struct ck_rec {
    unsigned long long tests, passed;
    uint32_t clock_first, clock_last, base, deadline_last, e4_last;
    uint32_t c8, bc;
};
static struct ck_rec g_ck[CK_GENS];
static unsigned g_ck_gen;
static unsigned long long g_ck_par_calls, g_ck_par_inc, g_ck_par_skip;
static uint32_t g_ck_par_counter, g_ck_par_arg;

void recomp_ck_gen(unsigned gen) { if (gen < CK_GENS) g_ck_gen = gen; }

void recomp_probe_ck(uint32_t clock, uint32_t dec)
{
    struct ck_rec *r;
    if (g_ck_gen >= CK_GENS || !dec) return;
    r = &g_ck[g_ck_gen];
    ++r->tests;
    if (!r->clock_first) r->clock_first = clock;
    r->clock_last = clock;
    r->base = MEM32(dec + 0xB8);
    r->deadline_last = MEM32(dec + 0xF8);
    r->e4_last = MEM32(dec + 0xE4);
    r->c8 = MEM32(dec + 0xC8);
    r->bc = MEM32(dec + 0xBC);
}

void recomp_probe_ckpar(uint32_t counter, uint32_t arg)
{
    ++g_ck_par_calls;
    g_ck_par_counter = counter;
    g_ck_par_arg = arg;
    if (((counter ^ arg) & 1u) == 0u) ++g_ck_par_inc; else ++g_ck_par_skip;
}

void recomp_dump_ck(void)
{
    unsigned i, any = 0;
    for (i = 0; i < CK_GENS; ++i) if (g_ck[i].tests) any = 1;
    fprintf(stderr, "[CK] clock [dev+0x1DE8] increment gate in sub_0053D6C0: "
            "calls=%llu  incremented=%llu  skipped=%llu  last counter=%08X "
            "arg=%08X" "\n",
            g_ck_par_calls, g_ck_par_inc, g_ck_par_skip,
            g_ck_par_counter, g_ck_par_arg);
    if (!any) return;
    fprintf(stderr, "[CK] the raw timing fields, for the hardware diff:" "\n");
    for (i = 0; i < CK_GENS; ++i)
        if (g_ck[i].tests)
            fprintf(stderr, "[CK]   gen %u  +B8=%08X  +BC=%08X  +C8=%08X  "
                    "+E4=%08X  +F8=%08X  clock=%08X" "\n",
                    i, g_ck[i].base, g_ck[i].bc, g_ck[i].c8,
                    g_ck[i].e4_last, g_ck[i].deadline_last,
                    g_ck[i].clock_last);
    fprintf(stderr, "[CK] gen | deadline tests | clock first/last | base +B8 | "
            "elapsed | deadline +F8 | base +E4 | needed | needed-elapsed" "\n");
    for (i = 0; i < CK_GENS; ++i) {
        struct ck_rec *r = &g_ck[i];
        unsigned long long needed;
        long long delta;
        if (!r->tests) continue;
        needed = r->bc ? ((((unsigned long long)r->c8 +
                            (unsigned long long)r->deadline_last) * 65536ull
                           + 0xFFFFull) / (unsigned long long)r->bc) : 0ull;
        delta = (long long)needed - (long long)(r->clock_last - r->base);
        fprintf(stderr, "[CK] %3u | %14llu | %08X/%08X | %8u | %7u | %12X | "
                "%8X | %6llu | %+lld" "\n",
                i, r->tests, r->clock_first, r->clock_last, r->base,
                r->clock_last - r->base, r->deadline_last, r->e4_last,
                needed, delta);
    }
    fflush(stderr);
}

/* TEMPORARY: every input to the +0xC8 drift calculation.
 *
 *   sub_004E2631(dec, expected, flag)
 *     sub_0042E13B(&now64)                       rdtsc
 *     delta64 = now64 - [dec+0xB0..B4]
 *     quotient = delta64 / [0x509C58..5C]        ticks per millisecond
 *     [dec+0xC8] = quotient - expected
 *
 * Statically the units already check out: sub_0042E14C returns the hardcoded
 * 0x2BB5C755 = 733,333,333 Hz, the divisor is that over 1000, and our rdtsc
 * is QPC rescaled to exactly XBOX_PERF_FREQUENCY = 733333333.  So quotient is
 * real milliseconds.  Hardware holds the result near zero, which means its
 * `expected` tracks real time; the recomp lets it reach 49,611.
 */
#define DR_LOG 16u

struct dr_rec {
    unsigned long long now, start, delta;
    unsigned long long divisor;
    unsigned quotient, expected, result;
    unsigned long long call, frame;
};
static struct dr_rec g_dr[DR_LOG];
static unsigned g_dr_n;
static unsigned long long g_dr_calls;
static uint32_t g_dr_start_lo, g_dr_start_hi;
static unsigned long long g_dr_rebase;

void recomp_probe_drift(uint32_t dec, uint32_t now_lo, uint32_t now_hi,
                        uint32_t expected)
{
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long mcpx_apu_frame(void);
    uint32_t s_lo = MEM32(dec + 0xB0), s_hi = MEM32(dec + 0xB4);
    unsigned long long now = ((unsigned long long)now_hi << 32) | now_lo;
    unsigned long long start = ((unsigned long long)s_hi << 32) | s_lo;
    unsigned long long div = ((unsigned long long)MEM32(0x509C5Cu) << 32)
                             | MEM32(0x509C58u);
    struct dr_rec *r;

    ++g_dr_calls;
    if (s_lo != g_dr_start_lo || s_hi != g_dr_start_hi) {
        ++g_dr_rebase;                     /* [+0xB0/B4] was re-based */
        g_dr_start_lo = s_lo; g_dr_start_hi = s_hi;
    }
    if (g_dr_n >= DR_LOG) return;
    r = &g_dr[g_dr_n++];
    r->now = now; r->start = start; r->delta = now - start;
    r->divisor = div;
    r->quotient = div ? (unsigned)((now - start) / div) : 0u;
    r->expected = expected;
    r->result = r->quotient - expected;
    r->call = recomp_rfl_call();
    r->frame = mcpx_apu_frame();
}

void recomp_dump_drift(void)
{
    unsigned i;
    if (g_dr_calls == 0ull) {
        fprintf(stderr, "[DR] sub_004E2631 never reached the calculation" "\n");
        return;
    }
    fprintf(stderr, "[DR] sub_004E2631 calls=%llu   [+0xB0/B4] re-based %llu "
            "times   divisor=%llu ticks/ms" "\n",
            g_dr_calls, g_dr_rebase,
            g_dr_n ? g_dr[0].divisor : 0ull);
    fprintf(stderr, "[DR]  #  now64            start64          delta64        "
            "  quotient(ms)  expected  +0xC8    call/frame" "\n");
    for (i = 0; i < g_dr_n; ++i) {
        struct dr_rec *r = &g_dr[i];
        fprintf(stderr, "[DR] %2u  %016llX %016llX %016llX  %11u  %8u  %+8d   "
                "%llu/%llu" "\n",
                i, r->now, r->start, r->delta, r->quotient, r->expected,
                (int)r->result, r->call, r->frame);
    }
    fflush(stderr);
}

/* TEMPORARY: one presentation deadline, creation to clear.
 *
 *   loc_004E2ADB  [+0xF8] = (slice_hdr >> 17) + [+0xE4]      created
 *   loc_004E2B27  elapsed = [dev+0x1DE8] - [+0xB8]           checked
 *                 needed  = (([+0xC8] + [+0xF8]) << 16 + 0xFFFF) / [+0xBC]
 *                 needed > elapsed -> wait
 *   presentation  [+0xF8] = 0xFFFFFFFF                        cleared
 *
 * The first few deadlines are the ones that matter -- +0xC8 is still zero
 * there, so any gap against hardware is not the drift feedback.
 */
#define DL_MAX   6u
#define DL_CHECK 48u

struct dl_rec {
    uint32_t f8, e4, c8, bc, b8;
    uint32_t create_clock, create_elapsed;
    unsigned long long needed;
    uint32_t check_clock[DL_CHECK];
    unsigned char check_pass[DL_CHECK];
    unsigned checks, checks_total;
    uint32_t clear_clock;
    int cleared;
};
static struct dl_rec g_dl[DL_MAX];
static unsigned g_dl_n;
static struct dl_rec *g_dl_cur;

void recomp_dl_create(uint32_t dec, uint32_t f8)
{
    struct dl_rec *r;
    if (g_dl_n >= DL_MAX) { g_dl_cur = 0; return; }
    r = &g_dl[g_dl_n++];
    memset(r, 0, sizeof(*r));
    r->f8 = f8;
    r->e4 = MEM32(dec + 0xE4);
    r->c8 = MEM32(dec + 0xC8);
    r->bc = MEM32(dec + 0xBC);
    r->b8 = MEM32(dec + 0xB8);
    r->needed = r->bc ? ((((unsigned long long)r->c8 + f8) * 65536ull + 0xFFFFull)
                         / (unsigned long long)r->bc) : 0ull;
    g_dl_cur = r;
}

void recomp_dl_check(uint32_t dec, uint32_t clock, uint32_t parity)
{
    struct dl_rec *r = g_dl_cur;
    uint32_t elapsed;
    unsigned long long needed;
    (void)parity;
    if (!r || r->cleared) return;
    elapsed = clock - r->b8;
    if (r->create_clock == 0u) {
        r->create_clock = clock;
        r->create_elapsed = elapsed;
    }
    needed = r->bc ? ((((unsigned long long)MEM32(dec + 0xC8) +
                        MEM32(dec + 0xF8)) * 65536ull + 0xFFFFull)
                      / (unsigned long long)r->bc) : 0ull;
    ++r->checks_total;
    if (r->checks < DL_CHECK) {
        r->check_clock[r->checks] = clock;
        r->check_pass[r->checks] = (needed <= elapsed) ? 1u : 0u;
        ++r->checks;
    }
}

void recomp_dl_clear(uint32_t clock)
{
    if (!g_dl_cur || g_dl_cur->cleared) return;
    g_dl_cur->clear_clock = clock;
    g_dl_cur->cleared = 1;
}

void recomp_dump_dl(void)
{
    unsigned i, k;
    if (g_dl_n == 0u) return;
    fprintf(stderr, "[DL] the first %u presentation deadlines" "\n", g_dl_n);
    for (i = 0; i < g_dl_n; ++i) {
        struct dl_rec *r = &g_dl[i];
        fprintf(stderr, "[DL] #%u  +F8=%08X +E4=%08X +C8=%08X +BC=%08X +B8=%08X" "\n",
                i, r->f8, r->e4, r->c8, r->bc, r->b8);
        fprintf(stderr, "[DL]     created at clock=%u elapsed=%u   needed=%llu"
                "   checks=%u   %s" "\n",
                r->create_clock, r->create_elapsed, r->needed, r->checks_total,
                r->cleared ? "cleared" : "STILL PENDING");
        if (r->cleared)
            fprintf(stderr, "[DL]     cleared at clock=%u   waited %d ticks" "\n",
                    r->clear_clock, (int)(r->clear_clock - r->create_clock));
        fprintf(stderr, "[DL]     clocks seen:");
        for (k = 0; k < r->checks; ++k)
            fprintf(stderr, " %u%s", r->check_clock[k],
                    r->check_pass[k] ? "*" : "");
        fprintf(stderr, "   (* = needed <= elapsed)" "\n");
    }
    fflush(stderr);
}

/* TEMPORARY: the recomp side of the hardware xmv3c measurement.
 *
 * Same three sites and the same definitions, so the numbers are directly
 * comparable:
 *
 *   reload       loc_004E2ABD   [+0x3C] = (packet_hdr >> 23) & 0xFF
 *   decrement    loc_004E2B10   [+0x3C] -= 1
 *   publication  loc_004E2DB9
 *
 * Hardware, over 1941 ticks: 116 reloads, 896 decrements, 115 publications;
 * 7.79 decrements per publication, 7.72 per reload, 1.009 reloads per
 * publication -- one packet fully decoded, one frame out, every time.
 *
 * The question this answers is the last line of the summary: how often does
 * [+0x3C] reach zero and then a new packet load without a frame having been
 * published.  Those are the cases where a packet was decoded for nothing.
 */
#define X3_GROUPS 64u
#define X3_FAIL   4u

struct x3_group {
    unsigned index, value, decs;
    uint32_t clock_reload, clock_pub;
    unsigned char reached_zero, published, closed_by_reload;
};
static struct x3_group g_x3[X3_GROUPS];
static unsigned g_x3_n;
static struct x3_group *g_x3_cur;

static unsigned long long g_x3_reloads, g_x3_decs, g_x3_pubs;
static unsigned long long g_x3_failed;      /* zero reached, no publication */
static uint32_t g_x3_last_pub_clock;
static unsigned long long g_x3_tick_sum;
static unsigned g_x3_tick_min = 0xFFFFFFFFu, g_x3_tick_max;
static unsigned long long g_x3_tick_n;

/* state captured for the first success and the first failure */
struct x3_state {
    unsigned valid;
    unsigned index, value, decs;
    uint32_t f60, f64, f68, f6C, f74, fF8, arr;
    uint32_t status[4];
};
static struct x3_state g_x3_ok, g_x3_bad;

static void x3_capture(struct x3_state *s, uint32_t dec, struct x3_group *g)
{
    unsigned i;
    if (s->valid || !dec) return;
    s->valid = 1;
    s->index = g->index; s->value = g->value; s->decs = g->decs;
    s->f60 = MEM32(dec + 0x60); s->f64 = MEM32(dec + 0x64);
    s->f68 = MEM32(dec + 0x68); s->f6C = MEM32(dec + 0x6C);
    s->f74 = MEM32(dec + 0x74); s->fF8 = MEM32(dec + 0xF8);
    s->arr = MEM32(dec + 0x148);
    for (i = 0; i < 4u; ++i)
        s->status[i] = s->arr ? MEM32(s->arr + i * 4u) : 0xDEADDEADu;
}

static void x3_close(uint32_t dec, unsigned by_reload)
{
    struct x3_group *g = g_x3_cur;
    if (!g) return;
    g->closed_by_reload = (unsigned char)by_reload;
    if (g->reached_zero && !g->published) {
        ++g_x3_failed;
        x3_capture(&g_x3_bad, dec, g);
    }
}

void recomp_probe_x3(unsigned site, uint32_t dec, uint32_t value)
{
    uint32_t clock;
    uint32_t devp = MEM32(0x5499E8u);
    clock = devp ? MEM32(devp + 0x1DE8u) : 0u;

    switch (site) {
    case 0:                                   /* reload */
        x3_close(dec, 1u);
        ++g_x3_reloads;
        if (g_x3_n < X3_GROUPS) {
            g_x3_cur = &g_x3[g_x3_n];
            memset(g_x3_cur, 0, sizeof(*g_x3_cur));
            g_x3_cur->index = ++g_x3_n;
            { extern void recomp_pk23_reload(unsigned);
              recomp_pk23_reload(g_x3_cur->index); }
            g_x3_cur->value = value;
            g_x3_cur->clock_reload = clock;
        } else {
            g_x3_cur = 0;
        }
        break;
    case 1:                                   /* decrement */
        ++g_x3_decs;
        if (g_x3_cur) {
            ++g_x3_cur->decs;
            if (dec && MEM32(dec + 0x3C) <= 1u)
                g_x3_cur->reached_zero = 1u;
        }
        break;
    case 2:                                   /* publication */
        ++g_x3_pubs;
        if (g_x3_last_pub_clock) {
            unsigned d = clock - g_x3_last_pub_clock;
            g_x3_tick_sum += d; ++g_x3_tick_n;
            if (d < g_x3_tick_min) g_x3_tick_min = d;
            if (d > g_x3_tick_max) g_x3_tick_max = d;
        }
        g_x3_last_pub_clock = clock;
        if (g_x3_cur && !g_x3_cur->published) {
            g_x3_cur->published = 1u;
            g_x3_cur->clock_pub = clock;
            if (g_x3_cur->reached_zero)
                x3_capture(&g_x3_ok, dec, g_x3_cur);
        }
        break;
    default: break;
    }
}

static void x3_show(const char *tag, const struct x3_state *s)
{
    if (!s->valid) {
        fprintf(stderr, "[X3] %s: none seen" "\n", tag);
        return;
    }
    fprintf(stderr, "[X3] %s  reload #%u value=%u decrements=%u" "\n",
            tag, s->index, s->value, s->decs);
    fprintf(stderr, "[X3]    +60=%08X +64=%08X +68=%08X +6C=%08X +74=%08X +F8=%08X" "\n", s->f60, s->f64, s->f68, s->f6C, s->f74, s->fF8);
    fprintf(stderr, "[X3]    [+148]=%08X  status %08X %08X %08X %08X" "\n",
            s->arr, s->status[0], s->status[1], s->status[2], s->status[3]);
}

void recomp_dump_x3(void)
{
    unsigned i;
    if (g_x3_reloads == 0ull) return;
    fprintf(stderr, "[X3] reload # | value | decs | zero? | pub? | closed by reload? | clock reload/pub" "\n");
    for (i = 0; i < g_x3_n; ++i) {
        struct x3_group *g = &g_x3[i];
        fprintf(stderr, "[X3] %8u | %5u | %4u | %5s | %4s | %17s | %u/%u" "\n",
                g->index, g->value, g->decs,
                g->reached_zero ? "yes" : "no",
                g->published ? "yes" : "NO",
                g->closed_by_reload ? "yes" : "-",
                g->clock_reload, g->clock_pub);
    }
    fprintf(stderr, "[X3] reloads=%llu  decrements=%llu  publications=%llu" "\n",
            g_x3_reloads, g_x3_decs, g_x3_pubs);
    if (g_x3_pubs)
        fprintf(stderr, "[X3]   decrements per publication = %.3f" "\n",
                (double)g_x3_decs / (double)g_x3_pubs);
    if (g_x3_reloads)
        fprintf(stderr, "[X3]   decrements per reload      = %.3f" "\n",
                (double)g_x3_decs / (double)g_x3_reloads);
    if (g_x3_pubs)
        fprintf(stderr, "[X3]   reloads per publication    = %.3f" "\n",
                (double)g_x3_reloads / (double)g_x3_pubs);
    if (g_x3_tick_n)
        fprintf(stderr, "[X3]   ticks per publication: mean %.1f  min %u  max %u" "\n",
                (double)g_x3_tick_sum / (double)g_x3_tick_n,
                g_x3_tick_min, g_x3_tick_max);
    fprintf(stderr, "[X3]   completed reloads with NO publication = %llu" "\n",
            g_x3_failed);
    fprintf(stderr, "[X3] hardware reference: reloads=116 decrements=896 publications=115" "\n");
    fprintf(stderr, "[X3]   7.79 per publication, 7.72 per reload, 1.009 reloads per publication" "\n");
    x3_show("FIRST SUCCESS", &g_x3_ok);
    x3_show("FIRST FAILURE", &g_x3_bad);
    fflush(stderr);
}

/* TEMPORARY: every slice deadline in packets 2 and 3 only.
 *
 * Packet 2 publishes in 46 ticks, inside hardware's 7-48 range.  Packet 3
 * takes 90, the first step outside it.  Both carry a similar slice count
 * (15 and 14), so the question is which slice first waits longer than it
 * should and what changed by then.
 *
 * Restricted to those two groups deliberately -- later packets are already
 * deep in the drift feedback and would drown the comparison.
 */
#define PK_SLICES 40u

struct pk_slice {
    unsigned packet, index;
    uint32_t e4, f8, c8, b8;
    uint32_t clock_create, elapsed_create;
    unsigned long long needed;
    uint32_t clock_clear;
    int cleared;
};
static struct pk_slice g_pk[PK_SLICES];
static unsigned g_pk_n;
static struct pk_slice *g_pk_cur;
static unsigned g_pk_group;          /* which reload group we are in */
static unsigned g_pk_index;          /* slice index inside it */

/* when +0xC8 first goes non-zero */
static uint32_t g_pk_c8_first_value;
static unsigned g_pk_c8_first_packet, g_pk_c8_first_slice;
static uint32_t g_pk_c8_first_clock;
static int g_pk_c8_seen;

void recomp_pk23_reload(unsigned group)
{
    g_pk_group = group;
    g_pk_index = 0u;
    g_pk_cur = 0;
}

void recomp_pk23_create(uint32_t dec, uint32_t f8)
{
    struct pk_slice *s;
    uint32_t devp, clock, c8;
    if (g_pk_group != 2u && g_pk_group != 3u) { g_pk_cur = 0; return; }
    if (g_pk_n >= PK_SLICES) { g_pk_cur = 0; return; }
    devp = MEM32(0x5499E8u);
    clock = devp ? MEM32(devp + 0x1DE8u) : 0u;
    c8 = MEM32(dec + 0xC8);

    if (!g_pk_c8_seen && c8 != 0u) {
        g_pk_c8_seen = 1;
        g_pk_c8_first_value = c8;
        g_pk_c8_first_packet = g_pk_group;
        g_pk_c8_first_slice = g_pk_index;
        g_pk_c8_first_clock = clock;
    }

    s = &g_pk[g_pk_n++];
    memset(s, 0, sizeof(*s));
    s->packet = g_pk_group;
    s->index = g_pk_index++;
    s->e4 = MEM32(dec + 0xE4);
    s->f8 = f8;
    s->c8 = c8;
    s->b8 = MEM32(dec + 0xB8);
    s->clock_create = clock;
    s->elapsed_create = clock - s->b8;
    { uint32_t bc = MEM32(dec + 0xBC);
      s->needed = bc ? ((((unsigned long long)c8 + f8) * 65536ull + 0xFFFFull)
                        / (unsigned long long)bc) : 0ull; }
    g_pk_cur = s;
}

void recomp_pk23_clear(void)
{
    uint32_t devp;
    if (!g_pk_cur || g_pk_cur->cleared) return;
    devp = MEM32(0x5499E8u);
    g_pk_cur->clock_clear = devp ? MEM32(devp + 0x1DE8u) : 0u;
    g_pk_cur->cleared = 1;
    g_pk_cur = 0;
}

void recomp_dump_pk23(void)
{
    unsigned i, p;
    if (g_pk_n == 0u) return;
    fprintf(stderr, "[PK] slice deadlines in packets 2 and 3" "\n");
    fprintf(stderr, "[PK] pkt sl | +E4      +F8      +C8      +B8      | clock elapsed needed | cleared waited" "\n");
    for (i = 0; i < g_pk_n; ++i) {
        struct pk_slice *s = &g_pk[i];
        fprintf(stderr, "[PK]  %2u %2u | %08X %08X %08X %08X | %5u %7u %6llu | %7u %6d%s" "\n",
                s->packet, s->index, s->e4, s->f8, s->c8, s->b8,
                s->clock_create, s->elapsed_create, s->needed,
                s->clock_clear,
                s->cleared ? (int)(s->clock_clear - s->clock_create) : -1,
                s->cleared ? "" : "   NOT CLEARED");
    }
    for (p = 2u; p <= 3u; ++p) {
        unsigned n = 0, first = 0, last = 0;
        uint32_t c8_first = 0, c8_last = 0, clk_first = 0, clk_last = 0;
        unsigned long long need_first = 0, need_last = 0;
        for (i = 0; i < g_pk_n; ++i) {
            if (g_pk[i].packet != p) continue;
            if (n == 0u) {
                first = i; c8_first = g_pk[i].c8;
                clk_first = g_pk[i].clock_create; need_first = g_pk[i].needed;
            }
            last = i; c8_last = g_pk[i].c8;
            clk_last = g_pk[i].cleared ? g_pk[i].clock_clear
                                       : g_pk[i].clock_create;
            need_last = g_pk[i].needed;
            ++n;
        }
        if (n == 0u) continue;
        (void)first; (void)last;
        fprintf(stderr, "[PK] packet %u: slices=%u  +C8 %u -> %u  needed %llu -> %llu  clock %u -> %u  total %u ticks" "\n",
                p, n, c8_first, c8_last, need_first, need_last,
                clk_first, clk_last, clk_last - clk_first);
    }
    if (g_pk_c8_seen)
        fprintf(stderr, "[PK] +0xC8 first non-zero: value=%u  at packet %u slice %u  clock %u" "\n",
                g_pk_c8_first_value, g_pk_c8_first_packet,
                g_pk_c8_first_slice, g_pk_c8_first_clock);
    else
        fprintf(stderr, "[PK] +0xC8 stayed zero across packets 2 and 3" "\n");
    fflush(stderr);
}

/* TEMPORARY: every sub_004E2631 call from decoder creation onward.
 *
 * The earlier drift probe started logging too late -- its first record already
 * had +0xC8 at 3769.  The transition that matters is the first write of a
 * non-zero value, measured at packet 3 slice 0 as 1545.
 *
 *   sub_004E2631(dec, expected, flag)     called indirectly, so the caller is
 *     [ebp+0x10] != 0 -> return                   the return address at entry
 *     [ebp+0xC] == 0  -> return
 *     sub_0042E13B(&now64)                        rdtsc
 *     quotient = (now64 - [dec+0xB0..B4]) / [0x509C58..5C]
 *     [dec+0xC8] = quotient - expected
 */
#define D2_LOG 24u

struct d2_rec {
    uint32_t caller, dec, expected, flag;
    unsigned long long now, start, delta;
    unsigned quotient, old_c8, new_c8;
    uint32_t f3c, e4, f8, f68, f6C, clock;
    unsigned long long call;
    unsigned char published_since;
    unsigned char stored;
};
static struct d2_rec g_d2[D2_LOG];
static unsigned g_d2_n;
static unsigned long long g_d2_calls, g_d2_entries;
static unsigned char g_d2_pub_flag;
static struct d2_rec *g_d2_cur;
static int g_d2_first_nonzero = -1;

void recomp_d2_published(void) { g_d2_pub_flag = 1u; }

void recomp_d2_enter(uint32_t caller, uint32_t dec, uint32_t expected,
                     uint32_t flag)
{
    extern unsigned long long recomp_rfl_call(void);
    uint32_t devp;
    struct d2_rec *r;
    ++g_d2_entries;
    if (g_d2_n >= D2_LOG) { g_d2_cur = 0; return; }
    r = &g_d2[g_d2_n];
    memset(r, 0, sizeof(*r));
    r->caller = caller; r->dec = dec; r->expected = expected; r->flag = flag;
    r->call = recomp_rfl_call();
    r->published_since = g_d2_pub_flag;
    g_d2_pub_flag = 0u;
    devp = MEM32(0x5499E8u);
    r->clock = devp ? MEM32(devp + 0x1DE8u) : 0u;
    if (dec) {
        r->old_c8 = MEM32(dec + 0xC8);
        r->f3c = MEM32(dec + 0x3C); r->e4 = MEM32(dec + 0xE4);
        r->f8 = MEM32(dec + 0xF8);
        r->f68 = MEM32(dec + 0x68); r->f6C = MEM32(dec + 0x6C);
    }
    g_d2_cur = r;
}

void recomp_d2_store(uint32_t dec, uint32_t now_lo, uint32_t now_hi,
                     uint32_t new_c8)
{
    struct d2_rec *r = g_d2_cur;
    unsigned long long div;
    ++g_d2_calls;
    if (!r) return;
    r->now = ((unsigned long long)now_hi << 32) | now_lo;
    r->start = ((unsigned long long)MEM32(dec + 0xB4) << 32) | MEM32(dec + 0xB0);
    r->delta = r->now - r->start;
    div = ((unsigned long long)MEM32(0x509C5Cu) << 32) | MEM32(0x509C58u);
    r->quotient = div ? (unsigned)(r->delta / div) : 0u;
    r->new_c8 = new_c8;
    r->stored = 1u;
    if (g_d2_first_nonzero < 0 && new_c8 != 0u && r->old_c8 == 0u)
        g_d2_first_nonzero = (int)g_d2_n;
    ++g_d2_n;
    g_d2_cur = 0;
}

void recomp_dump_d2(void)
{
    unsigned i;
    if (g_d2_entries == 0ull) return;
    fprintf(stderr, "[D2] sub_004E2631 entries=%llu  reached the store=%llu" "\n",
            g_d2_entries, g_d2_calls);
    fprintf(stderr, "[D2]  #  caller   xmv  clock | expected quotient | "
            "+C8 old->new | +3C +E4      +F8      +68      +6C      | pub?" "\n");
    for (i = 0; i < g_d2_n; ++i) {
        struct d2_rec *r = &g_d2[i];
        fprintf(stderr, "[D2] %2u  %08X %4llu %5u | %8u %8u | %6d->%-6d | "
                "%3u %08X %08X %08X %08X | %s%s" "\n",
                i, r->caller, r->call, r->clock, r->expected, r->quotient,
                (int)r->old_c8, (int)r->new_c8, r->f3c, r->e4, r->f8, r->f68, r->f6C,
                r->published_since ? "yes" : "-",
                ((int)i == g_d2_first_nonzero) ? "   <-- FIRST NON-ZERO" : "");
    }
    if (g_d2_first_nonzero >= 0) {
        struct d2_rec *r = &g_d2[g_d2_first_nonzero];
        fprintf(stderr, "[D2] first non-zero +0xC8 = %d at call #%d" "\n",
                (int)r->new_c8, g_d2_first_nonzero);
        fprintf(stderr, "[D2]   now64=%016llX start64=%016llX delta=%016llX" "\n",
                r->now, r->start, r->delta);
        fprintf(stderr, "[D2]   quotient=%u ms  expected=%u ms  -> %u - %u = %d" "\n",
                r->quotient, r->expected, r->quotient, r->expected,
                (int)r->new_c8);
    } else {
        fprintf(stderr, "[D2] +0xC8 never went non-zero in the logged window" "\n");
    }
    fflush(stderr);
}

/* TEMPORARY: the stream-start stamp against the first audio packets.
 *
 *   loc_004E2C76  sub_0042E13B(&[dec+0xB0])    rdtsc -> stream start
 *   loc_004E2C82  [dec+0xB8] = presentation clock
 *
 * both set once, guarded on [+0xB0]|[+0xB4] being zero.  Everything after is
 * measured from that instant, so if it is taken before audio actually starts
 * the deficit is baked in from the first callback.
 *
 * All times below are milliseconds since that stamp, computed the same way the
 * title does it: (rdtsc - [+0xB0..B4]) / [0x509C58..5C].
 */
#define AS_LOG 10u

static unsigned long long g_as_start;
static uint32_t g_as_start_clock;
static unsigned long long g_as_start_call;
static int g_as_stamped;
static unsigned long long g_as_apu0;
static uint32_t g_as_dec;

struct as_ev { unsigned ms; uint32_t clock, slot; unsigned long long call; };
static struct as_ev g_as_sub[AS_LOG], g_as_ret[AS_LOG];
static unsigned g_as_subs, g_as_rets;

static unsigned as_ms_now(void)
{
    extern uint64_t recomp_rdtsc(void);
    unsigned long long div = ((unsigned long long)MEM32(0x509C5Cu) << 32)
                             | MEM32(0x509C58u);
    unsigned long long now = recomp_rdtsc();
    if (!g_as_stamped || !div || now < g_as_start) return 0u;
    return (unsigned)((now - g_as_start) / div);
}

static uint32_t as_clock(void)
{
    uint32_t devp = MEM32(0x5499E8u);
    return devp ? MEM32(devp + 0x1DE8u) : 0u;
}

void recomp_as_stamp(uint32_t dec)
{
    extern unsigned long long recomp_rfl_call(void);
    if (g_as_stamped) return;
    g_as_stamped = 1;
    g_as_dec = dec;
    g_as_start = ((unsigned long long)MEM32(dec + 0xB4) << 32) | MEM32(dec + 0xB0);
    g_as_start_clock = as_clock();
    g_as_start_call = recomp_rfl_call();
    { extern unsigned long long g_apu_iters; g_as_apu0 = g_apu_iters; }
}

void recomp_as_submit(uint32_t slot)
{
    extern unsigned long long recomp_rfl_call(void);
    if (g_as_subs >= AS_LOG) return;
    g_as_sub[g_as_subs].ms = as_ms_now();
    g_as_sub[g_as_subs].clock = as_clock();
    g_as_sub[g_as_subs].slot = slot;
    g_as_sub[g_as_subs].call = recomp_rfl_call();
    ++g_as_subs;
}

void recomp_as_retire(uint32_t slot)
{
    extern unsigned long long recomp_rfl_call(void);
    if (g_as_rets >= AS_LOG) return;
    g_as_ret[g_as_rets].ms = as_ms_now();
    g_as_ret[g_as_rets].clock = as_clock();
    g_as_ret[g_as_rets].slot = slot;
    g_as_ret[g_as_rets].call = recomp_rfl_call();
    ++g_as_rets;
}

/* TEMPORARY: the IDCT constant tables the XMV decoder multiplies against.
 * Every MMX helper is now bit-exact against the host CPU and the emitted
 * operand order is correct, so if the picture is still wrong the inputs
 * are.  sub_004E5928 loads its multipliers from fixed addresses in the
 * XBE data section; if those are zero or garbage the IDCT produces
 * structurally perfect macroblocks full of wrong pixels. */
static void recomp_dump_idct_tables(void)
{
    static const uint32_t addrs[] = {
        0x004EA440u, 0x004EA448u, 0x004EA450u, 0x004EA458u,
        0x004EA460u, 0x004EA468u, 0x004EA470u, 0x004EA478u,
        0x004EA4A0u, 0x004EA4A8u,   /* luma half-pel filter */
        0x004EA480u, 0x004EA488u }; /* chroma average masks */
    unsigned i;
    fprintf(stderr, "[IDCT] decoder constant tables:" "\n");
    for (i = 0; i < sizeof(addrs) / sizeof(addrs[0]); ++i) {
        uint32_t lo = MEM32(addrs[i]), hi = MEM32(addrs[i] + 4);
        fprintf(stderr, "[IDCT]   %08X: %08X%08X   words %6d %6d %6d %6d" "\n",
                addrs[i], hi, lo,
                (int)(int16_t)(lo & 0xFFFFu), (int)(int16_t)(lo >> 16),
                (int)(int16_t)(hi & 0xFFFFu), (int)(int16_t)(hi >> 16));
    }
    fflush(stderr);
}
void recomp_dump_as(void)
{
    recomp_dump_idct_tables();
    unsigned i;
    if (!g_as_stamped) {
        fprintf(stderr, "[AS] the stream start was never stamped" "\n");
        return;
    }
    fprintf(stderr, "[AS] stream start stamped at presentation clock %u, "
            "XMV update call %llu   decoder=%08X" "\n",
            g_as_start_clock, g_as_start_call, g_as_dec);
    {   /* the APU own pacing, measured over the same window */
        extern unsigned long long g_apu_iters;
        extern unsigned long long g_thr_calls, g_thr_rebases, g_thr_waits;
        extern unsigned long long g_thr_req_us, g_thr_act_us;
        extern unsigned long long g_thr_spins;
        extern unsigned g_thr_worst_us;
        extern uint64_t recomp_rdtsc(void);
        unsigned long long div = ((unsigned long long)MEM32(0x509C5Cu) << 32)
                                 | MEM32(0x509C58u);
        unsigned long long ms = div ? (recomp_rdtsc() - g_as_start) / div : 0ull;
        unsigned long long frames = g_apu_iters - g_as_apu0;
        fprintf(stderr, "[AS] APU: %llu frames in %llu ms since the stamp = "
                "%.1f Hz   (48000/32 = 1500 Hz required)" "\n",
                frames, ms,
                ms ? (double)frames * 1000.0 / (double)ms : 0.0);
        {   /* where the frame thread actually spends its time */
            extern unsigned long long g_apu_t_iter, g_apu_t_locked;
            extern unsigned long long g_apu_t_lockwait, g_apu_t_backpressure;
            extern unsigned long long g_apu_t_idle, g_apu_backpressure_loops;
            extern unsigned long long g_xa2_attempts, g_xa2_accepted, g_xa2_full;
            extern int xa2_get_buffer_size(void);
            extern unsigned long long g_xa2_audible, g_xa2_silent;
            extern int g_xa2_peak;
            extern volatile int g_mixer_active_count;
            extern double apu_qpc_ms_scale(void);
            double k = apu_qpc_ms_scale();
            fprintf(stderr, "[AS] frame thread: total %.0f ms = "
                    "%.0f working + %.0f lock-yield;  of the working part "
                    "%.0f ms was XAudio2 back-pressure (%llu Sleep(1) loops) "
                    "and %.0f ms idle" "\n",
                    g_apu_t_iter * k, g_apu_t_locked * k,
                    g_apu_t_lockwait * k, g_apu_t_backpressure * k,
                    g_apu_backpressure_loops, g_apu_t_idle * k);
            fprintf(stderr, "[AS] XAudio2: %llu submits attempted, %llu accepted, "
                    "%llu refused (queue full).  %d samples per submit, one submit "
                    "per 8 APU frames = %d pipeline samples" "\n",
                    g_xa2_attempts, g_xa2_accepted, g_xa2_full,
                    xa2_get_buffer_size(), 8 * 32);
            fprintf(stderr, "[AS] audio content: %llu submissions carried signal, %llu were silence; peak |sample| = %d of 32767; software-mixer voices active = %d" "\n",
                    g_xa2_audible, g_xa2_silent, g_xa2_peak,
                    g_mixer_active_count);
        }
        {   /* the vblank raiser claims 60 Hz -- does it deliver it? */
            extern unsigned long long g_vbl_ticks, g_vbl_qpc0, g_vbl_qpc1, g_vbl_resyncs;
            extern double apu_qpc_ms_scale(void);
            double ms = (double)(g_vbl_qpc1 - g_vbl_qpc0) * apu_qpc_ms_scale();
            fprintf(stderr, "[AS] vblank raiser: %llu ticks in %.0f ms = %.1f Hz "
                    "(target 59.94 Hz); mean period %.3f ms, %llu resyncs" "\n",
                    g_vbl_ticks, ms,
                    ms > 0.0 ? (double)g_vbl_ticks * 1000.0 / ms : 0.0,
                    g_vbl_ticks > 1 ? ms / (double)(g_vbl_ticks - 1) : 0.0,
                    g_vbl_resyncs);
        }
        fprintf(stderr, "[AS] throttle: calls=%llu rebases=%llu waits=%llu  "
                "asked %llu us, slept %llu us (%.2fx), worst %u us, %llu tail spins" "\n",
                g_thr_calls, g_thr_rebases, g_thr_waits,
                g_thr_req_us, g_thr_act_us,
                g_thr_req_us ? (double)g_thr_act_us / (double)g_thr_req_us : 0.0,
                g_thr_worst_us, g_thr_spins);
    }
    fprintf(stderr, "[AS] audio SUBMITs, ms since that stamp:" "\n");
    for (i = 0; i < g_as_subs; ++i)
        fprintf(stderr, "[AS]   #%u  %6u ms  clock %5u  xmv %llu  slot %08X" "\n",
                i, g_as_sub[i].ms, g_as_sub[i].clock, g_as_sub[i].call,
                g_as_sub[i].slot);
    fprintf(stderr, "[AS] audio RETIREs, ms since that stamp:" "\n");
    for (i = 0; i < g_as_rets; ++i)
        fprintf(stderr, "[AS]   #%u  %6u ms  clock %5u  xmv %llu  slot %08X" "\n",
                i, g_as_ret[i].ms, g_as_ret[i].clock, g_as_ret[i].call,
                g_as_ret[i].slot);
    fflush(stderr);
}

/* XMV frame presentation, stage 1: what does a published frame contain?
 *
 * At loc_004E2DB9 the decoder rotates its ring -- [+0x6C] = [+0x68], then
 * [+0x68] = 0 -- so the frame being published is the [+0x68] value read
 * BEFORE the rotation.  This records that pointer, the decoder header, and
 * a content check of the memory it points at, so "the decoded surface is
 * itself black" can be separated from everything downstream.
 */
#define VP_LOG 6u

struct vp_pub {
    uint32_t dec, f60, f64, f68, f6C, slot;
    uint32_t hdr[24];        /* decoder +0x00 .. +0x5C */
    uint32_t buf[8];         /* first dwords at the frame pointer */
    unsigned nonzero, sampled;
    uint32_t hash;
    uint32_t clock;
};
static struct vp_pub g_vp[VP_LOG];
static unsigned g_vp_n;
static unsigned long long g_vp_pubs;

/* Count non-zero bytes over a span, cheaply and without faulting: the
 * guest heap is a flat mapping, so bound the walk by the arena rather
 * than trusting the pointer. */
static void vp_scan(uint32_t base, unsigned bytes, unsigned *nonzero,
                    unsigned *sampled, uint32_t *hash)
{
    unsigned i;
    uint32_t h = 2166136261u;
    *nonzero = 0; *sampled = 0; *hash = 0;
    if (base < 0x10000u || base > 0x07F00000u) return;
    for (i = 0; i < bytes; i += 4u) {
        uint32_t v = MEM32(base + i);
        ++(*sampled);
        if (v) ++(*nonzero);
        h = (h ^ v) * 16777619u;
    }
    *hash = h;
}

void recomp_vp_publish(uint32_t dec)
{
    struct vp_pub *p;
    unsigned i;
    uint32_t devp, frame;
    ++g_vp_pubs;
    if (g_vp_n >= VP_LOG) return;
    p = &g_vp[g_vp_n++];
    memset(p, 0, sizeof(*p));
    p->dec = dec;
    p->f60 = MEM32(dec + 0x60); p->f64 = MEM32(dec + 0x64);
    p->f68 = MEM32(dec + 0x68); p->f6C = MEM32(dec + 0x6C);
    p->slot = MEM32(dec + 0x148);
    devp = MEM32(0x5499E8u);
    p->clock = devp ? MEM32(devp + 0x1DE8u) : 0u;
    for (i = 0; i < 24u; ++i) p->hdr[i] = MEM32(dec + i * 4u);
    frame = p->f68;                    /* the one about to be published */
    for (i = 0; i < 8u; ++i)
        p->buf[i] = (frame >= 0x10000u && frame < 0x07F00000u)
                    ? MEM32(frame + i * 4u) : 0xDEADDEADu;
    vp_scan(frame, 0x10000u, &p->nonzero, &p->sampled, &p->hash);
}

void recomp_dump_vp(void)
{
    unsigned i, j;
    if (g_vp_pubs == 0ull) return;
    fprintf(stderr, "[VP] %llu publications at loc_004E2DB9; first %u logged" "\n",
            g_vp_pubs, g_vp_n);
    for (i = 0; i < g_vp_n; ++i) {
        struct vp_pub *p = &g_vp[i];
        fprintf(stderr, "[VP] #%u clock=%u dec=%08X  ring +60=%08X +64=%08X +68=%08X +6C=%08X  [+148]=%08X" "\n",
                i, p->clock, p->dec, p->f60, p->f64, p->f68, p->f6C, p->slot);
        fprintf(stderr, "[VP]    published frame %08X: %u of %u dwords non-zero, hash %08X" "\n",
                p->f68, p->nonzero, p->sampled, p->hash);
        fprintf(stderr, "[VP]    first dwords:");
        for (j = 0; j < 8u; ++j) fprintf(stderr, " %08X", p->buf[j]);
        fprintf(stderr, "\n");
        fprintf(stderr, "[VP]    decoder header:");
        for (j = 0; j < 24u; ++j) {
            if ((j % 8u) == 0u) fprintf(stderr, "\n" "[VP]      +%02X:", j * 4u);
            fprintf(stderr, " %08X", p->hdr[j]);
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* TEMPORARY: the 8x8 coefficient block going into the XMV IDCT.
 *
 * sub_004E5928 is `ret 12`: arg1 [ebp+0x74] is the output block, arg2
 * [ebp+0x78] the input coefficients, arg3 [ebp+0x7C] a nonzero-column
 * mask.  Both blocks are 8x8 int16 with a 16-byte row stride.
 *
 * Every MMX helper is bit-exact against the host CPU and the IDCT constant
 * tables are correct, so this decides between "the coefficients arriving
 * are already wrong" (bitstream/VLC decode) and "they are fine and
 * something after the transform mishandles them".
 *
 * Reads guest memory only -- no GPU work, so it cannot stall the pipeline
 * the way the earlier after-draw readback did. */
#define IDCT_BLOCKS 8u

static int g_idct_on = -1;
static unsigned g_idct_seen;
static uint32_t g_idct_out_ptr, g_idct_in_ptr, g_idct_mask;
static int g_idct_open;

static void idct_dump_block(const char *tag, uint32_t base, int full)
{
    int16_t c[64];
    unsigned r, k;
    int dc; unsigned nz = 0; int maxac = 0; long sum = 0;

    if (base < 0x1000u || base > 0x07F00000u) {
        fprintf(stderr, "[IDCT] %s: bad pointer %08X" "\n", tag, base);
        return;
    }
    for (r = 0; r < 8u; ++r)
        for (k = 0; k < 8u; ++k)
            c[r * 8u + k] = (int16_t)MEM16(base + r * 16u + k * 2u);

    dc = c[0];
    for (k = 0; k < 64u; ++k) {
        int v = c[k], av = v < 0 ? -v : v;
        sum += av;
        if (k == 0u) continue;
        if (v) ++nz;
        if (av > maxac) maxac = av;
    }
    fprintf(stderr, "[IDCT] %s @%08X  DC=%6d  nonzero AC=%2u/63  max|AC|=%6d  sum|c|=%ld" "\n", tag, base, dc, nz, maxac, sum);
    if (full) {
        for (r = 0; r < 8u; ++r) {
            fprintf(stderr, "[IDCT]    ");
            for (k = 0; k < 8u; ++k)
                fprintf(stderr, " %6d", c[r * 8u + k]);
            fprintf(stderr, "\n");
        }
    }
}

void recomp_idct_in(uint32_t out_ptr, uint32_t in_ptr, uint32_t mask)
{
    if (g_idct_on < 0) g_idct_on = (getenv("CONKER_IDCT_DUMP") != 0);
    if (!g_idct_on || g_idct_seen >= IDCT_BLOCKS) { g_idct_open = 0; return; }
    g_idct_out_ptr = out_ptr; g_idct_in_ptr = in_ptr; g_idct_mask = mask;
    g_idct_open = 1;
    fprintf(stderr, "[IDCT] ---- block %u ----  in=%08X out=%08X column-mask=%08X" "\n", g_idct_seen, in_ptr, out_ptr, mask);
    idct_dump_block("BEFORE", in_ptr, 1);
}

void recomp_idct_out(void)
{
    if (!g_idct_open) return;
    g_idct_open = 0;
    idct_dump_block("AFTER ", g_idct_out_ptr, 1);
    ++g_idct_seen;
    fflush(stderr);
}

/* TEMPORARY: one macroblock through motion compensation, sub_004E7C54.
 *
 * EIGHT args, not seven: the eighth is pushed before a conditional branch
 * well above the other seven, so a narrow look at the call site misses it.
 * The callee reads [ebp+0x24] eight times, so it is load-bearing.
 *
 *   arg1 [ebp+08] source pointer        arg5 [ebp+18]
 *   arg2 [ebp+0C] source pitch          arg6 [ebp+1C]
 *   arg3 [ebp+10] destination pointer   arg7 [ebp+20]
 *   arg4 [ebp+14] destination pitch     arg8 [ebp+24]
 *
 * Samples are captured into memory and printed later, from the dump chain.
 * Writing them inline made every logged call do stderr I/O inside the
 * decoder, which slowed the guest thread enough to crash the run 5 times
 * out of 5 -- in guest code, at an address unrelated to this probe.  The
 * measurement has to stay cheap enough not to change what it measures. */
#define MC_LOG 8u

struct mc_rec {
    uint32_t a[8];
    unsigned long long call;
    uint8_t src0[16], src1[16], before[16], after[16], after1[16];
    int have_after;
};
static struct mc_rec g_mcb[MC_LOG];
static unsigned g_mcb_n;
static int g_mc_on = -1;
static unsigned long long g_mc_calls, g_mc_from;
static struct mc_rec *g_mc_cur;
static unsigned long long g_mc_total, g_mc_zero_mv, g_mc_nonzero_mv;
static unsigned long long g_mc_a5nz, g_mc_a6nz, g_mc_a7nz, g_mc_oddpitch;
static int32_t g_mc_dmin = 0x7FFFFFFF, g_mc_dmax = -0x7FFFFFFF;

/* Args parked at entry, consumed at exit.  sub_004E7C54 does not
 * recurse and the decoder runs on one thread, so a single slot is
 * enough; a nested call would simply lose a sample. */
static uint32_t g_ip_p[7];
static int g_ip_pending;

void recomp_hx_arm(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
void recomp_vd_arm(unsigned, uint32_t, uint32_t, uint32_t, uint32_t);
void recomp_cl_note(uint32_t, uint32_t, uint32_t, uint32_t,
                    uint32_t, uint32_t, uint32_t, uint32_t);
static void ip_check(uint32_t, uint32_t, uint32_t, uint32_t,
                     uint32_t, uint32_t, uint32_t);

static void mc_grab(uint8_t *dst, uint32_t p)
{
    unsigned i;
    /* Only the first 64 MB is backed; above it the guest window is
     * reserved and uncommitted, so reading there would fault. */
    if (p < 0x10000u || (uint64_t)p + 16u > 0x04000000u) {
        memset(dst, 0xEE, 16u);
        return;
    }
    for (i = 0; i < 16u; ++i) dst[i] = (uint8_t)MEM8(p + i);
}

void recomp_mc_enter(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                     uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8)
{
    struct mc_rec *r;
    ++g_mc_calls;
    if (g_mc_on < 0) {
        const char *s = getenv("CONKER_MC_FROM");
        g_mc_on = (s != 0);
        g_mc_from = 0ull;
        if (s) { const char *q = s;
                 while (*q >= 0x30 && *q <= 0x39)
                     g_mc_from = g_mc_from * 10ull +
                                 (unsigned long long)(*q++ - 0x30); }
    }
    /* Tally EVERY call, not just the logged ones: eight adjacent blocks can
     * legitimately all be static background, so a sample cannot answer
     * whether the motion vector is ever applied at all.  The reference sits
     * exactly one frame buffer (0x70800) below the destination, so any real
     * displacement shows up as a delta from that. */
    if (g_mc_on > 0) {
        int32_t d = (int32_t)(a3 - a1) - 0x70800;
        ++g_mc_total;
        if (d == 0) ++g_mc_zero_mv; else ++g_mc_nonzero_mv;
        if (d < g_mc_dmin) g_mc_dmin = d;
        if (d > g_mc_dmax) g_mc_dmax = d;
        if (a5) ++g_mc_a5nz;
        if (a6) ++g_mc_a6nz;
        if (a7) ++g_mc_a7nz;
        if (a2 != 640u) ++g_mc_oddpitch;
        /* Only the reference-fetch shape carries interpolation; the
         * residual-only shape has a stack source at pitch 16.  The
         * comparison itself has to wait for the exit hook: at entry the
         * destination still holds the previous contents, and checking
         * against those made the all-zero-flags control match "copy" for
         * 14% of pixels instead of all of them. */
        recomp_cl_note(a2, a4, a5, a6, a7, a1, a3, a8);
        if (a2 == 640u && a5 == 0u && a6 != 0u && a7 == 0u)
            recomp_vd_arm(1u, a1, a2, a3, a8);
        if (a2 == 640u && a5 != 0u && a6 != 0u && a7 == 0u)
            recomp_vd_arm(2u, a1, a2, a3, a8);
        if (a2 == 640u && a5 != 0u && a6 == 0u && a7 == 0u)
            recomp_hx_arm(a1, a2, a3, a4, a8);
        if (a2 == 640u) {
            g_ip_p[0] = a1; g_ip_p[1] = a2; g_ip_p[2] = a3;
            g_ip_p[3] = a5; g_ip_p[4] = a6; g_ip_p[5] = a7;
            g_ip_p[6] = a8; g_ip_pending = 1;
        } else {
            g_ip_pending = 0;
        }
    }
    g_mc_cur = 0;
    if (!g_mc_on || g_mc_calls < g_mc_from || g_mcb_n >= MC_LOG) return;
    r = &g_mcb[g_mcb_n++];
    memset(r, 0, sizeof(*r));
    r->call = g_mc_calls;
    r->a[0] = a1; r->a[1] = a2; r->a[2] = a3; r->a[3] = a4;
    r->a[4] = a5; r->a[5] = a6; r->a[6] = a7; r->a[7] = a8;
    mc_grab(r->src0, a1);
    mc_grab(r->src1, a1 + a2);
    mc_grab(r->before, a3);
    g_mc_cur = r;
}

void recomp_mc_exit(void)
{
    if (g_ip_pending) {
        g_ip_pending = 0;
        ip_check(g_ip_p[0], g_ip_p[1], g_ip_p[2],
                 g_ip_p[3], g_ip_p[4], g_ip_p[5], g_ip_p[6]);
    }
    struct mc_rec *r = g_mc_cur;
    if (!r) return;
    g_mc_cur = 0;
    mc_grab(r->after, r->a[2]);
    mc_grab(r->after1, r->a[2] + r->a[3]);
    r->have_after = 1;
}

static void mc_show(const char *tag, const uint8_t *b)
{
    unsigned i;
    fprintf(stderr, "[MCB]   %-11s", tag);
    for (i = 0; i < 16u; ++i) fprintf(stderr, " %02X", b[i]);
    fprintf(stderr, "\n");
}

/* TEMPORARY: is the half-pel interpolation in sub_004E7C54 correct?
 *
 * The function does reference fetch AND residual add in one call, so the
 * interpolated pixels cannot be observed on their own.  Instead the whole
 * result is predicted from the raw source and the residual, under each
 * candidate interpolation, and compared with what actually landed:
 *
 *   copy   s0[x]
 *   horiz  (s0[x] + s0[x+1] + 1) >> 1
 *   vert   (s0[x] + s1[x] + 1) >> 1
 *   diag   (s0[x] + s0[x+1] + s1[x] + s1[x+1] + 2) >> 2
 *
 * pavgb rounding (+1 before the shift) is assumed, and a truncating variant
 * is tallied beside it so the data decides which it is.
 *
 * Calls with every flag zero are tallied too, as a control: those must
 * match "copy" exactly.  If they do not, the harness is wrong rather than
 * the title, and nothing else in the table can be trusted. */
#define IP_COMBOS 8u
#define IP_HYP    13u

static const char *ip_hyp_name[IP_HYP] = {
    "copy", "horiz+1", "vert+1", "diag+2", "horizTr",
    "horizBack", "vertBack", "diagBack",
    "copyNoR", "horizNoR", "vertNoR", "diagNoR",
    "bicubicH" };
static unsigned long long g_ip_calls[IP_COMBOS];
static unsigned long long g_ip_px[IP_COMBOS];
static unsigned long long g_ip_hit[IP_COMBOS][IP_HYP];
static uint32_t g_ip_first_bad[IP_COMBOS];
static int g_ip_have_bad[IP_COMBOS];
static unsigned long long g_ip_v5[8], g_ip_v6[8], g_ip_v7[8];
static unsigned long long g_ip_o5, g_ip_o6, g_ip_o7;

static int ip_clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void ip_check(uint32_t src, uint32_t sp, uint32_t dst,
                     uint32_t a5, uint32_t a6, uint32_t a7, uint32_t res)
{
    unsigned combo, x, h;
    /* Pixels -1..9: the bicubic tap for x=7 reaches source pixel 9. */
    int s0[11], s1[11], sm[11], out[8], rz[8];

    if (src < 0x10000u + sp + 1u ||
        (uint64_t)src + 2u * sp + 32u > 0x04000000u) return;
    if (dst < 0x10000u || (uint64_t)dst + 16u > 0x04000000u) return;
    if (res < 0x10000u || (uint64_t)res + 16u > 0x04000000u) return;

    combo = (a5 ? 1u : 0u) | (a6 ? 2u : 0u) | (a7 ? 4u : 0u);
    if (a5 < 8u) ++g_ip_v5[a5]; else ++g_ip_o5;
    if (a6 < 8u) ++g_ip_v6[a6]; else ++g_ip_o6;
    if (a7 < 8u) ++g_ip_v7[a7]; else ++g_ip_o7;
    ++g_ip_calls[combo];

    /* One pixel to the left and one row above are read too, so the
     * "source pair off by one" hypotheses can be evaluated. */
    for (x = 0; x < 11u; ++x) {
        sm[x] = (int)MEM8(src - 1u - sp + x);
        s0[x] = (int)MEM8(src - 1u + x);
        s1[x] = (int)MEM8(src - 1u + sp + x);
    }
    for (x = 0; x < 8u; ++x) {
        out[x] = (int)MEM8(dst + x);
        rz[x]  = (int)(int16_t)MEM16(res + x * 2u);
    }

    for (x = 0; x < 8u; ++x) {
        int cand[IP_HYP];
        /* s0[i] is source pixel i-1: the arrays start one to the left. */
        int p = (int)x + 1;
        cand[0] = s0[p];
        cand[1] = (s0[p] + s0[p + 1] + 1) >> 1;
        cand[2] = (s0[p] + s1[p] + 1) >> 1;
        cand[3] = (s0[p] + s0[p + 1] + s1[p] + s1[p + 1] + 2) >> 2;
        cand[4] = (s0[p] + s0[p + 1]) >> 1;
        cand[5] = (s0[p - 1] + s0[p] + 1) >> 1;
        cand[6] = (sm[p] + s0[p] + 1) >> 1;
        cand[7] = (s0[p - 1] + s0[p] + sm[p - 1] + sm[p] + 2) >> 2;
        /* Same four, but with no residual added -- in case the half-pel
         * path leaves the residual to a later call. */
        cand[8]  = cand[0]; cand[9] = cand[1];
        cand[10] = cand[2]; cand[11] = cand[3];
        /* The sequence sub_004E7C54 actually runs for arg5=1,arg6=0:
         *   widen unsigned against a zeroed mm7
         *   (C1*(s[x]+s[x+1]) - (s[x-1]+s[x+2]) + C2) >> 4   arithmetic shift
         *   packuswb  -- saturates to 0..255 BEFORE the residual
         *   then widen again, add residual, packuswb again
         * C1 and C2 are 16-bit lanes at 0x4EA4A8 and 0x4EA4A0. */
        { int c1 = (int)(int16_t)MEM16(0x004EA4A8u);
          int c2 = (int)(int16_t)MEM16(0x004EA4A0u);
          int v = (c1 * (s0[p] + s0[p + 1])
                   - (s0[p - 1] + s0[p + 2]) + c2) >> 4;
          cand[12] = ip_clamp(v); }
        ++g_ip_px[combo];
        for (h = 0; h < IP_HYP; ++h) {
            int want = (h >= 8u) ? ip_clamp(cand[h])
                                 : ip_clamp(cand[h] + rz[x]);
            if (want == out[x]) ++g_ip_hit[combo][h];
        }
        if (!g_ip_have_bad[combo] && ip_clamp(cand[0] + rz[x]) != out[x]) {
            g_ip_have_bad[combo] = 1;
            g_ip_first_bad[combo] = dst + x;
        }
    }
}

/* TEMPORARY: one horizontal-half-pel call, instruction by instruction.
 *
 * A scalar model of the sequence read out of loc_004E7D87 matches only 26%
 * of pixels, so either the model or the lift is wrong.  Rather than argue
 * from aggregates, this snapshots every MMX intermediate of the FIRST row
 * of ONE call and prints it beside the scalar prediction, so the first
 * step where they diverge is visible directly. */
#define HX_STEPS 14u

static int g_hx_state;        /* 0 idle, 1 armed, 2 captured */
static unsigned g_hx_row;
static uint64_t g_hx_v[HX_STEPS];
static int g_hx_seen[HX_STEPS];
static uint32_t g_hx_src, g_hx_sp, g_hx_dst, g_hx_dp, g_hx_res;
static uint8_t g_hx_srcb[11];
static int16_t g_hx_resw[8];

static const char *hx_name[HX_STEPS] = {
    "mm0 unpack src[x..x+3]", "mm2 unpack src[x+1..x+4]",
    "mm0 = s[x]+s[x+1]",      "mm0 *= 9",
    "mm2 = s[x+2]+s[x-1]",    "mm0 -= mm2",
    "mm0 += 8",               "mm0 >>= 4 (psraw)",
    "mm0 packuswb (pred)",    "mm0 widen pred",
    "mm1 = residual",         "mm1 += pred",
    "mm1 packuswb (stored)",  "mm7 (must be 0)" };

void recomp_hx_arm(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                   uint32_t a8)
{
    unsigned i;
    if (g_hx_state != 0) return;
    if (a1 < 0x10001u || (uint64_t)a1 + 32u > 0x04000000u) return;
    if (a8 < 0x10000u || (uint64_t)a8 + 16u > 0x04000000u) return;
    g_hx_state = 1;
    g_hx_row = 0u;
    g_hx_src = a1; g_hx_sp = a2; g_hx_dst = a3; g_hx_dp = a4; g_hx_res = a8;
    for (i = 0; i < 11u; ++i) g_hx_srcb[i] = (uint8_t)MEM8(a1 - 1u + i);
    for (i = 0; i < 8u; ++i) g_hx_resw[i] = (int16_t)MEM16(a8 + i * 2u);
}

void recomp_hx(unsigned step, uint64_t v)
{
    if (g_hx_state != 1 || g_hx_row != 0u || step >= HX_STEPS) return;
    g_hx_v[step] = v;
    g_hx_seen[step] = 1;
}

void recomp_hx_row(void)
{
    if (g_hx_state != 1) return;
    if (++g_hx_row >= 1u) g_hx_state = 2;   /* first row only */
}

static int hx_clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void recomp_dump_hx(void)
{
    unsigned i;
    int first_bad = -1;
    if (g_hx_state == 0) return;

    fprintf(stderr, "[HX] horizontal half-pel, one call, first row" "\n");
    fprintf(stderr, "[HX]   src=%08X srcPitch=%u  dst=%08X dstPitch=%u  residual=%08X" "\n",
            g_hx_src, g_hx_sp, g_hx_dst, g_hx_dp, g_hx_res);
    fprintf(stderr, "[HX]   source bytes s[-1..9]:");
    for (i = 0; i < 11u; ++i) fprintf(stderr, " %02X", g_hx_srcb[i]);
    fprintf(stderr, "\n" "[HX]   residual int16:");
    for (i = 0; i < 8u; ++i) fprintf(stderr, " %6d", g_hx_resw[i]);
    fprintf(stderr, "\n");

    for (i = 0; i < HX_STEPS; ++i) {
        uint64_t v = g_hx_v[i];
        unsigned k;
        if (!g_hx_seen[i]) {
            fprintf(stderr, "[HX]   %-24s <not reached>" "\n", hx_name[i]);
            continue;
        }
        fprintf(stderr, "[HX]   %-24s %016llX  w:", hx_name[i],
                (unsigned long long)v);
        for (k = 0; k < 4u; ++k)
            fprintf(stderr, " %6d", (int)(int16_t)(uint16_t)(v >> (k * 16)));
        fprintf(stderr, "   b:");
        for (k = 0; k < 8u; ++k)
            fprintf(stderr, " %02X", (unsigned)((v >> (k * 8)) & 0xFFu));
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "[HX] pixel | s[-1] s[0] s[1] s[2] | resid | scalar | actual" "\n");
    for (i = 0; i < 8u; ++i) {
        int sm1 = g_hx_srcb[i];        /* array starts at s[-1] */
        int s0  = g_hx_srcb[i + 1];
        int s1  = g_hx_srcb[i + 2];
        int s2  = g_hx_srcb[i + 3];
        int pred = hx_clamp((9 * (s0 + s1) - (sm1 + s2) + 8) >> 4);
        int scal = hx_clamp(pred + g_hx_resw[i]);
        int act  = g_hx_seen[12]
                   ? (int)((g_hx_v[12] >> (i * 8)) & 0xFFu) : -1;
        fprintf(stderr, "[HX]   %u   |  %3d  %3d  %3d  %3d | %5d | %6d | %6d%s" "\n",
                i, sm1, s0, s1, s2, g_hx_resw[i], scal, act,
                (act >= 0 && act != scal) ? "   <-- MISMATCH" : "");
        if (first_bad < 0 && act >= 0 && act != scal) first_bad = (int)i;
    }
    if (first_bad >= 0)
        fprintf(stderr, "[HX] first mismatching pixel: %d" "\n", first_bad);
    else
        fprintf(stderr, "[HX] every pixel matches the scalar model" "\n");
    fflush(stderr);
}

/* TEMPORARY: vertical and diagonal half-pel, one call each, first row,
 * instruction by instruction -- the same single-call method that proved the
 * horizontal branch correct.  Aggregate scores are not used.
 *
 * vertical  (loc_004E7F0A): taps are rows -1,0,+1,+2 at the same x.
 * diagonal  (loc_004E8194): two passes.  Pass 1 runs the horizontal filter
 *   over ELEVEN rows and packuswb-saturates each to bytes into the aligned
 *   scratch at [ebp-4] with stride 8.  Pass 2 runs the vertical filter over
 *   that scratch (taps +0,+8,+0x10,+0x18) and adds the residual.
 *   Pass 2 is checked against the ACTUAL captured intermediate, so a wrong
 *   pass 1 cannot mask a wrong pass 2; pass 1 is checked separately. */
#define VD_STEPS 14u

static int g_vd_state;         /* 0 idle, 1 armed, 2 captured */
static int g_vd_done[4];
static unsigned g_vd_mode;     /* 1 vertical, 2 diagonal */
static unsigned g_vd_row;
static uint64_t g_vd_v[VD_STEPS];
static int g_vd_seen[VD_STEPS];
static uint32_t g_vd_src, g_vd_sp, g_vd_dst, g_vd_res;
/* x = -1..9: the horizontal tap for x=7 reaches source pixel 9, which a
 * 10-byte row does not hold -- the same off-by-one that already cost a
 * round on ip_check. */
static uint8_t g_vd_rows[11][11];
static uint8_t g_vd_inter[4][8];    /* diagonal: captured scratch rows */
static int g_vd_have_inter;
static int16_t g_vd_resw[8];

static const char *vd_name[VD_STEPS] = {
    "mm0 unpack tapA", "mm2 unpack tapB", "mm0 = A+B", "mm0 *= 9",
    "mm2 = outer pair", "mm0 -= mm2", "mm0 += 8", "mm0 >>= 4",
    "mm0 packuswb (pred)", "mm0 widen pred", "mm1 = residual",
    "mm1 += pred", "mm1 packuswb (stored)", "mm7 (must be 0)" };

void recomp_vd_arm(unsigned mode, uint32_t a1, uint32_t a2, uint32_t a3,
                   uint32_t a8)
{
    unsigned r, i;
    /* One capture per MODE, so a run yields both branches rather than
     * whichever the decoder happened to reach first. */
    if (g_vd_done[mode & 3u]) return;
    if (g_vd_state != 0) return;
    /* rows -1..9 and x -1..8 must all be inside the mapping */
    if (a1 < 0x10000u + a2 + 1u) return;
    if ((uint64_t)a1 + 10u * a2 + 16u > 0x04000000u) return;
    if (a8 < 0x10000u || (uint64_t)a8 + 16u > 0x04000000u) return;
    g_vd_state = 1; g_vd_mode = mode; g_vd_row = 0u;
    g_vd_done[mode & 3u] = 1;
    g_vd_src = a1; g_vd_sp = a2; g_vd_dst = a3; g_vd_res = a8;
    for (r = 0; r < 11u; ++r)
        for (i = 0; i < 11u; ++i)
            g_vd_rows[r][i] =
                (uint8_t)MEM8(a1 + (r - 1) * a2 - 1u + i);
    for (i = 0; i < 8u; ++i) g_vd_resw[i] = (int16_t)MEM16(a8 + i * 2u);
}

void recomp_vd(unsigned step, uint64_t v)
{
    if (g_vd_state != 1 || g_vd_row != 0u || step >= VD_STEPS) return;
    g_vd_v[step] = v; g_vd_seen[step] = 1;
}

/* Called at pass 2 row 0 of the diagonal with the scratch base. */
void recomp_vd_inter(uint32_t scratch)
{
    unsigned r, i;
    if (g_vd_state != 1 || g_vd_row != 0u || g_vd_have_inter) return;
    g_vd_have_inter = 1;
    for (r = 0; r < 4u; ++r)
        for (i = 0; i < 8u; ++i)
            g_vd_inter[r][i] = (uint8_t)MEM8(scratch + r * 8u + i);
}

void recomp_vd_row(void)
{
    if (g_vd_state != 1) return;
    if (++g_vd_row >= 1u) g_vd_state = 2;
}

static int vd_clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int vd_bicubic(int a, int b, int c, int d)
{   /* (9*(b+c) - (a+d) + 8) >> 4, saturated -- the sequence in the code */
    return vd_clamp((9 * (b + c) - (a + d) + 8) >> 4);
}

void recomp_dump_vd(void)
{
    unsigned i;
    int bad = -1;
    if (g_vd_state == 0) return;
    fprintf(stderr, "[VD] %s branch, one call, first row" "\n",
            g_vd_mode == 1u ? "VERTICAL" : "DIAGONAL");
    fprintf(stderr, "[VD]   src=%08X pitch=%u dst=%08X residual=%08X" "\n",
            g_vd_src, g_vd_sp, g_vd_dst, g_vd_res);
    for (i = 0; i < 4u; ++i) {
        unsigned k;
        fprintf(stderr, "[VD]   src row %+d (x=-1..9):", (int)i - 1);
        for (k = 0; k < 11u; ++k) fprintf(stderr, " %02X", g_vd_rows[i][k]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[VD]   residual int16:");
    for (i = 0; i < 8u; ++i) fprintf(stderr, " %6d", g_vd_resw[i]);
    fprintf(stderr, "\n");
    for (i = 0; i < VD_STEPS; ++i) {
        uint64_t v = g_vd_v[i];
        unsigned k;
        if (!g_vd_seen[i]) {
            fprintf(stderr, "[VD]   %-21s <not reached>" "\n", vd_name[i]);
            continue;
        }
        fprintf(stderr, "[VD]   %-21s %016llX  w:", vd_name[i],
                (unsigned long long)v);
        for (k = 0; k < 4u; ++k)
            fprintf(stderr, " %6d", (int)(int16_t)(uint16_t)(v >> (k * 16)));
        fprintf(stderr, "   b:");
        for (k = 0; k < 8u; ++k)
            fprintf(stderr, " %02X", (unsigned)((v >> (k * 8)) & 0xFFu));
        fprintf(stderr, "\n");
    }

    if (g_vd_mode == 2u && g_vd_have_inter) {
        fprintf(stderr, "[VD]   pass 1 intermediate, captured vs expected:" "\n");
        for (i = 0; i < 4u; ++i) {
            unsigned k; int diff = 0;
            fprintf(stderr, "[VD]     row %u  got:", i);
            for (k = 0; k < 8u; ++k)
                fprintf(stderr, " %02X", g_vd_inter[i][k]);
            fprintf(stderr, "  want:");
            for (k = 0; k < 8u; ++k) {
                int w = vd_bicubic(g_vd_rows[i][k], g_vd_rows[i][k + 1],
                                   g_vd_rows[i][k + 2], g_vd_rows[i][k + 3]);
                fprintf(stderr, " %02X", (unsigned)w);
                if (w != (int)g_vd_inter[i][k]) diff = 1;
            }
            fprintf(stderr, "%s" "\n", diff ? "   <-- MISMATCH" : "");
        }
    }

    fprintf(stderr, "[VD] pixel |  tapA tapB outer | resid | scalar | actual" "\n");
    for (i = 0; i < 8u; ++i) {
        int a, b, c, d, pred, scal, act;
        if (g_vd_mode == 1u) {           /* vertical: rows -1,0,+1,+2 at x */
            a = g_vd_rows[0][i + 1]; b = g_vd_rows[1][i + 1];
            c = g_vd_rows[2][i + 1]; d = g_vd_rows[3][i + 1];
        } else {                          /* diagonal pass 2 over the scratch */
            a = g_vd_inter[0][i]; b = g_vd_inter[1][i];
            c = g_vd_inter[2][i]; d = g_vd_inter[3][i];
        }
        pred = vd_bicubic(a, b, c, d);
        scal = vd_clamp(pred + g_vd_resw[i]);
        act  = g_vd_seen[12] ? (int)((g_vd_v[12] >> (i * 8)) & 0xFFu) : -1;
        fprintf(stderr, "[VD]   %u   |  %3d  %3d  %3d %3d | %5d | %6d | %6d%s" "\n",
                i, b, c, a, d, g_vd_resw[i], scal, act,
                (act >= 0 && act != scal) ? "   <-- MISMATCH" : "");
        if (bad < 0 && act >= 0 && act != scal) bad = (int)i;
    }
    if (bad >= 0) fprintf(stderr, "[VD] first mismatching pixel: %d" "\n", bad);
    else fprintf(stderr, "[VD] every pixel matches the scalar model" "\n");
    fflush(stderr);
    /* Release the slot so the other branch is captured after this dump. */
    g_vd_state = 0; g_vd_have_inter = 0;
    memset(g_vd_seen, 0, sizeof(g_vd_seen));
}

/* TEMPORARY: the arg7=1 sub-variants.
 *
 * Each is textually the base filter plus two instructions after the first
 * packuswb -- load a second operand, then pavgb.  The bicubic feeding it is
 * the same instruction sequence already verified bit-exact three times
 * (horizontal, vertical, diagonal pass 1 and pass 2), so what is new here
 * and worth measuring is the composition:
 *
 *   copy+arg7  pavgb(bicubicH(src), src[x])      pavgb site 12452
 *   horiz+arg7 pavgb(bicubicH(src), src[x+1])    pavgb site 12596
 *   vert+arg7  pavgb(vert-over-src, vert-over-scratch)   site 12835
 *   diag+arg7  same shape, source shifted by +1          site 13112
 *
 * Captured per variant: both pavgb operands, its result, the residual, and
 * the stored bytes.  Checked: pavgb(a,b) equals the result, and the store
 * equals clamp(result + residual) per lane. */
#define A7_VARIANTS 4u

static const char *a7_name[A7_VARIANTS] = {
    "copy+arg7", "horiz+arg7", "vert+arg7", "diag+arg7" };
static int g_a7_done[A7_VARIANTS];
static int g_a7_row[A7_VARIANTS];
static uint64_t g_a7_op0[A7_VARIANTS], g_a7_op1[A7_VARIANTS];
static uint64_t g_a7_res[A7_VARIANTS], g_a7_rz[A7_VARIANTS];
static uint64_t g_a7_rz2[A7_VARIANTS], g_a7_store[A7_VARIANTS];
static int g_a7_seen[A7_VARIANTS];

void recomp_a7(unsigned v, unsigned what, uint64_t x)
{
    if (v >= A7_VARIANTS || g_a7_done[v]) return;
    if (g_a7_row[v] != 0) return;
    switch (what) {
    case 0: g_a7_op0[v] = x; break;      /* mm0 before pavgb */
    case 1: g_a7_op1[v] = x; break;      /* the other operand */
    case 2: g_a7_res[v] = x; g_a7_seen[v] = 1; break;  /* after pavgb */
    case 3: g_a7_rz[v]  = x; break;      /* residual words 0..3 */
    case 4: g_a7_rz2[v] = x; break;      /* residual words 4..7 */
    case 5: g_a7_store[v] = x; break;    /* bytes stored */
    default: break;
    }
}

void recomp_a7_row(unsigned v)
{
    if (v >= A7_VARIANTS) return;
    if (++g_a7_row[v] >= 1) g_a7_done[v] = 1;
}

void recomp_dump_a7(void)
{
    unsigned v, i;
    int any = 0;
    for (v = 0; v < A7_VARIANTS; ++v) if (g_a7_seen[v]) any = 1;
    if (!any) return;
    fprintf(stderr, "[A7] arg7=1 sub-variants, one row each" "\n");
    for (v = 0; v < A7_VARIANTS; ++v) {
        uint64_t want, got;
        int bad = -1;
        if (!g_a7_seen[v]) {
            fprintf(stderr, "[A7]   %-11s not reached" "\n", a7_name[v]);
            continue;
        }
        want = mmx_pavgb(g_a7_op0[v], g_a7_op1[v]);
        got  = g_a7_res[v];
        fprintf(stderr, "[A7]   %-11s pavgb(%016llX, %016llX)" "\n",
                a7_name[v], (unsigned long long)g_a7_op0[v],
                (unsigned long long)g_a7_op1[v]);
        fprintf(stderr, "[A7]                 got %016llX  want %016llX  %s" "\n",
                (unsigned long long)got, (unsigned long long)want,
                got == want ? "ok" : "MISMATCH");
        fprintf(stderr, "[A7]     pixel | pred | resid | scalar | actual" "\n");
        for (i = 0; i < 8u; ++i) {
            int pred = (int)((got >> (i * 8)) & 0xFFu);
            int rz = (i < 4u)
                ? (int)(int16_t)(uint16_t)(g_a7_rz[v] >> (i * 16))
                : (int)(int16_t)(uint16_t)(g_a7_rz2[v] >> ((i - 4u) * 16));
            int scal = pred + rz;
            int act = (int)((g_a7_store[v] >> (i * 8)) & 0xFFu);
            scal = scal < 0 ? 0 : (scal > 255 ? 255 : scal);
            fprintf(stderr, "[A7]       %u   | %4d | %5d | %6d | %6d%s" "\n",
                    i, pred, rz, scal, act,
                    scal != act ? "   <-- MISMATCH" : "");
            if (bad < 0 && scal != act) bad = (int)i;
        }
        fprintf(stderr, "[A7]     %s: %s" "\n", a7_name[v],
                (got == want && bad < 0) ? "8 pixels, 0 mismatches"
                                         : "MISMATCH");
    }
    fflush(stderr);
}

/* TEMPORARY: classify every sub_004E7C54 call by pitch and flags.
 *
 * Everything verified so far had srcPitch = 640, i.e. luma.  10605 calls in
 * one sample had a different pitch and were never looked at.  This buckets
 * ALL calls by (srcPitch, dstPitch, arg5, arg6, arg7) with counts and
 * address ranges, so the chroma modes and their planes can be identified
 * before anything is traced. */
#define CL_BUCKETS 24u

struct cl_bucket {
    uint32_t sp, dp, flags;
    unsigned long long calls;
    uint32_t src_lo, src_hi, dst_lo, dst_hi;
    uint32_t res_lo, res_hi;
};
static struct cl_bucket g_cl[CL_BUCKETS];
static unsigned g_cl_n;
static unsigned long long g_cl_dropped;

void recomp_cl_note(uint32_t sp, uint32_t dp, uint32_t a5, uint32_t a6,
                    uint32_t a7, uint32_t src, uint32_t dst, uint32_t res)
{
    uint32_t flags = (a5 ? 1u : 0u) | (a6 ? 2u : 0u) | (a7 ? 4u : 0u);
    unsigned i;
    for (i = 0; i < g_cl_n; ++i) {
        struct cl_bucket *b = &g_cl[i];
        if (b->sp != sp || b->dp != dp || b->flags != flags) continue;
        ++b->calls;
        if (src < b->src_lo) b->src_lo = src;
        if (src > b->src_hi) b->src_hi = src;
        if (dst < b->dst_lo) b->dst_lo = dst;
        if (dst > b->dst_hi) b->dst_hi = dst;
        if (res < b->res_lo) b->res_lo = res;
        if (res > b->res_hi) b->res_hi = res;
        return;
    }
    if (g_cl_n >= CL_BUCKETS) { ++g_cl_dropped; return; }
    { struct cl_bucket *b = &g_cl[g_cl_n++];
      b->sp = sp; b->dp = dp; b->flags = flags; b->calls = 1;
      b->src_lo = b->src_hi = src;
      b->dst_lo = b->dst_hi = dst;
      b->res_lo = b->res_hi = res; }
}

void recomp_dump_cl(void)
{
    unsigned i;
    if (g_cl_n == 0u) return;
    fprintf(stderr, "[CL] sub_004E7C54 calls by pitch and flags" "\n");
    fprintf(stderr, "[CL]  srcPitch dstPitch a5,a6,a7      calls | src range              | dst range              | residual range" "\n");
    for (i = 0; i < g_cl_n; ++i) {
        struct cl_bucket *b = &g_cl[i];
        fprintf(stderr, "[CL]  %8u %8u   %u,%u,%u %10llu | %08X..%08X | %08X..%08X | %08X..%08X" "\n",
                b->sp, b->dp, b->flags & 1u, (b->flags >> 1) & 1u,
                (b->flags >> 2) & 1u, b->calls,
                b->src_lo, b->src_hi, b->dst_lo, b->dst_hi,
                b->res_lo, b->res_hi);
    }
    if (g_cl_dropped)
        fprintf(stderr, "[CL]  %llu calls in buckets beyond the table" "\n",
                g_cl_dropped);
    fflush(stderr);
}

/* TEMPORARY: which motion-comp sibling handles chroma?
 *
 * sub_004E7C54 turned out to carry only srcPitch 640 (luma reference) and
 * srcPitch 16 (the intra/residual shape whose source is a stack block), and
 * dstPitch 640 throughout -- so no chroma passes through it.  Its siblings
 * in the macroblock loop take the same first six arguments, so the same
 * classification applies to them. */
#define FN_COUNT   5u
#define FN_BUCKETS 8u

static const char *fn_label[FN_COUNT] = {
    "sub_004E75E7", "sub_004E6E8B", "sub_004E6C0A",
    "sub_004E7402", "sub_004E7203" };

struct fn_bucket {
    uint32_t sp, dp;
    unsigned long long calls;
    uint32_t src_lo, src_hi, dst_lo, dst_hi;
};
static struct fn_bucket g_fn[FN_COUNT][FN_BUCKETS];
static unsigned g_fn_n[FN_COUNT];
static unsigned long long g_fn_calls[FN_COUNT];

void recomp_fn_note(unsigned fn, uint32_t src, uint32_t sp,
                    uint32_t dst, uint32_t dp)
{
    unsigned i;
    if (fn >= FN_COUNT) return;
    ++g_fn_calls[fn];
    for (i = 0; i < g_fn_n[fn]; ++i) {
        struct fn_bucket *b = &g_fn[fn][i];
        if (b->sp != sp || b->dp != dp) continue;
        ++b->calls;
        if (src < b->src_lo) b->src_lo = src;
        if (src > b->src_hi) b->src_hi = src;
        if (dst < b->dst_lo) b->dst_lo = dst;
        if (dst > b->dst_hi) b->dst_hi = dst;
        return;
    }
    if (g_fn_n[fn] >= FN_BUCKETS) return;
    { struct fn_bucket *b = &g_fn[fn][g_fn_n[fn]++];
      b->sp = sp; b->dp = dp; b->calls = 1;
      b->src_lo = b->src_hi = src;
      b->dst_lo = b->dst_hi = dst; }
}

void recomp_dump_fn(void)
{
    unsigned f, i;
    int any = 0;
    for (f = 0; f < FN_COUNT; ++f) if (g_fn_calls[f]) any = 1;
    if (!any) return;
    fprintf(stderr, "[FN] motion-comp siblings, by pitch" "\n");
    for (f = 0; f < FN_COUNT; ++f) {
        if (!g_fn_calls[f]) {
            fprintf(stderr, "[FN]   %-13s never called" "\n", fn_label[f]);
            continue;
        }
        for (i = 0; i < g_fn_n[f]; ++i) {
            struct fn_bucket *b = &g_fn[f][i];
            fprintf(stderr, "[FN]   %-13s srcPitch=%-5u dstPitch=%-5u calls=%-9llu src %08X..%08X  dst %08X..%08X" "\n",
                    fn_label[f], b->sp, b->dp, b->calls,
                    b->src_lo, b->src_hi, b->dst_lo, b->dst_hi);
        }
    }
    fflush(stderr);
}

/* TEMPORARY: chroma interpolation in sub_004E6E8B, one call per mode.
 *
 * Dispatch (arg5 = [ebp+0x18] x-fraction, arg6 = [ebp+0x1C] y-fraction):
 *   0,0  loc_004E6EB7  plain copy, 8 rows of 8 bytes
 *   1,0  loc_004E6F0C  horizontal, taps esi and esi+1
 *   0,1  loc_004E6FD5  vertical,   taps esi and esi+srcPitch
 *   1,1  loc_004E70A0  diagonal,   taps esi, +1, +pitch, +pitch+1
 *
 * This is not the luma bicubic.  It is the no-unpack byte average:
 *   two-tap  (a>>1 & M7F) + (b>>1 & M7F) + ((a&b) & M01)
 *   four-tap sum(x>>2 & M3F) + ((sum(x & M03) + round) >> 2 & M03)
 * with psrlw doing a 16-bit shift and the masks stopping the bleed across
 * byte lanes.
 *
 * The operands and the stored result are captured for the first row of one
 * call, then the same sequence is replayed through the hardware-verified
 * mmx_* helpers in the same order.  That checks the composition, which is
 * what a mis-lift breaks; the primitives are already verified.
 *
 * ebp is the row counter inside the diagonal loop, so the arguments are
 * latched at the prologue instead of being read from the frame. */
#define CH_MODES 4u

static const char *ch_name[CH_MODES] = {
    "copy 0,0", "horiz 1,0", "vert 0,1", "diag 1,1" };
static int g_ch_done[CH_MODES], g_ch_seen[CH_MODES];
static unsigned g_ch_best[CH_MODES], g_ch_rows[CH_MODES];
static uint64_t g_ch_op[CH_MODES][4], g_ch_res[CH_MODES];
static uint64_t g_ch_cand[CH_MODES][4];
static uint32_t g_ch_csrc[CH_MODES], g_ch_cdst[CH_MODES];
static uint32_t g_ch_src[CH_MODES], g_ch_dst[CH_MODES];
static uint32_t g_ch_sp[CH_MODES], g_ch_dp[CH_MODES];
static uint32_t g_ch_a5[CH_MODES], g_ch_a6[CH_MODES];
static uint32_t g_ch_pend[6];

void recomp_ch_call(uint32_t src, uint32_t sp, uint32_t dst, uint32_t dp,
                    uint32_t a5, uint32_t a6)
{
    g_ch_pend[0] = src; g_ch_pend[1] = sp; g_ch_pend[2] = dst;
    g_ch_pend[3] = dp;  g_ch_pend[4] = a5; g_ch_pend[5] = a6;
}

/* The same four loops also serve luma-shaped calls (srcPitch 640, and the
 * 16-pitch intra/residual shape).  Chroma is srcPitch = dstPitch = 320, so
 * the capture is gated on the pitch latched at the prologue. */
static int ch_ok(unsigned m)
{
    return m < CH_MODES && !g_ch_done[m]
        && g_ch_pend[1] == 320u && g_ch_pend[3] == 320u;
}

/* Byte spread of the taps.  A row whose source bytes are all but identical
 * cannot separate a correct filter from a wrong one: the first chroma call of
 * each mode turned out to be flat, and the horizontal one had a == b.  So keep
 * scanning and report the single most informative call rather than the first.
 * Still one call, still every intermediate, no pass rate over many calls. */
static unsigned ch_spread(unsigned m)
{
    unsigned n = (m == 3u) ? 4u : ((m == 0u) ? 1u : 2u);
    unsigned k, i, lo = 255u, hi = 0u;
    for (k = 0; k < n; ++k)
        for (i = 0; i < 8u; ++i) {
            unsigned b = (unsigned)((g_ch_cand[m][k] >> (i * 8)) & 0xFFu);
            if (b < lo) lo = b;
            if (b > hi) hi = b;
        }
    return hi - lo;
}

void recomp_ch(unsigned m, unsigned what, uint64_t v)
{
    if (!ch_ok(m)) return;
    if (what < 4u) { g_ch_cand[m][what] = v; return; }
    {
        unsigned s = ch_spread(m);
        if (s > g_ch_best[m]) {
            unsigned k;
            for (k = 0; k < 4u; ++k) g_ch_op[m][k] = g_ch_cand[m][k];
            g_ch_res[m] = v;
            g_ch_best[m] = s;
            g_ch_seen[m] = 1;
            g_ch_src[m] = g_ch_csrc[m]; g_ch_dst[m] = g_ch_cdst[m];
            g_ch_sp[m] = g_ch_pend[1];  g_ch_dp[m] = g_ch_pend[3];
            g_ch_a5[m] = g_ch_pend[4];  g_ch_a6[m] = g_ch_pend[5];
            if (s >= 32u) g_ch_done[m] = 1;   /* informative enough, stop */
        }
        if (++g_ch_rows[m] > 200000u) g_ch_done[m] = 1;
    }
}

void recomp_ch_ptr(unsigned m, uint32_t esi, uint32_t edi)
{
    if (!ch_ok(m)) return;
    g_ch_csrc[m] = esi; g_ch_cdst[m] = edi;
}

void recomp_ch_row(unsigned m)
{
    (void)m;   /* every row is a candidate; the pick happens at the store */
}

void recomp_dump_ch(void)
{
    unsigned m, i;
    int any = 0;
    uint64_t M01, M7F, M03, M3F;
    for (m = 0; m < CH_MODES; ++m) if (g_ch_seen[m]) any = 1;
    if (!any) return;
    M03 = MEM64(0x004EA470u); M3F = MEM64(0x004EA478u);
    M01 = MEM64(0x004EA480u); M7F = MEM64(0x004EA488u);
    fprintf(stderr, "[CH] sub_004E6E8B chroma interpolation, one call per mode" "\n");
    fprintf(stderr, "[CH]   masks 4EA470=%016llX 4EA478=%016llX 4EA480=%016llX 4EA488=%016llX" "\n",
            (unsigned long long)M03, (unsigned long long)M3F,
            (unsigned long long)M01, (unsigned long long)M7F);
    for (m = 0; m < CH_MODES; ++m) {
        uint64_t a, b, c, d, want, got;
        int bad = -1;
        if (!g_ch_seen[m]) {
            fprintf(stderr, "[CH]   %-10s not reached" "\n", ch_name[m]);
            continue;
        }
        a = g_ch_op[m][0]; b = g_ch_op[m][1];
        c = g_ch_op[m][2]; d = g_ch_op[m][3];
        got = g_ch_res[m];
        if (m == 0u) {
            want = a;
        } else if (m == 1u || m == 2u) {
            uint64_t m5 = mmx_pand(a, b);
            uint64_t h0 = mmx_psrlw(a, 1), h1 = mmx_psrlw(b, 1);
            m5 = mmx_pand(m5, M01);
            h0 = mmx_pand(h0, M7F);
            h1 = mmx_pand(h1, M7F);
            want = mmx_paddw(mmx_paddw(h0, m5), h1);
        } else {
            uint64_t lo = mmx_pand(a, M03);
            uint64_t hi;
            lo = mmx_paddw(lo, mmx_pand(b, M03));
            lo = mmx_paddw(lo, mmx_pand(c, M03));
            lo = mmx_paddw(lo, mmx_pand(d, M03));
            lo = mmx_paddw(lo, M01);
            lo = mmx_pand(mmx_psrlw(lo, 2), M03);
            hi = mmx_pand(mmx_psrlw(a, 2), M3F);
            hi = mmx_paddw(hi, mmx_pand(mmx_psrlw(b, 2), M3F));
            hi = mmx_paddw(hi, mmx_pand(mmx_psrlw(c, 2), M3F));
            hi = mmx_paddw(hi, mmx_pand(mmx_psrlw(d, 2), M3F));
            want = mmx_paddw(hi, lo);
        }
        fprintf(stderr, "[CH]   %-10s a5=%u a6=%u src=%08X srcPitch=%u dst=%08X dstPitch=%u"
                        "  tap spread=%u, picked from %u rows" "\n",
                ch_name[m], g_ch_a5[m], g_ch_a6[m], g_ch_src[m],
                g_ch_sp[m], g_ch_dst[m], g_ch_dp[m],
                g_ch_best[m], g_ch_rows[m]);
        fprintf(stderr, "[CH]     taps a=%016llX b=%016llX" "\n",
                (unsigned long long)a, (unsigned long long)b);
        if (m == 3u)
            fprintf(stderr, "[CH]          c=%016llX d=%016llX" "\n",
                    (unsigned long long)c, (unsigned long long)d);
        fprintf(stderr, "[CH]     got  %016llX" "\n", (unsigned long long)got);
        fprintf(stderr, "[CH]     want %016llX" "\n", (unsigned long long)want);
        fprintf(stderr, "[CH]     pixel got/want:");
        for (i = 0; i < 8u; ++i) {
            unsigned gv = (unsigned)((got >> (i * 8)) & 0xFFu);
            unsigned wv = (unsigned)((want >> (i * 8)) & 0xFFu);
            fprintf(stderr, " %02X/%02X", gv, wv);
            if (bad < 0 && gv != wv) bad = (int)i;
        }
        fprintf(stderr, "\n" "[CH]     %-10s | 8 pixels checked | %s" "\n",
                ch_name[m], bad < 0 ? "0 mismatches" : "MISMATCH");
        if (bad >= 0)
            fprintf(stderr, "[CH]     first differing pixel index %d" "\n", bad);
    }
    fflush(stderr);
}

/* TEMPORARY: caller-side chroma motion-vector reduction and address
 * generation.
 *
 * sub_004E49AD reduces the luma MV to chroma precision into a local pair at
 * ebp+0x72 / ebp+0x73, then calls sub_004E4641 three times: once per luma
 * 8x8 quadrant (base [ctx+0xEC]), once for U ([ctx+0xF0]) and once for V
 * ([ctx+0xF4]).  sub_004E4641 is generic: it turns (mv, blockX, blockY,
 * pitch) into the final source pointer and the two half-pel fractions, then
 * dispatches to one of the interpolators.
 *
 * Captured here: the reduction inputs and output, and for one call of each
 * interesting kind the full argument set plus the pointer sub_004E4641
 * actually produced.  The dump reconstructs both independently. */
#define MV_SLOTS 8u

static const char *mv_why[MV_SLOTS] = {
    "U plane, mv >= 0", "V plane, mv >= 0", "mvx < 0", "mvy < 0",
    "mvx odd", "mvy odd", "U plane, any mv", "V plane, any mv" };

struct mv_rec {
    int used;
    int lmvx, lmvy, cmvx, cmvy;      /* reduction in / out */
    unsigned plane;                  /* 0 luma, 1 U, 2 V, 3 unknown */
    uint32_t lbase, ubase, vbase;
    uint32_t base, dst, pitch, width, height;
    int32_t  bx, by, mvx, mvy;
    uint32_t fsrc, fdst, fsp, fdp, a5, a6;
};
static struct mv_rec g_mv[MV_SLOTS];
static struct mv_rec g_mv_cur;
static int g_mv_lmvx, g_mv_lmvy, g_mv_cmvx, g_mv_cmvy, g_mv_red;
static unsigned long long g_mv_calls, g_mv_chroma, g_mv_clamped;
static unsigned long long g_mv_addr_ok, g_mv_addr_bad;
static unsigned long long g_mv_frac_ok, g_mv_frac_bad;
static unsigned long long g_mv_red_ok, g_mv_red_bad;
static unsigned long long g_mv_stale;
static int g_mv_bad_shown;

static int mv_reduce_ref(int mv);

/* the reduction itself, read out of sub_004E49AD */
void recomp_mv_reduce(int lmvx, int lmvy, int cmvx, int cmvy)
{
    g_mv_lmvx = lmvx; g_mv_lmvy = lmvy;
    g_mv_cmvx = cmvx; g_mv_cmvy = cmvy;
    g_mv_red = 1;
    if (mv_reduce_ref(lmvx) == cmvx && mv_reduce_ref(lmvy) == cmvy)
        ++g_mv_red_ok;
    else
        ++g_mv_red_bad;
}

void recomp_mv_enter(uint32_t ctx, uint32_t mvptr, uint32_t bx, uint32_t by,
                     uint32_t width, uint32_t height, uint32_t pitch,
                     uint32_t dst, uint32_t base)
{
    struct mv_rec *r = &g_mv_cur;
    r->lbase = MEM32(ctx + 0xECu);
    r->ubase = MEM32(ctx + 0xF0u);
    r->vbase = MEM32(ctx + 0xF4u);
    r->base = base; r->dst = dst; r->pitch = pitch;
    r->width = width; r->height = height;
    r->bx = (int32_t)bx; r->by = (int32_t)by;
    r->mvx = (int32_t)(int8_t)MEM8(mvptr);
    r->mvy = (int32_t)(int8_t)MEM8(mvptr + 1u);
    r->plane = (base == r->lbase) ? 0u
             : (base == r->ubase) ? 1u
             : (base == r->vbase) ? 2u : 3u;
    r->lmvx = g_mv_lmvx; r->lmvy = g_mv_lmvy;
    r->cmvx = g_mv_cmvx; r->cmvy = g_mv_cmvy;
    r->used = g_mv_red;
}

void recomp_mv_call(uint32_t fsrc, uint32_t fsp, uint32_t fdst, uint32_t fdp,
                    uint32_t a5, uint32_t a6)
{
    struct mv_rec *r = &g_mv_cur;
    unsigned i;
    int fast;
    { extern void recomp_pad_finish(uint32_t, uint32_t, uint32_t, uint32_t);
      recomp_pad_finish(fsrc, fsp, a5, a6); }
    ++g_mv_calls;
    r->fsrc = fsrc; r->fsp = fsp; r->fdst = fdst; r->fdp = fdp;
    r->a5 = a5; r->a6 = a6;
    if (r->plane != 1u && r->plane != 2u) return;
    ++g_mv_chroma;
    /* the border-clamp path rebuilds the reference into a padded stack
     * block and passes pitch 16, so its pointer is not base + offset */
    fast = (fsp == r->pitch);
    if (!fast) { ++g_mv_clamped; return; }
    {
        int32_t xi = (r->mvx + 2 * r->bx) >> 1;
        int32_t yi = (r->mvy + 2 * r->by) >> 1;
        uint32_t esrc = r->base + (uint32_t)(yi * (int32_t)r->pitch)
                      + (uint32_t)xi;
        if (fsrc == esrc) ++g_mv_addr_ok; else ++g_mv_addr_bad;
        if (a5 == (uint32_t)(r->mvx & 1) && a6 == (uint32_t)(r->mvy & 1))
            ++g_mv_frac_ok;
        else
            ++g_mv_frac_bad;
        /* the MV sub_004E4641 reads must be the pair the parent just reduced */
        if (r->mvx != r->cmvx || r->mvy != r->cmvy) ++g_mv_stale;
        if (fsrc != esrc && !g_mv_bad_shown) {
            g_mv_bad_shown = 1;
            fprintf(stderr, "[MV] first address mismatch: plane=%u base=%08X "
                    "bx=%ld by=%ld mv=%ld,%ld pitch=%u want=%08X got=%08X" "\n",
                    r->plane, r->base, (long)r->bx, (long)r->by,
                    (long)r->mvx, (long)r->mvy, r->pitch, esrc, fsrc);
        }
    }
    for (i = 0; i < MV_SLOTS; ++i) {
        int want;
        if (g_mv[i].used) continue;
        switch (i) {
        case 0: want = (r->plane == 1u && r->mvx >= 0 && r->mvy >= 0
                        && (r->mvx | r->mvy) != 0); break;
        case 1: want = (r->plane == 2u && r->mvx >= 0 && r->mvy >= 0
                        && (r->mvx | r->mvy) != 0); break;
        case 2: want = (r->mvx < 0); break;
        case 3: want = (r->mvy < 0); break;
        case 4: want = ((r->mvx & 1) != 0); break;
        case 5: want = ((r->mvy & 1) != 0); break;
        case 6: want = (r->plane == 1u); break;
        case 7: want = (r->plane == 2u); break;
        default: want = 0; break;
        }
        if (want) { g_mv[i] = *r; g_mv[i].used = 1; break; }
    }
}

/* independent reduction, written from the instruction sequence at
 * loc_004E50D8: q = trunc(mv/2); if q is odd keep it, else redo the
 * division on mv moved one step away from zero. */
static int mv_reduce_ref(int mv)
{
    int q = (mv < 0) ? -((-mv) / 2) : (mv / 2);
    if (q & 1) return q;
    { int m = (mv < 0) ? (mv - 1) : (mv + 1);
      return (m < 0) ? -((-m) / 2) : (m / 2); }
}

void recomp_dump_mv(void)
{
    unsigned i;
    int any = 0;
    for (i = 0; i < MV_SLOTS; ++i) if (g_mv[i].used) any = 1;
    if (!any) return;
    fprintf(stderr, "[MV] chroma motion-vector reduction and addressing" "\n");
    fprintf(stderr, "[MV]   sub_004E4641 calls=%llu chroma=%llu border-clamped=%llu" "\n",
            g_mv_calls, g_mv_chroma, g_mv_clamped);
    fprintf(stderr, "[MV]   every chroma fast-path call checked: src %llu ok / %llu wrong,"
                    " fractions %llu ok / %llu wrong, reductions %llu ok / %llu wrong,"
                    " stale MV %llu" "\n",
            g_mv_addr_ok, g_mv_addr_bad, g_mv_frac_ok, g_mv_frac_bad,
            g_mv_red_ok, g_mv_red_bad, g_mv_stale);

    for (i = 0; i < MV_SLOTS; ++i) {
        struct mv_rec *r = &g_mv[i];
        int32_t xi, yi, cx, cy;
        uint32_t esrc;
        int rx, ry;
        if (!r->used) {
            fprintf(stderr, "[MV]   %-18s not seen" "\n", mv_why[i]);
            continue;
        }
        xi = (r->mvx + 2 * r->bx) >> 1;
        yi = (r->mvy + 2 * r->by) >> 1;
        cx = xi - r->bx;
        cy = yi - r->by;
        esrc = r->base + (uint32_t)(yi * (int32_t)r->pitch) + (uint32_t)xi;
        rx = mv_reduce_ref(r->lmvx);
        ry = mv_reduce_ref(r->lmvy);
        fprintf(stderr, "[MV]   --- %s ---" "\n", mv_why[i]);
        fprintf(stderr, "[MV]     plane                  %s" "\n",
                r->plane == 1u ? "U" : "V");
        fprintf(stderr, "[MV]     macroblock X/Y         %ld / %ld  (chroma units, 8 px per block)" "\n",
                (long)(r->bx / 8), (long)(r->by / 8));
        fprintf(stderr, "[MV]     block origin X/Y       %ld / %ld" "\n",
                (long)r->bx, (long)r->by);
        fprintf(stderr, "[MV]     luma MV X/Y            %d / %d  (half-pel, signed byte)" "\n", r->lmvx, r->lmvy);
        fprintf(stderr, "[MV]     reduced chroma MV X/Y  %d / %d  (stored)" "\n",
                r->cmvx, r->cmvy);
        fprintf(stderr, "[MV]     reduction recomputed   %d / %d  %s" "\n",
                rx, ry,
                (rx == r->cmvx && ry == r->cmvy) ? "match" : "MISMATCH");
        fprintf(stderr, "[MV]     MV seen by 004E4641    %ld / %ld  %s" "\n",
                (long)r->mvx, (long)r->mvy,
                (r->mvx == r->cmvx && r->mvy == r->cmvy)
                    ? "same as reduced" : "DIFFERENT from reduced");
        fprintf(stderr, "[MV]     integer displacement   %ld / %ld  (chroma px)" "\n", (long)cx, (long)cy);
        fprintf(stderr, "[MV]     fraction bits          %ld / %ld" "\n",
                (long)(r->mvx & 1), (long)(r->mvy & 1));
        fprintf(stderr, "[MV]     arg5 / arg6 actual     %u / %u  %s" "\n",
                r->a5, r->a6,
                (r->a5 == (uint32_t)(r->mvx & 1)
                 && r->a6 == (uint32_t)(r->mvy & 1)) ? "match" : "MISMATCH");
        fprintf(stderr, "[MV]     luma / U / V bases     %08X / %08X / %08X" "\n",
                r->lbase, r->ubase, r->vbase);
        fprintf(stderr, "[MV]     U-luma=%ld  V-U=%ld  (one 320x240 plane = 76800)" "\n",
                (long)(r->ubase - r->lbase), (long)(r->vbase - r->ubase));
        fprintf(stderr, "[MV]     reference base used    %08X" "\n", r->base);
        fprintf(stderr, "[MV]     chroma x/y             %ld / %ld" "\n",
                (long)xi, (long)yi);
        fprintf(stderr, "[MV]     expected src           %08X  = base + %ld*%u + %ld" "\n",
                esrc, (long)yi, r->pitch, (long)xi);
        fprintf(stderr, "[MV]     actual   src           %08X  %s" "\n",
                r->fsrc, r->fsrc == esrc ? "match" : "MISMATCH");
        fprintf(stderr, "[MV]     dst passed in / out    %08X / %08X  %s" "\n",
                r->dst, r->fdst,
                r->dst == r->fdst ? "unchanged" : "CHANGED");
        fprintf(stderr, "[MV]     dst offset from plane  %ld" "\n",
                (long)(r->fdst - (r->plane == 1u ? r->ubase : r->vbase)));
        fprintf(stderr, "[MV]     src / dst pitch        %u / %u" "\n",
                r->fsp, r->fdp);
        fprintf(stderr, "[MV]     width / height         %u / %u" "\n",
                r->width, r->height);
    }
    fflush(stderr);
}

/* TEMPORARY: the border-clamp path in sub_004E4641 (loc_004E46AA).
 *
 * When the motion-compensated window crosses a plane edge, the function
 * builds an edge-replicated copy on the stack via sub_004E432B and passes
 * that to the interpolator instead of the plane:
 *
 *   scratch window   11 rows x 16 columns at ebp-432
 *   src handed on    ebp-415 = scratch + 17, i.e. row 1 column 1
 *   srcPitch         16
 *
 * The window origin is (X_int - 1, Y_int - 1), the same one-pixel border the
 * interpolator taps need.  Two fill directions exist: for a top overrun the
 * builder starts at scratch row 10 and walks up (row step -16), otherwise it
 * starts at row 0 and walks down.  Either way the same 176 bytes end up
 * covered, so one dump serves both.
 *
 * The expected content is computed at capture time, not at dump time: the
 * reference frame is a rolling buffer and would have been overwritten by
 * the time the dump runs. */
#define PAD_SLOTS 5u
#define PAD_ROWS  11u
#define PAD_COLS  16u
#define PAD_BYTES (PAD_ROWS * PAD_COLS)

static const char *pad_name[PAD_SLOTS] = {
    "left", "right", "top", "bottom", "corner" };

struct pad_rec {
    int used, done;
    unsigned char got[PAD_BYTES], want[PAD_BYTES];
    uint32_t scratch, base, pitch, width, height, plane;
    int32_t  bx, by, mvx, mvy, srcx, srcy;
    int32_t  over_l, over_r, over_t, over_b;
    uint32_t usrc, fsrc, fsp, a5, a6;
};
static struct pad_rec g_pad[PAD_SLOTS];
static int g_pad_pending = -1;
static unsigned long long g_pad_seen;

static int32_t pad_clamp(int32_t v, int32_t hi)
{
    if (v < 0) return 0;
    if (v > hi) return hi;
    return v;
}

void recomp_pad_build(uint32_t scratch)
{
    struct mv_rec *c = &g_mv_cur;
    int32_t srcx, srcy, l, r, tp, b;
    unsigned slot, x, y;
    struct pad_rec *k;
    if (c->plane != 1u && c->plane != 2u) return;
    ++g_pad_seen;
    srcx = ((c->mvx + 2 * c->bx) >> 1) - 1;
    srcy = ((c->mvy + 2 * c->by) >> 1) - 1;
    l = (srcx < 0) ? -srcx : 0;
    r = (srcx + (int32_t)PAD_COLS > (int32_t)c->width)
        ? srcx + (int32_t)PAD_COLS - (int32_t)c->width : 0;
    tp = (srcy < 0) ? -srcy : 0;
    b = (srcy + (int32_t)PAD_ROWS > (int32_t)c->height)
        ? srcy + (int32_t)PAD_ROWS - (int32_t)c->height : 0;
    if ((l || r) && (tp || b)) slot = 4u;
    else if (l)  slot = 0u;
    else if (r)  slot = 1u;
    else if (tp) slot = 2u;
    else if (b)  slot = 3u;
    else return;      /* clamp path taken but nothing actually crosses */
    k = &g_pad[slot];
    if (k->used) return;
    k->used = 1;
    k->scratch = scratch; k->base = c->base; k->pitch = c->pitch;
    k->width = c->width;  k->height = c->height; k->plane = c->plane;
    k->bx = c->bx; k->by = c->by; k->mvx = c->mvx; k->mvy = c->mvy;
    k->srcx = srcx; k->srcy = srcy;
    k->over_l = l; k->over_r = r; k->over_t = tp; k->over_b = b;
    k->usrc = c->base + (uint32_t)(srcy * (int32_t)c->pitch)
            + (uint32_t)srcx;
    for (y = 0; y < PAD_ROWS; ++y)
        for (x = 0; x < PAD_COLS; ++x) {
            int32_t sx = pad_clamp(srcx + (int32_t)x, (int32_t)c->width - 1);
            int32_t sy = pad_clamp(srcy + (int32_t)y, (int32_t)c->height - 1);
            k->want[y * PAD_COLS + x] =
                (unsigned char)MEM8(c->base + (uint32_t)(sy * (int32_t)c->pitch)
                                    + (uint32_t)sx);
            k->got[y * PAD_COLS + x] =
                (unsigned char)MEM8(scratch + y * PAD_COLS + x);
        }
    g_pad_pending = (int)slot;
}

/* completed from the interpolator dispatch, so the pointer, the pitch and
 * the fraction bits the callee really receives are the recorded ones */
void recomp_pad_finish(uint32_t fsrc, uint32_t fsp, uint32_t a5, uint32_t a6)
{
    if (g_pad_pending < 0) return;
    { struct pad_rec *k = &g_pad[g_pad_pending];
      k->fsrc = fsrc; k->fsp = fsp; k->a5 = a5; k->a6 = a6;
      k->done = 1; }
    g_pad_pending = -1;
}

void recomp_dump_pad(void)
{
    unsigned s, x, y;
    int any = 0;
    for (s = 0; s < PAD_SLOTS; ++s) if (g_pad[s].done) any = 1;
    if (!any) return;
    fprintf(stderr, "[PAD] border-clamp scratch blocks, %llu chroma clamp calls seen" "\n", g_pad_seen);
    for (s = 0; s < PAD_SLOTS; ++s) {
        struct pad_rec *k = &g_pad[s];
        unsigned bad = 0;
        int first = -1;
        if (!k->done) {
            fprintf(stderr, "[PAD]   %-7s not captured" "\n", pad_name[s]);
            continue;
        }
        for (y = 0; y < PAD_BYTES; ++y)
            if (k->got[y] != k->want[y]) {
                ++bad;
                if (first < 0) first = (int)y;
            }
        fprintf(stderr, "[PAD]   --- %s ---" "\n", pad_name[s]);
        fprintf(stderr, "[PAD]     plane %s  base %08X  %ux%u  pitch %u" "\n",
                k->plane == 1u ? "U" : "V", k->base, k->width, k->height,
                k->pitch);
        fprintf(stderr, "[PAD]     block X/Y %ld / %ld   mv %ld / %ld" "\n",
                (long)k->bx, (long)k->by, (long)k->mvx, (long)k->mvy);
        fprintf(stderr, "[PAD]     window origin %ld / %ld  (X_int-1, Y_int-1)" "\n",
                (long)k->srcx, (long)k->srcy);
        fprintf(stderr, "[PAD]     unclamped src %08X" "\n", k->usrc);
        fprintf(stderr, "[PAD]     overrun  left %ld  right %ld  top %ld  bottom %ld" "\n",
                (long)k->over_l, (long)k->over_r, (long)k->over_t,
                (long)k->over_b);
        fprintf(stderr, "[PAD]     scratch base %08X  handed on %08X  (= scratch + %ld)" "\n",
                k->scratch, k->fsrc, (long)(k->fsrc - k->scratch));
        fprintf(stderr, "[PAD]     srcPitch %u   arg5/arg6 %u/%u   unclamped would be %ld/%ld  %s" "\n",
                k->fsp, k->a5, k->a6, (long)(k->mvx & 1), (long)(k->mvy & 1),
                (k->a5 == (uint32_t)(k->mvx & 1)
                 && k->a6 == (uint32_t)(k->mvy & 1)) ? "match" : "MISMATCH");
        fprintf(stderr, "[PAD]     scratch (11 rows x 16), got / want" "\n");
        for (y = 0; y < PAD_ROWS; ++y) {
            fprintf(stderr, "[PAD]      r%-2u", y);
            for (x = 0; x < PAD_COLS; ++x)
                fprintf(stderr, " %02X", k->got[y * PAD_COLS + x]);
            fprintf(stderr, "   |");
            for (x = 0; x < PAD_COLS; ++x)
                fprintf(stderr, " %02X", k->want[y * PAD_COLS + x]);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[PAD]     %-7s | %u bytes checked | %u mismatches | ",
                pad_name[s], (unsigned)PAD_BYTES, bad);
        if (first < 0) fprintf(stderr, "none" "\n");
        else fprintf(stderr, "offset %d (row %d col %d)" "\n",
                     first, first / (int)PAD_COLS, first % (int)PAD_COLS);
    }
    fflush(stderr);
}

/* TEMPORARY: decoder progress around sub_004E2801, the XMV update.
 *
 * Chain: sub_004E2801 -> sub_004E5450 (slice) -> sub_004E49AD (macroblock)
 *        -> sub_004E4641 (motion compensation).
 *
 * Structure of the update, read from the generated code:
 *
 *   +6C  nonzero means "input needed"; cleared once a buffer is taken
 *   +60  the input buffer pointer currently held
 *   +50  bytes left to consume from the current packet
 *   +58  running total of bytes handed over by the read callback
 *   +68  the buffer awaiting recycle; moved into +6C at loc_004E2DB9
 *   +74  set when the source has signalled end of stream
 *   +3C  slices remaining in this packet, loaded from the packet header
 *        as (hdr >> 0x17) & 0xFF and decremented per slice
 *   +F8  deferred bit position, -1 when there is nothing deferred
 *   +E4  base offset the deferred position is measured from
 *   +64  frame/sequence counter
 *
 * Decode only runs while +F8 == -1 AND +3C != 0:
 *
 *   loc_004E2ACD  if (+F8 != -1) leave the loop
 *   loc_004E2AD6  if (+3C == 0)  leave the loop
 *   loc_004E2ADB  call sub_004E5450
 *   loc_004E2C47  if (+3C != 0)  go round again
 *
 * Every branch point is tagged so the path through one update can be
 * replayed exactly, rather than inferred from field values. */
#define DEC_TAGS 64u

struct dec_rec {
    unsigned long long idx;
    uint32_t dec, stat_ptr;
    uint32_t in[10], out[10];
    uint32_t status, ret;
    uint32_t cb_called, cb_ret, cb_buf, cb_len;
    uint32_t b0, b4, b8, m36, m16;
    uint32_t n48, p148, st[8];
    unsigned slices, pubs;
    unsigned long long mb_before, mb_after;
    unsigned char tags[DEC_TAGS];
    unsigned ntags;
};

static const char *dec_field[10] = {
    "+3C slices", "+50 pkt left", "+58 bytes in", "+60 buffer",
    "+64 frame", "+68 recycle", "+6C need in", "+74 eof",
    "+E4 base", "+F8 defer" };
static const unsigned dec_off[10] = {
    0x3Cu, 0x50u, 0x58u, 0x60u, 0x64u, 0x68u, 0x6Cu, 0x74u, 0xE4u, 0xF8u };

static struct dec_rec g_dec_cur, g_dec_good, g_dec_bad, g_dec_late;
static int g_dec_have_good, g_dec_have_bad;
static unsigned long long g_dec_calls, g_dec_with_mb;


static void dec_read(struct dec_rec *r, uint32_t *dst)
{
    unsigned i;
    for (i = 0; i < 10u; ++i) dst[i] = MEM32(r->dec + dec_off[i]);
}

void recomp_dec_enter(uint32_t dec, uint32_t stat_ptr)
{
    struct dec_rec *r = &g_dec_cur;
    memset(r, 0, sizeof(*r));
    r->idx = ++g_dec_calls;
    r->dec = dec; r->stat_ptr = stat_ptr;
    r->mb_before = g_mv_calls;
    dec_read(r, r->in);
}

unsigned long long g_dec_pubs_total, g_dec_slices_total;

void recomp_dec_tag(unsigned t)
{
    struct dec_rec *r = &g_dec_cur;
    if (t == 13u) { ++r->slices; ++g_dec_slices_total; }
    if (t == 19u) { ++r->pubs;   ++g_dec_pubs_total; }
    if (r->ntags < DEC_TAGS) r->tags[r->ntags++] = (unsigned char)t;
}

void recomp_dec_cb(unsigned what, uint32_t v)
{
    struct dec_rec *r = &g_dec_cur;
    if (what == 0u) { r->cb_called = 1u; }
    else if (what == 1u) r->cb_ret = v;
    else if (what == 2u) r->cb_buf = v;
    else r->cb_len = v;
}

void recomp_dec_exit(uint32_t ret)
{
    struct dec_rec *r = &g_dec_cur;
    r->ret = ret;
    r->b0 = MEM32(r->dec + 0xB0u);
    r->b4 = MEM32(r->dec + 0xB4u);
    r->b8 = MEM32(r->dec + 0xB8u);
    r->n48  = MEM32(r->dec + 0x48u);
    r->p148 = MEM32(r->dec + 0x148u);
    { unsigned i; for (i = 0; i < 8u; ++i)
        r->st[i] = (i < r->n48 && r->p148) ? MEM32(r->p148 + i * 4u) : 0u; }
    r->mb_after = g_mv_calls;
    dec_read(r, r->out);
    r->status = MEM32(r->stat_ptr);
    if (r->mb_after > r->mb_before) {
        ++g_dec_with_mb;
        g_dec_good = *r; g_dec_have_good = 1;
        g_dec_have_bad = 0;      /* the next silent update is the one to keep */
    } else if (g_dec_have_good && !g_dec_have_bad) {
        g_dec_bad = *r; g_dec_have_bad = 1;
    }
    g_dec_late = *r;
}

static const char *dec_tag_name(unsigned t)
{
    if (t == 1u) return "loc_004E2838  +6C nonzero, entering the input-fetch chain";
    if (t == 2u) return "loc_004E2841  +60 zero, no buffer held";
    if (t == 3u) return "loc_004E2846  +50 zero, nothing left in the packet";
    if (t == 4u) return "loc_004E284F  +68 nonzero";
    if (t == 5u) return "loc_004E2858  status := 2, stream believed finished";
    if (t == 6u) return "loc_004E2863  calling the read callback [+1C]";
    if (t == 7u) return "loc_004E2871  callback returned";
    if (t == 8u) return "loc_004E2879  checking the returned buffer";
    if (t == 9u) return "loc_004E2884  buffer accepted, +60/+50 reloaded";
    if (t == 10u) return "loc_004E29E5  join: fetch skipped or done";
    if (t == 11u) return "loc_004E2ACD  slice loop head";
    if (t == 12u) return "loc_004E2AD6  +F8 == -1, ready to decode";
    if (t == 13u) return "loc_004E2ADB  +3C nonzero, decoding a slice";
    if (t == 14u) return "loc_004E2B10  slice returned, +3C decremented";
    if (t == 15u) return "loc_004E2B1E  slice loop not entered / left";
    if (t == 16u) return "loc_004E2C3E  tail: +F8 test";
    if (t == 17u) return "loc_004E2C47  tail: +3C test";
    if (t == 18u) return "loc_004E2C50  tail: loop finished";
    if (t == 19u) return "loc_004E2DB9  publish: +6C := +68, +68 := 0";
    if (t == 20u) return "loc_004E2E75  success exit";
    if (t == 21u) return "loc_004E2E77  early/error exit";
    if (t == 30u) return "loc_004E2C5D";
    if (t == 31u) return "loc_004E2C62";
    if (t == 32u) return "loc_004E2C6B";
    if (t == 33u) return "loc_004E2C70";
    if (t == 34u) return "loc_004E2C76";
    if (t == 35u) return "loc_004E2C82";
    if (t == 36u) return "loc_004E2C92";
    if (t == 37u) return "loc_004E2C9F";
    if (t == 38u) return "loc_004E2CA4";
    if (t == 39u) return "loc_004E2CAF";
    if (t == 40u) return "loc_004E2CB7";
    if (t == 41u) return "loc_004E2CBF";
    if (t == 42u) return "loc_004E2CCA";
    if (t == 43u) return "loc_004E2CCF";
    if (t == 44u) return "loc_004E2CD7";
    if (t == 45u) return "loc_004E2CDF";
    if (t == 46u) return "loc_004E2CE4";
    if (t == 47u) return "loc_004E2CE9";
    if (t == 48u) return "loc_004E2CEF";
    if (t == 49u) return "loc_004E2CF7";
    if (t == 50u) return "loc_004E2D00";
    if (t == 51u) return "loc_004E2D11";
    if (t == 52u) return "loc_004E2D13";
    if (t == 53u) return "loc_004E2D1C";
    if (t == 54u) return "loc_004E2D21";
    if (t == 55u) return "loc_004E2D28";
    if (t == 56u) return "loc_004E2D2D";
    if (t == 57u) return "loc_004E2D5E";
    if (t == 58u) return "loc_004E2D6A";
    if (t == 59u) return "loc_004E2D73";
    if (t == 60u) return "loc_004E2D78";
    if (t == 61u) return "loc_004E2D7D";
    if (t == 62u) return "loc_004E2D8B";
    if (t == 63u) return "loc_004E2D91";
    if (t == 64u) return "loc_004E2D99";
    if (t == 65u) return "loc_004E2DA2";
    if (t == 66u) return "loc_004E2DA4";
    if (t == 67u) return "loc_004E2DA7";
    if (t == 68u) return "loc_004E2DB0";
    return "?";
}

static void dec_show(const char *what, struct dec_rec *r, int have)
{
    unsigned i;
    if (!have) {
        fprintf(stderr, "[DEC]   %s: none" "\n", what);
        return;
    }
    fprintf(stderr, "[DEC]   --- %s: update #%llu, decoder %08X ---" "\n",
            what, r->idx, r->dec);
    fprintf(stderr, "[DEC]     field           entry      exit" "\n");
    for (i = 0; i < 10u; ++i)
        fprintf(stderr, "[DEC]     %-14s %08X   %08X%s" "\n",
                dec_field[i], r->in[i], r->out[i],
                r->in[i] != r->out[i] ? "  changed" : "");
    fprintf(stderr, "[DEC]     status=%u ret=%08X  slices=%u pubs=%u  macroblock calls=%llu" "\n",
            r->status, r->ret, r->slices, r->pubs,
            r->mb_after - r->mb_before);
    fprintf(stderr, "[DEC]     +B0=%08X +B4=%08X +B8=%08X" "\n",
            r->b0, r->b4, r->b8);
    fprintf(stderr, "[DEC]     +48 count=%u  +148 array=%08X  status:",
            r->n48, r->p148);
    { unsigned i; for (i = 0; i < 8u && i < r->n48; ++i)
        fprintf(stderr, " [%u]=%08X%s", i, r->st[i],
                r->st[i] == 0x8000000Au ? "(PENDING)" : ""); }
    fprintf(stderr, "\n");
    if (r->cb_called)
        fprintf(stderr, "[DEC]     read callback: ret=%d buffer=%08X length=%u" "\n",
                (int)r->cb_ret, r->cb_buf, r->cb_len);
    else
        fprintf(stderr, "[DEC]     read callback: not called" "\n");
    fprintf(stderr, "[DEC]     path:" "\n");
    for (i = 0; i < r->ntags; ++i)
        fprintf(stderr, "[DEC]       %2u  %s" "\n", i, dec_tag_name(r->tags[i]));
}

void recomp_dump_dec(void)
{
    if (!g_dec_calls) return;
    fprintf(stderr, "[DEC] XMV update progress: %llu updates, %llu produced macroblocks" "\n",
            g_dec_calls, g_dec_with_mb);
    fprintf(stderr, "[DEC]   publications=%llu  slices decoded=%llu" "\n",
            g_dec_pubs_total, g_dec_slices_total);
    dec_show("last update that decoded", &g_dec_good, g_dec_have_good);
    dec_show("first update that did not", &g_dec_bad, g_dec_have_bad);
    dec_show("most recent update", &g_dec_late, 1);
    fflush(stderr);
}

/* TEMPORARY: DirectSound packet lifetime, correlated by status-slot address.
 *
 * Structures read out of the generated code:
 *
 *   sub_004C9358(this, pPacket)   submit
 *     node = alloc from [this+0xB0]
 *     six dwords of the XMEDIAPACKET are copied to node+8, so
 *       node+0x08 pvBuffer      node+0x0C dwMaxSize
 *       node+0x10 pdwCompletedSize   node+0x14 pdwStatus
 *       node+0x18 hCompletionEvent   node+0x1C context
 *     node+0x20 := 0, node+0x24 := 0x8000000A (the node status)
 *     the node is then linked into the list at [this+0xA8]
 *
 *   sub_004C8E22(this, stopNode, status)   queue walk, two modes:
 *     stopNode == this+0xB8  -> walk [this+0xB0], complete via sub_004C8C02
 *     otherwise              -> walk from this+0xB8, only stamp node+0x24
 *
 *   sub_004C8C02(this, node, status)   completion
 *     unlocks the buffer pages, then
 *     sub_0055943E(node+8, [node+0xC], cb, ctx, status)
 *
 *   sub_0055943E(pkt, size, cb, ctx, status)
 *     if ([pkt+8])  *[pkt+8]  = size        (pdwCompletedSize)
 *     if ([pkt+0xC]) *[pkt+0xC] = status     (pdwStatus)  <-- the write
 *     then the callback, or SetEvent on [pkt+0x10]
 *
 * Everything is keyed on the pdwStatus pointer so packets belonging to
 * other DirectSound users cannot contaminate the XMV slots. */
#define DS_SLOTS  24u
#define DS_EVENTS 12u

enum { DS_SUBMIT = 1, DS_LINK, DS_WALK_DONE, DS_WALK_STAMP,
       DS_COMPLETE, DS_WRITE, DS_FLUSH };

static const char *ds_ev_name(unsigned c)
{
    switch (c) {
    case DS_SUBMIT:     return "SUBMIT       sub_004C9358";
    case DS_LINK:       return "LINKED       into [obj+0xA8]";
    case DS_WALK_DONE:  return "WALK-COMPLETE sub_004C8E22 completing mode";
    case DS_WALK_STAMP: return "WALK-STAMP   sub_004C8E22 stamp-only mode";
    case DS_COMPLETE:   return "COMPLETE     sub_004C8C02";
    case DS_WRITE:      return "STATUS-WRITE sub_0055943E";
    case DS_FLUSH:      return "FLUSH        sub_004C8E98";
    default:            return "?";
    }
}

struct ds_ev {
    unsigned code;
    unsigned long long seq;
    unsigned long long xmv_update;
    uint32_t a, b, c;
};

struct ds_slot {
    uint32_t status_ptr;
    uint32_t obj, node, buffer, maxsize, sizeptr, event, ctx;
    unsigned nev, submits, writes, dropped;
    uint32_t last_written;
    struct ds_ev ev[DS_EVENTS];
};
static struct ds_slot g_ds[DS_SLOTS];
static unsigned g_ds_n;
static unsigned long long g_ds_seq;
static unsigned long long g_ds_submit_all, g_ds_write_all, g_ds_complete_all;
static unsigned long long g_ds_walk_done, g_ds_walk_stamp, g_ds_flush;
static unsigned long long g_ds_srv_8EED, g_ds_srv_8FB0;
static unsigned long long g_ds_vtbl_4445;
static unsigned long long g_ds_site[5];
static uint32_t g_ds_lists[6];
static uint32_t g_ds_list_obj;

static struct ds_slot *ds_find(uint32_t sp, int create)
{
    unsigned i;
    if (!sp) return 0;
    for (i = 0; i < g_ds_n; ++i)
        if (g_ds[i].status_ptr == sp) return &g_ds[i];
    if (!create || g_ds_n >= DS_SLOTS) return 0;
    g_ds[g_ds_n].status_ptr = sp;
    return &g_ds[g_ds_n++];
}

static void ds_push(struct ds_slot *s, unsigned code, uint32_t a,
                    uint32_t b, uint32_t c)
{
    struct ds_ev *e;
    if (!s) return;
    if (s->nev >= DS_EVENTS) { ++s->dropped; return; }
    e = &s->ev[s->nev++];
    e->code = code; e->seq = ++g_ds_seq;
    e->xmv_update = g_dec_calls;
    e->a = a; e->b = b; e->c = c;
}

void recomp_ds_submit(uint32_t obj, uint32_t node)
{
    uint32_t sp = MEM32(node + 0x14u);
    struct ds_slot *s = ds_find(sp, 1);
    ++g_ds_submit_all;
    if (!s) return;
    s->obj = obj; s->node = node;
    s->buffer  = MEM32(node + 0x08u);
    s->maxsize = MEM32(node + 0x0Cu);
    s->sizeptr = MEM32(node + 0x10u);
    s->event   = MEM32(node + 0x18u);
    s->ctx     = MEM32(node + 0x1Cu);
    ++s->submits;
    ds_push(s, DS_SUBMIT, node, MEM32(node + 0x24u), sp ? MEM32(sp) : 0u);
}

void recomp_ds_link(uint32_t obj, uint32_t node)
{
    struct ds_slot *s = ds_find(MEM32(node + 0x14u), 0);
    ds_push(s, DS_LINK, node, MEM32(obj + 0xA8u), 0u);
}

void recomp_ds_walk(unsigned mode, uint32_t obj, uint32_t stop, uint32_t st)
{
    if (mode) ++g_ds_walk_done; else ++g_ds_walk_stamp;
    (void)obj; (void)stop; (void)st;
}

void recomp_ds_complete(uint32_t obj, uint32_t node, uint32_t st)
{
    struct ds_slot *s = ds_find(MEM32(node + 0x14u), 0);
    ++g_ds_complete_all;
    ds_push(s, DS_COMPLETE, node, MEM32(node + 0x24u), st);
    (void)obj;
}

void recomp_ds_write(uint32_t pkt, uint32_t val)
{
    uint32_t sp = MEM32(pkt + 0x0Cu);
    struct ds_slot *s = ds_find(sp, 0);
    ++g_ds_write_all;
    if (s) { ++s->writes; s->last_written = val; }
    ds_push(s, DS_WRITE, pkt, sp, val);
}

void recomp_ds_flush(uint32_t obj)
{
    ++g_ds_flush;
    (void)obj;
}

void recomp_ds_service(unsigned which)
{
    if (which) ++g_ds_srv_8FB0; else ++g_ds_srv_8EED;
}

/* the vtable method that drives packet retirement; it has no direct caller
 * in the recompiled image, so it can only arrive through a COM slot */
void recomp_ds_vtbl(void)
{
    ++g_ds_vtbl_4445;
}

/* which of the sub_004C8FB0 call sites actually fires */
void recomp_ds_site(unsigned i)
{
    if (i < 5u) ++g_ds_site[i];
}

/* where the submitted nodes actually sit, sampled at flush time */
void recomp_ds_lists(uint32_t obj)
{
    if (g_ds_list_obj && g_ds_list_obj != obj) return;
    g_ds_list_obj = obj;
    g_ds_lists[0] = MEM32(obj + 0xA8u); g_ds_lists[1] = MEM32(obj + 0xACu);
    g_ds_lists[2] = MEM32(obj + 0xB0u); g_ds_lists[3] = MEM32(obj + 0xB4u);
    g_ds_lists[4] = MEM32(obj + 0xB8u); g_ds_lists[5] = MEM32(obj + 0xBCu);
}

void recomp_dump_ds(void)
{
    unsigned i, j, k;
    uint32_t xmv[8]; unsigned nxmv = 0;
    if (!g_ds_submit_all) return;
    fprintf(stderr, "[DS] DirectSound packet lifetime by status slot" "\n");
    fprintf(stderr, "[DS]   submits=%llu completions=%llu status-writes=%llu" "\n",
            g_ds_submit_all, g_ds_complete_all, g_ds_write_all);
    fprintf(stderr, "[DS]   queue walks: completing=%llu stamp-only=%llu  flushes=%llu" "\n",
            g_ds_walk_done, g_ds_walk_stamp, g_ds_flush);
    fprintf(stderr, "[DS]   service entries: sub_004C8EED=%llu sub_004C8FB0=%llu" "\n",
            g_ds_srv_8EED, g_ds_srv_8FB0);
    fprintf(stderr, "[DS]   vtable service sub_004C4445 entries=%llu" "\n", g_ds_vtbl_4445);
    fprintf(stderr, "[DS]   sub_004C8FB0 call sites: 004C447B=%llu 004C45FA=%llu 004C909F=%llu 004C91C1=%llu 004C922F=%llu" "\n",
            g_ds_site[0], g_ds_site[1], g_ds_site[2], g_ds_site[3],
            g_ds_site[4]);
    if (g_ds_list_obj)
        fprintf(stderr, "[DS]   obj %08X lists: +A8=%08X +AC=%08X +B0=%08X +B4=%08X +B8=%08X +BC=%08X" "\n",
                g_ds_list_obj, g_ds_lists[0], g_ds_lists[1],
                g_ds_lists[2], g_ds_lists[3], g_ds_lists[4],
                g_ds_lists[5]);
    /* the four XMV slots, taken from the decoder itself */
    if (g_dec_cur.dec) {
        uint32_t p = MEM32(g_dec_cur.dec + 0x148u);
        uint32_t n = MEM32(g_dec_cur.dec + 0x48u);
        if (p && n <= 8u) { for (k = 0; k < n; ++k) xmv[nxmv++] = p + k * 4u; }
        fprintf(stderr, "[DS]   XMV status slots (%u):", nxmv);
        for (k = 0; k < nxmv; ++k)
            fprintf(stderr, " %08X=%08X", xmv[k], MEM32(xmv[k]));
        fprintf(stderr, "\n");
    }
    for (i = 0; i < g_ds_n; ++i) {
        struct ds_slot *s = &g_ds[i];
        int is_xmv = 0;
        for (k = 0; k < nxmv; ++k) if (xmv[k] == s->status_ptr) is_xmv = 1;
        fprintf(stderr, "[DS]   --- slot %08X %s ---" "\n",
                s->status_ptr, is_xmv ? "(XMV)" : "");
        fprintf(stderr, "[DS]     obj=%08X node=%08X buffer=%08X max=%u" "\n",
                s->obj, s->node, s->buffer, s->maxsize);
        fprintf(stderr, "[DS]     pdwCompletedSize=%08X event=%08X ctx=%08X" "\n",
                s->sizeptr, s->event, s->ctx);
        fprintf(stderr, "[DS]     submits=%u status-writes=%u last written=%08X  now=%08X" "\n",
                s->submits, s->writes, s->last_written,
                MEM32(s->status_ptr));
        for (j = 0; j < s->nev; ++j) {
            struct ds_ev *e = &s->ev[j];
            fprintf(stderr, "[DS]       #%llu xmv-update %llu  %-46s a=%08X b=%08X c=%08X" "\n",
                    e->seq, e->xmv_update, ds_ev_name(e->code),
                    e->a, e->b, e->c);
        }
        if (s->dropped)
            fprintf(stderr, "[DS]       (%u further events not recorded)" "\n",
                    s->dropped);
    }
    fflush(stderr);
}

/* TEMPORARY: who dispatches [vtable+0x18] on the DirectSound stream class.
 *
 * The class vtable is installed by
 *   0x004C42CA  mov dword ptr [esi], 0x586038
 *   0x004C5D40  mov dword ptr [esi], 0x586038
 * so the object holds 0x00586038 and its slots are
 *   +00 004C42FC  +04 004C4343  +08 004C4391  +0C 004C4490
 *   +10 004C44E1  +14 004C43F8  +18 004C4445  +1C 004C677A
 *
 * sub_004C4445 is therefore [vtable+0x18], and it is the only slot whose
 * body is "service the packet queue" (it calls sub_004C8FB0).
 *
 * Every [vtable+0x18] dispatch in the whole recompiled image is counted
 * here, both in total and restricted to objects carrying this vtable, so
 * a site that runs on some other class cannot be mistaken for a candidate.
 * The site is identified by the return address the call pushes, which is
 * the instruction after the indirect call. */
#define VT_SITES 64u
#define VT_VTBL  0x00586038u

struct vt_site {
    uint32_t pc;
    unsigned long long total, matched;
    uint32_t last_obj, last_vtbl, last_target;
};
static struct vt_site g_vt[VT_SITES];
static unsigned g_vt_n;
static unsigned long long g_vt_any, g_vt_hit;

void recomp_vt18(uint32_t pc, uint32_t vtbl, uint32_t obj)
{
    unsigned i;
    struct vt_site *s = 0;
    ++g_vt_any;
    for (i = 0; i < g_vt_n; ++i) if (g_vt[i].pc == pc) { s = &g_vt[i]; break; }
    if (!s) {
        if (g_vt_n >= VT_SITES) return;
        s = &g_vt[g_vt_n++];
        s->pc = pc;
    }
    ++s->total;
    if (vtbl == VT_VTBL) {
        ++s->matched; ++g_vt_hit;
        s->last_obj = obj; s->last_vtbl = vtbl;
        s->last_target = MEM32(vtbl + 0x18u);
    }
}

void recomp_dump_vt(void)
{
    unsigned i;
    fprintf(stderr, "[VT] [vtable+0x18] dispatches: %llu total, %llu on the DirectSound stream class" "\n",
            g_vt_any, g_vt_hit);
    fprintf(stderr, "[VT]   class vtable %08X, slot +0x18 = %08X" "\n",
            VT_VTBL, MEM32(VT_VTBL + 0x18u));
    for (i = 0; i < g_vt_n; ++i) {
        struct vt_site *s = &g_vt[i];
        fprintf(stderr, "[VT]   site %08X  reached %llu  on-this-class %llu",
                s->pc, s->total, s->matched);
        if (s->matched)
            fprintf(stderr, "  obj=%08X vtbl=%08X target=%08X",
                    s->last_obj, s->last_vtbl, s->last_target);
        fprintf(stderr, "\n");
    }
    if (!g_vt_n) fprintf(stderr, "[VT]   no dispatch site executed at all" "\n");
    fflush(stderr);
}

/* TEMPORARY: does the DirectSound interrupt cadence exist at all.
 *
 * DSOUND installs two hardware ISRs:
 *   0x004C8718 HalGetInterruptVector(5)  -> IRQ 5, the MCPX APU
 *     0x004C8733 KeInitializeInterrupt(0x4E1D48, ISR=0x004C86A0, ...)
 *     0x004C873A KeConnectInterrupt(0x4E1D48)
 *   0x004CE826 HalGetInterruptVector(6)
 *     0x004CE83A KeInitializeInterrupt(0x4E1C80, ISR=0x004CE6DA, ...)
 *     0x004CE847 KeConnectInterrupt(0x4E1C80)
 *   with KeInitializeDpc(edi+0x1C, 0x004CE6EB, edi) at 0x004CE819
 *
 * The ISR at 0x004C8604 ends in KeInsertQueueDpc (0x004C8648), so on
 * hardware the chain is: APU interrupt -> ISR -> DPC -> packet service
 * -> sub_0055943E -> status write.  These counters say which link is
 * missing here. */
#define IR_N 9u

static const char *ir_name[IR_N] = {
    "sub_004C67FD  calls the audio init",
    "sub_004C86AC  connects IRQ 5, ISR sub_004C86A0",
    "sub_004C86A0  the IRQ 5 ISR itself",
    "sub_004C8604  ISR body, queues the DPC",
    "sub_004C8659  DPC-side routine",
    "sub_004CE6DA  the IRQ 6 ISR",
    "sub_004CE6EB  the IRQ 6 DPC routine",
    "sub_004C90A0  calls sub_004C8EED",
    "sub_004C8EED  APU-position retirement",
};
static unsigned long long g_ir[IR_N];

void recomp_ir(unsigned i) { if (i < IR_N) ++g_ir[i]; }

/* which of the three early-outs in sub_004C86AC aborts the registration */
static uint32_t g_ir_bail[3];
static int g_ir_bail_n[3];
void recomp_ir_bail(unsigned k, uint32_t v)
{
    if (k < 3u) { g_ir_bail[k] = v; ++g_ir_bail_n[k]; }
}

void recomp_dump_ir(void)
{
    unsigned i;
    fprintf(stderr, "[IR] DirectSound interrupt/DPC cadence" "\n");
    { extern unsigned long long g_vec_delivered[64];
      extern unsigned long long g_dpc_inserted, g_dpc_insert_rejected;
      extern unsigned long long g_apu_irq_raises, g_apu_irq_edges;
      fprintf(stderr, "[IR]   vector 53 delivered=%llu   vector 51 delivered=%llu"
              "   vector 54 delivered=%llu" "\n",
              g_vec_delivered[53], g_vec_delivered[51], g_vec_delivered[54]);
      fprintf(stderr, "[IR]   APU raises=%llu edges=%llu   DPC inserted=%llu rejected=%llu" "\n",
              g_apu_irq_raises, g_apu_irq_edges, g_dpc_inserted,
              g_dpc_insert_rejected); }
    for (i = 0; i < 3u; ++i)
        fprintf(stderr, "[IR]   sub_004C86AC gate %u: taken %d times, eax=%08X\n",
                i, g_ir_bail_n[i], g_ir_bail[i]);
    for (i = 0; i < IR_N; ++i)
        fprintf(stderr, "[IR]   %-52s entries=%llu" "\n", ir_name[i], g_ir[i]);
    fflush(stderr);
}

/* TEMPORARY: 5 Hz timeline of the XMV decoder, its four DirectSound
 * streams and the APU, so a run that stops early can be lined up against
 * one that reaches EOF at the same stream offset.
 *
 * One line per sample, streamed as it is taken rather than buffered: the
 * process is killed rather than exited, so anything held in a ring would
 * be lost.  Reads are plain loads of guest memory and of counters that are
 * only ever incremented, so a torn value costs one noisy sample and
 * nothing else. */
void recomp_tl_sample(void)
{
    static unsigned long long t0;
    static double freq;
    unsigned long long now;
    uint32_t dec, arr;
    uint32_t s[4] = { 0, 0, 0, 0 };
    LARGE_INTEGER c;
    if (freq == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (double)f.QuadPart;
        QueryPerformanceCounter(&c);
        t0 = (unsigned long long)c.QuadPart;
    }
    QueryPerformanceCounter(&c);
    now = (unsigned long long)c.QuadPart;
    dec = g_dec_cur.dec;
    if (!dec) return;
    arr = MEM32(dec + 0x148u);
    if (arr) { unsigned i; for (i = 0; i < 4u; ++i) s[i] = MEM32(arr + i * 4u); }
    {
        extern unsigned long long g_apu_vp_frames, g_apu_irq_raises;
        extern unsigned long long g_vec_delivered[64];
        extern unsigned long long g_dpc_inserted;
        extern unsigned long long g_apu_notifies, g_apu_irq_asserts;
        extern unsigned long long g_apu_irq_calls;
        extern int g_apu_irq_level_get(void);
        fprintf(stderr,
            "[TLINE] t=%.2f upd=%llu mb=%llu 3C=%X 50=%X 58=%X 60=%08X 68=%08X 6C=%08X 74=%X F8=%08X"
            " st=%08X,%08X,%08X,%08X sub=%llu cmp=%llu out=%lld"
            " vp=%llu rz=%llu dl=%llu isr=%llu dpc=%llu"
            " ISTS=%08X IEN=%08X lvl=%d ntf=%llu asr=%llu uic=%llu"
            " mode=%08X vidflag=%08X vidclk=%08X player=%08X pstate=%08X ctrl=%08X" "\n",
            (double)(now - t0) / freq, g_dec_calls, g_dec_with_mb,
            MEM32(dec + 0x3Cu), MEM32(dec + 0x50u), MEM32(dec + 0x58u),
            MEM32(dec + 0x60u), MEM32(dec + 0x68u), MEM32(dec + 0x6Cu),
            MEM32(dec + 0x74u), MEM32(dec + 0xF8u),
            s[0], s[1], s[2], s[3],
            g_ds_submit_all, g_ds_complete_all,
            (long long)g_ds_submit_all - (long long)g_ds_complete_all,
            g_apu_vp_frames, g_apu_irq_raises, g_vec_delivered[53],
            g_ir[2], g_dpc_inserted,
            (unsigned)MEM32(0xFE801000u), (unsigned)MEM32(0xFE801004u),
            g_apu_irq_level_get(), g_apu_notifies, g_apu_irq_asserts,
            g_apu_irq_calls,
            /* known title state, from the existing xemu scripts:
             *   007FA1F8 title mode, 0x1D == Frontend
             *   00849C60 video "still playing", 0 idle / 2 playing
             *   00849C6C playback clock, float
             *   008497F0 video player singleton; [p+0] 1 playing / 3 finished
             *   008493BC compositor controller slot */
            MEM32(0x007FA1F8u), MEM32(0x00849C60u), MEM32(0x00849C6Cu),
            MEM32(0x008497F0u),
            MEM32(0x008497F0u) ? MEM32(MEM32(0x008497F0u)) : 0u,
            MEM32(0x008493BCu));
    }
}

/* TEMPORARY: the D3D pushbuffer wait at sub_0053C120.
 *
 * The guest function is 98 bytes; the +0x256 in the watchdog stack is a
 * host offset into the recompiled body, not a guest offset.  The body is:
 *
 *   loc_0053C130:  eax = MEM32(edx + 0x400B10)   ; NV2A DMA get
 *                  ecx = MEM32(edi) << 2         ; pushbuffer put
 *                  if (((ecx ^ eax) & 0x7C) != 0) loop
 *
 * so it spins until the GPU get pointer catches up with put, to within the
 * window that bits 2..6 encode.  Both reads come from the NV2A aperture,
 * which this runtime traps with an access violation and a vectored
 * handler -- that is why the thread shows repeated
 * KiUserExceptionDispatcher frames.  Those exceptions are the MMIO
 * mechanism working, not a fault.
 *
 * What is recorded here: the first entry, the last entry that left the
 * loop, and the entry that never leaves. */
static unsigned long long g_sp_entries, g_sp_iters, g_sp_exits;
static unsigned long long g_sp_last_iters;
static uint32_t g_sp_first[6], g_sp_lastok[6], g_sp_stuck[6];
static int g_sp_have_first, g_sp_have_stuck;
static unsigned long long g_sp_cur_iters;

void recomp_spin_enter(uint32_t esi, uint32_t edx, uint32_t edi)
{
    ++g_sp_entries;
    g_sp_cur_iters = 0;
    if (!g_sp_have_first) {
        g_sp_have_first = 1;
        g_sp_first[0] = esi; g_sp_first[1] = edx; g_sp_first[2] = edi;
        g_sp_first[3] = MEM32(edx + 0x400B10u);
        g_sp_first[4] = MEM32(edi);
        g_sp_first[5] = 0;
    }
}

void recomp_spin_iter(uint32_t edx, uint32_t edi, uint32_t get, uint32_t put)
{
    ++g_sp_iters; ++g_sp_cur_iters;
    if ((g_sp_cur_iters % 5000000ull) == 0ull)
        fprintf(stderr, "[SPIN] iter %llu: get=%08X put=%08X put<<2=%08X"
                " xor&7C=%02X\n",
                g_sp_cur_iters, get, put, put << 2,
                (unsigned)(((put << 2) ^ get) & 0x7Cu));
    /* after this many the loop is not going to leave on its own */
    if (g_sp_cur_iters == 2000000ull && !g_sp_have_stuck) {
        g_sp_have_stuck = 1;
        g_sp_stuck[0] = edx; g_sp_stuck[1] = edi;
        g_sp_stuck[2] = get; g_sp_stuck[3] = put;
        g_sp_stuck[4] = ((put << 2) ^ get);
        g_sp_stuck[5] = (uint32_t)(edx + 0x400B10u);
    }
}

void recomp_spin_exit(uint32_t edx, uint32_t edi, uint32_t get, uint32_t put)
{
    ++g_sp_exits;
    g_sp_last_iters = g_sp_cur_iters;
    g_sp_lastok[0] = edx; g_sp_lastok[1] = edi;
    g_sp_lastok[2] = get; g_sp_lastok[3] = put;
    g_sp_lastok[4] = ((put << 2) ^ get);
    g_sp_lastok[5] = (uint32_t)g_sp_cur_iters;
}

void recomp_dump_spin(void)
{
    if (!g_sp_entries) return;
    fprintf(stderr, "[SPIN] sub_0053C120 pushbuffer wait: entries=%llu exits=%llu iterations=%llu" "\n",
            g_sp_entries, g_sp_exits, g_sp_iters);
    if (g_sp_have_first)
        fprintf(stderr, "[SPIN]   first entry: esi=%08X edx=%08X edi=%08X get=%08X put=%08X" "\n",
                g_sp_first[0], g_sp_first[1], g_sp_first[2],
                g_sp_first[3], g_sp_first[4]);
    if (g_sp_exits)
        fprintf(stderr, "[SPIN]   last exit:   edx=%08X edi=%08X get=%08X put=%08X xor=%08X after %u iters" "\n",
                g_sp_lastok[0], g_sp_lastok[1], g_sp_lastok[2],
                g_sp_lastok[3], g_sp_lastok[4], g_sp_lastok[5]);
    if (g_sp_have_stuck)
        fprintf(stderr, "[SPIN]   STUCK:       edx=%08X edi=%08X get=%08X put=%08X xor=%08X  reading %08X" "\n",
                g_sp_stuck[0], g_sp_stuck[1], g_sp_stuck[2],
                g_sp_stuck[3], g_sp_stuck[4], g_sp_stuck[5]);
    else
        fprintf(stderr, "[SPIN]   no entry has exceeded the stuck threshold" "\n");
    fflush(stderr);
}

/* TEMPORARY: ring geometry at the push-buffer allocator.
 *
 * sub_0053C190(R) head, with esi = the device at [0x5499E8]:
 *
 *   S = [[esi+0x30]]        the cursor the guest polls
 *   L = [esi+0x2C]          limit
 *   R = [esp+0xC]           requested position
 *   if ((L - R) >= (L - S)) return        unsigned
 *
 * (L-R) >= (L-S) is R <= S, so the fast path is taken when the requested
 * position is at or behind the cursor.  The tail loop at 0x0053C2C0 is the
 * same inequality spun on: it waits for S to reach R.  So S is the boundary
 * the writer may not pass -- a reclaim / safe-to-overwrite pointer -- and R
 * is a position in the same units, not a byte count.
 *
 * Recorded here so the units and the wrap behaviour can be read off real
 * values rather than argued about. */
#define RG_FIRST 6u
#define RG_LAST  6u

struct rg_rec {
    unsigned long long seq;
    uint32_t dev, base, lim, shadow_ptr, shadow, req;
    uint32_t f24, f28, f44;
    int fast;
};
static struct rg_rec g_rg_first[RG_FIRST], g_rg_last[RG_LAST];
static unsigned long long g_rg_calls, g_rg_fast, g_rg_slow;

void recomp_rg_enter(uint32_t dev, uint32_t req)
{
    struct rg_rec r;
    uint32_t sp = MEM32(dev + 0x30u);
    memset(&r, 0, sizeof(r));
    r.seq = ++g_rg_calls;
    r.dev = dev; r.req = req;
    r.base = MEM32(dev);
    r.lim  = MEM32(dev + 0x2Cu);
    r.shadow_ptr = sp;
    r.shadow = sp ? MEM32(sp) : 0u;
    r.f24 = MEM32(dev + 0x24u);
    r.f28 = MEM32(dev + 0x28u);
    r.f44 = MEM32(dev + 0x44u);
    if (g_rg_calls <= RG_FIRST) g_rg_first[g_rg_calls - 1u] = r;
    g_rg_last[(g_rg_calls - 1u) % RG_LAST] = r;
}

void recomp_rg_fast(void) { ++g_rg_fast; }

/* The tail spin of the allocator, loc_0053C2C0: wait until S >= R.  S is
 * written by the guest on its publish path, so if this loop is entered the
 * only way out is a callback the guest registers at [dev+0x19FC] -- checked
 * at loc_0053C293 and skipped when null. */
static uint32_t g_rg_cb, g_rg_s0, g_rg_r0, g_rg_dev;
static unsigned long long g_rg_spin;
void recomp_rg_slow(void)
{
    ++g_rg_slow;
    ++g_rg_spin;
    if (g_rg_spin == 1ull) {
        uint32_t dev = MEM32(0x5499E8u);
        g_rg_dev = dev;
        g_rg_cb  = MEM32(dev + 0x19FCu);
        g_rg_r0  = MEM32(dev + 0x2Cu);
        g_rg_s0  = MEM32(MEM32(dev + 0x30u));
    }
}

void recomp_dump_rgspin(void)
{
    uint32_t s_now;
    if (!g_rg_spin) return;
    s_now = g_rg_dev ? MEM32(MEM32(g_rg_dev + 0x30u)) : 0u;
    fprintf(stderr, "[RGSPIN] tail wait iterations=%llu" "\n", g_rg_spin);
    fprintf(stderr, "[RGSPIN]   callback [dev+0x19FC] = %08X  %s" "\n",
            g_rg_cb, g_rg_cb ? "present" : "NULL, loc_0053C293 skips it");
    fprintf(stderr, "[RGSPIN]   at entry S=%08X R=%08X  R-S=%d" "\n",
            g_rg_s0, g_rg_r0, (int)(g_rg_r0 - g_rg_s0));
    fprintf(stderr, "[RGSPIN]   now      S=%08X  advanced %d" "\n",
            s_now, (int)(s_now - g_rg_s0));
    fflush(stderr);
}

static void rg_show(const char *what, struct rg_rec *r)
{
    if (!r->seq) return;
    fprintf(stderr, "[RING] %s #%llu  base=%08X lim=%08X  shadowptr=%08X" "\n"
            "[RING]     S=%08X S<<2=%08X  R=%08X  R<=S:%s"
            "  +24=%08X +28=%08X +44=%08X" "\n",
            what, r->seq, r->base, r->lim, r->shadow_ptr,
            r->shadow, r->shadow << 2, r->req,
            (uint32_t)(r->lim - r->req) >= (uint32_t)(r->lim - r->shadow)
                ? "yes" : "NO",
            r->f24, r->f28, r->f44);
}

void recomp_dump_ring(void)
{
    unsigned i;
    if (!g_rg_calls) return;
    fprintf(stderr, "[RING] sub_0053C190 calls=%llu fast=%llu slow=%llu" "\n",
            g_rg_calls, g_rg_fast, g_rg_slow);
    for (i = 0; i < RG_FIRST; ++i) rg_show("first", &g_rg_first[i]);
    for (i = 0; i < RG_LAST; ++i)
        rg_show("last ", &g_rg_last[(g_rg_calls + i) % RG_LAST]);
    fflush(stderr);
}

/* TEMPORARY: the slow path of the push-buffer allocator.
 *
 * sub_0053C190 returns immediately when S >= R (21,215 of 21,216 calls).
 * The calls that do not are the only ones that can distinguish what S is,
 * so they are recorded whole: state on entry, every pass through the wait
 * loop, and state on exit.
 *
 * PGRAPH 0xB10 is read the same way the guest reads it, through the MMIO
 * trap, at [dev+0x934] + 0x400B10.  It is a plain read with no side effect
 * in this model. */
#define SL_N     16u
#define SL_SAMP  12u

struct sl_rec {
    int used, done;
    uint32_t base, end, Lv, s_before, req, wptr, b10_before;
    uint32_t s_after, b10_after, wptr_after;
    uint32_t put_before, put_after;
    unsigned iters, nsamp;
    uint32_t s_samp[SL_SAMP], b10_samp[SL_SAMP];
};
static struct sl_rec g_sl[SL_N];
static unsigned g_sl_n;
static int g_sl_cur = -1;

static uint32_t sl_b10(uint32_t dev)
{
    uint32_t nv = MEM32(dev + 0x934u);
    return nv ? MEM32(nv + 0x400B10u) : 0u;
}

void recomp_sl_enter(uint32_t dev, uint32_t req)
{
    struct sl_rec *r;
    extern uint32_t nv2a_get_dma_put(void);
    if (g_sl_n >= SL_N) { g_sl_cur = -1; return; }
    g_sl_cur = (int)g_sl_n++;
    r = &g_sl[g_sl_cur];
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->base = MEM32(dev + 0x24u);
    r->end  = MEM32(dev + 0x28u);
    r->Lv   = MEM32(dev + 0x2Cu);
    { uint32_t sp = MEM32(dev + 0x30u);
      r->s_before = sp ? MEM32(sp) : 0u; }
    r->req  = req;
    r->wptr = MEM32(dev);
    r->b10_before = sl_b10(dev);
    r->put_before = nv2a_get_dma_put();
}

void recomp_sl_iter(uint32_t dev)
{
    struct sl_rec *r;
    if (g_sl_cur < 0) return;
    r = &g_sl[g_sl_cur];
    ++r->iters;
    if (r->nsamp < SL_SAMP) {
        uint32_t sp = MEM32(dev + 0x30u);
        r->s_samp[r->nsamp] = sp ? MEM32(sp) : 0u;
        r->b10_samp[r->nsamp] = sl_b10(dev);
        ++r->nsamp;
    }
}

void recomp_sl_exit(uint32_t dev)
{
    struct sl_rec *r;
    extern uint32_t nv2a_get_dma_put(void);
    if (g_sl_cur < 0) return;
    r = &g_sl[g_sl_cur];
    { uint32_t sp = MEM32(dev + 0x30u);
      r->s_after = sp ? MEM32(sp) : 0u; }
    r->b10_after = sl_b10(dev);
    r->wptr_after = MEM32(dev);
    r->put_after = nv2a_get_dma_put();
    r->done = 1;
    g_sl_cur = -1;
}

void recomp_dump_slow(void)
{
    unsigned i, j;
    if (!g_sl_n) return;
    fprintf(stderr, "[SLOW] %u slow-path calls in sub_0053C190" "\n", g_sl_n);
    fprintf(stderr, "[SLOW] call | S_before |    R | S_after  | delta_S | B10_before | B10_after | class" "\n");
    for (i = 0; i < g_sl_n; ++i) {
        struct sl_rec *r = &g_sl[i];
        long ds = (long)(int32_t)(r->s_after - r->s_before);
        const char *cls;
        if (!r->done) cls = "NEVER EXITED";
        else if (ds == 0) cls = "S unchanged";
        else if ((uint32_t)ds == r->req) cls = "B: advanced by R";
        else if (r->s_after >= r->req && r->s_before < r->req)
            cls = "A: jumped to >= R";
        else if (r->s_after < r->s_before) cls = "C: wrapped/reset";
        else cls = "B: advanced";
        fprintf(stderr, "[SLOW] %4u | %08X | %4X | %08X | %7ld | %10X | %9X | %s" "\n",
                i, r->s_before, r->req, r->s_after, ds,
                r->b10_before, r->b10_after, cls);
        fprintf(stderr, "[SLOW]        ring %08X..%08X  L=%08X  wptr %08X->%08X  DMA_PUT %08X->%08X  iters=%u" "\n",
                r->base, r->end, r->Lv, r->wptr, r->wptr_after,
                r->put_before, r->put_after, r->iters);
        fprintf(stderr, "[SLOW]        B10_before vs S_before<<2: %08X vs %08X  %s" "\n",
                r->b10_before, r->s_before << 2,
                r->b10_before == (r->s_before << 2) ? "equal" : "differ");
        for (j = 0; j < r->nsamp; ++j)
            fprintf(stderr, "[SLOW]          iter %2u  S=%08X  B10=%08X  S<<2=%08X  %s" "\n",
                    j, r->s_samp[j], r->b10_samp[j], r->s_samp[j] << 2,
                    r->b10_samp[j] == (r->s_samp[j] << 2) ? "equal" : "differ");
    }
    fflush(stderr);
}

/* TEMPORARY: which link of the render chain stops after the intro.
 *
 * Recovered from the caller chain of the last DMA_PUT publication:
 *
 *   sub_0042DF42 -> sub_0042F075 -> sub_002A3670 -> sub_002A93D0
 *     -> sub_002A3F10 -> sub_002A4610 -> sub_0053D700
 *       -> sub_0053BEA0 -> sub_0053C450 -> DMA_PUT
 *
 * Counting entries per link separates "the update stopped being called"
 * from "it runs but the render is skipped" from "the render runs but
 * submits nothing".  Deltas are printed per dump so a link that goes
 * quiet is obvious without reading absolute totals. */
#define CH_N 16u

static const char *chain_name[CH_N] = {
    "sub_0042F075  outer loop",
    "sub_002A3670  frame step",
    "sub_002A93D0  update",
    "sub_002A3F10  render entry",
    "sub_002A4610  scene submit",
    "sub_0053D700  D3D submit",
    "sub_0053BEA0  push-buffer commit",
    "sub_0053D240  D3D callee A",
    "sub_0053D3C0  D3D callee B",
    "sub_0053D6C0  D3D callee C",
    "sub_00543900  D3D callee D",
    "sub_005439B0  D3D callee E",
    "sub_0053C190  ring allocator",
    "sub_0053C630  PFB cache flush wait",
    "sub_0053BF70  allocator callee A",
    "sub_0053BFA0  allocator callee B",
};
static unsigned long long g_chain[CH_N], g_chain_prev[CH_N];

void recomp_chain(unsigned i) { if (i < CH_N) ++g_chain[i]; }
static unsigned long long g_chain_out[CH_N], g_chain_out_prev[CH_N];
void recomp_chain_exit(unsigned i) { if (i < CH_N) ++g_chain_out[i]; }

void recomp_dump_chain(void)
{
    unsigned i;
    int any = 0;
    for (i = 0; i < CH_N; ++i) if (g_chain[i]) any = 1;
    if (!any) return;
    fprintf(stderr, "[CHAIN] render chain entries (total / since last dump)" "\n");
    for (i = 0; i < CH_N; ++i) {
        fprintf(stderr, "[CHAIN]   %-34s %10llu  %+lld%s" "\n",
                chain_name[i], g_chain[i],
                (long long)(g_chain[i] - g_chain_prev[i]),
                (g_chain[i] == g_chain_prev[i]) ? "   STOPPED" : "");
        fprintf(stderr, "[CHAIN]       returns %10llu  %+lld   inside now %lld" "\n",
                g_chain_out[i], (long long)(g_chain_out[i] - g_chain_out_prev[i]),
                (long long)(g_chain[i] - g_chain_out[i]));
        g_chain_prev[i] = g_chain[i];
        g_chain_out_prev[i] = g_chain_out[i];
    }
    fflush(stderr);
}

/* TEMPORARY: the kernel wait that actually holds the render thread.
 *
 * The tail of sub_0053C190, which none of the earlier counters covered
 * because it is an import call through a pointer:
 *
 *   0x0053C2D1  edi = [0x561150]            xbox_KeWaitForSingleObject
 *   0x0053C2D7  esi = dev + 0x1DCC          the dispatcher object
 *   0x0053C2E0  push 0, 0, 1, 6, esi
 *   0x0053C2E9  call edi
 *   0x0053C2EB  test eax, eax
 *   0x0053C2ED  jne 0x53C2E0                retry until it returns 0
 *
 * i.e. KeWaitForSingleObject(dev+0x1DCC, WaitReason 6, UserMode, not
 * alertable, no timeout), looped until STATUS_SUCCESS.  The import identity
 * is from labels.json, confidence 1.0.
 *
 * DISPATCHER_HEADER: byte 0 Type, dword +4 SignalState, +8 WaitListHead. */
static unsigned long long g_kw_iters, g_kw_calls;
static uint32_t g_kw_obj, g_kw_hdr0, g_kw_sig0, g_kw_ret;
static uint32_t g_kw_hdr_last, g_kw_sig_last;
static int g_kw_seen;

void recomp_kw_iter(uint32_t obj)
{
    ++g_kw_iters;
    if (!g_kw_seen) {
        g_kw_seen = 1;
        g_kw_obj  = obj;
        g_kw_hdr0 = MEM32(obj);
        g_kw_sig0 = MEM32(obj + 4u);
    }
    g_kw_hdr_last = MEM32(obj);
    g_kw_sig_last = MEM32(obj + 4u);
}

void recomp_kw_ret(uint32_t v) { ++g_kw_calls; g_kw_ret = v; }

void recomp_dump_kw(void)
{
    if (!g_kw_seen) {
        fprintf(stderr, "[KWAIT] tail wait never reached" "\n");
        return;
    }
    fprintf(stderr, "[KWAIT] KeWaitForSingleObject loop at 0x0053C2E0" "\n");
    fprintf(stderr, "[KWAIT]   object        %08X   (dev + 0x1DCC)" "\n", g_kw_obj);
    fprintf(stderr, "[KWAIT]   loop entries  %llu   returns %llu   inside %lld" "\n",
            g_kw_iters, g_kw_calls,
            (long long)g_kw_iters - (long long)g_kw_calls);
    fprintf(stderr, "[KWAIT]   last return   %08X" "\n", g_kw_ret);
    fprintf(stderr, "[KWAIT]   header  first %08X   now %08X" "\n",
            g_kw_hdr0, g_kw_hdr_last);
    fprintf(stderr, "[KWAIT]   signal  first %08X   now %08X   %s" "\n",
            g_kw_sig0, g_kw_sig_last,
            g_kw_sig_last ? "signalled" : "NOT signalled");
    fprintf(stderr, "[KWAIT]   type byte     %02X" "\n", g_kw_hdr_last & 0xFFu);
    fflush(stderr);
}

void recomp_dump_ip(void)
{
    unsigned c, h;
    unsigned long long any = 0;
    for (c = 0; c < IP_COMBOS; ++c) any += g_ip_calls[c];
    if (!any) return;
    fprintf(stderr, "[IP] arg5 values:");
    for (h = 0; h < 8u; ++h) if (g_ip_v5[h])
        fprintf(stderr, " %u:%llu", h, g_ip_v5[h]);
    if (g_ip_o5) fprintf(stderr, " other:%llu", g_ip_o5);
    fprintf(stderr, "\n" "[IP] arg6 values:");
    for (h = 0; h < 8u; ++h) if (g_ip_v6[h])
        fprintf(stderr, " %u:%llu", h, g_ip_v6[h]);
    if (g_ip_o6) fprintf(stderr, " other:%llu", g_ip_o6);
    fprintf(stderr, "\n" "[IP] arg7 values:");
    for (h = 0; h < 8u; ++h) if (g_ip_v7[h])
        fprintf(stderr, " %u:%llu", h, g_ip_v7[h]);
    if (g_ip_o7) fprintf(stderr, " other:%llu", g_ip_o7);
    fprintf(stderr, "\n");
    fprintf(stderr, "[IP] flags a5,a6,a7 | calls | pixels |");
    for (h = 0; h < IP_HYP; ++h) fprintf(stderr, " %8s", ip_hyp_name[h]);
    fprintf(stderr, " | first copy-miss" "\n");
    for (c = 0; c < IP_COMBOS; ++c) {
        if (!g_ip_calls[c]) continue;
        fprintf(stderr, "[IP]      %u,%u,%u       | %5llu | %6llu |",
                c & 1u, (c >> 1) & 1u, (c >> 2) & 1u,
                g_ip_calls[c], g_ip_px[c]);
        for (h = 0; h < IP_HYP; ++h)
            fprintf(stderr, " %8llu", g_ip_hit[c][h]);
        if (g_ip_have_bad[c])
            fprintf(stderr, " | %08X" "\n", g_ip_first_bad[c]);
        else
            fprintf(stderr, " | none" "\n");
    }
    fflush(stderr);
}

void recomp_dump_mcb(void)
{
    unsigned i;
    if (g_mc_total) {
        fprintf(stderr, "[MCB] over %llu calls: zero displacement %llu, non-zero %llu" "\n",
                g_mc_total, g_mc_zero_mv, g_mc_nonzero_mv);
        fprintf(stderr, "[MCB]   displacement range %d .. %d bytes  (one row = 640)" "\n", g_mc_dmin, g_mc_dmax);
        fprintf(stderr, "[MCB]   arg5 non-zero %llu, arg6 %llu, arg7 %llu, srcPitch != 640 %llu" "\n",
                g_mc_a5nz, g_mc_a6nz, g_mc_a7nz, g_mc_oddpitch);
    }
    if (g_mcb_n == 0u) return;
    fprintf(stderr, "[MCB] %u motion-compensation calls, from call %llu" "\n",
            g_mcb_n, g_mc_from);
    for (i = 0; i < g_mcb_n; ++i) {
        struct mc_rec *r = &g_mcb[i];
        uint32_t src = r->a[0], sp = r->a[1], dst = r->a[2], dp = r->a[3];
        fprintf(stderr, "[MCB] ==== call %llu ====" "\n", r->call);
        fprintf(stderr, "[MCB]   src=%08X srcPitch=%-5u  dst=%08X dstPitch=%u" "\n",
                src, sp, dst, dp);
        fprintf(stderr, "[MCB]   arg5=%08X arg6=%08X arg7=%08X arg8=%08X" "\n",
                r->a[4], r->a[5], r->a[6], r->a[7]);
        fprintf(stderr, "[MCB]   dst-src=%08X  dst coord x=%u y=%u  (dstPitch %u)" "\n",
                dst - src, dp ? (dst % dp) : 0u, dp ? (dst / dp) : 0u, dp);
        if (sp)
            fprintf(stderr, "[MCB]   src coord x=%u y=%u  (srcPitch %u)" "\n",
                    src % sp, src / sp, sp);
        mc_show("src row0", r->src0);
        mc_show("src row1", r->src1);
        mc_show("dst before", r->before);
        if (r->have_after) {
            mc_show("dst after", r->after);
            mc_show("dst +pitch", r->after1);
        } else {
            fprintf(stderr, "[MCB]   (no exit sample)" "\n");
        }
    }
    fflush(stderr);
}

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] unknown target 0x%08X (icall #%llu)\n",
            va, (unsigned long long)g_icall_count);
}

/* ============================================================================
 * Allocator block trace: sub_0053C190
 *
 * Temporary.  The question it answers is "which basic block is the guest in
 * when rendering stops, and how did it get there", which entry/return counts
 * cannot answer and a native stack sample can only approximate.
 *
 * Two buffers, alternated per call, so that whenever the trace is dumped one
 * holds the last call that RETURNED and the other the call still IN PROGRESS.
 * That is exactly the pair being compared, without having to know in advance
 * which call would be the last.
 *
 * Records are capped, but per-PC hit counts are not, so a block executed
 * millions of times is still visible as a count even though only its first
 * few visits carry a full state snapshot.
 * ==========================================================================*/

#define C190_RECS 96u
#define C190_PCS  48u

struct c190_rec {
    uint32_t pc, kind;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
    uint32_t dev_w, base, end, lr, s, req, flags;
    uint32_t dma_get, dma_put, b10;
};

/* kind, so the report can tag what each transition was */
#define C190_BLOCK  0u
#define C190_BRANCH 1u
#define C190_CALL   2u
#define C190_ICALL  3u
#define C190_BACK   4u
#define C190_RET    5u

struct c190_call {
    struct c190_rec rec[C190_RECS];
    unsigned n;
    uint32_t pc[C190_PCS];
    unsigned long long hits[C190_PCS];
    unsigned pcs;
    unsigned long long seq;
    uint32_t arg0, arg1;
    int returned;
    double t_in, t_out;
};

static struct c190_call g_c190[2];
static int g_c190_cur = -1;      /* buffer being filled, -1 = not inside */
static int g_c190_last = -1;     /* buffer of the last completed call */
unsigned long long g_c190_calls, g_c190_rets;

/* Independent site counters.  Deliberately NOT derived from each other:
 * "inside sub_0053C190" must not be inferred from entry minus return. */
enum {
    C190_S_C120, C190_S_C630, C190_S_TAIL, C190_S_KEWAIT,
    C190_S_CB19FC, C190_S_BEA0, C190_S_BFA0, C190_S_BF70, C190_S_COUNT
};
static const char *g_c190_site_name[C190_S_COUNT] = {
    "sub_0053C120 wait", "sub_0053C630 PFB flush", "tail S/R loop 0x0053C2C0",
    "KeWaitForSingleObject 0x0053C2E0", "callback [dev+0x19FC]",
    "sub_0053BEA0", "sub_0053BFA0", "sub_0053BF70"
};
static unsigned long long g_c190_site_in[C190_S_COUNT];
static unsigned long long g_c190_site_out[C190_S_COUNT];

static double c190_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

void recomp_c190_site(unsigned site, int leaving)
{
    if (site >= (unsigned)C190_S_COUNT) return;
    if (leaving) ++g_c190_site_out[site];
    else         ++g_c190_site_in[site];
}

void recomp_c190_enter(void)
{
    struct c190_call *c;

    g_c190_cur = (g_c190_cur == 0) ? 1 : 0;
    c = &g_c190[g_c190_cur];
    c->n = 0; c->pcs = 0; c->returned = 0;
    c->seq = ++g_c190_calls;
    c->t_in = c190_now();
    c->t_out = 0.0;
    /* ret 8: [esp]=return address, then the two arguments */
    c->arg0 = MEM32(g_esp + 4u);
    c->arg1 = MEM32(g_esp + 8u);
}

void recomp_c190_leave(void)
{
    if (g_c190_cur < 0) return;
    g_c190[g_c190_cur].returned = 1;
    g_c190[g_c190_cur].t_out = c190_now();
    g_c190_last = g_c190_cur;
    ++g_c190_rets;
}

void recomp_c190_bb(uint32_t pc, uint32_t kind)
{
    extern uint32_t nv2a_get_dma_put(void);
    extern uint32_t nv2a_get_consumed_abs(void);
    extern uint32_t nv2a_get_ring_base(void);
    extern uint32_t nv2a_peek_b10(void);
    struct c190_call *c;
    struct c190_rec *r;
    uint32_t dev, sp;
    unsigned i;

    if (g_c190_cur < 0) return;
    c = &g_c190[g_c190_cur];

    /* per-PC counts first: these are the only thing that survives a loop */
    for (i = 0; i < c->pcs; ++i)
        if (c->pc[i] == pc) { ++c->hits[i]; break; }
    if (i == c->pcs && c->pcs < C190_PCS) {
        c->pc[c->pcs] = pc; c->hits[c->pcs] = 1u; ++c->pcs;
    }

    if (c->n >= C190_RECS) return;
    r = &c->rec[c->n++];

    r->pc = pc; r->kind = kind;
    r->eax = g_eax; r->ebx = g_ebx; r->ecx = g_ecx; r->edx = g_edx;
    r->esi = g_esi; r->edi = g_edi; r->ebp = g_ebp; r->esp = g_esp;
    r->req = c->arg0; r->flags = c->arg1;

    dev = MEM32(0x5499E8u);
    if (dev) {
        r->dev_w = MEM32(dev);
        r->base  = MEM32(dev + 0x24u);
        r->end   = MEM32(dev + 0x28u);
        r->lr    = MEM32(dev + 0x2Cu);
        sp       = MEM32(dev + 0x30u);
        r->s     = sp ? MEM32(sp) : 0u;
    } else {
        r->dev_w = r->base = r->end = r->lr = r->s = 0u;
    }
    r->dma_get = nv2a_get_consumed_abs();
    r->dma_put = nv2a_get_dma_put();
    r->b10     = nv2a_peek_b10();
    (void)nv2a_get_ring_base;
}

static const char *c190_kind(uint32_t k)
{
    switch (k) {
    case C190_BRANCH: return "branch";
    case C190_CALL:   return "call  ";
    case C190_ICALL:  return "icall ";
    case C190_BACK:   return "BACK  ";
    case C190_RET:    return "ret   ";
    default:          return "block ";
    }
}

static void c190_dump_call(const char *tag, int which)
{
    const struct c190_call *c;
    unsigned i;

    if (which < 0) { fprintf(stderr, "[C190] %s: none recorded\n", tag); return; }
    c = &g_c190[which];
    fprintf(stderr, "[C190] ---- %s: call #%llu  arg0(pos)=%08X arg1(flags)=%08X"
            "  %s  %.3f s\n", tag, c->seq, c->arg0, c->arg1,
            c->returned ? "RETURNED" : "*** STILL INSIDE ***",
            c->returned ? (c->t_out - c->t_in) : (c190_now() - c->t_in));
    fprintf(stderr, "[C190]   %-10s %-6s %-8s %-8s %-8s %-8s %-8s %-8s %-8s "
            "%-8s %-8s %-8s\n", "pc", "kind", "eax", "ecx", "edx", "esi",
            "edi", "[dev]", "[+0x2C]", "S", "DMA_GET", "0xB10");
    for (i = 0; i < c->n; ++i) {
        const struct c190_rec *r = &c->rec[i];
        fprintf(stderr, "[C190]   0x%08X %-6s %08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X\n",
                r->pc, c190_kind(r->kind), r->eax, r->ecx, r->edx, r->esi,
                r->edi, r->dev_w, r->lr, r->s, r->dma_get, r->b10);
    }
    if (c->n >= C190_RECS)
        fprintf(stderr, "[C190]   (record cap reached; per-PC counts below are "
                "complete)\n");
    fprintf(stderr, "[C190]   per-PC visit counts:\n");
    for (i = 0; i < c->pcs; ++i)
        fprintf(stderr, "[C190]     0x%08X  %llu\n", c->pc[i], c->hits[i]);
    if (c->n) {
        const struct c190_rec *l = &c->rec[c->n - 1];
        fprintf(stderr, "[C190]   ring: base=%08X end=%08X size=%08X  write=%08X"
                "  L/R=%08X  S=%08X  DMA_PUT=%08X\n",
                l->base, l->end, l->end - l->base, l->dev_w, l->lr, l->s,
                l->dma_put);
    }
}

void recomp_c190_report(void)
{
    unsigned i;

    fprintf(stderr, "[C190] ================ allocator trace ================\n");
    fprintf(stderr, "[C190] entries=%llu returns=%llu  unmatched=%lld\n",
            g_c190_calls, g_c190_rets,
            (long long)(g_c190_calls - g_c190_rets));
    fprintf(stderr, "[C190] independent site counters (in / out):\n");
    for (i = 0; i < (unsigned)C190_S_COUNT; ++i)
        fprintf(stderr, "[C190]   %-34s %12llu / %-12llu %s\n",
                g_c190_site_name[i], g_c190_site_in[i], g_c190_site_out[i],
                g_c190_site_in[i] != g_c190_site_out[i] ? "<-- UNMATCHED" : "");
    { extern void nv2a_sem_report(void); nv2a_sem_report(); }
    c190_dump_call("last call that RETURNED", g_c190_last);
    c190_dump_call("current call", g_c190_cur);
    fflush(stderr);
}

/* ---- temporary: the c146..c149 constant source ---------------------------
 *
 * sub_00063A60 reaches its SET_VERTEX_SHADER_CONSTANT(146) through two paths:
 * one builds a transposed 4x4 matrix on the stack and points edx at it, the
 * other points edx at an object field and builds nothing.  The uploaded block
 * has been observed carrying a return address, a stack pointer and the D3D
 * device pointer, so the question is which path runs and what wrote the block.
 *
 * The probe reports the path, the source pointer and the sixteen dwords, and
 * flags a block as bad when any word is a non-zero denormal -- an integer
 * wearing a float's clothes, which is what stale stack data looks like here. */

int g_c146_path;                 /* set at each path before the upload */
unsigned long long g_c146_calls, g_c146_bad;
static unsigned long long g_c146_path_calls[3], g_c146_path_bad[3];

void recomp_c146_probe(uint32_t src, uint32_t esi, uint32_t ebx, uint32_t ebp)
{
    const uint32_t *w;
    unsigned i, bad = 0;
    static unsigned logs;
    int path = g_c146_path;

    ++g_c146_calls;
    if (path < 0 || path > 2) path = 0;
    ++g_c146_path_calls[path];

    if (src < 0x1000u || src + 64u > 0x04000000u) {
        ++g_c146_bad;
        ++g_c146_path_bad[path];
        if (logs++ < 8u)
            fprintf(stderr, "[C146] path=%d source=%08X OUT OF RANGE\n",
                    path, src);
        return;
    }
    w = (const uint32_t *)(uintptr_t)((uintptr_t)src +
                                      (uintptr_t)g_xbox_mem_offset);
    for (i = 0; i < 16u; ++i)
        if (w[i] != 0u && (w[i] & 0x7F800000u) == 0u) ++bad;

    if (bad) { ++g_c146_bad; ++g_c146_path_bad[path]; }

    if (bad && logs++ < 6u) {
        fprintf(stderr,
                "[C146] BAD  path=%s source=%08X esi=%08X ebx=%08X ebp=%08X\n"
                "[C146]      [0x84A13C]=%08X  MEM8(0x7FA275)=%02X  "
                "bad lanes=%u\n",
                path == 1 ? "A (transpose)" : path == 2 ? "B (object field)"
                                                        : "?",
                src, esi, ebx, ebp, MEM32(0x84A13Cu),
                (unsigned)MEM8(0x7FA275u), bad);
        for (i = 0; i < 16u; ++i) {
            union { uint32_t u; float f; } c;
            c.u = w[i];
            fprintf(stderr, "[C146]      [%2u] %08X  %-14.6g %s\n", i, c.u,
                    (double)c.f,
                    (c.u != 0u && (c.u & 0x7F800000u) == 0u) ? "<- stale"
                                                            : "");
        }
        fflush(stderr);
    }
}

void recomp_c146_report(void)
{
    fprintf(stderr, "[C146] uploads=%llu bad=%llu   path A %llu (%llu bad)   "
            "path B %llu (%llu bad)   unmarked %llu (%llu bad)\n",
            g_c146_calls, g_c146_bad,
            g_c146_path_calls[1], g_c146_path_bad[1],
            g_c146_path_calls[2], g_c146_path_bad[2],
            g_c146_path_calls[0], g_c146_path_bad[0]);
    fflush(stderr);
}


/* Entry probe on the two set-constant helpers, so every caller is seen

 * regardless of how the constant index is computed. */

extern void recomp_dump_guest_stack(const char *tag);

unsigned long long g_c146e_calls, g_c146e_bad;



void recomp_c146_entry(const char *who, uint32_t idx, uint32_t src)

{

    const uint32_t *w;

    unsigned i, bad = 0, n;

    static unsigned logs;



    if (idx != 146u) return;

    ++g_c146e_calls;

    if (src < 0x1000u || src + 64u > 0x04000000u) return;

    w = (const uint32_t *)(uintptr_t)((uintptr_t)src +

                                      (uintptr_t)g_xbox_mem_offset);

    /* four constants = sixteen dwords */

    for (i = 0, n = 16u; i < n; ++i)

        if (w[i] != 0u && (w[i] & 0x7F800000u) == 0u) ++bad;

    if (!bad) return;

    ++g_c146e_bad;

    if (logs++ < 3u) {

        fprintf(stderr, "[C146E] %s(index=146, source=%08X) has %u stale lanes\n", who, src, bad);

        for (i = 0; i < 16u; ++i) {

            union { uint32_t u; float f; } c; c.u = w[i];

            fprintf(stderr, "[C146E]   [%2u] %08X  %-14.6g %s\n", i,

                    c.u, (double)c.f,

                    (c.u && (c.u & 0x7F800000u) == 0u) ? "<- stale" : "");

        }

        recomp_dump_guest_stack("C146E");

    }

}



void recomp_c146e_report(void)

{

    fprintf(stderr, "[C146E] index-146 uploads=%llu with stale lanes=%llu\n", g_c146e_calls, g_c146e_bad);

    fflush(stderr);

}

/* Full-block upload diagnostic. Suspect bits are not proof of stale memory. */
typedef struct C146Snapshot {
    uint32_t src, count, index, checked;
    uint32_t words[1];
} C146Snapshot;
static unsigned long long c146_calls, c146_words, c146_tail, c146_suspect;
static unsigned long long c146_packets, c146_compared, c146_mismatch, c146_skipped;
static unsigned c146_logs, c146_diff_logs;
static int c146_enabled = -1;
void recomp_c146h_report(void);

/* Validate the complete mapped host range, including high guest arenas. */
static int c146_readable(uint32_t src, uint32_t count)
{
    uintptr_t at, end;
    uint64_t bytes = (uint64_t)count * 4;
    if (!count || bytes > 0x100000000ull - src) return 0;
    at = (uintptr_t)src + (uintptr_t)g_xbox_mem_offset;
    end = at + (uintptr_t)bytes;
    if (end < at) return 0;
    while (at < end) {
        MEMORY_BASIC_INFORMATION m;
        uintptr_t next;
        if (!VirtualQuery((void *)at, &m, sizeof(m)) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        next = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (next <= at) return 0;
        at = next;
    }
    return 1;
}
void *recomp_c146_helper(uint32_t index, uint32_t src, uint32_t count)
{
    C146Snapshot *s;
    uint32_t i;
    if (c146_enabled < 0) c146_enabled = getenv("CONKER_UPLOAD_PROBE") != NULL;
    if (!c146_enabled) return NULL;
    ++c146_calls;
    if (c146_calls == 1) fprintf(stderr, "[UPLOAD-v2] enabled: full entry snapshot and per-packet comparison\n");
    if (!c146_readable(src, count)) {
        if (c146_skipped < 8) fprintf(stderr,"[UPLOAD-v2] SKIP source=%08X count=%u index=%u\n",src,count,index);
        ++c146_skipped; return NULL;
    }
    s = malloc(sizeof(*s) + ((size_t)count-1)*4);
    if (!s) { ++c146_skipped; return NULL; }
    s->src=src; s->count=count; s->index=index; s->checked=0;
    for (i=0; i<count; ++i) s->words[i]=MEM32(src+i*4);
    c146_words += count;
    if (count>16) c146_tail += count-16;
    for (i=0; i<count; ++i) {
        uint32_t v=s->words[i];
        if ((v & 0x7fffffffu) && !(v & 0x7f800000u)) {
            ++c146_suspect;
            if (c146_logs++ < 64) {
                fprintf(stderr,"[UPLOAD-v2] suspect call=%llu index=%u source=%08X count=%u chunk=%u lane=%u word=%u address=%08X bits=%08X\n",c146_calls,index,src,count,i/16,i%16,i,src+i*4,v);
                if (c146_logs==1) recomp_dump_guest_stack("UPLOAD-v2");
            }
        }
    }
    return s;
}
void recomp_c146_packet(void *ctx, uint32_t src, uint32_t dst, uint32_t count)
{
    C146Snapshot *s=ctx;
    uint32_t i, offset;
    if (!s) return;
    offset=(src-s->src)/4;
    if (src<s->src || (src-s->src)%4 || offset>s->count || count>s->count-offset) {
        ++c146_skipped; return;
    }
    ++c146_packets;
    for (i=0;i<count;++i) {
        uint32_t actual=MEM32(dst+i*4), expected=s->words[offset+i];
        ++c146_compared; ++s->checked;
        if(actual!=expected) {
            ++c146_mismatch;
            if(c146_diff_logs++<32) fprintf(stderr,"[UPLOAD-v2] MISMATCH index=%u source=%08X count=%u chunk=%u lane=%u srcaddr=%08X dstaddr=%08X entry=%08X now=%08X emitted=%08X\n",s->index,s->src,s->count,(offset+i)/16,(offset+i)%16,src+i*4,dst+i*4,expected,MEM32(src+i*4),actual);
        }
    }
}
void recomp_c146_finish(void *ctx)
{
    C146Snapshot *s=ctx;
    if (!s) return;
    if(s->checked!=s->count) { ++c146_skipped; fprintf(stderr,"[UPLOAD-v2] INCOMPLETE count=%u checked=%u\n",s->count,s->checked); }
    free(s);
    if (c146_calls % 100000 == 0) recomp_c146h_report();
}
void recomp_c146h_report(void)
{
    if(c146_enabled!=1) return;
    fprintf(stderr,"[UPLOAD-v2] calls=%llu snapshot_words=%llu tail_words=%llu suspect_lanes=%llu packets=%llu compared=%llu mismatches=%llu skipped=%llu\n",c146_calls,c146_words,c146_tail,c146_suspect,c146_packets,c146_compared,c146_mismatch,c146_skipped);
    fflush(stderr);
}


/* Paired with tools/capture_xemu_constants.py. Diagnostic output only. */
void recomp_capture_constants(uint32_t helper, uint32_t index, uint32_t src,
                              uint32_t count, uint32_t caller, uint32_t stack)
{
    static int enabled=-1;
    static ULONGLONG epoch;
    static double after;
    static FILE *out;
    static struct { uint32_t helper,index,caller,count,hits; } keys[128];
    static unsigned used, records;
    unsigned k,i;
    if(enabled<0) {
        const char *path=getenv("CONKER_CONSTANT_CAPTURE");
        enabled=path && *path;
        epoch=GetTickCount64();
        { const char *value=getenv("CONKER_CONSTANT_AFTER"); after=value?atof(value):0.0; }
        if(enabled) { out=fopen(path,"w"); if(!out) enabled=0; }
    }
    if(!enabled || records>=256 || !((index>=64 && index<128)||(index>=146 && index<=149))) return;
    if ((double)(GetTickCount64()-epoch)<after*1000.0) return;
    /* Restrict this paired capture to the observed xemu call sites. */
    if (!((helper==0x00536AA0u && caller==0x003EB373u && index==96u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003EB381u && index==97u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003ED9D6u && index==103u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F822Au && index==100u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F822Au && index==104u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F822Au && index==108u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F8267u && index==101u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F8267u && index==105u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F8290u && index==102u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F8290u && index==106u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F829Bu && index==103u && count==4u) ||
        (helper==0x00536AA0u && caller==0x003F829Bu && index==107u && count==4u) ||
        (helper==0x00536B50u && caller==0x003F0343u && index==104u && count==16u) ||
        (helper==0x00536B50u && caller==0x003F3750u && index==99u && count==16u) ||
        (helper==0x00536C00u && caller==0x0012FB59u && index==146u && count==48u) ||
        (helper==0x00536C00u && caller==0x003F0F41u && index==108u && count==12u) ||
        (helper==0x00536C00u && caller==0x003F36E8u && index==96u && count==12u))) return;
    for(k=0;k<used;k++) if(keys[k].helper==helper && keys[k].index==index && keys[k].caller==caller && keys[k].count==count) break;
    if(k==used) {
        if(used==128) return;
        keys[k].helper=helper; keys[k].index=index; keys[k].caller=caller; keys[k].count=count; ++used;
    }
    if(keys[k].hits>=8) return;
    if(!c146_readable(src,count) || count>768) return;
    ++keys[k].hits; ++records;
    fprintf(out,"{\"event\":\"upload\",\"sample\":%u,\"helper\":\"%08X\",\"caller_return\":\"%08X\",\"index\":%u,\"source\":\"%08X\",\"count_dwords\":%u,\"esp\":\"%08X\",\"words\":[",records,helper,caller,index,src,count,stack);
    for(i=0;i<count;i++) fprintf(out,"%s\"%08X\"",i?",":"",MEM32(src+i*4));
    fprintf(out,"]}\n"); fflush(out);
}


/* Per-call matrix transpose oracle. Snapshots are host locals, never guest data. */
int recomp_matrix_begin(uint32_t *snapshot, uint32_t src, uint32_t caller)
{
    static int enabled=-1;
    unsigned i;
    if(enabled<0) enabled=getenv("CONKER_MATRIX_PROBE")!=NULL;
    if(!enabled) return 0;
    if(!c146_readable(src,16)) { fprintf(stderr,"[MATRIX] unreadable src=%08X caller=%08X\n",src,caller); return 0; }
    for(i=0;i<16;i++) snapshot[i]=MEM32(src+i*4);
    snapshot[16]=src; snapshot[17]=caller;
    return 1;
}
void recomp_matrix_end(const uint32_t *snapshot, uint32_t dst)
{
    static unsigned long long calls, words, mismatches;
    static uint32_t callers[32];
    static unsigned caller_count, samples;
    unsigned r,c,k,bad=0, fresh=0;
    ++calls;
    for(r=0;r<4;r++) for(c=0;c<4;c++) {
        uint32_t got=MEM32(dst+4*(r*4+c)), expected=snapshot[c*4+r];
        ++words;
        if(got!=expected) {
            ++bad; ++mismatches;
            if(mismatches<=32) fprintf(stderr,"[MATRIX] MISMATCH caller=%08X src=%08X dst=%08X row=%u col=%u expected=%08X got=%08X\n",snapshot[17],snapshot[16],dst,r,c,expected,got);
        }
    }
    for(k=0;k<caller_count;k++) if(callers[k]==snapshot[17]) break;
    if(k==caller_count && caller_count<32) { callers[caller_count++]=snapshot[17]; fresh=1; }
    if(fresh || calls==100 || calls==1000 || calls==10000 || (bad && samples<32)) {
        ++samples;
        fprintf(stderr,"[MATRIX] sample call=%llu caller=%08X src=%08X dst=%08X bad=%u source=",calls,snapshot[17],snapshot[16],dst,bad);
        for(k=0;k<16;k++) fprintf(stderr,"%s%08X",k?",":"",snapshot[k]);
        fprintf(stderr," output=");
        for(k=0;k<16;k++) fprintf(stderr,"%s%08X",k?",":"",MEM32(dst+k*4));
        fprintf(stderr,"\n");
        if(fresh) recomp_dump_guest_stack("MATRIX");
    }
    if(calls==1 || calls%4096==0) { fprintf(stderr,"[MATRIX] calls=%llu compared=%llu mismatches=%llu distinct_callers=%u\n",calls,words,mismatches,caller_count); fflush(stderr); }
}


int recomp_matrix_math_begin(uint32_t *s,uint32_t op,uint32_t a,uint32_t b,uint32_t dst,uint32_t caller)
{
    static int enabled=-1;
    unsigned i;
    if(enabled<0) enabled=getenv("CONKER_MATRIX_PROBE")!=NULL;
    if(!enabled || !c146_readable(a,16) || (op==2 && !c146_readable(b,16))) return 0;
    for(i=0;i<16;i++) { s[i]=MEM32(a+i*4); s[16+i]=op==2?MEM32(b+i*4):0; }
    s[32]=op;s[33]=a;s[34]=b;s[35]=dst;s[36]=caller; return 1;
}
void recomp_matrix_math_end(const uint32_t *s,uint32_t result)
{
    static FILE *out;
    static unsigned calls[3];
    static unsigned camera_samples;
    unsigned i,op=s[32],n=++calls[op];
    if(!c146_readable(s[35],16)) return;
    /* Keep camera construction calls that sparse global sampling can miss. */
    if(op==2 && s[36]>=0x00046490u && s[36]<=0x00047010u && camera_samples<2048u) ++camera_samples;
    else if(n>16 && n%257!=0) return;
    if(!out) out=fopen("matrix-math-samples.jsonl","w");
    if(!out) return;
    fprintf(out,"{\"op\":%u,\"call\":%u,\"caller\":\"%08X\",\"a\":\"%08X\",\"b\":\"%08X\",\"dst\":\"%08X\",\"result\":\"%08X\",\"input\":[",op,n,s[36],s[33],s[34],s[35],result);
    for(i=0;i<(op==2?32u:16u);i++) fprintf(out,"%s\"%08X\"",i?",":"",s[i]);
    fprintf(out,"],\"output\":[");
    for(i=0;i<16;i++) fprintf(out,"%s\"%08X\"",i?",":"",MEM32(s[35]+i*4));
    fprintf(out,"]}\n");fflush(out);
}
