/**
 * Entry point - Conker: Live and Reloaded static recompilation.
 *
 * Loads the user's locally prepared game data and starts the translated
 * guest with the D3D11 renderer. See CHECKPOINT.md for current limitations.
 */

#include "input/xinput_xbox.h"
#include "input/conker_input.h"
extern void recomp_wait_for_guest_threads(void);
void recomp_dump_guest_stack(const char *tag);

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "xbox_memory.h"
#include "kernel.h"
#include "d3d/d3d8_xbox.h"
#include "nv2a/nv2a_mmio_hook.h"
#include "apu/apu_mmio_hook.h"
#include "runtime_config.h"
#include "d3d/d3d8_compile.h"

#pragma comment(lib, "Dbghelp.lib")

/* Conker's decoded entry point, from:
 *   py -3 -m tools.xbe_parser game_files/default.xbe
 * "Entry Point: 0x0042F0E9 (raw: 0xA8BEA742)"
 * The raw value is XOR-obfuscated per XDK convention; 0x0042F0E9 is
 * already decoded and is what recomp_lookup() expects. */
#define CONKER_ENTRY_VA 0x0042F0E9u

static void *load_file(const wchar_t *path, size_t *out_size)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        fprintf(stderr, "Failed to open %ls\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    void *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "Short read on %ls\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

/* Keep the first native access violation visible in run.log.  The generated
 * code otherwise terminates before its Xbox-side register diagnostics can
 * identify the failing instruction. */
/*
 * Serve GPU register and framebuffer accesses that the address fold no longer
 * redirects into system RAM.
 *
 * XBOX_CANONICAL_ADDR maps the VRAM aliases (0xF0000000-0xF3FFFFFF) onto the
 * 64MB the CPU already sees, which is what the hardware does.  Everything above
 * that is real MMIO and must not be backed by plain memory: a register whose
 * bits the hardware clears would latch instead, which is exactly how the D3D8
 * device init used to hang on NV_PFB_WBC's flush bit.
 *
 * Runs as a vectored handler so it sees the fault before the SEH chain, and
 * resumes the faulting instruction once the access has been serviced.
 */
/* TEMPORARY: census of every access violation this handler sees, bucketed by
 * the top byte of the guest address.  Uncapped -- the verbose print above is
 * capped at 8 and that is exactly how a repeating fault gets mistaken for a
 * one-off.  The question it answers: which high ranges does the title
 * actually touch, and over what span, before anything is aliased for it. */
static unsigned long long g_av_hits[256];
static uint32_t g_av_lo[256], g_av_hi[256];
static unsigned long long g_av_reads[256];

static void recomp_av_census(uint32_t va, int is_write)
{
    unsigned b = (unsigned)(va >> 24);
    if (g_av_hits[b] == 0ull) { g_av_lo[b] = va; g_av_hi[b] = va; }
    else {
        if (va < g_av_lo[b]) g_av_lo[b] = va;
        if (va > g_av_hi[b]) g_av_hi[b] = va;
    }
    ++g_av_hits[b];
    if (!is_write) ++g_av_reads[b];
}

void recomp_dump_avmap(void)
{
    unsigned b, any = 0;
    for (b = 0; b < 256u; ++b) {
        if (g_av_hits[b] == 0ull) continue;
        if (!any) { fprintf(stderr, "[AVMAP] faults by top byte:" "\n"); any = 1; }
        fprintf(stderr, "[AVMAP]   %02X.. n=%-8llu reads=%-8llu span %08X..%08X" "\n",
                b, g_av_hits[b], g_av_reads[b], g_av_lo[b], g_av_hi[b]);
    }
    if (any) fflush(stderr);
}


/* TEMPORARY: guest address under a hardware write-watchpoint, 0 = none.
 * Set CONKER_WATCH_VA=<hex guest address> to arm it. Uses the debug registers
 * rather than a guard page because the interesting address shares its page
 * with the fake TIB, which __SEH_prolog writes on essentially every call. */
static uint32_t g_watch_guest_va;

/* The parsed watchpoints, kept so threads created later can arm the same set.
 * Debug registers are per-thread and the title does its work on threads it
 * creates, so arming only the thread that parsed the setting watched a thread
 * that never touches the address. */
static DWORD64 g_watch_slot[4];
static uint32_t g_watch_slot_va[4];   /* guest VA per slot, for reporting */
static int      g_watch_slot_exec[4]; /* 1 = break on execution, 0 = on write */
static int g_watch_slot_count;

/* Apply the parsed watchpoints to the calling thread. */
void recomp_watch_arm_current_thread(void)
{
    CONTEXT ctx;
    int i;

    if (g_watch_slot_count <= 0)
        return;

    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    ctx.Dr0 = g_watch_slot_count > 0 ? g_watch_slot[0] : 0;
    ctx.Dr1 = g_watch_slot_count > 1 ? g_watch_slot[1] : 0;
    ctx.Dr2 = g_watch_slot_count > 2 ? g_watch_slot[2] : 0;
    ctx.Dr3 = g_watch_slot_count > 3 ? g_watch_slot[3] : 0;
    ctx.Dr7 = 0;
    for (i = 0; i < g_watch_slot_count; i++) {
        ctx.Dr7 |= (DWORD64)(1u << (2 * i));
        if (!g_watch_slot_exec[i]) {
            /* R/W = 01 breaks on writes only.  CONKER_WATCH_RW=1 asks for 11
             * instead -- reads as well -- which is the only way to find who
             * consumes a field rather than who fills it.  x86 has no
             * read-only data breakpoint, so this reports both and the value
             * printed is simply what the location holds at the time. */
            static int rw = -1;
            if (rw < 0) rw = getenv("CONKER_WATCH_RW") ? 1 : 0;
            ctx.Dr7 |= ((DWORD64)(rw ? 3u : 1u) << (16 + 4 * i)) |
                       ((DWORD64)3u << (18 + 4 * i));
        }
    }
    SetThreadContext(GetCurrentThread(), &ctx);
}

static void recomp_watch_guest(void)
{
    const char *env = getenv("CONKER_WATCH_VA");
    CONTEXT ctx;
    DWORD64 slot[4];
    int n = 0;
    const char *p;

    /* Either setting on its own is enough: a run may want only execution
     * breakpoints. */
    if ((!env || !*env) && getenv("CONKER_WATCH_EXEC") == NULL)
        return;
    if (env == NULL)
        env = "";

    /* Comma-separated, up to four -- x86 has four debug registers. Watch the
     * physical aliases as well as the cached address: 0x80000000 and
     * 0xA0000000 are separate views of the same page, so a write through one
     * changes what the other reads without ever touching its native address.
     * A watchpoint on the cached view alone stays silent through it. */
    for (p = env; *p && n < 4; ) {
        uint32_t va = (uint32_t)strtoul(p, (char **)&p, 16);
        slot[n] = (DWORD64)((uintptr_t)va + (uintptr_t)g_xbox_mem_offset);
        g_watch_slot_va[n] = va;
        ++n;
        if (n == 1)
            g_watch_guest_va = va;
        while (*p == ',' || *p == ' ')
            p++;
    }

    /* CONKER_WATCH_EXEC=<hex guest VA>[,...] breaks when a guest function is
     * entered.  The address has to be translated first: recompiled code does
     * not execute at guest addresses, it runs as native functions, so a debug
     * register pointed at the guest VA would never fire. */
    { const char *ex = getenv("CONKER_WATCH_EXEC");
      if (ex && *ex) {
          const char *q;
          for (q = ex; *q && n < 4; ) {
              uint32_t va = (uint32_t)strtoul(q, (char **)&q, 16);
              recomp_func_t fn = recomp_lookup(va);
              if (fn != NULL) {
                  slot[n] = (DWORD64)(uintptr_t)fn;
                  g_watch_slot_va[n] = va;
                  g_watch_slot_exec[n] = 1;
                  ++n;
                  fprintf(stderr, "[WATCH] exec breakpoint on guest 0x%08X "
                          "(native %p)\n", va, (void *)fn);
              } else {
                  fprintf(stderr, "[WATCH] guest 0x%08X has no translation; "
                          "cannot break on it\n", va);
              }
              while (*q == ',' || *q == ' ') q++;
          }
      } }

    memcpy(g_watch_slot, slot, sizeof(slot));
    g_watch_slot_count = n;

    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    ctx.Dr0 = n > 0 ? slot[0] : 0;
    ctx.Dr1 = n > 1 ? slot[1] : 0;
    ctx.Dr2 = n > 2 ? slot[2] : 0;
    ctx.Dr3 = n > 3 ? slot[3] : 0;
    /* Per slot i: Li enable (bit 2i), RW = 01 write-only, LEN = 11 four bytes
     * (bits 16+4i .. 19+4i). */
    ctx.Dr7 = 0;
    for (int i = 0; i < n; i++) {
        ctx.Dr7 |= (DWORD64)(1u << (2 * i));           /* Li enable */
        if (!g_watch_slot_exec[i])                      /* RW=01 write, LEN=11 */
            ctx.Dr7 |= ((DWORD64)1u << (16 + 4 * i)) |
                       ((DWORD64)3u << (18 + 4 * i));
        /* execution breakpoints are RW=00 LEN=00: nothing more to set */
    }

    if (!SetThreadContext(GetCurrentThread(), &ctx)) {
        fprintf(stderr, "[WATCH] could not arm watchpoints (err=%lu)\n",
                (unsigned long)GetLastError());
        g_watch_guest_va = 0;
        return;
    }
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    fprintf(stderr, "[WATCH] armed %d watchpoint(s), first guest 0x%08X\n",
            n, g_watch_guest_va);
}

static LONG CALLBACK recomp_mmio_veh(EXCEPTION_POINTERS *info)
{
    EXCEPTION_RECORD *record = info->ExceptionRecord;
    ULONG_PTR operation;
    uintptr_t native;
    long long guest;
    uint32_t va;

    /* TEMPORARY: hardware write-watchpoint report (see recomp_watch_guest). */
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && g_watch_slot_count > 0) {
        static unsigned hits;
        static unsigned traced;
        /* Dr6's low four bits say which breakpoint fired.  Reading the first
         * armed address regardless -- as this did -- reports the wrong value
         * and the wrong address whenever more than one is armed, which is
         * worse than no instrument at all. */
        uint32_t fired_va = g_watch_guest_va;
        int fired_exec = 0;
        uint32_t written;
        { unsigned b;
          for (b = 0; b < 4u; ++b)
              if ((info->ContextRecord->Dr6 & (1ull << b)) && g_watch_slot_va[b]) {
                  fired_va = g_watch_slot_va[b];
                  fired_exec = g_watch_slot_exec[b];
                  break;
              } }
        /* An execution breakpoint has no value to read; the address is code,
         * and dereferencing it would report whatever the instruction bytes
         * happen to spell. */
        written = fired_exec ? 0u
                : *(uint32_t *)((char *)(uintptr_t)fired_va + g_xbox_mem_offset);
        /* A byte-wise memset over the watched dword reports four times, and an
         * 8-hit budget was being spent on the pool allocator's zeroing before
         * anything interesting happened. Spend the stack walk on writes that
         * store a non-zero value. */
        /* Report every hit, zero-writes included: an object being built and
         * then re-zeroed is exactly the sequence worth seeing, and hiding the
         * zeros hides it. */
        ++hits;
        /* Optional value filter for tracing a known source-lane writer. */
        static int watch_value_ready;
        static const char *watch_value_env;
        static uint32_t watch_value;
        if (!watch_value_ready) {
            watch_value_env = getenv("CONKER_WATCH_VALUE");
            if (watch_value_env) watch_value = (uint32_t)strtoul(watch_value_env, NULL, 16);
            watch_value_ready = 1;
        }
        if (watch_value_env ? written == watch_value : (hits < 3000u || (hits % 20000u) == 0u)) {
            void *frames[14];
            /* Walk the LAST writes, not the first: an address in the guest
             * stack is reused by many call chains over a run, and the early
             * hits belong to whoever happened to occupy it at start-up.  The
             * writes that matter are the ones happening while the guest is
             * stuck.  CONKER_WATCH_LATE=N starts walking after N hits. */
            int walk;
            { static unsigned late;
              if (late == 0u) {
                  const char *e = getenv("CONKER_WATCH_LATE");
                  late = (e && atoi(e) > 0) ? (unsigned)atoi(e) : 0xFFFFFFFFu;
              }
              walk = (late == 0xFFFFFFFFu) ? (hits <= 6u || (hits % 20000u) == 0u)
                                          : (hits >= late && hits < late + 80u);
              /* always walk the constant-packet header, whatever the budget */
              if (written == 0x00400B80u || watch_value_env) walk = 1; }
            if (written != 0) traced++;
            (void)traced;
            USHORT n = walk ? CaptureStackBackTrace(0, 14, frames, NULL) : 0;
            HANDLE proc = GetCurrentProcess();
            char sbuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
            SYMBOL_INFO *sym = (SYMBOL_INFO *)sbuf;
            USHORT i;
            fprintf(stderr, fired_exec
                    ? "[WATCH] guest 0x%08X ENTERED (value %08X unused), rip=%p\n"
                    : "[WATCH] guest 0x%08X written -> %08X, rip=%p\n",
                    fired_va, written,
                    (void *)info->ContextRecord->Rip);
            sym->SizeOfStruct = sizeof(*sym);
            sym->MaxNameLen = MAX_SYM_NAME;
            for (i = 0; i < n; ++i) {
                DWORD64 disp = 0;
                if (SymFromAddr(proc, (DWORD64)(uintptr_t)frames[i], &disp, sym))
                    fprintf(stderr, "[WATCH]   %s+0x%llX\n",
                            sym->Name, (unsigned long long)disp);
            }
            fflush(stderr);
        }
        info->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;

    operation = record->ExceptionInformation[0];   /* 0 read, 1 write */
    native = (uintptr_t)record->ExceptionInformation[1];
    guest = (long long)((intptr_t)native - (intptr_t)g_xbox_mem_offset);

    if (guest >= 0 && guest <= 0xFFFFFFFFLL)
        recomp_av_census((uint32_t)guest, operation == 1);

    /* TEMPORARY: report every access violation this handler sees, before any
     * range test.  A fault raised while a vectored handler runs skips the
     * unhandled filter, so an ordinary crash can otherwise vanish as a bare
     * 0xC0000005 with no diagnostics. */
    {
        static unsigned seen;
        if (seen < 8u && (guest < 0 || guest > 0xFFFFFFFFLL ||
                          (uint32_t)guest < 0xF4000000u)) {
            seen++;
            fprintf(stderr, "[MMIO] AV not in GPU space: %s native %p "
                    "(guest 0x%llX) rip=%p\n",
                    operation == 1 ? "write" : "read ", (void *)native,
                    (unsigned long long)guest,
                    (void *)info->ContextRecord->Rip);
            fflush(stderr);
            /* Name the frames.  The unhandled filter symbolizes crashes on the
             * main thread, but the title runs on a thread it created and that
             * path does not reach here -- so a fault like this arrived as a
             * bare address with no function attached to it. */
            recomp_dump_guest_stack("access violation");
        }
    }

    if (guest < 0 || guest > 0xFFFFFFFFLL)
        return EXCEPTION_CONTINUE_SEARCH;
    va = (uint32_t)guest;

    /* NV2A register space: decode the access and run it through the register
     * model, which knows which bits are status and which are storage. */
    if (va >= 0xFD000000u && va < 0xFE000000u) {
        /* TEMPORARY: a fault raised inside this handler terminates the process
         * without reaching the unhandled filter, so a mis-decode looks like a
         * silent 0xC0000005.  Log each access before attempting it. */
        static unsigned traced;
        if (traced < 200u) {
            traced++;
            fprintf(stderr, "[MMIO] %s va=0x%08X rip=%p\n",
                    operation == 1 ? "write" : "read ", va,
                    (void *)info->ContextRecord->Rip);
            fflush(stderr);
        }
        if (nv2a_hook_handle_mmio(info->ContextRecord, native, va,
                                  operation == 1))
            return EXCEPTION_CONTINUE_EXECUTION;
        fprintf(stderr, "[MMIO] UNDECODED %s va=0x%08X rip=%p\n",
                operation == 1 ? "write" : "read ", va,
                (void *)info->ContextRecord->Rip);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* APU register space.  Same reasoning as the NV2A block above: these are
     * status registers, not storage, and the title waits on them.  Conker
     * polls 0xFE820010 until (value & ~3) >= 0x20 before it will initialise
     * its 256 voices; backed by a zeroed page that condition never comes
     * true, and the audio thread spins forever. */
    if (va >= APU_MMIO_VA_BASE && va < APU_MMIO_VA_BASE + APU_MMIO_VA_SIZE) {
        if (apu_hook_handle_mmio(info->ContextRecord, native, va,
                                 operation == 1))
            return EXCEPTION_CONTINUE_EXECUTION;
        fprintf(stderr, "[APU] UNDECODED %s va=0x%08X rip=%p\n",
                operation == 1 ? "write" : "read ", va,
                (void *)info->ContextRecord->Rip);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* AC'97 controller. Small enough to be a register file, but it must
     * not be a latch: DSOUND writes the NABM reset bit, reads it back
     * once, and spins forever if it has not cleared. */
    if (va >= ACI_MMIO_VA_BASE && va < ACI_MMIO_VA_BASE + ACI_MMIO_VA_SIZE) {
        if (aci_hook_handle_mmio(info->ContextRecord, native, va,
                                 operation == 1))
            return EXCEPTION_CONTINUE_EXECUTION;
        fprintf(stderr, "[ACI] UNDECODED %s va=0x%08X rip=%p\n",
                operation == 1 ? "write" : "read ", va,
                (void *)info->ContextRecord->Rip);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* Other high MMIO (anything else a driver pokes) has no model yet.  Back
     * it with zeroed pages so it behaves as it did when the fold sent it to
     * RAM, rather than turning a previously survivable access into a crash. */
    /* Upstream's XBOX_PTR does not fold high addresses, so the VRAM
     * aliases fault here too rather than being redirected into RAM. */
    if (va >= 0xF0000000u) {
        if (nv2a_hook_handle_vram(native, va))
            return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* TEMPORARY: report faults this handler does not own.  A fault raised
     * while a vectored handler is running skips the unhandled filter, so an
     * ordinary access violation here can otherwise vanish as a bare
     * 0xC0000005 with no diagnostics at all. */
    {
        static unsigned foreign;
        if (foreign < 8u) {
            foreign++;
            fprintf(stderr, "[MMIO] not ours: %s Xbox VA 0x%08X native %p "
                    "rip=%p\n", operation == 1 ? "write" : "read ", va,
                    (void *)native, (void *)info->ContextRecord->Rip);
            fflush(stderr);
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * Watchdog: periodically dump where the title is actually executing.
 *
 * A hang looks identical from outside to a slow load -- a window that never
 * paints and a log that stops.  Suspending the game thread and walking its
 * stack names the loop it is spinning in, which no amount of added logging
 * further up will do.
 *
 * Off unless CONKER_WATCHDOG is set, so normal runs are not spammed.  The
 * value, if numeric, is the interval in seconds (default 20).
 */
static HANDLE g_watchdog_target;

/* Aim the stack sampling at a different thread.  The title's work moved onto a
 * thread it creates, and sampling the thread that armed the watchdog now only
 * ever shows it waiting in the join. */
void recomp_watchdog_retarget(void *thread)
{
    HANDLE previous = g_watchdog_target;
    g_watchdog_target = (HANDLE)thread;
    if (previous)
        CloseHandle(previous);
    fprintf(stderr, "[WATCHDOG] retargeted onto the guest thread\n");
}

static void recomp_dump_thread_stack(HANDLE thread, unsigned round)
{
    HANDLE process = GetCurrentProcess();
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    CONTEXT ctx = {0};
    STACKFRAME64 frame = {0};
    unsigned i;

    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(thread) == (DWORD)-1)
        return;
    if (!GetThreadContext(thread, &ctx)) {
        ResumeThread(thread);
        return;
    }

    fprintf(stderr, "[WATCHDOG %u] still running; guest esp=0x%08X "
            "eax=0x%08X ebx=0x%08X ecx=0x%08X edx=0x%08X esi=0x%08X "
            "edi=0x%08X\\n", round, g_esp, g_eax, g_ebx, g_ecx, g_edx,
            g_esi, g_edi);
    {
        /* A spin on a guest pointer is usually a poll of something that never
         * changes.  Report how the address in eax is actually backed: plain
         * committed RAM behaves as a latch, which is exactly how an MMIO
         * register that should self-clear turns into an infinite loop. */
        MEMORY_BASIC_INFORMATION mbi;
        void *probe = (void *)((uintptr_t)g_eax + (uintptr_t)g_xbox_mem_offset);
        if (VirtualQuery(probe, &mbi, sizeof(mbi)))
            fprintf(stderr, "[WATCHDOG %u] eax 0x%08X -> %p state=0x%lX "
                    "protect=0x%lX type=0x%lX region=%llu\\n", round, g_eax,
                    probe, (unsigned long)mbi.State, (unsigned long)mbi.Protect,
                    (unsigned long)mbi.Type,
                    (unsigned long long)mbi.RegionSize);
        else
            fprintf(stderr, "[WATCHDOG %u] eax 0x%08X -> %p unmapped\\n",
                    round, g_eax, probe);
    }
#if defined(_M_X64)
    /* SymInitialize succeeds once per process and returns FALSE ever after,
     * so gating the walk on it meant only the first call printed a stack --
     * exactly the repeat samples you need to tell a hang from slow work. */
    SymInitialize(process, NULL, TRUE);
    {
        symbol->SizeOfStruct = sizeof(*symbol);
        symbol->MaxNameLen = MAX_SYM_NAME;
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (i = 0; i < 20 &&
             StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                         &ctx, NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL); ++i) {
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line = {0};
            DWORD line_displacement = 0;
            line.SizeOfStruct = sizeof(line);
            if (!SymFromAddr(process, frame.AddrPC.Offset, &displacement,
                             symbol))
                continue;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset,
                                     &line_displacement, &line))
                fprintf(stderr, "[WATCHDOG %u] frame[%u]=%s+0x%llX  %s:%lu\\n",
                        round, i, symbol->Name,
                        (unsigned long long)displacement, line.FileName,
                        (unsigned long)line.LineNumber);
            else
                fprintf(stderr, "[WATCHDOG %u] frame[%u]=%s+0x%llX\\n", round, i,
                        symbol->Name, (unsigned long long)displacement);
        }
    }
#endif
    fflush(stderr);
    ResumeThread(thread);
}

/* Rounds are configurable because six of them at the default interval all
 * land during boot, and the phase worth sampling -- the one where the title
 * sits with nothing on screen -- starts well after that. */
static unsigned g_watchdog_rounds = 6;

static DWORD WINAPI recomp_watchdog(LPVOID param)
{
    unsigned interval = (unsigned)(uintptr_t)param;
    unsigned round;

    for (round = 0; round < g_watchdog_rounds; ++round) {
        Sleep(interval * 1000u);
        recomp_dump_thread_stack(g_watchdog_target, round);
    }
    return 0;
}

/*
 * Sample the title's video clock on a fixed cadence.
 *
 * The clock lives at 0x849C6C in milliseconds and the frame period at 0x64A720
 * is 33.3333, so a correct clock gains 1000 per second of wall time.  Sampling
 * it from the present path could not measure that: presents happen a handful of
 * times a run, so every reading was bunched inside one 15 ms tick and the rate
 * was unmeasurable either way.  A thread on its own clock can just watch it.
 */
static DWORD WINAPI recomp_video_clock_sampler(LPVOID param)
{
    ULONGLONG start = GetTickCount64();
    unsigned i;

    (void)param;
    for (i = 0; i < 40u; ++i) {
        ULONGLONG wall;
        float clock;
        uint32_t playing;

        Sleep(500);
        wall = GetTickCount64() - start;
        clock = *(const float *)((const uint8_t *)(uintptr_t)g_xbox_mem_offset
                                 + 0x849C6Cu);
        playing = *(const uint32_t *)((const uint8_t *)(uintptr_t)g_xbox_mem_offset
                                      + 0x849C60u);
        fprintf(stderr, "[VCLOCK] wall=%llums clock=%.3fms playing=%u",
                (unsigned long long)wall, clock, playing);
        if (wall > 0)
            fprintf(stderr, " rate=%.4f (1.0 is correct)",
                    (double)clock / (double)wall);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    return 0;
}

static void recomp_start_video_clock_sampler(void)
{
    if (!getenv("CONKER_VIDEO_CLOCK"))
        return;
    fprintf(stderr, "[VCLOCK] sampling the title's video clock every 500ms\n");
    CloseHandle(CreateThread(NULL, 0, recomp_video_clock_sampler, NULL, 0, NULL));
}

/*
 * TEMPORARY: find the tables that hold a given guest function pointer.
 *
 * Some of the title's per-frame work is reached only through function
 * pointers -- sub_001F6B70, the media tick, has no direct caller anywhere in
 * the generated code.  Knowing which table holds it, and what sits in the
 * neighbouring slots, is how the sibling entries get identified: the entry
 * that drives playback is one of them.
 *
 * CONKER_FIND_PTR=<hex guest VA>[,<delay seconds>] scans guest RAM for that
 * dword and prints each hit with the slots around it.  Read-only.
 */
static DWORD WINAPI recomp_find_pointer(LPVOID param)
{
    const char *env = (const char *)param;
    uint32_t target = (uint32_t)strtoul(env, NULL, 16);
    unsigned delay = 25u;
    int window = 6;
    const char *comma = strchr(env, ',');
    const uint8_t *base;
    uint32_t va;
    unsigned hits = 0;

    if (comma && atoi(comma + 1) > 0) {
        const char *second;
        delay = (unsigned)atoi(comma + 1);
        second = strchr(comma + 1, ',');
        if (second && atoi(second + 1) > 0)
            window = atoi(second + 1);
    }
    Sleep(delay * 1000u);

    /* After the wait, not before: this thread starts ahead of the memory
     * mapping, so reading the base at entry captured 0 and the scan walked
     * off into native address space. */
    base = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    if (base == NULL) {
        fprintf(stderr, "[FINDPTR] guest memory not mapped yet; giving up\n");
        return 0;
    }

    fprintf(stderr, "[FINDPTR] scanning guest RAM for 0x%08X\n", target);

    /* Tables are dword-aligned; step by 4 rather than by byte. */
    for (va = 0x00001000u; va < 0x04000000u && hits < 24u; va += 4u) {
        if (*(const uint32_t *)(base + va) != target)
            continue;
        ++hits;
        fprintf(stderr, "[FINDPTR] hit %u at guest 0x%08X:\n", hits, va);
        {
            int i;
            for (i = -window; i <= window; ++i) {
                uint32_t slot_va = va + (uint32_t)(i * 4);
                uint32_t slot;
                if (slot_va < 0x00001000u || slot_va >= 0x04000000u)
                    continue;
                slot = *(const uint32_t *)(base + slot_va);
                fprintf(stderr, "[FINDPTR]   [%+3d] 0x%08X = 0x%08X%s\n",
                        i, slot_va, slot,
                        i == 0 ? "   <== the one being looked for" :
                        (slot >= 0x00011000u && slot < 0x00561000u) ?
                            "   (looks like code)" : "");
            }
        }
        fflush(stderr);
    }
    fprintf(stderr, "[FINDPTR] done, %u hit(s)\n", hits);
    fflush(stderr);
    return 0;
}

/* Periodic dump of which kernel exports the title has called.  A run is
 * killed rather than exited, so an atexit hook would never fire. */
extern void recomp_dump_kernel_ordinal_histogram(void);

static DWORD WINAPI recomp_ordinal_dumper(LPVOID param)
{
    unsigned delay = (unsigned)(uintptr_t)param;

    for (;;) {
        Sleep(delay * 1000u);
        recomp_dump_kernel_ordinal_histogram();
    }
}

/*
 * Watch a handful of guest addresses over time.
 *
 * CONKER_PROBE=<hex va>[:<hex va>...][,<seconds>] prints each as a dword and
 * as a byte on a fixed cadence.  Written for gates like the one in
 * sub_002A4610, where the video controller's per-frame update is called only
 * while a single byte is zero: knowing the byte's value over a run says
 * outright whether the update is being skipped.
 */
static DWORD WINAPI recomp_probe_thread(LPVOID param)
{
    const char *env = (const char *)param;
    uint32_t va[8];
    unsigned count = 0, delay = 5u, round;
    const char *p = env;
    const char *comma = strchr(env, ',');

    while (*p && count < 8u) {
        va[count++] = (uint32_t)strtoul(p, (char **)&p, 16);
        if (*p == ':') p++;
        else break;
    }
    if (comma && atoi(comma + 1) > 0)
        delay = (unsigned)atoi(comma + 1);

    for (round = 0; ; ++round) {
        const uint8_t *base;
        unsigned i;

        Sleep(delay * 1000u);
        base = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
        if (base == NULL)
            continue;
        fprintf(stderr, "[PROBE %2u]", round);
        for (i = 0; i < count; ++i)
            fprintf(stderr, "  0x%08X = %08X (byte %02X)", va[i],
                    *(const uint32_t *)(base + va[i]),
                    *(const uint8_t *)(base + va[i]));
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

/*
 * Dump the object a guest pointer points at, on a cadence.
 *
 * CONKER_PROBE_OBJ=<hex va of the pointer>,<dwords>,<seconds>.  Comparing
 * consecutive dumps shows which fields move -- frame counters and buffer
 * positions advance while a decoder is working, and stay put when it is
 * stalled -- without touching the generated code, which gets regenerated.
 */
static DWORD WINAPI recomp_probe_obj_thread(LPVOID param)
{
    const char *env = (const char *)param;
    uint32_t ptr_va = (uint32_t)strtoul(env, NULL, 16);
    unsigned dwords = 32u, delay = 8u, round;
    const char *c1 = strchr(env, ',');
    const char *c2 = c1 ? strchr(c1 + 1, ',') : NULL;

    if (c1 && atoi(c1 + 1) > 0) dwords = (unsigned)atoi(c1 + 1);
    if (c2 && atoi(c2 + 1) > 0) delay  = (unsigned)atoi(c2 + 1);
    if (dwords > 128u) dwords = 128u;

    for (round = 0; ; ++round) {
        const uint8_t *base;
        uint32_t obj;
        unsigned i;

        Sleep(delay * 1000u);
        base = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
        if (base == NULL)
            continue;
        obj = *(const uint32_t *)(base + ptr_va);
        if (obj == 0u || obj >= 0x04000000u) {
            fprintf(stderr, "[OBJ %2u] [0x%08X] = 0x%08X (not a guest object)\n",
                    round, ptr_va, obj);
            fflush(stderr);
            continue;
        }
        fprintf(stderr, "[OBJ %2u] object at 0x%08X:\n", round, obj);
        for (i = 0; i < dwords; i += 4u) {
            fprintf(stderr, "[OBJ %2u]  +%03X:", round, i * 4u);
            { unsigned k;
              for (k = 0; k < 4u && i + k < dwords; ++k)
                  fprintf(stderr, " %08X",
                          *(const uint32_t *)(base + obj + (i + k) * 4u)); }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
}

/*
 * Check the lifted 64-bit arithmetic helpers against real 64-bit arithmetic.
 *
 * sub_00470A00 is __allmul and sub_00470D50 is _alldiv.  The XMV decoder's
 * per-frame slice budget is computed through both, and the loop that decodes
 * slices continues or stops on the result -- so a wrong answer here stops the
 * video after a few slices instead of finishing the frame.
 *
 * Both take their two 64-bit arguments on the stack, low dword first, and
 * return the result in edx:eax, cleaning up their own arguments with ret 16.
 *
 * CONKER_TEST_MATH=1 runs it.  Read-only apart from a scratch stack.
 */
extern uint32_t xbox_heap_alloc(uint32_t size, uint32_t alignment);

static long long recomp_call_ll_helper(uint32_t va, long long a, long long b,
                                       uint32_t scratch_top)
{
    recomp_func_t fn = recomp_lookup(va);
    uint32_t saved_esp = g_esp;
    long long result;

    if (!fn)
        return 0;

    g_esp = scratch_top;
    PUSH32(g_esp, (uint32_t)((unsigned long long)b >> 32));  /* B high */
    PUSH32(g_esp, (uint32_t)(unsigned long long)b);          /* B low  */
    PUSH32(g_esp, (uint32_t)((unsigned long long)a >> 32));  /* A high */
    PUSH32(g_esp, (uint32_t)(unsigned long long)a);          /* A low  */
    PUSH32(g_esp, 0xDEAD5150u);                              /* return address */
    fn();
    result = (long long)(((unsigned long long)g_edx << 32) | (unsigned long long)g_eax);
    g_esp = saved_esp;
    return result;
}

static void recomp_test_math(void)
{
    static const struct { long long a, b; } cases[] = {
        {           3,        2200 },   /* the shapes the decoder actually uses */
        {        2200,           3 },
        {           1,           1 },
        {          -1,           1 },
        {          -3,        2200 },
        {  0x100000000LL,       3 },   /* crosses the 32-bit boundary */
        {  0x7FFFFFFFLL,        2 },
        {  0x123456789ALL,   0x100 },
        { -0x123456789ALL,   0x100 },
        {  0x7FFFFFFFFFFFLL,    -7 },
    };
    uint32_t scratch, top;
    unsigned i, mul_bad = 0, div_bad = 0;

    if (!getenv("CONKER_TEST_MATH"))
        return;

    scratch = xbox_heap_alloc(8192u, 16u);
    if (!scratch) {
        fprintf(stderr, "[MATH] no scratch stack; skipping\n");
        return;
    }
    top = scratch + 8192u - 256u;

    fprintf(stderr, "[MATH] checking sub_00470A00 (__allmul) and "
            "sub_00470D50 (_alldiv)\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        long long a = cases[i].a, b = cases[i].b;
        long long want_mul = (long long)((unsigned long long)a * (unsigned long long)b);
        long long got_mul = recomp_call_ll_helper(0x00470A00u, a, b, top);

        if (got_mul != want_mul) {
            ++mul_bad;
            fprintf(stderr, "[MATH] MUL WRONG  %lld * %lld = %lld, lifted gave %lld\n",
                    a, b, want_mul, got_mul);
        }

        if (b != 0) {
            long long want_div = a / b;
            long long got_div = recomp_call_ll_helper(0x00470D50u, a, b, top);
            if (got_div != want_div) {
                ++div_bad;
                fprintf(stderr, "[MATH] DIV WRONG  %lld / %lld = %lld, lifted gave %lld\n",
                        a, b, want_div, got_div);
            }
        }
    }

    fprintf(stderr, "[MATH] __allmul: %u wrong of %u   _alldiv: %u wrong\n",
            mul_bad, (unsigned)(sizeof(cases) / sizeof(cases[0])), div_bad);
    fflush(stderr);
}

/*
 * Uncapped progress sampler.
 *
 * Reports the pushbuffer submission count on a wall clock, with the
 * safe-point counters beside it, once a second and without a cap.  It exists
 * because every progress figure taken from the periodic frame samples or the
 * capped unhandled-method warnings in this tree has at some point been read as
 * a measure of guest work, and been wrong.
 *
 * CONKER_FPS=1 enables it.
 */
extern unsigned nv2a_frame_count(void);
extern unsigned long long g_sp_hits, g_sp_delivered, g_dpc_runs_total, g_sp_ticks;

/* 5 Hz decoder/stream/APU timeline.  Separate from the 1 Hz sampler because
 * the events being lined up happen inside a single second. */
static DWORD WINAPI recomp_timeline_sampler(LPVOID param)
{
    extern void recomp_tl_sample(void);
    (void)param;
    for (;;) {
        Sleep(200);
        recomp_tl_sample();
    }
}

static DWORD WINAPI recomp_fps_sampler(LPVOID param)
{
    unsigned prev_frames = 0;
    unsigned long long prev_hits = 0, prev_dpc = 0, prev_ticks = 0;
    LARGE_INTEGER freq;
    unsigned sec = 0;

    (void)param;
    QueryPerformanceFrequency(&freq);
    for (;;) {
        unsigned frames;
        unsigned long long hits, dpc, ticks;
        double sp_ms;

        Sleep(1000);
        frames = nv2a_frame_count();
        hits = g_sp_hits; dpc = g_dpc_runs_total; ticks = g_sp_ticks;
        sp_ms = (double)(ticks - prev_ticks) * 1000.0 / (double)freq.QuadPart;

        { extern void recomp_dump_bb(void);
          extern void recomp_dump_pstat(void);
          extern void recomp_dump_xmv(void);
          if (sec >= 20u && (sec % 20u) == 0u) {  recomp_dump_pstat();
                                          recomp_dump_xmv();
                                          { extern void recomp_dump_pub(void);
                                            recomp_dump_pub();
                                            { extern void recomp_dump_pend(void);
                                              recomp_dump_pend();
                                              { extern void recomp_dump_ab(void);

                                                { extern void recomp_dump_aisr(void);
                                                  recomp_dump_aisr();
                                                  { extern void recomp_dump_retire(void);
                                                    recomp_dump_retire();
                                                    { extern void recomp_dump_queue(void);
                                                      recomp_dump_queue();
                                                      { extern void recomp_dump_a8(void);
                                                        recomp_dump_a8();
                                                        { extern void recomp_dump_site(void);
                                                          recomp_dump_site();
                                                          { extern void recomp_dump_prod(void);
                                                            recomp_dump_prod();
                                                            { extern void recomp_dump_wrap(void);
                                                              recomp_dump_wrap();
                                                              { extern void recomp_dump_load(void);
                                                                recomp_dump_load();
                                                                { extern void recomp_dump_up(void);
                                                                  recomp_dump_up();
                                                                  { extern void recomp_dump_sing(void);
                                                                    recomp_dump_sing();
                                                                    { extern void recomp_dump_avmap(void);
                                                                      recomp_dump_avmap();
                                                                      { extern void recomp_dump_e28(void);
                                                                        recomp_dump_e28();
                                                                        { extern void recomp_dump_e28clk(void);
                                                                          recomp_dump_e28clk();
                                                                          { extern void recomp_dump_rf(void);
                                                                            recomp_dump_rf();
                                                                            { extern void recomp_dump_pk(void);
                                                                              recomp_dump_pk();
                                                                              { extern void recomp_dump_gate38(void);
                                                                                recomp_dump_gate38();
                                                                                { extern void recomp_dump_caps38(void);
                                                                                  recomp_dump_caps38();
                                                                                  { extern void recomp_dump_rot(void);
                                                                                    recomp_dump_rot();
                                                                                    { extern void recomp_dump_s8(void);
                                                                                      recomp_dump_s8();
                                                                                      { extern void recomp_dump_disp(void);
                                                                                        recomp_dump_disp();
                                                                                        { extern void recomp_dump_done(void);
                                                                                          recomp_dump_done();
                                                                                          { extern void mcpx_apu_vt_dump(void);
                                                                                            mcpx_apu_vt_dump();
                                                                                            { extern void recomp_dump_poll(void);
                                                                                              recomp_dump_poll();
                                                                                              { extern void recomp_dump_arm(void);
                                                                                                recomp_dump_arm();
                                                                                                { extern void recomp_dump_ev(void);
                                                                                                  recomp_dump_ev();
                                                                                                  { extern void recomp_dump_pc(void);
                                                                                                    recomp_dump_pc();
                                                                                                    { extern void recomp_dump_chain2(void);
                                                                                                      recomp_dump_chain2();
                                                                                                      { extern void recomp_dump_irqt(void);
                                                                                                        recomp_dump_irqt();
                                                                                                        { extern void recomp_dump_dpcq(void);
                                                                                                          recomp_dump_dpcq();
                                                                                                          { extern void recomp_dump_aci(void);
                                                                                                            recomp_dump_aci();
                                                                                                            { extern void recomp_dump_aciw(void);
                                                                                                              recomp_dump_aciw();
                                                                                                              { extern void recomp_dump_isr2(void);
                                                                                                                recomp_dump_isr2();
                                                                { extern void recomp_dump_cb(void);
                                                                  recomp_dump_cb();
                                                                  { extern void recomp_dump_amode(void);
                                                                    recomp_dump_amode();
                                                                    { extern void recomp_dump_reg(void);
                                                                      recomp_dump_reg();
                                                                      { extern void recomp_dump_sv(void);
                                                                        recomp_dump_sv();
                                                                        { extern void recomp_dump_rfl(void);
                                                                          recomp_dump_rfl();
                                                                          { extern void recomp_dump_p74(void);
                                                                            recomp_dump_p74();
                                                                            { extern void recomp_dump_slot(void);
                                                                              recomp_dump_slot();
                                                                              { extern void recomp_dump_own(void);
                                                                                recomp_dump_own();
                                                                                { extern void recomp_dump_cmp(void);
                                                                                  recomp_dump_cmp();
                                                                                  { extern void recomp_dump_node(void);
                                                                                    recomp_dump_node();
                                                                                    { extern void recomp_dump_se(void);
                                                                                      recomp_dump_se();
                                                                                      { extern void recomp_dump_tl(void);
                                                                                        recomp_dump_tl();
                                                                                        { extern void recomp_dump_iv(void);
                                                                                          recomp_dump_iv();
                                                                                          { extern void recomp_dump_gen(void);
                                                                                            recomp_dump_gen();
                                                                                            { extern void recomp_dump_sg(void);
                                                                                              recomp_dump_sg();
                                                                                              { extern void recomp_dump_ra(void);
                                                                                                recomp_dump_ra();
                                                                                                { extern void recomp_dump_c2(void);
                                                                                                  recomp_dump_c2();
                                                                                                  { extern void recomp_dump_g68(void);
                                                                                                    recomp_dump_g68();
                                                                                                    { extern void recomp_dump_pg(void);
                                                                                                      recomp_dump_pg();
                                                                                                      { extern void recomp_dump_pd(void);
                                                                                                        recomp_dump_pd();
                                                                                                        { extern void recomp_dump_dc(void);
                                                                                                          recomp_dump_dc();
                                                                                                          { extern void recomp_dump_ck(void);
                                                                                                            recomp_dump_ck();
                                                                                                            { extern void recomp_dump_drift(void);
                                                                                                              recomp_dump_drift();
                                                                                                              { extern void recomp_dump_dl(void);
                                                                                                                recomp_dump_dl();
                                                                                                                { extern void recomp_dump_x3(void);
                                                                                                                  recomp_dump_x3();
                                                                                                                  { extern void recomp_dump_pk23(void);
                                                                                                                    recomp_dump_pk23();
                                                                                                                    { extern void recomp_dump_d2(void);
                                                                                                                      recomp_dump_d2();
                                                                                                                      { extern void recomp_dump_as(void);
                                                                                                                        recomp_dump_as();
                                                                                                                        { extern void recomp_dump_vp(void);
                                                                                                                          recomp_dump_vp();
                                                                                                                          { extern void pgraph_d3d11_dump_texsrc(void);
                                                                                                                            pgraph_d3d11_dump_texsrc();
                            { extern void recomp_dump_mcb(void);
                              recomp_dump_mcb();
                              { extern void recomp_dump_ip(void); recomp_dump_ip();
                                { extern void recomp_dump_hx(void); recomp_dump_hx();
                                  { extern void recomp_dump_vd(void); recomp_dump_vd();
                                    { extern void recomp_dump_a7(void); recomp_dump_a7();
                                      { extern void recomp_dump_cl(void); recomp_dump_cl();
                                        { extern void recomp_dump_fn(void); recomp_dump_fn();
                                          { extern void recomp_dump_ch(void); recomp_dump_ch(); }
                                          { extern void recomp_dump_mv(void); recomp_dump_mv(); }
                                          { extern void recomp_dump_pad(void); recomp_dump_pad(); }
                                          { extern void recomp_dump_dec(void); recomp_dump_dec(); }
                                          { extern void recomp_dump_ds(void); recomp_dump_ds(); }
                                          { extern void recomp_dump_spin(void); recomp_dump_spin(); }
                                          { extern void nv2a_sem_report(void); nv2a_sem_report(); }
                                          { extern void nv2a_dump_putlog(void); nv2a_dump_putlog(); }
                                          { extern void recomp_dump_ring(void); recomp_dump_ring(); }
                                          { extern void recomp_dump_slow(void); recomp_dump_slow(); }
                                          { extern void recomp_dump_rgspin(void); recomp_dump_rgspin(); }
                                          { extern void recomp_dump_chain(void); recomp_dump_chain(); }
                                          { extern void recomp_dump_kw(void); recomp_dump_kw(); }
                                          { extern void xbox_kdisp_report(void); xbox_kdisp_report(); }
                                          { extern void recomp_dump_vt(void); recomp_dump_vt(); }
                                          { extern void recomp_dump_ir(void); recomp_dump_ir(); } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } }
        fprintf(stderr, "[FPS] t=%us frames=%u (+%u/s)  safepoints=%llu (+%llu)  "
                "delivered=%llu  dpc=%llu (+%llu)  sp_cost=%.1fms/s\n",
                ++sec, frames, frames - prev_frames,
                hits, hits - prev_hits, g_sp_delivered,
                dpc, dpc - prev_dpc, sp_ms);
        fflush(stderr);

        /* Once the frame counter stops, dump the allocator trace a few
         * times: the pair of calls it holds is the whole comparison. */
        { static unsigned stalled_for, dumped;
          if (frames == prev_frames) ++stalled_for; else stalled_for = 0;
          if (stalled_for == 3u || stalled_for == 10u || stalled_for == 25u) {
              extern void recomp_c190_report(void);
              fprintf(stderr, "[C190] frame counter stalled %u s\n", stalled_for);
              recomp_c190_report();
              ++dumped;
          }
          (void)dumped; }

        prev_frames = frames; prev_hits = hits; prev_dpc = dpc; prev_ticks = ticks;
    }
}


#define WD_STALL_SECONDS  5u
#define WD_MAX_REPORTS    4u
#define WD_MAX_THREADS    16u

/* Every thread in the process, not just the guest ones.
 *
 * A guest blocked on a lock says nothing about who holds it, and the holder is
 * usually not a guest thread at all -- the APU and NV2A run threads of their
 * own that the guest's MMIO faults reach into.  Dumping only the guest half of
 * a deadlock is what made the last one look like a mystery loop.
 *
 * Threads are suspended one at a time and resumed before the next, so the
 * process keeps moving if the stall turns out to be transient.
 */
static void wd_dump_every_thread(unsigned round)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry;
    DWORD self = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    void *guests[WD_MAX_THREADS];
    unsigned guest_count;
    BOOL more;

    {
        extern unsigned recomp_guest_thread_handles(void **out, unsigned max);
        guest_count = recomp_guest_thread_handles(guests, WD_MAX_THREADS);
    }
    if (snapshot == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[WD] cannot snapshot threads (err=%lu)\n",
                GetLastError());
        return;
    }

    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    for (more = Thread32First(snapshot, &entry); more;
         more = Thread32Next(snapshot, &entry)) {
        HANDLE thread;
        const char *label = "host";
        unsigned i;

        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
            continue;
        for (i = 0; i < guest_count; ++i)
            if (GetThreadId((HANDLE)guests[i]) == entry.th32ThreadID)
                label = "GUEST";

        thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                            THREAD_QUERY_INFORMATION, FALSE,
                            entry.th32ThreadID);
        if (!thread) {
            fprintf(stderr, "[WD]   tid %lu (%s): cannot open (err=%lu)\n",
                    entry.th32ThreadID, label, GetLastError());
            continue;
        }
        fprintf(stderr, "[WD] tid %lu (%s):\n", entry.th32ThreadID, label);
        recomp_dump_thread_stack(thread, round);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

/*
 * Stall trigger for the watchdog above.
 *
 * The interval watchdog samples on a clock, which answers "what is it doing
 * now" but not "when did it stop": the phase worth sampling is whenever the
 * guest quits making progress, and that is not on a schedule.
 *
 * The safe-point counter is bumped by every loop back-edge, so it moving is
 * the definition of the guest running.  When it stops while guest threads
 * exist, the guest is either blocked in a host call or spinning somewhere with
 * no back-edge, and neither shows up in any counter.  This notices that and
 * reuses recomp_dump_thread_stack() on every guest thread, not just the one
 * the interval watchdog was retargeted onto -- a deadlock needs both sides.
 *
 * Reports at 5s of stall and then at widening intervals, capped, so a long
 * operation stays distinguishable from a dead one without filling the log.
 * Diagnostic only: CONKER_WATCHDOG=1 enables these thread samplers.
 * CONKER_NO_WATCHDOG=1 retains the legacy override to disable them.
 */
/*
 * Render-progress watchdog.
 *
 * One run in two stops rendering at frame 8 while guest safe points and
 * DPCs keep advancing -- so the guest is alive and the freeze belongs to
 * the host renderer.  recomp_stall_watchdog watches SAFE POINTS and so
 * says nothing about this case; this one watches the render frame counter
 * and fires on the opposite condition: render stopped, guest still moving.
 *
 * On a stall it takes RP_SHOTS RIP samples of every thread ~50 ms apart --
 * enough to tell a thread parked in a wait from one spinning inside a
 * driver call or looping without progress -- then one full stack dump.
 *
 * Nothing here changes renderer behaviour.
 */
/* Defined further down, next to the other watchdog helpers. */
static void wd_sym_name(DWORD64 address, char *out, size_t out_size);
static void wd_dump_every_thread(unsigned round);

#define RP_SHOTS       16u
#define RP_SHOT_MS     50u
#define RP_STALL_MS    2000u
#define RP_MAX_REPORTS 3u

static const char *rp_phase_name(unsigned p)
{
    switch (p) {
    case 1: return "pushbuffer decode";
    case 2: return "submit_draw";
    case 3: return "DrawPrimitiveUP";
    case 4: return "texture upload";
    case 5: return "render-target bind";
    case 6: return "Present / scene composite";
    default: return "idle/unknown";
    }
}

/* One RIP sample per thread, taken with the thread suspended. */
static void rp_sample_all(unsigned shot)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 e;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    BOOL more;
    extern unsigned g_rp_phase;

    if (snap == INVALID_HANDLE_VALUE) return;
    e.dwSize = sizeof(e);
    for (more = Thread32First(snap, &e); more; more = Thread32Next(snap, &e)) {
        HANDLE th;
        CONTEXT ctx;
        char name[128];
        if (e.th32OwnerProcessID != pid || e.th32ThreadID == self) continue;
        th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                        FALSE, e.th32ThreadID);
        if (!th) continue;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(th) != (DWORD)-1) {
            if (GetThreadContext(th, &ctx)) {
                wd_sym_name((DWORD64)ctx.Rip, name, sizeof(name));
                fprintf(stderr, "[RPW]  shot %2u tid %5lu  phase=%s  %s" "\n",
                        shot, (unsigned long)e.th32ThreadID,
                        rp_phase_name(g_rp_phase), name);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}

static DWORD WINAPI recomp_render_watchdog(LPVOID param)
{
    extern unsigned nv2a_frame_count(void);
    extern unsigned g_rp_phase, g_rp_draw_seq;
    extern uint32_t g_rp_rt, g_rp_tex, g_rp_last_method;
    extern unsigned long long g_rp_presents;
    extern unsigned long long g_apu_iters;
    unsigned prev_frames = 0u;
    unsigned long long prev_sp = 0ull;
    unsigned frozen_ms = 0u, reports = 0u;

    (void)param;
    /* Say so once, so a silent watchdog can be told from a dead one. */
    fprintf(stderr, "[RPW] render-progress watchdog armed (fires after %u ms of no render progress while the guest still runs)\n",
            RP_STALL_MS);
    fflush(stderr);
    for (;;) {
        unsigned frames;
        unsigned long long sp;
        Sleep(200);
        frames = nv2a_frame_count();
        sp = g_sp_hits;

        if (frames != prev_frames) {          /* rendering: re-arm */
            if (reports)
                fprintf(stderr, "[RPW] rendering resumed at frame %u after %u ms" "\n", frames, frozen_ms);
            prev_frames = frames; prev_sp = sp;
            frozen_ms = 0u; reports = 0u;
            continue;
        }
        frozen_ms += 200u;

        /* Only interesting while the GUEST is still moving -- otherwise
         * this is an ordinary guest stall and the other watchdog owns it. */
        if (sp == prev_sp) { prev_sp = sp; continue; }
        prev_sp = sp;
        if (frames == 0u) continue;
        if (frozen_ms < RP_STALL_MS || reports >= RP_MAX_REPORTS) continue;

        fprintf(stderr, "\n" "[RPW] ===== RENDER STALLED =====  frame=%u for %u ms, guest still advancing (safepoints=%llu)" "\n",
                frames, frozen_ms, sp);
        fprintf(stderr, "[RPW] phase=%s  draw_seq=%u  RT=%08X  tex=%08X  "
                "last method=%08X  presents=%llu  apu_frames=%llu" "\n",
                rp_phase_name(g_rp_phase), g_rp_draw_seq, g_rp_rt, g_rp_tex,
                g_rp_last_method, g_rp_presents, g_apu_iters);
        fflush(stderr);

        { unsigned s;
          for (s = 0; s < RP_SHOTS; ++s) {
              rp_sample_all(s);
              Sleep(RP_SHOT_MS);
          } }
        fprintf(stderr, "[RPW] full stacks:" "\n");
        wd_dump_every_thread(reports);
        fflush(stderr);
        ++reports;
    }
}
static DWORD WINAPI recomp_stall_watchdog(LPVOID param)
{
    extern unsigned recomp_guest_thread_handles(void **out, unsigned max);
    unsigned long long prev = 0ull;
    unsigned stalled = 0, reports = 0, next_report = WD_STALL_SECONDS;

    (void)param;
    for (;;) {
        void *threads[WD_MAX_THREADS];
        unsigned count;
        unsigned long long hits;

        Sleep(1000);
        hits = g_sp_hits;

        if (hits != prev) {          /* moving: re-arm, and say nothing */
            prev = hits;
            if (reports)
                fprintf(stderr, "[WD] guest moving again after %us "
                        "(safepoints=%llu)\n", stalled, hits);
            stalled = 0; reports = 0; next_report = WD_STALL_SECONDS;
            continue;
        }
        ++stalled;
        if (hits == 0ull)            /* nothing has started yet */
            continue;
        if (stalled < next_report || reports >= WD_MAX_REPORTS)
            continue;

        count = recomp_guest_thread_handles(threads, WD_MAX_THREADS);
        fprintf(stderr, "[WD] safe points stalled at %llu for %us; "
                "%u guest thread%s; all threads follow\n",
                hits, stalled, count, count == 1u ? "" : "s");
        wd_dump_every_thread(reports);
        fflush(stderr);

        ++reports;
        next_report = stalled + 10u * (reports + 1u);
    }
}

/*
 * Per-second throughput sampler for the APU frame thread.
 *
 * The packet cadence measurement showed the audio-domain packet lifetime is
 * healthy while update calls per APU frame grow fifteen-fold, so the question
 * is where the frame thread's wall time goes.  Everything here is measured,
 * nothing is adjusted.
 *
 * CONKER_HZ=1 enables it.
 */
static DWORD WINAPI recomp_hz_sampler(LPVOID param)
{
    extern unsigned long long mcpx_apu_frame(void);
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned nv2a_frame_count(void);
    extern unsigned long long g_xa2_attempts, g_xa2_accepted, g_xa2_full;
    extern unsigned g_xa2_depth, g_xa2_depth_max;
    extern unsigned long long g_apu_iters, g_apu_t_iter, g_apu_t_locked;
    extern unsigned long long g_apu_t_lockwait, g_apu_t_backpressure;
    extern unsigned long long g_apu_backpressure_loops;
    extern unsigned long long g_apu_idle_entries, g_apu_t_idle;
    unsigned long long p_idle = 0, p_tidle = 0;

    unsigned long long p_apu = 0, p_xmv = 0, p_att = 0, p_acc = 0, p_full = 0;
    unsigned long long p_iters = 0, p_iter = 0, p_locked = 0, p_wait = 0;
    unsigned long long p_bp = 0, p_bpl = 0;
    unsigned p_gpu = 0, sec = 0;
    LARGE_INTEGER freq;

    (void)param;
    QueryPerformanceFrequency(&freq);
    fprintf(stderr, "[HZ] t  apuHz  xmvHz  ratio  gpuHz | qdepth acc/att full |"
            " loop_us lock_us wait_us bp_us bp_loops idle_n idle_ms\n");
    for (;;) {
        unsigned long long apu, xmv, att, acc, full, iters, iter, locked, wait, bp, bpl;
        unsigned gpu;
        double us;

        Sleep(1000);
        ++sec;
        apu = mcpx_apu_frame(); xmv = recomp_rfl_call(); gpu = nv2a_frame_count();
        att = g_xa2_attempts; acc = g_xa2_accepted; full = g_xa2_full;
        iters = g_apu_iters; iter = g_apu_t_iter; locked = g_apu_t_locked;
        wait = g_apu_t_lockwait; bp = g_apu_t_backpressure; bpl = g_apu_backpressure_loops;
        us = 1000000.0 / (double)freq.QuadPart;

        fprintf(stderr,
                "[HZ] %2u  %5llu  %5llu  %5.2f  %5u | %6u %llu/%llu %llu |"
                " %7.1f %7.1f %7.1f %7.1f %llu %llu %.1f\n",
                sec, apu - p_apu, xmv - p_xmv,
                (xmv - p_xmv) ? (double)(apu - p_apu) / (double)(xmv - p_xmv) : 0.0,
                gpu - p_gpu,
                g_xa2_depth, acc - p_acc, att - p_att, full - p_full,
                (iters - p_iters) ? (double)(iter - p_iter) * us / (double)(iters - p_iters) : 0.0,
                (iters - p_iters) ? (double)(locked - p_locked) * us / (double)(iters - p_iters) : 0.0,
                (iters - p_iters) ? (double)(wait - p_wait) * us / (double)(iters - p_iters) : 0.0,
                (double)(bp - p_bp) * us / 1000.0,
                bpl - p_bpl,
                g_apu_idle_entries - p_idle,
                (double)(g_apu_t_idle - p_tidle) * us / 1000.0);
        fflush(stderr);

        p_apu = apu; p_xmv = xmv; p_gpu = gpu;
        p_att = att; p_acc = acc; p_full = full;
        p_iters = iters; p_iter = iter; p_locked = locked; p_wait = wait;
        p_bp = bp; p_bpl = bpl;
        p_idle = g_apu_idle_entries; p_tidle = g_apu_t_idle;
    }
}

/* Every change in XAudio2 queue depth, timestamped. */
void recomp_hz_depth(unsigned depth)
{
    static unsigned last = 0xFFFFFFFFu;
    static unsigned shown;
    extern unsigned long long mcpx_apu_frame(void);
    extern unsigned long long recomp_rfl_call(void);
    if (depth == last) return;
    last = depth;
    if (shown++ < 60u)
        fprintf(stderr, "[HZD] depth -> %u   APU frame %llu   XMV call %llu\n",
                depth, mcpx_apu_frame(), recomp_rfl_call());
}

/*
 * APU-progress watchdog.
 *
 * The general stall watchdog triggers on guest safe points, and the APU
 * failure does not look like that: the guest keeps running at 30 Hz XMV and
 * thousands of GPU frames a second while the APU frame counter goes to exactly
 * zero and stays there.  So this watches the APU counter instead, with the
 * guest counter as the liveness check that distinguishes "the audio stopped"
 * from "everything stopped".
 *
 * On trigger it reuses the all-thread dump, so if the frame thread is blocked
 * on a host primitive the thread that would wake it is in the same capture,
 * and adds a native register/stack dump for the frame thread itself plus the
 * APU-side state that says which part of the loop body it died in.
 *
 * CONKER_APU_STALL_WATCHDOG=1 enables it.  It changes no APU timing, no lock
 * behaviour, no queue limits and no wake conditions -- it only reads, and it
 * resumes every thread it suspends.
 */
#define APUWD_STALL_SECONDS 2u
#define APUWD_MAX_REPORTS   3u

static const char *apu_phase_name(unsigned p)
{
    switch (p) {
    case 0: return "not started";
    case 1: return "top of loop";
    case 2: return "loop body, between phases";
    case 3: return "inside se_frame (VP/DSP frame)";
    case 5: return "inside mcpx_apu_monitor_frame";
    case 6: return "output submission";
    case 7: return "output back-pressure wait";
    default: return "unknown";
    }
}

/* Local symbol resolver: the interval watchdog resolves inline and this
 * needs the name on its own, for the RIP line and each frame. */
static void wd_sym_name(DWORD64 address, char *out, size_t out_size)
{
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    DWORD64 displacement = 0;
    BOOL named = FALSE;

    out[0] = 0;
    __try {
        symbol->SizeOfStruct = sizeof(*symbol);
        symbol->MaxNameLen = MAX_SYM_NAME;
        named = SymFromAddr(GetCurrentProcess(), address, &displacement,
                            symbol);
    } __except (EXCEPTION_EXECUTE_HANDLER) { named = FALSE; }
    if (named)
        _snprintf_s(out, out_size, _TRUNCATE, "%s+0x%llX", symbol->Name,
                    (unsigned long long)displacement);
}

static void apuwd_dump_apu_thread(DWORD tid, unsigned round)
{
    extern unsigned long long g_apu_phase_dummy;
    HANDLE thread;
    CONTEXT ctx;
    CONTEXT walk;
    STACKFRAME64 frame;
    char name[MAX_SYM_NAME + 32];
    unsigned depth;

    thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                        THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) {
        fprintf(stderr, "[APUWD] cannot open the APU thread %lu (err=%lu)\n",
                tid, GetLastError());
        return;
    }
    if (SuspendThread(thread) == (DWORD)-1) {
        fprintf(stderr, "[APUWD] cannot suspend the APU thread\n");
        CloseHandle(thread);
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) {
        fprintf(stderr, "[APUWD] no context for the APU thread\n");
        ResumeThread(thread);
        CloseHandle(thread);
        return;
    }

    wd_sym_name((DWORD64)ctx.Rip, name, sizeof(name));
    fprintf(stderr, "[APUWD] APU thread %lu native state:\n", tid);
    fprintf(stderr, "[APUWD]   RIP=%016llX  %s\n",
            (unsigned long long)ctx.Rip, name[0] ? name : "(unnamed)");
    fprintf(stderr, "[APUWD]   RSP=%016llX RBP=%016llX\n",
            (unsigned long long)ctx.Rsp, (unsigned long long)ctx.Rbp);
    fprintf(stderr, "[APUWD]   RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
            (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rbx,
            (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx);
    fprintf(stderr, "[APUWD]   RSI=%016llX RDI=%016llX R8 =%016llX R9 =%016llX\n",
            (unsigned long long)ctx.Rsi, (unsigned long long)ctx.Rdi,
            (unsigned long long)ctx.R8, (unsigned long long)ctx.R9);
    fprintf(stderr, "[APUWD]   R10=%016llX R11=%016llX R12=%016llX R13=%016llX\n",
            (unsigned long long)ctx.R10, (unsigned long long)ctx.R11,
            (unsigned long long)ctx.R12, (unsigned long long)ctx.R13);
    fprintf(stderr, "[APUWD]   R14=%016llX R15=%016llX EFL=%08lX\n",
            (unsigned long long)ctx.R14, (unsigned long long)ctx.R15,
            (unsigned long)ctx.EFlags);

    walk = ctx;
    memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset    = ctx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    for (depth = 0; depth < 24u; ++depth) {
        BOOL ok = FALSE;
        __try {
            ok = StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(),
                             thread, &frame, &walk, NULL,
                             SymFunctionTableAccess64, SymGetModuleBase64, NULL);
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
        if (!ok || frame.AddrPC.Offset == 0) break;
        wd_sym_name(frame.AddrPC.Offset, name, sizeof(name));
        fprintf(stderr, "[APUWD]     %016llX  %s\n",
                (unsigned long long)frame.AddrPC.Offset,
                name[0] ? name : "(unnamed)");
    }
    ResumeThread(thread);
    CloseHandle(thread);
    (void)round;
}

/* Burst sampler: 20 captures ~50 ms apart while the APU frame is stuck.
 *
 * One capture per stall said the thread was executing but not which loop and
 * not whether it was making progress.  Twenty in a second answers both: the
 * tick counters distinguish "not iterating" from "iterating", and the loop
 * variables distinguish "iterating without advancing" from "advancing far too
 * slowly" from "finite but enormous".
 *
 * The stuck voice is whatever the loops report, never a hardcoded handle.
 */
#define BURST_N 20u

static void apuwd_burst(DWORD tid)
{
    extern unsigned g_vs_voice, g_vs_ssl_index, g_vs_ssl_seg, g_vs_pitch;
    extern unsigned g_vs_sample_count, g_vs_requested, g_vs_inner_requested;
    extern unsigned g_vs_cbo, g_vs_ebo, g_vs_lbo;
    extern unsigned long long g_vs_tick_resample, g_vs_tick_inner,
                              g_vs_calls_get;
    struct shot {
        DWORD64 rip;
        char name[96];
        unsigned voice, sc, req, ireq, cbo, ebo, ssl_i, ssl_s, pitch;
        unsigned long long t_res, t_inner, calls;
        DWORD64 rcx, rdx;
    } shots[BURST_N];
    HANDLE thread;
    unsigned i;

    thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                        THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!thread) {
        fprintf(stderr, "[BURST] cannot open the APU thread\n");
        return;
    }

    for (i = 0; i < BURST_N; ++i) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        shots[i].rip = 0; shots[i].name[0] = 0;
        if (SuspendThread(thread) != (DWORD)-1) {
            if (GetThreadContext(thread, &ctx)) {
                shots[i].rip = ctx.Rip;
                shots[i].rcx = ctx.Rcx;
                shots[i].rdx = ctx.Rdx;
                wd_sym_name(ctx.Rip, shots[i].name, sizeof(shots[i].name));
            }
            /* Read the published loop state while the thread is held, so a
             * sample is internally consistent. */
            shots[i].voice = g_vs_voice;
            shots[i].sc    = g_vs_sample_count;
            shots[i].req   = g_vs_requested;
            shots[i].ireq  = g_vs_inner_requested;
            shots[i].cbo   = g_vs_cbo;
            shots[i].ebo   = g_vs_ebo;
            shots[i].ssl_i = g_vs_ssl_index;
            shots[i].ssl_s = g_vs_ssl_seg;
            shots[i].pitch = g_vs_pitch;
            shots[i].t_res = g_vs_tick_resample;
            shots[i].t_inner = g_vs_tick_inner;
            shots[i].calls = g_vs_calls_get;
            ResumeThread(thread);
        }
        Sleep(50);
    }
    CloseHandle(thread);

    fprintf(stderr, "[BURST] 20 samples, 50 ms apart, on the stuck APU frame\n");
    fprintf(stderr, "[BURST]  #  voice  sample_count/req  inner_req  cbo/ebo"
            "     ssl i/seg  pitch   resample_ticks  inner_ticks  get_calls"
            "   RIP symbol\n");
    for (i = 0; i < BURST_N; ++i) {
        struct shot *sh = &shots[i];
        fprintf(stderr, "[BURST] %2u  %5u  %8u/%-6u %8u  %6u/%-6u %3u/%-3u %6u"
                "  %14llu %12llu %10llu   %s\n",
                i, sh->voice, sh->sc, sh->req, sh->ireq, sh->cbo, sh->ebo,
                sh->ssl_i, sh->ssl_s, sh->pitch,
                sh->t_res, sh->t_inner, sh->calls,
                sh->name[0] ? sh->name : "(unnamed)");
    }
    /* The verdict, stated from the deltas rather than left to the reader. */
    {
        unsigned long long dres = shots[BURST_N - 1].t_res - shots[0].t_res;
        unsigned long long dinn = shots[BURST_N - 1].t_inner - shots[0].t_inner;
        unsigned long long dcal = shots[BURST_N - 1].calls - shots[0].calls;
        int same_vars = (shots[0].sc == shots[BURST_N - 1].sc &&
                         shots[0].cbo == shots[BURST_N - 1].cbo);
        fprintf(stderr, "[BURST] over ~1s: resample ticks +%llu, inner ticks "
                "+%llu, voice_get_samples calls +%llu\n", dres, dinn, dcal);
        if (dres == 0ull && dinn == 0ull && dcal == 0ull)
            fprintf(stderr, "[BURST] verdict: neither loop is iterating -- the "
                    "thread is stuck somewhere else in the frame\n");
        else if (same_vars)
            fprintf(stderr, "[BURST] verdict: iterating without advancing "
                    "sample_count/cbo -- no-progress loop\n");
        else
            fprintf(stderr, "[BURST] verdict: variables advance; finite but "
                    "pathological workload\n");
    }
    fflush(stderr);
}

static DWORD WINAPI recomp_apu_stall_watchdog(LPVOID param)
{
    extern unsigned long long mcpx_apu_frame(void);
    extern unsigned long long recomp_rfl_call(void);
    extern unsigned long long g_apu_thread_id, g_apu_last_frame_qpc;
    extern unsigned g_apu_phase;
    extern unsigned xa2_queue_depth(void);
    extern unsigned xa2_queue_cap(void);
    extern unsigned long long g_apu_iters, g_apu_idle_entries;

    unsigned long long p_apu = 0, p_xmv = 0;
    unsigned stalled = 0, reports = 0;
    LARGE_INTEGER freq;

    (void)param;
    QueryPerformanceFrequency(&freq);
    for (;;) {
        unsigned long long apu, xmv;
        LARGE_INTEGER now;

        Sleep(1000);
        apu = mcpx_apu_frame();
        xmv = recomp_rfl_call();

        if (apu != p_apu || g_apu_thread_id == 0ull) {
            if (stalled >= APUWD_STALL_SECONDS)
                fprintf(stderr, "[APUWD] APU moving again after %us "
                        "(frame %llu)\n", stalled, apu);
            p_apu = apu; p_xmv = xmv; stalled = 0;
            continue;
        }
        ++stalled;

        /* Only interesting while the guest is still alive: a whole-process
         * stall is the other watchdog's business. */
        if (xmv == p_xmv) { p_xmv = xmv; continue; }
        if (stalled < APUWD_STALL_SECONDS || reports >= APUWD_MAX_REPORTS) {
            p_xmv = xmv;
            continue;
        }

        QueryPerformanceCounter(&now);
        fprintf(stderr, "[APUWD] ==== APU stalled %us at frame %llu while the "
                "guest advanced %llu XMV calls ====\n",
                stalled, apu, xmv - p_xmv);
        fprintf(stderr, "[APUWD]   loop iterations completed=%llu  idle-wait "
                "entries=%llu\n", g_apu_iters, g_apu_idle_entries);
        fprintf(stderr, "[APUWD]   phase=%u (%s)\n",
                g_apu_phase, apu_phase_name(g_apu_phase));
        fprintf(stderr, "[APUWD]   last completed frame %.3f s ago\n",
                g_apu_last_frame_qpc
                    ? (double)(now.QuadPart - (LONGLONG)g_apu_last_frame_qpc)
                      / (double)freq.QuadPart : -1.0);
        fprintf(stderr, "[APUWD]   XAudio2 queue depth=%u of %u\n",
                xa2_queue_depth(), xa2_queue_cap());

        apuwd_dump_apu_thread((DWORD)g_apu_thread_id, reports);
        apuwd_burst((DWORD)g_apu_thread_id);
        fprintf(stderr, "[APUWD]   ---- every other thread ----\n");
        wd_dump_every_thread(reports);
        fflush(stderr);

        ++reports;
        p_xmv = xmv;
    }
}

static void recomp_start_fps(void)
{
    /* Thread-suspending diagnostics are opt-in, independent of CONKER_FPS. */
    if (conker_runtime_options()->watchdogs)
        CloseHandle(CreateThread(NULL, 0, recomp_stall_watchdog,
                                 NULL, 0, NULL));
    if (conker_runtime_options()->watchdogs) {
      /* Honor the same opt-out for the render-progress sampler. Suspending
       * a thread that owns a logging/symbol lock can stall the diagnostic. */
      CloseHandle(CreateThread(NULL, 0, recomp_render_watchdog,
                               NULL, 0, NULL)); }
    if (getenv("CONKER_APU_STALL_WATCHDOG"))
        CloseHandle(CreateThread(NULL, 0, recomp_apu_stall_watchdog,
                                 NULL, 0, NULL));
    if (getenv("CONKER_HZ"))
        CloseHandle(CreateThread(NULL, 0, recomp_hz_sampler, NULL, 0, NULL));
    if (!getenv("CONKER_FPS"))
        return;
    CloseHandle(CreateThread(NULL, 0, recomp_fps_sampler, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, recomp_timeline_sampler, NULL, 0, NULL));
}

static void recomp_start_probe_obj(void)
{
    const char *env = getenv("CONKER_PROBE_OBJ");

    if (!env || !*env)
        return;
    CloseHandle(CreateThread(NULL, 0, recomp_probe_obj_thread, (LPVOID)env, 0, NULL));
}

static void recomp_start_probe(void)
{
    const char *env = getenv("CONKER_PROBE");

    if (!env || !*env)
        return;
    CloseHandle(CreateThread(NULL, 0, recomp_probe_thread, (LPVOID)env, 0, NULL));
}

static void recomp_start_ordinal_dumper(void)
{
    const char *env = getenv("CONKER_ORDINALS");
    unsigned delay;

    if (!env || !*env)
        return;
    delay = (unsigned)atoi(env);
    if (delay == 0u)
        delay = 30u;
    CloseHandle(CreateThread(NULL, 0, recomp_ordinal_dumper,
                             (LPVOID)(uintptr_t)delay, 0, NULL));
}

static void recomp_start_find_pointer(void)
{
    const char *env = getenv("CONKER_FIND_PTR");

    if (!env || !*env)
        return;
    CloseHandle(CreateThread(NULL, 0, recomp_find_pointer, (LPVOID)env, 0, NULL));
}

static void recomp_start_watchdog(void)
{
    const char *setting = getenv("CONKER_WATCHDOG");
    unsigned interval;

    if (!setting)
        return;
    interval = (unsigned)atoi(setting);
    if (interval == 0)
        interval = 20;
    { const char *comma = strchr(setting, ',');
      if (comma && atoi(comma + 1) > 0)
          g_watchdog_rounds = (unsigned)atoi(comma + 1); }

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &g_watchdog_target,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        fprintf(stderr, "[WATCHDOG] could not duplicate thread handle\n");
        return;
    }
    fprintf(stderr, "[WATCHDOG] armed: stack dump every %u s, %u rounds\n",
            interval, g_watchdog_rounds);
    CloseHandle(CreateThread(NULL, 0, recomp_watchdog,
                             (LPVOID)(uintptr_t)interval, 0, NULL));
}

/*
 * Report who ended the process.
 *
 * A silent exit(0) is otherwise indistinguishable from a clean shutdown: the
 * title can leave through several paths that call ExitProcess (a WM_QUIT pump,
 * a HAL shutdown thunk, a CRT exit from recompiled code), and only a backtrace
 * says which.  Registered from main, so it fires for every exit that is not a
 * crash -- crashes still go through the unhandled-exception filter above.
 */
/*
 * The guest call stack at an arbitrary point, by name.
 *
 * The title reads 2 MiB of its video and then loops forever waiting for its own
 * playback to finish.  Naming the functions that issue that read names the
 * player, which is the only way to find the flag it is polling -- there are no
 * symbols for the guest, only the sub_XXXXXXXX names the recompiler gives.
 */
/* Print the call stack, safely enough to run from inside a vectored handler.
 *
 * Three hazards, each of which turns a diagnostic into a second failure that
 * hides the first:
 *
 *   - dbghelp is not reentrant and SymInitialize takes the loader lock.  A
 *     fault raised while that lock is held deadlocks, and a fault raised
 *     inside dbghelp re-enters this function.  A recursion guard covers both,
 *     and SymInitialize now runs once instead of on every call.
 *   - symbolization can fault on a frame belonging to no loaded module.  It
 *     is wrapped, and a failure degrades to the raw address rather than
 *     losing the frame.
 *   - the frame addresses are worth having even when no name resolves, so
 *     they are printed unconditionally.
 *
 * The rule is that this reports what it can and returns.  It must never be
 * the reason an exception goes unexplained.
 */
void recomp_dump_guest_stack(const char *tag)
{
    static volatile LONG in_dump;
    static volatile LONG sym_state;   /* 0 unset, 1 usable, 2 unavailable */
    void *frames[20];
    USHORT captured;
    USHORT i;

    if (InterlockedCompareExchange(&in_dump, 1, 0) != 0) {
        fprintf(stderr, "[GSTACK] %s: (reentered, stack omitted)\n",
                tag ? tag : "?");
        fflush(stderr);
        return;
    }

    captured = CaptureStackBackTrace(0, 20, frames, NULL);
    fprintf(stderr, "[GSTACK] %s:\n", tag ? tag : "?");

    if (InterlockedCompareExchange(&sym_state, 1, 0) == 0) {
        __try {
            if (!SymInitialize(GetCurrentProcess(), NULL, TRUE))
                InterlockedExchange(&sym_state, 2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&sym_state, 2);
        }
    }

    for (i = 0; i < captured; ++i) {
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
        SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
        DWORD64 displacement = 0;
        BOOL named = FALSE;

        if (sym_state == 1) {
            __try {
                symbol->SizeOfStruct = sizeof(*symbol);
                symbol->MaxNameLen = MAX_SYM_NAME;
                named = SymFromAddr(GetCurrentProcess(),
                                    (DWORD64)(uintptr_t)frames[i],
                                    &displacement, symbol);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                named = FALSE;
            }
        }
        if (named)
            fprintf(stderr, "[GSTACK]   %s+0x%llX\n", symbol->Name,
                    (unsigned long long)displacement);
        else
            fprintf(stderr, "[GSTACK]   %p\n", frames[i]);
    }
    fflush(stderr);
    InterlockedExchange(&in_dump, 0);
}

static void recomp_report_exit(void)
{
    void *frames[24];
    USHORT captured = CaptureStackBackTrace(0, 24, frames, NULL);
    HANDLE process = GetCurrentProcess();
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    USHORT i;

    fprintf(stderr, "[EXIT] process exiting; %u-frame backtrace follows\\n",
            (unsigned)captured);
    if (!SymInitialize(process, NULL, TRUE)) {
        fflush(stderr);
        return;
    }
    symbol->SizeOfStruct = sizeof(*symbol);
    symbol->MaxNameLen = MAX_SYM_NAME;
    for (i = 0; i < captured; ++i) {
        DWORD64 displacement = 0;
        if (SymFromAddr(process, (DWORD64)(uintptr_t)frames[i],
                        &displacement, symbol))
            fprintf(stderr, "[EXIT] frame[%u]=%s+0x%llX\\n", (unsigned)i,
                    symbol->Name, (unsigned long long)displacement);
        else
            fprintf(stderr, "[EXIT] frame[%u]=%p\\n", (unsigned)i, frames[i]);
    }
    fflush(stderr);
}

static LONG WINAPI recomp_unhandled_exception_filter(EXCEPTION_POINTERS *info)
{
    CONTEXT *ctx = info->ContextRecord;
    HANDLE process = GetCurrentProcess();
    DWORD64 displacement = 0;
    char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buffer;
    IMAGEHLP_LINE64 line = {0};
    DWORD line_displacement = 0;

    line.SizeOfStruct = sizeof(line);
    fprintf(stderr, "[FATAL] native exception=0x%08lX address=%p\\n",
            (unsigned long)info->ExceptionRecord->ExceptionCode,
            info->ExceptionRecord->ExceptionAddress);

    /* For an access violation the faulting data address says far more than the
     * instruction pointer: recompiled code addresses guest memory as
     * native = guest + g_xbox_mem_offset, so translating it back names the
     * exact Xbox VA the title tried to touch. */
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR op   = info->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR addr = info->ExceptionRecord->ExceptionInformation[1];
        const char *kind = (op == 0) ? "read" : (op == 1) ? "write" : "execute";
        long long guest = (long long)((intptr_t)addr - (intptr_t)g_xbox_mem_offset);
        if (guest >= 0 && guest < (long long)XBOX_MEM_SIZE)
            fprintf(stderr, "[FATAL] %s fault at native %p = Xbox VA 0x%08X\\n",
                    kind, (void *)addr, (unsigned)guest);
        else
            fprintf(stderr, "[FATAL] %s fault at native %p (Xbox VA 0x%llX is "
                    "outside the 64MB guest mapping)\\n",
                    kind, (void *)addr, (unsigned long long)guest);
    }
#if defined(_M_X64)
    fprintf(stderr, "[FATAL] RIP=%p RSP=%p RAX=%p RCX=%p RDX=%p\\n",
            (void *)ctx->Rip, (void *)ctx->Rsp, (void *)ctx->Rax,
            (void *)ctx->Rcx, (void *)ctx->Rdx);
    /* The bytes of the faulting instruction.
     *
     * The line table points at a C statement, and a statement is many
     * instructions: placing a probe on the reported line moved the offset
     * three times (+0x308, +0x3F8, +0x3EB) while the fault stayed
     * byte-identical, so the line was not identifying the instruction. The
     * bytes are. Dump a window either side so it can be disassembled
     * backwards to a real boundary, and mark RIP itself.
     */
    {
        const unsigned char *ip = (const unsigned char *)(uintptr_t)ctx->Rip;
        MEMORY_BASIC_INFORMATION lo, hi;
        if (VirtualQuery(ip - 128, &lo, sizeof(lo)) &&
            VirtualQuery(ip + 31, &hi, sizeof(hi)) &&
            lo.State == MEM_COMMIT && hi.State == MEM_COMMIT) {
            int k;
            fprintf(stderr, "[FATAL] bytes RIP-128..RIP+31 ('|' marks RIP):");
            for (k = -128; k < 32; k++)
                fprintf(stderr, "%s%02X", k == 0 ? " |" : " ",
                        (unsigned)ip[k]);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[FATAL] R8=%p R9=%p RBX=%p RSI=%p RDI=%p RBP=%p\n",
                (void *)ctx->R8, (void *)ctx->R9, (void *)ctx->Rbx,
                (void *)ctx->Rsi, (void *)ctx->Rdi, (void *)ctx->Rbp);
        /* The x64 registers above are the *compiler's*, and say almost
         * nothing about the guest: the faulting address usually arrives in
         * whichever scratch register the C expression happened to use. The
         * simulated register file is what identifies the object being
         * dereferenced -- 0xCCCCCCCC in eax with ebx naming the structure it
         * was read out of is the whole diagnosis. These are thread-local and
         * the handler runs on the faulting thread, so they read correctly.
         *
         * ebp is omitted deliberately: lifted functions keep it in a C local
         * and only publish g_ebp across calls, so the global is stale here
         * and would be worse than saying nothing. */
        fprintf(stderr, "[FATAL] guest eax=0x%08X ecx=0x%08X edx=0x%08X "
                "ebx=0x%08X esp=0x%08X esi=0x%08X edi=0x%08X\n",
                g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi);
        /* And the register file over the last N basic blocks, for the
         * functions built with --trace-blocks. A single snapshot says what
         * was wrong; the ring says where it stopped being right. Empty and
         * silent unless something was traced. */
        recomp_block_trace_dump();
    }
    if (SymInitialize(process, NULL, TRUE)) {
        CONTEXT walk_context = *ctx;
        STACKFRAME64 frame = {0};
        DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        unsigned int frame_index;

        symbol->SizeOfStruct = sizeof(*symbol);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (SymFromAddr(process, ctx->Rip, &displacement, symbol))
            fprintf(stderr, "[FATAL] symbol=%s+0x%llX\\n", symbol->Name,
                    (unsigned long long)displacement);
        if (SymGetLineFromAddr64(process, ctx->Rip, &line_displacement, &line))
            fprintf(stderr, "[FATAL] source=%s:%lu+0x%lX\\n", line.FileName,
                    (unsigned long)line.LineNumber, (unsigned long)line_displacement);

        /* The faulting CRT routine is often only a memcpy/memmove.  Walk the
         * captured exception context to identify the game or D3D caller that
         * passed it a bad Xbox virtual address. */
        frame.AddrPC.Offset = walk_context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = walk_context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = walk_context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (frame_index = 0; frame_index < 8 &&
             StackWalk64(machine, process, GetCurrentThread(), &frame,
                         &walk_context, NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL);
             ++frame_index) {
            DWORD64 frame_displacement = 0;
            if (SymFromAddr(process, frame.AddrPC.Offset,
                            &frame_displacement, symbol)) {
                fprintf(stderr, "[FATAL] caller[%u]=%s+0x%llX\\n", frame_index,
                        symbol->Name, (unsigned long long)frame_displacement);
            } else {
                fprintf(stderr, "[FATAL] caller[%u]=%p\\n", frame_index,
                        (void *)(uintptr_t)frame.AddrPC.Offset);
            }
        }
        SymCleanup(process);
    }
#elif defined(_M_IX86)
    fprintf(stderr, "[FATAL] EIP=%p ESP=%p EAX=%p ECX=%p EDX=%p\\n",
            (void *)ctx->Eip, (void *)ctx->Esp, (void *)ctx->Eax,
            (void *)ctx->Ecx, (void *)ctx->Edx);
#endif
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Keep comparison runs at native 480p/4:3 until launcher display options exist.
 * These are client pixels; the caption and borders are added separately. */
enum { CONKER_DISPLAY_WIDTH = 640, CONKER_DISPLAY_HEIGHT = 480 };
enum { CONKER_FPS_TIMER = 1 };

static void recomp_update_fps_title(HWND hwnd)
{
    extern unsigned long long g_xmv_decode_calls;
    extern uint64_t pgraph_d3d11_present_count(void);
    static ULONGLONG previous_time;
    static uint64_t previous_count;
    static int previous_video = -1;
    ULONGLONG now = GetTickCount64();
    int video = g_xbox_mem_offset != 0 &&
        *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x849C60u) != 0;
    uint64_t count = video ? g_xmv_decode_calls : pgraph_d3d11_present_count();
    char title[112];
    if (!previous_time || video != previous_video || count < previous_count) {
        snprintf(title, sizeof(title), "Conker: Live & Reloaded | %s: -- FPS",
                 video ? "Video" : "Game");
    } else {
        double elapsed = (double)(now - previous_time) * 0.001;
        double fps = elapsed > 0 ? (double)(count - previous_count) / elapsed : 0;
        snprintf(title, sizeof(title), "Conker: Live & Reloaded | %s: %.1f FPS",
                 video ? "Video" : "Game", fps);
    }
    previous_time = now;
    previous_count = count;
    previous_video = video;
    SetWindowTextA(hwnd, title);
}
static const DWORD recomp_window_style = WS_OVERLAPPED | WS_CAPTION |
                                         WS_SYSMENU | WS_MINIMIZEBOX;

static RECT recomp_window_bounds(UINT dpi)
{
    RECT bounds = {0, 0, CONKER_DISPLAY_WIDTH, CONKER_DISPLAY_HEIGHT};
    AdjustWindowRectExForDpi(&bounds, recomp_window_style, FALSE, 0, dpi);
    return bounds;
}

static LRESULT CALLBACK recomp_window_proc(HWND hwnd, UINT message,
                                            WPARAM wparam, LPARAM lparam)
{
    if (message == WM_TIMER && wparam == CONKER_FPS_TIMER) {
        recomp_update_fps_title(hwnd);
        return 0;
    }
    if (message == WM_DPICHANGED) {
        const RECT *suggested = (const RECT *)lparam;
        RECT bounds = recomp_window_bounds(HIWORD(wparam));
        /* Moving between monitors changes border sizes, not the game pixels. */
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                     bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (message == WM_CLOSE) {
        fprintf(stderr, "[INFO WINDOW] WM_CLOSE received hwnd=%p\n", (void *)hwnd);
        fflush(stderr);
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(hwnd, CONKER_FPS_TIMER);
        fprintf(stderr, "[INFO WINDOW] WM_DESTROY received hwnd=%p\n", (void *)hwnd);
        fflush(stderr);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

/* Establish the native presentation device before the translated game enters
 * hardware init.  Game-side D3D object wiring is a separate next step. */
static BOOL recomp_init_graphics(void)
{
    static const char class_name[] = "ConkerRecompWindow";
    WNDCLASSA wc = {0};
    D3DPRESENT_PARAMETERS pp = {0};
    IDirect3D8 *d3d;
    IDirect3DDevice8 *device = NULL;
    HWND hwnd;
    HRESULT hr;
    RECT bounds = recomp_window_bounds(GetDpiForSystem());

    wc.lpfnWndProc = recomp_window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = class_name;
    RegisterClassA(&wc); /* already registered is harmless */

    hwnd = CreateWindowExA(0, class_name, "Conker: Live & Reloaded",
                           recomp_window_style | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           bounds.right - bounds.left, bounds.bottom - bounds.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        fprintf(stderr, "[D3D] failed to create window (error=%lu)\n",
                (unsigned long)GetLastError());
        return FALSE;
    }

    /* Wall-clock rate, refreshed even when scene rendering is slow. */
    SetTimer(hwnd, CONKER_FPS_TIMER, 1000u, NULL);

    bounds = recomp_window_bounds(GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, NULL, 0, 0,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    pp.BackBufferWidth = CONKER_DISPLAY_WIDTH;
    pp.BackBufferHeight = CONKER_DISPLAY_HEIGHT;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;

    d3d = xbox_Direct3DCreate8(0);
    if (!d3d) {
        fprintf(stderr, "[D3D] Direct3DCreate8 failed\n");
        return FALSE;
    }
    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 0, hwnd, 0, &pp, &device);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D] CreateDevice failed: 0x%08lX\n",
                (unsigned long)hr);
        return FALSE;
    }

    fprintf(stderr, "[D3D] bootstrap device ready: %p\n", (void *)device);
    GetClientRect(hwnd, &bounds);
    fprintf(stderr, "[D3D] display: backbuffer=%ux%u client=%ldx%ld, fixed 4:3\n",
            pp.BackBufferWidth, pp.BackBufferHeight,
            bounds.right - bounds.left, bounds.bottom - bounds.top);
    return TRUE;
}

/* Native D3D allocation must wait until the translated game has reserved
 * its fixed startup heap address. */
void recomp_init_graphics_after_heap(void)
{
    static BOOL attempted = FALSE;

    if (attempted)
        return;
    attempted = TRUE;

    if (!recomp_init_graphics())
        fprintf(stderr, "[D3D] continuing without a presentation device\n");
}

int wmain(int argc, wchar_t *argv[])
{
    const wchar_t *requested_dir = NULL;
    int check_startup = 0;
    ConkerRuntimePaths paths;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--data-dir") == 0 && i + 1 < argc && argv[i + 1][0])
            requested_dir = argv[++i];
        else if (wcscmp(argv[i], L"--check-startup") == 0)
            check_startup = 1;
        else if (wcscmp(argv[i], L"--help") == 0) {
            puts("Usage: conker_recomp.exe [--data-dir <extracted-game-directory>] [--check-startup]\n"
                 "--check-startup reports data discovery and renderer options without running the game.\n"
                 "It does not verify game version or completeness of the extracted files.");
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %ls (use --help)\n", argv[i]);
            return 1;
        }
    }
    const ConkerRuntimeOptions *options = conker_runtime_options();
    if (!conker_runtime_paths(requested_dir, &paths)) return 1;
    fprintf(stderr, "[startup] game=%s\n[startup] save=%s\n"
                    "[startup] renderer: VSH=%d RC_COMBINERS=%d MULTISTAGE=%d watchdogs=%d\n",
            paths.game_dir, paths.save_dir, options->vertex_shaders,
            options->register_combiners, options->multistage_textures, options->watchdogs);
    if (check_startup) return 0;
    d3d8_shader_cache_init(paths.game_dir);

    /* Avoid Windows bitmap scaling the 640x480 comparison image. */
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        fprintf(stderr, "[WINDOW] DPI awareness setup returned error %lu\n",
                (unsigned long)GetLastError());

    SetUnhandledExceptionFilter(recomp_unhandled_exception_filter);
    atexit(recomp_report_exit);
    /* GPU register access must be serviced before the title runs: D3D8 device
     * init polls NV2A registers during startup. */
    if (!AddVectoredExceptionHandler(1, recomp_mmio_veh))
        fprintf(stderr, "[NV2A] WARNING: could not install the MMIO handler; "
                "GPU register access will fault\n");
    nv2a_hook_init(g_xbox_mem_offset);
    recomp_start_watchdog();
    recomp_start_find_pointer();
    recomp_start_ordinal_dumper();
    recomp_start_probe();
    recomp_start_probe_obj();
    recomp_start_fps();

    size_t xbe_size = 0;
    void *xbe = load_file(paths.xbe, &xbe_size);
    if (!xbe) {
        fprintf(stderr, "Could not load locally prepared XBE: %ls\n", paths.xbe);
        return 1;
    }

    if (!xbox_memory_init(xbe, xbe_size)) {
        fprintf(stderr, "xbox_memory_init failed\n");
        return 1;
    }

    recomp_watch_guest();   /* TEMPORARY: needs g_xbox_mem_offset, so after init */
    { extern void nv2a_vblank_start(void); nv2a_vblank_start(); }
    recomp_test_math();     /* needs the guest heap, so after init too */

    /* The APU reads sample data straight out of guest RAM, so it cannot be
     * brought up until xbox_memory_init has mapped it.  Guest address 0 sits
     * at g_xbox_mem_offset. */
    g_apu_state = mcpx_apu_init_standalone((uint8_t *)g_xbox_mem_offset);
    if (!g_apu_state)
        fprintf(stderr, "[APU] WARNING: init failed; APU registers will read "
                "as unbacked and the title's voice setup will spin\n");

    /* D: is the extracted disc root.  The title's own executable-root
       builder appends its shipped "dvddata\\" content directory. */
    xbox_path_init(paths.game_dir, paths.save_dir);

    fprintf(stderr, "[D3D] device creation deferred until Xbox heap reservation\n");

    fprintf(stderr, "[main] jumping to entry point 0x%08X\n", CONKER_ENTRY_VA);

    recomp_func_t entry = recomp_lookup(CONKER_ENTRY_VA);
    if (!entry) {
        fprintf(stderr, "[main] FATAL: entry point 0x%08X not found in "
                "generated dispatch table. Check recomp_dispatch.c / "
                "functions.json - the entry VA should be in there.\n",
                CONKER_ENTRY_VA);
        return 1;
    }

    /* Bring the gamepad up before the title runs.
     *
     * src/input has mapped the Xbox pad onto host XInput all along, but the
     * translation unit was never compiled into anything and nothing called it,
     * so no controller existed as far as the title was concerned. */
    recomp_start_video_clock_sampler();
    xbox_InputInit();
    conker_input_init();
    {
        XBOX_INPUT_STATE probe;
        unsigned port, found = 0u;
        for (port = 0u; port < XBOX_MAX_CONTROLLERS; ++port)
            if (xbox_InputGetState(port, &probe) == 0)
                ++found;
        fprintf(stderr, "[INFO INPUT] %u controller%s connected\n",
                found, found == 1u ? "" : "s");
    }

    entry();

    /* The title's main loop runs on a thread it created, so the entry point
     * returning is no longer the end of the game -- it is the point where the
     * creator has nothing left to do.  Exiting here would take the loop with
     * it. */
    recomp_wait_for_guest_threads();
    fprintf(stderr, "[INFO MAIN] translated entry point returned normally\n");
    fflush(stderr);

    fprintf(stderr, "[main] entry point returned (unexpected for a game - "
            "it should run its own loop and never get here cleanly)\n");

    /* Diagnostic: did anything actually happen, or did we no-op through
     * everything silently? RECOMP_ICALL never calls recomp_icall_fail_log,
     * so this is the only way to see what indirect calls were attempted. */
    fprintf(stderr, "[main] total ICALLs attempted: %llu\n",
            (unsigned long long)g_icall_count);
    fprintf(stderr, "[main] last %d ICALL targets (most recent last):\n",
            ICALL_TRACE_SIZE);
    uint32_t start = g_icall_trace_idx >= ICALL_TRACE_SIZE
                    ? g_icall_trace_idx - ICALL_TRACE_SIZE : 0;
    for (uint32_t i = start; i < g_icall_trace_idx; i++) {
        fprintf(stderr, "    0x%08X\n",
                g_icall_trace[i & (ICALL_TRACE_SIZE - 1)]);
    }

    return 0;
}
