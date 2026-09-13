/*
 * Stack-balance reporting for RECOMP_ESP_CHECK builds.
 *
 * A lifted function must leave esp where it found it before its `ret`. When
 * one does not, the caller's next `pop` reads a slot from another frame and a
 * callee-saved register comes back holding a live stack address. The damage
 * appears somewhere else entirely, in code that is correct, which is why this
 * class has taken five separate hand-hunts in this port.
 *
 * Reporting rules matter as much as the check:
 *
 *   - One line per offending function, not per call. A leaking function called
 *     in a loop would otherwise bury everything else, and the second occurrence
 *     tells you nothing the first did not.
 *   - The delta is signed and named. Negative means the function left bytes on
 *     the stack (a leak); positive means it popped bytes it never pushed (an
 *     over-unwind). The two corrupt the caller in different ways and the sign
 *     is the fastest way to tell which you are looking at.
 *   - A bounded table, and a count of what it could not hold, so a build that
 *     turns out to have hundreds of these still says so honestly.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"

#ifdef RECOMP_ESP_CHECK

#define RECOMP_ESP_MAX_REPORTED 256u

static uint32_t g_reported[RECOMP_ESP_MAX_REPORTED];
static unsigned g_reported_count;
static unsigned g_suppressed;
static unsigned g_total_imbalances;

/*
 * Block trace ring.
 *
 * Naming the unbalanced exit is not enough when the epilogue is shared: all 35
 * of sub_0035E210's returns tear down the same frame, so the exit index says
 * where the damage surfaced, not where it happened. The ring records the guest
 * register file as a function walks its basic blocks, and replaying it shows
 * the block where the value went wrong.
 *
 * It keeps the whole register file rather than just esp because the same
 * question keeps arriving in a different register: sub_0035E210 needed the esp
 * step (-348 into one block, 0 everywhere else), sub_00076F10 needed to know
 * which block last had ebx as a list node before it read back as 1.
 *
 * Dumped from two places: the check below when a return is unbalanced, and the
 * fault handler in main.c when the title crashes.
 */
#define RECOMP_BLOCK_TRACE_DEPTH 8192

struct block_trace_entry {
    uint32_t va, esp, eax, ebx, ecx, edx, esi, edi;
};

static __declspec(thread)
    struct block_trace_entry g_trace[RECOMP_BLOCK_TRACE_DEPTH];
static __declspec(thread) unsigned g_trace_count;

void recomp_block_trace(uint32_t va)
{
    struct block_trace_entry *e =
        &g_trace[g_trace_count % RECOMP_BLOCK_TRACE_DEPTH];

    e->va = va;
    e->esp = g_esp;
    e->eax = g_eax;
    e->ebx = g_ebx;
    e->ecx = g_ecx;
    e->edx = g_edx;
    e->esi = g_esi;
    e->edi = g_edi;
    g_trace_count++;
}

void recomp_block_trace_dump(void)
{
    unsigned total = g_trace_count;
    unsigned shown = total < RECOMP_BLOCK_TRACE_DEPTH
                     ? total : RECOMP_BLOCK_TRACE_DEPTH;
    unsigned i;

    if (shown == 0)
        return;

    fprintf(stderr, "[BLOCK] trace (last %u of %u blocks):\n", shown,
            total);
    for (i = total - shown; i < total; i++) {
        const struct block_trace_entry *e = &g_trace[i % RECOMP_BLOCK_TRACE_DEPTH];
        int32_t step = 0;

        if (i > total - shown) {
            const struct block_trace_entry *p =
                &g_trace[(i - 1) % RECOMP_BLOCK_TRACE_DEPTH];
            step = (int32_t)(e->esp - p->esp);
        }
        fprintf(stderr,
                "[BLOCK]   loc_%08X esp=0x%08X %+5d  eax=%08X ebx=%08X "
                "ecx=%08X edx=%08X esi=%08X edi=%08X\n",
                e->va, e->esp, (int)step, e->eax, e->ebx, e->ecx, e->edx,
                e->esi, e->edi);
    }
    fflush(stderr);
}

void recomp_esp_check(const char *name, uint32_t va, unsigned exit_index,
                      uint32_t entry, uint32_t now)
{
    int32_t delta;
    unsigned i;

    if (now == entry)
        return;

    g_total_imbalances++;

    for (i = 0; i < g_reported_count; i++) {
        if (g_reported[i] == va)
            return;                 /* already named; say it once */
    }
    if (g_reported_count >= RECOMP_ESP_MAX_REPORTED) {
        g_suppressed++;
        return;
    }
    g_reported[g_reported_count++] = va;

    delta = (int32_t)(now - entry);
    fprintf(stderr,
            "[ESP] %s (0x%08X) exit #%u returns unbalanced: "
            "entry=0x%08X now=0x%08X delta=%+d (%s)\n",
            name, va, exit_index, entry, now, (int)delta,
            delta < 0 ? "leaked -- caller's pops will read the wrong slots"
                      : "over-unwound -- caller's pops will read another frame");
    recomp_block_trace_dump();
    fflush(stderr);
}

void recomp_esp_check_summary(void)
{
    if (!g_total_imbalances) {
        fprintf(stderr, "[ESP] every lifted function returned balanced\n");
        return;
    }
    fprintf(stderr, "[ESP] %u unbalanced returns from %u distinct functions",
            g_total_imbalances, g_reported_count);
    if (g_suppressed)
        fprintf(stderr, " (+%u more functions not listed: table full)",
                g_suppressed);
    fprintf(stderr, "\n");
    fflush(stderr);
}

#else

void recomp_esp_check_summary(void) { }

#endif /* RECOMP_ESP_CHECK */
