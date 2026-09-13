/**
 * Kernel thunk table - maps Xbox kernel ordinals to kstub_*
 * implementations from templates/runtime/kernel_stubs.h.
 *
 * CONFIRMED (not guessed) from a real run: unresolved kernel calls
 * show up in the ICALL trace as VA = 0x80000000 | ordinal. E.g.
 * 0x800000FF = ordinal 255 = PsCreateSystemThreadEx, 0x800000BB =
 * ordinal 187 = NtClose. This matches the standard Xbox XBE import
 * encoding (high bit set + ordinal in the low bits) - simpler than
 * the synthetic 0xFE000000+ scheme originally guessed from a header
 * comment.
 *
 * IMPORTANT: recomp_types.h's RECOMP_ICALL/RECOMP_ICALL_SAFE macros
 * must have their garbage-VA fast-path upper bound changed from
 * 0xFE000000 to 0x80000000, or these calls get silently swallowed
 * before recomp_lookup_kernel() is ever reached. See chat notes.
 */

#include <stdlib.h>
#include "recomp_types.h"
#include "xbox_kernel_stubs.h"   /* renamed copy of templates/runtime/kernel_stubs.h */
#include <stdio.h>
#include <setjmp.h>
#include "kernel_guest_timer.h"

typedef void (*bridge_func_t)(void);

extern bridge_func_t bridge_for_ordinal(ULONG ordinal);
extern void recomp_watch_arm_current_thread(void);   /* main.c */
extern recomp_func_t xbox_kernel_bridge_direct(ULONG ordinal);
#define KTHUNK_BASE 0x80000000u
extern NTSTATUS __stdcall xbox_NtAllocateVirtualMemory(
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect);
extern uint32_t xbox_heap_alloc(uint32_t size, uint32_t alignment);

#define KTHUNK_BASE 0x80000000u

typedef struct {
    uint32_t ordinal;
    const char *name;
    void *fn;   /* actual kstub_* function pointer */
} kthunk_entry_t;

/* Populated from your xbe_parser "Kernel Imports" list.
 * Only entries with an actual kstub_* implementation are usable;
 * everything else falls through to a generic logging stub. */
static const kthunk_entry_t g_kthunks[] = {
    { 15,  "ExAllocatePoolWithTag",       (void*)kstub_ExAllocatePoolWithTag },
    { 17,  "ExFreePool",                  NULL }, /* TODO: implement */
    { 165, "MmAllocateContiguousMemory",  (void*)kstub_MmAllocateContiguousMemory },
    { 166, "MmAllocateContiguousMemoryEx",(void*)kstub_MmAllocateContiguousMemoryEx },
    { 171, "MmFreeContiguousMemory",      (void*)kstub_MmFreeContiguousMemory },
    { 173, "MmGetPhysicalAddress",        (void*)kstub_MmGetPhysicalAddress },
    { 181, "MmQueryStatistics",           (void*)kstub_MmQueryStatistics },
    { 184, "NtAllocateVirtualMemory",     (void*)kstub_NtAllocateVirtualMemory },
    { 187, "NtClose",                     (void*)kstub_NtClose },
    { 190, "NtCreateFile",                (void*)kstub_NtCreateFile },
    { 199, "NtFreeVirtualMemory",         (void*)kstub_NtFreeVirtualMemory },
    { 211, "NtQueryInformationFile",      (void*)kstub_NtQueryInformationFile },
    { 219, "NtReadFile",                  (void*)kstub_NtReadFile },
    { 236, "NtWriteFile",                 (void*)kstub_NtWriteFile },
    { 289, "RtlInitAnsiString",           (void*)kstub_RtlInitAnsiString },
    { 301, "RtlNtStatusToDosError",       (void*)kstub_RtlNtStatusToDosError },
    { 1,   "AvGetSavedDataAddress",       (void*)kstub_AvGetSavedDataAddress },
    { 2,   "AvSendTVEncoderOption",       (void*)kstub_AvSendTVEncoderOption },
    { 3,   "AvSetDisplayMode",            (void*)kstub_AvSetDisplayMode },
    { 4,   "AvSetSavedDataAddress",       (void*)kstub_AvSetSavedDataAddress },
    { 246, "ObReferenceObjectByHandle",   (void*)kstub_ObReferenceObjectByHandle },
    { 250, "ObfDereferenceObject",        (void*)kstub_ObfDereferenceObject },
    { 8,   "DbgPrint",                    (void*)kstub_DbgPrint },
    { 97,  "KeBugCheck",                  (void*)kstub_KeBugCheck },
    { 129, "KeRaiseIrqlToDpcLevel",       (void*)kstub_KeRaiseIrqlToDpcLevel },
    { 139, "KeRestoreFloatingPointState", (void*)kstub_KeRestoreFloatingPointState },
    { 142, "KeSaveFloatingPointState",    (void*)kstub_KeSaveFloatingPointState },
    { 49,  "HalReturnToFirmware",         (void*)kstub_HalReturnToFirmware },
    { 253, "PhyInitialize",               (void*)kstub_PhyInitialize },
    { 252, "PhyGetLinkState",             (void*)kstub_PhyGetLinkState },
    { 327, "XeLoadSection",               (void*)kstub_XeLoadSection },
    { 328, "XeUnloadSection",             (void*)kstub_XeUnloadSection },
    /* Remaining ~115 of Conker's 145 imports (Ke*, Io*, Rtl*, Xc*, etc.)
     * have no template stub yet - they need either a hand-written
     * implementation or a generic logging no-op. See the full list in
     * your xbe_parser output under "Kernel Imports (145)". */
};

static const size_t g_kthunks_count = sizeof(g_kthunks) / sizeof(g_kthunks[0]);

/* This wrapper trampoline problem is real and unsolved by this scaffold:
 * kstub_* functions have real C signatures (e.g. taking unsigned long),
 * but recomp_func_t is void(void) - arguments come off the simulated
 * Xbox stack instead. You need per-function trampolines that read
 * MEM32(g_esp+N) for each stdcall argument and call the real kstub_*
 * function, then write the return value to g_eax. That glue is
 * game/ABI-specific and is NOT provided by the templates - budget
 * real time for this. Example for one function: */

/* Pool block sizes.
 *
 * ExQueryPoolBlockSize is not bookkeeping. A real Xbox pool rounds an
 * allocation up to a block, and DSOUND's allocator wrapper (sub_004C6A41)
 * takes the *queried* size as the block's true extent: it zeroes that many
 * bytes, adds them to the running total at 0x004E1D18, and the heap it builds
 * over the block is described by it.
 *
 * The bridge for this was a stub returning 0 -- and it was wired to ordinal
 * 24, which is ExQueryNonVolatileSetting, so ordinal 23 had no bridge at all
 * and returned 0 by default either way. Every arena was therefore described as
 * empty, and the circular free-list walk in sub_004C996B never reached its
 * sentinel: the title hung with no crash and no log output.
 *
 * The guest pool is the bump allocator, so record what was handed out. Blocks
 * are searched newest-first: allocations number in the hundreds and a query
 * happens once per allocation, so a linear scan is not worth replacing with
 * anything cleverer that would have to assume the cursor never moves backward
 * (xbox_heap_alloc skips reserved ranges, so it can).
 */
#define XBOX_POOL_MAX_BLOCKS 8192u

typedef struct {
    uint32_t va;
    uint32_t size;
} xbox_pool_block_t;

static xbox_pool_block_t g_pool_blocks[XBOX_POOL_MAX_BLOCKS];
static uint32_t g_pool_block_count;

static void xbox_pool_record(uint32_t va, uint32_t size)
{
    if (!va)
        return;
    if (g_pool_block_count >= XBOX_POOL_MAX_BLOCKS) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[kthunk] pool block table full (%u); "
                    "ExQueryPoolBlockSize will start returning 0\n",
                    XBOX_POOL_MAX_BLOCKS);
        }
        return;
    }
    g_pool_blocks[g_pool_block_count].va = va;
    g_pool_blocks[g_pool_block_count].size = size;
    g_pool_block_count++;
}

/* Size of the pool block starting exactly at `va`, or 0 if we did not hand it
 * out. The caller treats 0 as "not a pool block", which is the truth. */
static uint32_t xbox_pool_block_size(uint32_t va)
{
    uint32_t i = g_pool_block_count;

    while (i-- > 0) {
        if (g_pool_blocks[i].va == va)
            return g_pool_blocks[i].size;
    }
    return 0;
}

static void thunk_ExQueryPoolBlockSize(void)
{
    uint32_t block = MEM32(g_esp + 4);

    g_eax = xbox_pool_block_size(block);
    fprintf(stderr, "[kthunk] ExQueryPoolBlockSize(0x%08X) = %u\n",
            block, g_eax);
    g_esp += 8;  /* stdcall: callee pops the return address + 1 arg */
}

static void thunk_ExAllocatePoolWithTag(void)
{
    unsigned long bytes = MEM32(g_esp + 4);
    unsigned long tag   = MEM32(g_esp + 8);

    /* Allocate out of guest memory, not the host process heap.
     *
     * This used to call kstub_ExAllocatePoolWithTag, which is a HeapAlloc, and
     * then convert with `(uint32_t)(p - g_xbox_mem_offset)`.  That conversion
     * only works for a pointer that is actually inside the 64MB guest mapping;
     * for host heap memory the difference does not fit in 32 bits, so the
     * truncated result mapped back to an address gigabytes away and the first
     * write through it faulted.  bridge_ExAllocatePoolWithTag already did the
     * right thing, but recomp_lookup_kernel special-cases ordinal 15 to this
     * thunk, so the broken one shadowed it. */
    (void)tag;
    g_eax = xbox_heap_alloc((uint32_t)bytes, 16);
    xbox_pool_record(g_eax, (uint32_t)bytes);
    if (g_eax) {
        uint32_t i;
        for (i = 0; i < (uint32_t)bytes; i++)
            MEM8(g_eax + i) = 0;   /* pool allocations are zeroed */
    }
    fprintf(stderr, "[kthunk] ExAllocatePoolWithTag(%lu) = Xbox VA 0x%08X\n",
            bytes, g_eax);
    g_esp += 12; /* stdcall: callee pops ret addr + 2 args */
}

/* Ordinal 187: NtClose(HANDLE Handle) - 1 stdcall arg. */
static void thunk_NtClose(void)
{
    bridge_func_t bridge = bridge_for_ordinal(187);
    if (!bridge) {
        g_eax = 0xC0000002u; /* STATUS_NOT_IMPLEMENTED */
        g_esp += 8;
        return;
    }

    fprintf(stderr, "[kthunk] NtClose(token=0x%08X)\\n", MEM32(g_esp + 4));
    g_esp += 4; /* expose Handle at STACK_ARG(0) */
    bridge();
    g_esp += 4; /* one stdcall argument */
}

/* Ordinal 202: NtOpenFile has six stdcall arguments.  The bridge functions
 * normally run via kernel_thunk_dispatch(), which consumes the dummy return
 * address before calling them.  Recompiled import calls bypass that layer. */
static void thunk_NtOpenFile(void)
{
    bridge_func_t bridge = bridge_for_ordinal(202);
    if (!bridge) {
        g_eax = 0xC0000002u; /* STATUS_NOT_IMPLEMENTED */
        g_esp += 28;
        return;
    }

    fprintf(stderr, "[kthunk] NtOpenFile(handle_ptr=0x%08X)\n", MEM32(g_esp + 4));
    g_esp += 4; /* expose arg 1 at the bridge's STACK_ARG(0) */
    bridge();
    g_esp += 24; /* six stdcall arguments */
}

/* Ordinal 190: NtCreateFile has nine stdcall arguments.  Like NtOpenFile,
 * direct recompiled import calls have not had their dummy return address
 * consumed by kernel_thunk_dispatch(). */
static void thunk_NtCreateFile(void)
{
    bridge_func_t bridge = bridge_for_ordinal(190);
    if (!bridge) {
        g_eax = 0xC0000002u; /* STATUS_NOT_IMPLEMENTED */
        g_esp += 40;
        return;
    }

    fprintf(stderr, "[kthunk] NtCreateFile(handle_ptr=0x%08X, object=0x%08X)\n",
            MEM32(g_esp + 4), MEM32(g_esp + 12));
    g_esp += 4; /* expose arg 1 at the bridge's STACK_ARG(0) */
    bridge();
    fprintf(stderr, "[kthunk] NtCreateFile returned status=0x%08X\n", g_eax);
    g_esp += 36; /* nine stdcall arguments */
}

/* Ordinal 219: NtReadFile has eight stdcall arguments.  As with the open
 * and create imports above, a translated indirect call leaves a synthetic
 * return slot at g_esp which the bridge API must not interpret as Handle. */
static void thunk_NtReadFile(void)
{
    bridge_func_t bridge = bridge_for_ordinal(219);
    if (!bridge) {
        g_eax = 0xC0000002u; /* STATUS_NOT_IMPLEMENTED */
        g_esp += 36;
        return;
    }

    g_esp += 4; /* expose Handle at STACK_ARG(0) */
    bridge();
    g_esp += 32; /* eight stdcall arguments */
}

/* Ordinal 236: NtWriteFile has the same eight-argument ABI as NtReadFile.
 * Direct imports retain the synthetic return slot, so dispatching it without
 * this wrapper shifts Handle through ByteOffset by one dword. */
static void thunk_NtWriteFile(void)
{
    bridge_func_t bridge = bridge_for_ordinal(236);
    if (!bridge) {
        g_eax = 0xC0000002u; /* STATUS_NOT_IMPLEMENTED */
        g_esp += 36;
        return;
    }

    fprintf(stderr, "[kthunk] NtWriteFile(handle=0x%08X, len=%u)\\n",
            MEM32(g_esp + 4), MEM32(g_esp + 28));
    g_esp += 4; /* expose Handle at STACK_ARG(0) */
    bridge();
    g_esp += 32; /* eight stdcall arguments */
}

/* Ordinal 166: MmAllocateContiguousMemoryEx(bytes, low, high, alignment,
 * protect).  Direct imports bypass the bridge dispatcher, so use the title's
 * Xbox-address bump heap and preserve the five-argument stdcall ABI. */
/* MmAllocateContiguousMemory hands out whole pages.
 *
 * This is not a detail a caller can ignore, because DSOUND does not: after
 * asking for N bytes it rounds N up to 4KB and treats everything past N in
 * that last page as memory it owns. sub_004C98E7 plants a 12-byte arena header
 * at base + N and puts the remainder on its pool free list -- for
 * MmAllocateContiguousMemoryEx(16) at 0x03CFC000 that is a free block at
 * 0x03CFC01C of 4096 - 16 - 12 = 0xFE4 bytes, which is exactly what the list
 * held.
 *
 * The runtime's bump allocator was byte-granular, so the next allocation
 * started inside that reclaimed tail. ExAllocatePoolWithTag(19912) landed at
 * 0x03D2C0A0, four bytes into the free-list node at 0x03D2C0A4 that
 * MmAllocateContiguousMemoryEx(152) at 0x03D2C000 had left there, and zeroed
 * its next and prev pointers. Both neighbours still pointed at the node, so
 * the list looked intact from either end; the forward walk in sub_004C996B
 * reached it, read next = 0, and chased address 0 forever.
 *
 * Rounding here is the whole fix: the tail DSOUND reclaims is then genuinely
 * unallocated, because nothing else can be given it.
 */
#define XBOX_PAGE_SIZE 4096u

static uint32_t xbox_contiguous_alloc(uint32_t bytes, uint32_t alignment)
{
    uint32_t pages;

    if (alignment < XBOX_PAGE_SIZE)
        alignment = XBOX_PAGE_SIZE;

    /* Guard the overflow on a nonsense request rather than wrapping to 0. */
    if (bytes > 0xFFFFFFFFu - (XBOX_PAGE_SIZE - 1u))
        return 0;
    pages = (bytes + (XBOX_PAGE_SIZE - 1u)) & ~(XBOX_PAGE_SIZE - 1u);

    /* Hand back the 0x80000000 cached alias, as the console does.  Titles
     * round-trip it as phys = va & 0x0FFFFFFF; va = phys | 0x80000000, and
     * D3D range-checks the result against its push-buffer ring -- with a low
     * base that check failed and the frontend took a recovery path it never
     * takes on hardware.  xbox_memory.c already maps the alias windows, so
     * loads through either address reach the same bytes. */
    { extern uint32_t xbox_ram_alias(uint32_t);
      return xbox_ram_alias(xbox_heap_alloc(pages, alignment)); }
}

static void thunk_MmAllocateContiguousMemoryEx(void)
{
    uint32_t bytes = MEM32(g_esp + 4);
    uint32_t alignment = MEM32(g_esp + 16);
    uint32_t xbox_va = xbox_contiguous_alloc(bytes, alignment);
    fprintf(stderr, "[kthunk] MmAllocateContiguousMemoryEx(bytes=%u, align=%u) = 0x%08X\n",
            bytes, alignment, xbox_va);
    g_eax = xbox_va;
    g_esp += 24; /* return address + five arguments */
}

/* Ordinal 218: NtQueryVolumeInformationFile has five stdcall arguments. */
static void thunk_NtQueryVolumeInformationFile(void)
{
    bridge_func_t bridge = bridge_for_ordinal(218);
    if (!bridge) {
        g_eax = 0xC0000002u; /* STATUS_NOT_IMPLEMENTED */
        g_esp += 24;
        return;
    }

    fprintf(stderr, "[kthunk] NtQueryVolumeInformationFile(handle=0x%08X class=%u length=%u caller=%08X)\n",
            MEM32(g_esp + 4), MEM32(g_esp + 20), MEM32(g_esp + 16), MEM32(g_esp));
    g_esp += 4; /* expose arg 1 at the bridge's STACK_ARG(0) */
    bridge();
    fprintf(stderr, "[kthunk] NtQueryVolumeInformationFile returned status=0x%08X\n", g_eax);
    g_esp += 20; /* five stdcall arguments */
}

/* Ordinal 67: IoCreateSymbolicLink(PANSI_STRING LinkName,
 * PANSI_STRING DeviceName).  Conker uses it during early drive setup.  The
 * runtime path mapper already handles the relevant Xbox device prefixes, so
 * no host symlink needs to be created here. */
static void thunk_IoCreateSymbolicLink(void)
{
    uint32_t link_name = MEM32(g_esp + 4);
    uint32_t device_name = MEM32(g_esp + 8);
    fprintf(stderr, "[kthunk] IoCreateSymbolicLink(link=0x%08X, target=0x%08X)\n",
            link_name, device_name);
    g_eax = 0; /* STATUS_SUCCESS */
    g_esp += 12; /* return address + two stdcall arguments */
}

/* Ordinal 37: FscSetCacheSize(ULONG CacheSize).
 *
 * This controls the Xbox filesystem cache.  The host file layer has no
 * equivalent per-title cache to configure, so accepting the request is
 * sufficient for startup.  It is a one-argument stdcall routine; consuming
 * its simulated return address and argument is essential even though the
 * operation itself is a no-op.
 */
static void thunk_FscSetCacheSize(void)
{
    uint32_t cache_size = MEM32(g_esp + 4);

    fprintf(stderr, "[kthunk] FscSetCacheSize(size=0x%08X) - accepted\n",
            cache_size);
    g_eax = 0;
    g_esp += 8; /* stdcall: return address + one argument */
}

/* Ordinal 1: AvGetSavedDataAddress(void).
 *
 * Conker probes this during startup before an AV saved-data buffer has been
 * installed.  Returning NULL is the expected "no saved data" result, but
 * this direct import must still consume its synthetic return address.  A
 * missing thunk returns NULL to RECOMP_ICALL instead, leaving that slot on
 * the emulated stack and corrupting later SEH unwinds.
 */
static void thunk_AvGetSavedDataAddress(void)
{
    fprintf(stderr, "[kthunk] AvGetSavedDataAddress() -> NULL\n");
    g_eax = 0;
    g_esp += 4; /* cdecl-like no-argument import: return address only */
}

/* Ordinal 47: HalRegisterShutdownNotification(PVOID, BOOLEAN).
 *
 * Conker imports this ordinal as the shutdown-registration API.  The generic
 * bridge table currently associates ordinal 47 with a different HAL routine,
 * whose incompatible argument layout dereferences the registration pointer as
 * something else and faults during CRT startup.  This explicit thunk keeps
 * the Xbox 32-bit stack ABI intact. Shutdown callbacks are intentionally
 * inert while the runtime has no Xbox-style shutdown path.
 */
static void thunk_HalRegisterShutdownNotification(void)
{
    uint32_t registration_va = MEM32(g_esp + 4);
    uint32_t register_notification = MEM32(g_esp + 8);

    fprintf(stderr,
        "[kthunk] HalRegisterShutdownNotification(registration=0x%08X, "
        "register=%u) - ignored\n",
        registration_va, register_notification);

    g_eax = 0;
    g_esp += 12; /* stdcall: return address + 2 arguments */
}

/* Ordinal 107: KeInitializeDpc(PKDPC, PKDEFERRED_ROUTINE, PVOID).
 * The bridge implementation assumes its dispatcher has already consumed the
 * dummy return address. Calls resolved through recomp_lookup_kernel() bypass
 * that dispatcher, so provide an ABI-correct direct thunk here. */
static void thunk_KeInitializeDpc(void)
{
    uint32_t dpc_va = MEM32(g_esp + 4);
    uint32_t routine_va = MEM32(g_esp + 8);
    uint32_t context_va = MEM32(g_esp + 12);

    /* The Xbox KDPC is 28 bytes, not the 32 of NT: it has no trailing
     * Lock field.  Zeroing 32 ran four bytes past the object.  Conker
     * puts a KTIMER at 0x75F0D8, this KDPC at 0x75F100 and its sound
     * singleton pointer at 0x75F11C -- exactly 0x28 + 0x1C -- so the
     * overrun cleared the singleton the moment the DPC was built, and
     * every sound bank loader afterwards read a null object. */
    for (uint32_t i = 0; i < 28; ++i)
        MEM8(dpc_va + i) = 0;
    MEM16(dpc_va) = 0x13;             /* DpcObject */
    MEM32(dpc_va + 12) = routine_va;  /* DeferredRoutine */
    MEM32(dpc_va + 16) = context_va;  /* DeferredContext */

    fprintf(stderr, "[kthunk] KeInitializeDpc(dpc=0x%08X, routine=0x%08X)\n",
            dpc_va, routine_va);
    g_eax = 0;
    g_esp += 16; /* stdcall: return address + 3 arguments */
}

/* Ordinal 113: KeInitializeTimerEx(PKTIMER, TIMER_TYPE). */
static void thunk_KeInitializeTimerEx(void)
{
    uint32_t timer_va = MEM32(g_esp + 4);
    uint32_t timer_type = MEM32(g_esp + 8);

    recomp_timer_init(timer_va, timer_type);

    fprintf(stderr, "[kthunk] KeInitializeTimerEx(timer=0x%08X, type=%u)\n",
            timer_va, timer_type);
    g_eax = 0;
    g_esp += 12; /* stdcall: return address + 2 arguments */
}

/* Ordinal 149: KeSetTimer(PKTIMER, LARGE_INTEGER, PKDPC).
 * LARGE_INTEGER occupies two 32-bit stack words, so this is four arguments
 * in the simulated x86 ABI. Expiration is delivered at a guest safe point. */
static void thunk_KeSetTimer(void)
{
    uint32_t timer_va = MEM32(g_esp + 4);
    uint32_t due_low = MEM32(g_esp + 8);
    uint32_t due_high = MEM32(g_esp + 12);
    uint32_t dpc_va = MEM32(g_esp + 16);

    g_eax = recomp_timer_set(timer_va,
        (int64_t)(((uint64_t)due_high << 32) | due_low), 0, dpc_va);
    g_esp += 20; /* stdcall: return address + four 32-bit arguments */
}

static void thunk_KeSetTimerEx(void)
{
    g_eax = recomp_timer_set(MEM32(g_esp + 4), (int64_t)MEM64(g_esp + 8),
                            (int32_t)MEM32(g_esp + 16), MEM32(g_esp + 20));
    g_esp += 24;
}

static void thunk_KeCancelTimer(void)
{
    g_eax = recomp_timer_cancel(MEM32(g_esp + 4));
    g_esp += 8;
}

/* Bring-up tracing that has outlived its usefulness.
 *
 * These started as one-off traces for calls that happened a handful of times
 * during init. Now that the title reaches its render loop they run once per
 * frame: ExQueryNonVolatileSetting alone wrote 285,643 lines and 71 MB in
 * three minutes, burying every other diagnostic in the log. The arguments
 * still matter the first few times, so keep those, say the tap has been
 * closed, and stop. */
#define KTHUNK_TRACE_LIMIT 8u
#define KTHUNK_TRACE(what, ...) do {                                          \
    static unsigned _kt_seen;                                                 \
    if (_kt_seen < KTHUNK_TRACE_LIMIT) {                                      \
        fprintf(stderr, __VA_ARGS__);                                         \
        if (++_kt_seen == KTHUNK_TRACE_LIMIT)                                 \
            fprintf(stderr, "[kthunk] %s: called every frame now, "           \
                    "further calls not logged\n", (what));                     \
    }                                                                         \
} while (0)

/* Ordinal 24: ExQueryNonVolatileSetting(index, type*, value, value_len,
 * result_len*).  Return a deterministic, zero-initialized setting until the
 * game needs a specific EEPROM value. */
static void thunk_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index = MEM32(g_esp + 4);
    uint32_t type_ptr = MEM32(g_esp + 8);
    uint32_t value_ptr = MEM32(g_esp + 12);
    uint32_t value_length = MEM32(g_esp + 16);
    uint32_t result_length_ptr = MEM32(g_esp + 20);

    KTHUNK_TRACE("ExQueryNonVolatileSetting raw",
        "[kthunk] ExQueryNonVolatileSetting raw: esp=0x%08X "
        "ret=%08X a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X\n",
        g_esp, MEM32(g_esp), value_index, type_ptr, value_ptr,
        value_length, result_length_ptr);

    /* sub_0046DA64 is a lifted import-wrapper. Its direct C tail-call
     * preserves one extra simulated return slot before the five stdcall
     * parameters, so detect and skip that slot. A Type* is always an Xbox
     * pointer here; a small integer therefore identifies the shifted frame. */
    if (type_ptr < 0x10000) {
        value_index = type_ptr;
        type_ptr = value_ptr;
        value_ptr = value_length;
        value_length = result_length_ptr;
        result_length_ptr = MEM32(g_esp + 24);
    }

    if (value_length > 0x10000) {
        fprintf(stderr, "[kthunk] ExQueryNonVolatileSetting: invalid length %u\n",
                value_length);
        g_eax = 0xC000000Du; /* STATUS_INVALID_PARAMETER */
        g_esp += 24; /* synthetic return address + five API arguments */
        return;
    }

    if (type_ptr)
        MEM32(type_ptr) = 4; /* REG_DWORD-like Xbox setting */
    for (uint32_t i = 0; value_ptr && i < value_length; ++i)
        MEM8(value_ptr + i) = 0;
    if (result_length_ptr)
        MEM32(result_length_ptr) = value_length;

    KTHUNK_TRACE("ExQueryNonVolatileSetting",
        "[kthunk] ExQueryNonVolatileSetting(index=0x%X, len=%u) - defaults\n",
        value_index, value_length);
    g_eax = 0; /* STATUS_SUCCESS */
    g_esp += 24; /* synthetic return address + five API arguments */
}

/* Ordinal 301: RtlNtStatusToDosError(NTSTATUS). */
static void thunk_RtlNtStatusToDosError(void)
{
    uint32_t status = MEM32(g_esp + 4);

    KTHUNK_TRACE("RtlNtStatusToDosError",
        "[kthunk] RtlNtStatusToDosError(status=0x%08X)\n", status);

    switch (status) {
    case 0x00000000: g_eax = 0; break;
    case 0xC0000034: g_eax = 2; break;
    case 0xC000003A: g_eax = 3; break;
    case 0xC0000022: g_eax = 5; break;
    case 0xC0000008: g_eax = 6; break;
    case 0xC0000017: g_eax = 8; break;
    case 0xC000000D: g_eax = 87; break;
    default:         g_eax = 317; break;
    }

    g_esp += 8; /* stdcall: return address + one argument */
}

/* Ordinal 277: RtlEnterCriticalSection(PRTL_CRITICAL_SECTION).
 * Runtime critical sections are deliberately no-ops in synchronous mode,
 * but this thunk must still emulate the stdcall return (ret 4). */
static void thunk_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = MEM32(g_esp + 4);
    KTHUNK_TRACE("RtlEnterCriticalSection",
        "[kthunk] RtlEnterCriticalSection(cs=0x%08X) - no-op\n", cs_va);
    g_eax = 0;
    g_esp += 8; /* dummy return address + one stdcall argument */
}

/* Ordinal 289: RtlInitAnsiString(PANSI_STRING, PCSZ). */
static void thunk_RtlInitAnsiString(void)
{
    uint32_t destination = MEM32(g_esp + 4);
    uint32_t source = MEM32(g_esp + 8);
    uint32_t length = 0;

    if (source) {
        while (length < 0xFFFEu && MEM8(source + length) != 0)
            length++;
    }
    if (destination) {
        MEM16(destination) = (uint16_t)length;
        MEM16(destination + 2) = (uint16_t)(source ? length + 1 : 0);
        MEM32(destination + 4) = source;
    }
    fprintf(stderr, "[kthunk] RtlInitAnsiString(dest=0x%08X source=0x%08X len=%u)\n",
            destination, source, length);

    g_eax = 0;
    g_esp += 12; /* return address + two stdcall arguments */
}

/* Ordinal 95: KeBugCheck(ULONG).  The generated title reaches this through
 * an internal service cleanup callback during bootstrap.  Keep it visible
 * but non-fatal while that callback infrastructure is being reconstructed. */
static void thunk_KeBugCheck(void)
{
    uint32_t code = MEM32(g_esp + 4);
    fprintf(stderr, "[kthunk] KeBugCheck(code=0x%08X) - ignored during bootstrap\\n", code);
    g_eax = 0;
    g_esp += 8; /* return address + one stdcall argument */
}

/* Ordinal 184: NtAllocateVirtualMemory
 *
 * NtAllocateVirtualMemory(
 *     PVOID* BaseAddress,
 *     ULONG_PTR ZeroBits,
 *     PSIZE_T RegionSize,
 *     ULONG AllocationType,
 *     ULONG Protect)
 */
static void thunk_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = MEM32(g_esp + 4);
    uint32_t zero_bits = MEM32(g_esp + 8);
    uint32_t size_ptr = MEM32(g_esp + 12);
    uint32_t alloc_type = MEM32(g_esp + 16);
    uint32_t protect = MEM32(g_esp + 20);

    (void)zero_bits;

    fprintf(stderr,
        "[kthunk] NtAllocateVirtualMemory("
        "base_ptr=0x%08X size_ptr=0x%08X "
        "type=0x%08X protect=0x%08X)\n",
        base_ptr, size_ptr, alloc_type, protect);

    if (!base_ptr || !size_ptr) {
        g_eax = 0xC000000Du; /* STATUS_INVALID_PARAMETER */
        g_esp += 24;
        return;
    }

    /*
     * Xbox is 32-bit:
     *   PVOID  = 32-bit
     *   SIZE_T = 32-bit
     *
     * Windows is 64-bit, so don't pass the Xbox SIZE_T pointer
     * directly as PSIZE_T.
     */
    uint32_t xbox_base = MEM32(base_ptr);
    uint32_t xbox_size = MEM32(size_ptr);

    SIZE_T native_size = (SIZE_T)xbox_size;

    /*
     * xbox_NtAllocateVirtualMemory() expects BaseAddress to
     * contain an Xbox VA, not a native Windows pointer.
     */
    PVOID xbox_base_value = (PVOID)(uintptr_t)xbox_base;

    NTSTATUS status = xbox_NtAllocateVirtualMemory(
        &xbox_base_value,
        0,
        &native_size,
        alloc_type,
        protect
    );

    if (status == STATUS_SUCCESS) {

        uint32_t result_xbox_va =
            (uint32_t)(uintptr_t)xbox_base_value;

        MEM32(base_ptr) = result_xbox_va;
        MEM32(size_ptr) = (uint32_t)native_size;

        fprintf(stderr,
            "[kthunk] NtAllocateVirtualMemory SUCCESS "
            "Xbox=0x%08X size=%u\n",
            result_xbox_va,
            (uint32_t)native_size);
    }

    g_eax = (uint32_t)status;

    /* stdcall: return address + 5 arguments */
    g_esp += 24;
}

/* Ordinal 255: PsCreateSystemThreadEx - 10 stdcall args.
 *
 * CONFIRMED against real disassembly (text.asm, call site 0x0042E013):
 * push order gives, at [esp+N] after the call instruction:
 *   +4  ThreadHandle*     +24 StartContext1 (= real user start routine)
 *   +8  ThreadExtraSize   +28 StartContext2 (= real user arg)
 *   +12 KernelStackSize   +32 CreateSuspended
 *   +16 TlsDataSize       +36 DebugStack
 *   +20 ThreadId*         +40 StartRoutine  (= CRT _callthreadstartex
 *                                             trampoline, sub_0042DF42
 *                                             at this call site)
 *
 * CONFIRMED: sub_0042DF42 is the standard MSVC CRT thread trampoline -
 * it does SEH setup + TLS init, then does:
 *     push [ebp+0xc]        ; the real ArgList
 *     call [ebp+8]           ; the real StartAddress
 * i.e. it expects to be invoked AS IF stdcall'd with two of its own
 * params: (StartAddress, ArgList). Those map directly to this
 * function's own StartContext1 (routine) and StartContext2 (arg).
 * It ends by calling PsTerminateSystemThread and never returns.
 *
 * SIMPLIFICATION (deliberate, not from any template): rather than
 * spinning a real OS thread - which would race on the global
 * g_eax/g_esp/etc. register state - this calls the trampoline
 * synchronously on the current thread, using setjmp/longjmp to
 * simulate "the thread exited" when it hits PsTerminateSystemThread,
 * instead of letting that stub's real ExitThread() kill your actual
 * process. See thunk_PsTerminateSystemThread below - it intentionally
 * does NOT use kstub_PsTerminateSystemThread's ExitThread() call.
 *
 * This is NOT correct for a game relying on real concurrency (e.g. a
 * thread meant to run alongside the main loop forever). Revisit if a
 * later hang looks like it's waiting on a thread that should still be
 * running.
 */
static jmp_buf g_thread_exit_buf;
static int g_bootstrap_thread_active;

/* ================================================================
 * Guest threads
 *
 * These used to run synchronously on the caller's thread, because the lifted
 * register file was described as global and two contexts would race on it.
 * It is not global: templates/runtime/recomp_types.h declares g_eax, g_esp and
 * the rest RECOMP_TLS, so every OS thread already has its own.
 *
 * Running them inline deadlocks anything built as a producer and a consumer.
 * The boot video is exactly that -- the streaming loop fills two 1 MiB buffers
 * and waits for a decoder to drain one, and the decoder cannot start until the
 * streaming loop returns, which it never does.  Two reads of a 23 MiB file and
 * then nothing, for the rest of the run.
 *
 * So spin a real thread, give it its own guest stack out of the heap, and let
 * PsTerminateSystemThread end that thread rather than unwinding the creator.
 * CONKER_SYNC_THREADS=1 restores the old behaviour for comparison.
 * ================================================================ */

#define RECOMP_GUEST_THREAD_STACK (1u * 1024u * 1024u)

typedef struct {
    uint32_t trampoline_va;
    uint32_t start_context1;
    uint32_t start_context2;
    uint32_t stack_base;
} recomp_guest_thread_start;

/* Per-thread, like the register file it unwinds. */
static RECOMP_TLS jmp_buf t_guest_thread_exit;
static RECOMP_TLS int t_guest_thread_active;

static unsigned long g_guest_threads_started;

/* Handles are kept so the process can outlive the function that created the
 * threads.  The title's real main loop runs on one of these: with the old
 * synchronous behaviour the creator stayed inside it, so returning from
 * xbe_entry_point meant the game was over.  Now the creator returns at once and
 * the process must wait rather than exit and take the loop with it. */
#define RECOMP_GUEST_THREAD_MAX 16
static HANDLE g_guest_threads[RECOMP_GUEST_THREAD_MAX];
static unsigned g_guest_thread_count;

/* The stall watchdog looks at the guest threads from a thread of its own, so
 * it needs the handles this table already keeps.  Copied out rather than
 * exposed directly: the table grows while the title runs, and the watchdog
 * suspends whatever it is handed. */
unsigned recomp_guest_thread_handles(void **out, unsigned max)
{
    unsigned i, n = g_guest_thread_count;

    if (n > max)
        n = max;
    for (i = 0; i < n; ++i)
        out[i] = (void *)g_guest_threads[i];
    return n;
}

void recomp_wait_for_guest_threads(void)
{
    if (g_guest_thread_count == 0u)
        return;
    fprintf(stderr, "[kthunk] waiting on %u guest thread%s\n",
            g_guest_thread_count, g_guest_thread_count == 1u ? "" : "s");
    /* Wait, but keep servicing the window.
     *
     * The window belongs to this thread -- main() created it -- and a message
     * queue is per thread, so only this thread can pump it.  Blocking here in
     * WaitForMultipleObjects meant nothing ever did: the title moved onto a
     * thread of its own and pumped a queue that was not the window's, so the
     * window went black and stopped responding.  Pumping from the draw path
     * could not have fixed it either -- the draws are on the wrong thread.
     *
     * MsgWaitForMultipleObjects wakes for messages as well as for the threads,
     * so this waits exactly as before and services the queue in between. */
    for (;;) {
        DWORD reason = MsgWaitForMultipleObjects(
            g_guest_thread_count, g_guest_threads, TRUE, INFINITE,
            QS_ALLINPUT);
        MSG msg;

        if (reason != WAIT_OBJECT_0 + g_guest_thread_count)
            break;   /* the guest threads finished, or the wait failed */

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                fprintf(stderr, "[INFO WINDOW] WM_QUIT while waiting on "
                        "guest threads\n");
                fflush(stderr);
                ExitProcess(0);
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

static DWORD WINAPI recomp_guest_thread_main(LPVOID param)
{
    recomp_guest_thread_start *start = (recomp_guest_thread_start *)param;
    recomp_func_t trampoline = recomp_lookup_manual(start->trampoline_va);

    /* Debug registers are per-thread, so CONKER_WATCH_VA has to be re-armed
     * here: the title does its work on the threads it creates, and arming
     * only the thread that read the setting watched a thread that never
     * touches the address. */
    recomp_watch_arm_current_thread();

    if (!trampoline)
        trampoline = recomp_lookup(start->trampoline_va);
    if (trampoline) {
        /* This thread's registers are its own; only esp needs establishing.
         * Same push order as a call through RECOMP_ICALL: arguments first,
         * rightmost first, then the return address last so it lands at
         * g_esp+0 with arg1 at +4. */
        g_esp = start->stack_base + RECOMP_GUEST_THREAD_STACK - 16u;
        PUSH32(g_esp, start->start_context2);
        PUSH32(g_esp, start->start_context1);
        PUSH32(g_esp, 0xDEADC0DE);

        /* The watchdog's stack sampling was armed on whichever thread called
         * for it, which is now the one parked in the join.  Point it at the
         * thread actually running the title. */
        if (getenv("CONKER_WATCHDOG")) {
            extern void recomp_watchdog_retarget(void *thread);
            HANDLE self = NULL;
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                                GetCurrentProcess(), &self, 0, FALSE,
                                DUPLICATE_SAME_ACCESS))
                recomp_watchdog_retarget(self);
        }

        t_guest_thread_active = 1;
        if (setjmp(t_guest_thread_exit) == 0)
            trampoline();
        t_guest_thread_active = 0;
    } else {
        fprintf(stderr, "[kthunk] guest thread: trampoline 0x%08X not "
                "found in dispatch table\n", start->trampoline_va);
    }

    xbox_heap_free(start->stack_base);
    free(start);
    return 0;
}

static int recomp_spawn_guest_thread(uint32_t trampoline_va,
                                     uint32_t context1, uint32_t context2)
{
    recomp_guest_thread_start *start;
    uint32_t stack;
    HANDLE thread;

    stack = xbox_heap_alloc(RECOMP_GUEST_THREAD_STACK, 16u);
    if (!stack) {
        fprintf(stderr, "[kthunk] guest thread: no guest stack available\n");
        return 0;
    }

    start = (recomp_guest_thread_start *)malloc(sizeof(*start));
    if (!start) {
        xbox_heap_free(stack);
        return 0;
    }
    start->trampoline_va = trampoline_va;
    start->start_context1 = context1;
    start->start_context2 = context2;
    start->stack_base = stack;

    thread = CreateThread(NULL, 0, recomp_guest_thread_main, start, 0, NULL);
    if (!thread) {
        free(start);
        xbox_heap_free(stack);
        return 0;
    }
    if (g_guest_thread_count < RECOMP_GUEST_THREAD_MAX)
        g_guest_threads[g_guest_thread_count++] = thread;
    else
        CloseHandle(thread);
    ++g_guest_threads_started;
    fprintf(stderr, "[kthunk] guest thread #%lu started: trampoline=0x%08X "
            "stack=0x%08X..0x%08X\n",
            g_guest_threads_started, trampoline_va, stack,
            stack + RECOMP_GUEST_THREAD_STACK);
    return 1;
}

typedef struct recomp_register_context {
    uint32_t eax, ecx, edx, ebx, esi, edi, seh_ebp;
} recomp_register_context_t;

/* Deferred sound work can use SIMD and x87 while the interrupted renderer
 * has live values in those registers. Preserve the emulated register file. */
typedef struct recomp_float_context {
    RecompXmm simd[8];
    uint64_t packed[8];
    double stack[8];
    int top, comparison;
    uint16_t control;
} recomp_float_context;

static void save_float_context(recomp_float_context *state)
{
    state->simd[0]=g_xmm0; state->simd[1]=g_xmm1; state->simd[2]=g_xmm2; state->simd[3]=g_xmm3;
    state->simd[4]=g_xmm4; state->simd[5]=g_xmm5; state->simd[6]=g_xmm6; state->simd[7]=g_xmm7;
    state->packed[0]=g_mm0; state->packed[1]=g_mm1; state->packed[2]=g_mm2; state->packed[3]=g_mm3;
    state->packed[4]=g_mm4; state->packed[5]=g_mm5; state->packed[6]=g_mm6; state->packed[7]=g_mm7;
    memcpy(state->stack,g_fp_stack,sizeof(state->stack));
    state->top=g_fp_top; state->comparison=g_fp_cmp; state->control=g_fp_control_word;
}

static void restore_float_context(const recomp_float_context *state)
{
    g_xmm0=state->simd[0]; g_xmm1=state->simd[1]; g_xmm2=state->simd[2]; g_xmm3=state->simd[3];
    g_xmm4=state->simd[4]; g_xmm5=state->simd[5]; g_xmm6=state->simd[6]; g_xmm7=state->simd[7];
    g_mm0=state->packed[0]; g_mm1=state->packed[1]; g_mm2=state->packed[2]; g_mm3=state->packed[3];
    g_mm4=state->packed[4]; g_mm5=state->packed[5]; g_mm6=state->packed[6]; g_mm7=state->packed[7];
    memcpy(g_fp_stack,state->stack,sizeof(state->stack));
    g_fp_top=state->top; g_fp_cmp=state->comparison; g_fp_control_word=state->control;
}

/*
 * Deliver a hardware interrupt to the routine the title connected for it.
 *
 * This has to run on the guest thread.  The lifted register file lives in
 * RECOMP_TLS storage, so calling recompiled code from a Windows timer or
 * worker thread would run it against an uninitialised guest stack and
 * register set -- the same defect xbox_timer_callback still has where it
 * calls a DPC's DeferredRoutine as a native function pointer.
 *
 * An interrupt arrives between two guest instructions and must leave the
 * interrupted code exactly as it found it, so the whole register file and the
 * stack pointer are saved and restored around the call.  The service routine
 * runs on the interrupted thread's stack, below its current esp, which is
 * what real hardware does.
 *
 * Argument order follows RECOMP_ICALL's convention: arguments pushed
 * rightmost first, then the return address last, so it lands at g_esp+0 with
 * argument 1 at g_esp+4.  The routine is
 *     BOOLEAN ServiceRoutine(PKINTERRUPT Interrupt, PVOID ServiceContext)
 * which is stdcall, so it pops its own two arguments.
 */
extern int recomp_isr_for_vector(uint32_t vector, uint32_t *routine,
                                 uint32_t *kinterrupt, uint32_t *context);

static int g_isr_in_progress;   /* no reentrant delivery */

/*
 * Deferred procedure calls.
 *
 * KeInitializeDpc had a bridge; KeInsertQueueDpc did not, so a DPC could be
 * built and never queued, and nothing ever drained one.  The title's display
 * path depends on this: its service routine for vector 51 queues a DPC whose
 * deferred routine drives the presentation counter, and with no queue that
 * counter never advances.
 *
 * A DPC runs at DISPATCH_LEVEL, after the interrupt that raised it returns --
 * not inside it.  So insertion only records the request, and the queue is
 * drained later from the guest thread, which is the only place the lifted
 * register file is coherent.  Draining an empty queue costs a load and a
 * branch, so the drain can sit on a hot path safely.
 *
 * Guest KDPC layout, confirmed against the title's own memory:
 *   +0x00 Type (0x13 = DpcObject)   +0x02 Inserted
 *   +0x04 DpcListEntry (8 bytes)
 *   +0x0C DeferredRoutine           +0x10 DeferredContext
 *   +0x14 SystemArgument1           +0x18 SystemArgument2
 */
/* Uncapped counters.  Every capped fprintf in this tree has at some point been
 * misread as a measurement; these are the numbers, and the printing is
 * throttled separately. */
unsigned long long g_sp_hits, g_sp_delivered, g_dpc_runs_total, g_sp_ticks;
static unsigned g_dpc_ran_this_drain;
static int g_sp_cap = -1;

static int safe_point_capped(void)
{
    if (g_sp_cap < 0)
        g_sp_cap = (getenv("CONKER_SP_CAP") != NULL);
    return g_sp_cap;
}

#define RECOMP_DPC_QUEUE 16

static uint32_t g_dpc_queue[RECOMP_DPC_QUEUE];
static unsigned g_dpc_head, g_dpc_tail;
static int      g_dpc_draining;

unsigned long long g_dpc_inserted, g_dpc_insert_rejected;
unsigned long long g_vec_delivered[64];

int recomp_dpc_insert(uint32_t dpc_va)
{
    unsigned next;

    if (dpc_va == 0u)
        return 0;
    /* Already queued: the real KeInsertQueueDpc returns FALSE and does not
     * queue it twice. */
    if (MEM8(dpc_va + 2u)) {
        ++g_dpc_insert_rejected;
        return 0;
    }
    ++g_dpc_inserted;

    next = (g_dpc_tail + 1u) % RECOMP_DPC_QUEUE;
    if (next == g_dpc_head) {
        static unsigned complained;
        if (complained++ < 2u)
            fprintf(stderr, "[DPC] queue full; dropping 0x%08X\n", dpc_va);
        return 0;
    }
    MEM8(dpc_va + 2u) = 1u;          /* Inserted */
    g_dpc_queue[g_dpc_tail] = dpc_va;
    g_dpc_tail = next;
    return 1;
}

void recomp_dpc_drain(void)
{
    extern unsigned recomp_irqt_irql(void);
    extern unsigned char __fastcall xbox_KfRaiseIrql(unsigned char);
    extern void __fastcall xbox_KfLowerIrql(unsigned char);
    /* An IRQ may queue completion while the title is updating a protected
     * stream list. Defer its callback until that DISPATCH_LEVEL scope ends. */
    if (g_dpc_head == g_dpc_tail || g_dpc_draining ||
        recomp_irqt_irql() >= 2u)
        return;

    g_dpc_draining = 1;
    unsigned char previous_irql = xbox_KfRaiseIrql(2u);
    while (g_dpc_head != g_dpc_tail) {
        /* Diagnosis: CONKER_SP_CAP=1 runs at most one DPC per drain, so an
         * unbounded drain can be told apart from a merely busy one. */
        if (safe_point_capped() && g_dpc_ran_this_drain >= 1u)
            break;
        ++g_dpc_ran_this_drain;
        ++g_dpc_runs_total;
        uint32_t dpc_va = g_dpc_queue[g_dpc_head];
        uint32_t routine, context, arg1, arg2, saved_esp, saved_ebp;
        recomp_register_context_t saved;
        recomp_float_context saved_float;
        recomp_func_t fn;

        g_dpc_head = (g_dpc_head + 1u) % RECOMP_DPC_QUEUE;
        MEM8(dpc_va + 2u) = 0u;      /* no longer inserted */

        routine = MEM32(dpc_va + 0x0Cu);
        context = MEM32(dpc_va + 0x10u);
        arg1    = MEM32(dpc_va + 0x14u);
        arg2    = MEM32(dpc_va + 0x18u);
        if (routine == 0u)
            continue;

        fn = recomp_lookup_manual(routine);
        if (!fn) fn = recomp_lookup(routine);
        if (!fn) {
            static unsigned complained;
            if (complained++ < 2u)
                fprintf(stderr, "[DPC] routine 0x%08X has no translation\n",
                        routine);
            continue;
        }

        /* A DPC interrupts whatever the thread was doing and must leave it
         * exactly as it found it. */
        saved.eax = g_eax; saved.ecx = g_ecx; saved.edx = g_edx;
        saved.ebx = g_ebx; saved.esi = g_esi; saved.edi = g_edi;
        saved.seh_ebp = g_seh_ebp;
        saved_ebp = g_ebp;      /* frameless callees read this global mid-body */
        saved_esp = g_esp;
        save_float_context(&saved_float);

        /* VOID DeferredRoutine(PKDPC, PVOID Context, PVOID Arg1, PVOID Arg2),
         * stdcall: arguments rightmost first, return address last. */
        PUSH32(g_esp, arg2);
        PUSH32(g_esp, arg1);
        PUSH32(g_esp, context);
        PUSH32(g_esp, dpc_va);
        PUSH32(g_esp, 0xDEADDBCCu);
        fn();

        g_esp = saved_esp;
        g_ebp = saved_ebp;
        g_eax = saved.eax; g_ecx = saved.ecx; g_edx = saved.edx;
        g_ebx = saved.ebx; g_esi = saved.esi; g_edi = saved.edi;
        g_seh_ebp = saved.seh_ebp;
        restore_float_context(&saved_float);

        { static unsigned logged;
          if (logged++ < 4u)
              fprintf(stderr, "[DPC] ran 0x%08X (context 0x%08X)\n",
                      routine, context); }
    }
    xbox_KfLowerIrql(previous_irql);
    g_dpc_draining = 0;
}

/*
 * Guest interrupt safe points.
 *
 * Delivery used to happen only where the guest called into the runtime -- from
 * the MMIO fault handler, in practice -- so a guest that went compute-bound
 * could never be interrupted.  The XMV decoder does exactly that: it waits on a
 * counter that only an interrupt can advance, while performing no MMIO, and
 * deadlocks.
 *
 * The lifter now emits RECOMP_SAFE_POINT() at loop back-edges, where the lifted
 * register state is coherent by construction.  Raising stays asynchronous from
 * delivery: something else sets g_recomp_irq_pending on its own cadence, and
 * the safe point only ever drains what is already pending.  The common case is
 * a load and a not-taken branch.
 */
volatile int g_recomp_irq_pending;

/* Which vectors are pending, one bit per vector.
 *
 * The safe point used to deliver vector 51 and nothing else, because the GPU
 * was the only device that could raise anything.  The APU raises vector 49 --
 * the title connects a routine there and it had never once fired -- so the
 * drain has to be able to carry more than one source.  Raising still stays
 * asynchronous from delivery: a device sets its bit on its own cadence and the
 * safe point only drains what is already pending. */
extern unsigned recomp_irqt_irql(void);
volatile unsigned long long g_recomp_irq_vectors;

/* TEMPORARY: vector 54 against vector 51 through the generic path.
 *
 * Vector 51 (the GPU) is delivered and drives its DPC; vector 54 (the audio
 * codec interface) is raised but its ISR is never entered.  Both go through
 * exactly the same code, so this counts each stage for the two vectors side
 * by side and the first stage where they differ is the answer.
 *
 * Stages, in the order the path visits them:
 *   raise      recomp_irq_raise called
 *   pending    the bit was actually set in the mask
 *   picked     the drain selected this vector
 *   busy       refused because an ISR was already running
 *   nolookup   recomp_isr_for_vector found no connected routine
 *   notrans    the routine had no translation
 *   entered    the ISR actually ran
 */
#define IRQT_V51 0
#define IRQT_V54 1
static unsigned long long g_irqt[2][7];
static const char *const g_irqt_stage[7] = {
    "raise    ", "pending  ", "picked   ", "busy     ",
    "no-lookup", "no-transl", "ENTERED  " };
static uint32_t g_irqt_routine[2];

static int irqt_slot(unsigned v)
{
    if (v == 51u) return IRQT_V51;
    if (v == 54u) return IRQT_V54;
    return -1;
}

void recomp_irqt(unsigned v, unsigned stage, uint32_t routine)
{
    int k = irqt_slot(v);
    if (k < 0 || stage >= 7u) return;
    ++g_irqt[k][stage];
    if (routine) g_irqt_routine[k] = routine;
}

void recomp_dump_irqt(void)
{
    unsigned s;
    extern int recomp_isr_for_vector(uint32_t, uint32_t *, uint32_t *,
                                     uint32_t *);
    uint32_t r51 = 0, r54 = 0, ki = 0, cx = 0;
    int c51 = recomp_isr_for_vector(51u, &r51, &ki, &cx);
    int c54 = recomp_isr_for_vector(54u, &r54, &ki, &cx);

    if (g_irqt[0][0] == 0ull && g_irqt[1][0] == 0ull) return;
    fprintf(stderr, "[IRQT] stage            vec51        vec54\n");
    for (s = 0; s < 7u; ++s)
        fprintf(stderr, "[IRQT]   %s  %10llu   %10llu\n",
                g_irqt_stage[s], g_irqt[0][s], g_irqt[1][s]);
    fprintf(stderr, "[IRQT] connected: v51=%d routine=%08X   v54=%d routine=%08X\n",
            c51, r51, c54, r54);
    fprintf(stderr, "[IRQT] pending mask now=%016llX  irql=%u\n",
            (unsigned long long)g_recomp_irq_vectors, recomp_irqt_irql());
    fflush(stderr);
}


void recomp_irq_raise(unsigned vector)
{
    recomp_irqt(vector, 0u, 0u);
    if (vector < 64u) {
        /* Device threads can raise different vectors concurrently. A plain
         * read/modify/write can erase the other device's pending interrupt. */
        InterlockedOr64((volatile LONG64 *)&g_recomp_irq_vectors, 1ull << vector);
        recomp_irqt(vector, 1u, 0u);
        InterlockedExchange((volatile LONG *)&g_recomp_irq_pending, 1);
    }
}

void recomp_safe_point(void)
{
    static int in_safe_point;

    if (in_safe_point || g_isr_in_progress)
        return;                 /* a service routine hitting its own back-edges */
    in_safe_point = 1;
    InterlockedExchange((volatile LONG *)&g_recomp_irq_pending, 0);


    /* The state-snapshot diagnostic that used to sit here reported "0 field(s)
     * differ" once g_ebp was added to the save/restore, so it is gone.  It also
     * early-returned, which would now swallow the first four drains. */

    { LARGE_INTEGER t0, t1;
      QueryPerformanceCounter(&t0);
      unsigned long long pending;
      unsigned v;

      ++g_sp_hits;
      g_dpc_ran_this_drain = 0;
      /* Take the whole set at once; a device raising again while we deliver
       * simply lands in the next drain. */
      pending = (unsigned long long)InterlockedExchange64(
          (volatile LONG64 *)&g_recomp_irq_vectors, 0);
      for (v = 0; v < 64u && pending; ++v) {
          if (!(pending & (1ull << v)))
              continue;
          pending &= ~(1ull << v);
          recomp_irqt(v, 2u, 0u);
          if (recomp_deliver_interrupt(v))
              ++g_sp_delivered;
      }
      recomp_timer_poll();
      recomp_dpc_drain();
      QueryPerformanceCounter(&t1);
      g_sp_ticks += (unsigned long long)(t1.QuadPart - t0.QuadPart);

      { static unsigned long long next_report;
        if (g_sp_hits >= next_report) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            next_report = g_sp_hits + 2000ull;
            fprintf(stderr, "[SP] hits=%llu delivered=%llu dpc_runs=%llu  "
                    "total=%.1fms  per-hit=%.1fus  capped=%d\n",
                    g_sp_hits, g_sp_delivered, g_dpc_runs_total,
                    g_sp_ticks * 1000.0 / (double)f.QuadPart,
                    g_sp_ticks * 1000000.0 / (double)f.QuadPart / (double)g_sp_hits,
                    safe_point_capped());
            fflush(stderr);
        } } }

    in_safe_point = 0;
}

/*
 * Call a guest routine that takes one stdcall argument.
 *
 * Kernel entry points that take a callback take a *guest* code address, and
 * the only way to run one is through the translated function the dispatcher
 * has for it -- casting the guest VA to a native function pointer jumps into
 * whatever happens to live at that host address.  This is the same sequence
 * recomp_deliver_interrupt uses, factored out so both callers agree.
 *
 * Interrupt delivery is held off for the duration.  The one caller so far is
 * KeSynchronizeExecution, whose entire contract is that the routine runs at
 * the interrupt's IRQL with its spinlock held, so the ISR cannot run
 * underneath it; g_isr_in_progress is what expresses that here.  It is saved
 * and restored rather than cleared, so a synchronize called from inside an ISR
 * does not re-open delivery on the way out.
 *
 * Returns 0 when the routine has no translation, in which case *out_eax is
 * untouched and the caller decides what a missing routine means.
 */
int recomp_call_guest_stdcall1(uint32_t routine_va, uint32_t arg,
                               uint32_t *out_eax)
{
    recomp_func_t fn;
    recomp_register_context_t saved;
    uint32_t saved_esp, saved_ebp;
    int saved_isr;

    if (!routine_va)
        return 0;
    fn = recomp_lookup_manual(routine_va);
    if (!fn) fn = recomp_lookup(routine_va);
    if (!fn) {
        static unsigned complained;
        if (complained++ < 4u)
            fprintf(stderr, "[kthunk] guest routine 0x%08X has no translation\n",
                    routine_va);
        return 0;
    }

    saved.eax = g_eax; saved.ecx = g_ecx; saved.edx = g_edx;
    saved.ebx = g_ebx; saved.esi = g_esi; saved.edi = g_edi;
    saved.seh_ebp = g_seh_ebp;
    saved_ebp = g_ebp;      /* frameless callees read this global mid-body */
    saved_esp = g_esp;

    saved_isr = g_isr_in_progress;
    g_isr_in_progress = 1;
    PUSH32(g_esp, arg);          /* the one argument  -> g_esp+4 */
    PUSH32(g_esp, 0xDEADBEEFu);  /* return address    -> g_esp+0 */
    fn();
    if (out_eax)
        *out_eax = g_eax;
    g_isr_in_progress = saved_isr;

    g_esp = saved_esp;
    g_ebp = saved_ebp;
    g_eax = saved.eax; g_ecx = saved.ecx; g_edx = saved.edx;
    g_ebx = saved.ebx; g_esi = saved.esi; g_edi = saved.edi;
    g_seh_ebp = saved.seh_ebp;
    return 1;
}

int recomp_deliver_interrupt(uint32_t vector)
{
    uint32_t routine = 0, kinterrupt = 0, context = 0;
    recomp_func_t fn;
    recomp_register_context_t saved;
    uint32_t saved_esp, saved_ebp;

    if (g_isr_in_progress) {
        recomp_irqt(vector, 3u, 0u);
        return 0;
    }
    if (!recomp_isr_for_vector(vector, &routine, &kinterrupt, &context)) {
        recomp_irqt(vector, 4u, 0u);
        return 0;
    }

    fn = recomp_lookup_manual(routine);
    if (!fn) fn = recomp_lookup(routine);
    if (!fn) {
        recomp_irqt(vector, 5u, routine);
        static unsigned complained;
        if (complained++ < 2u)
            fprintf(stderr, "[INTR] service routine 0x%08X has no translation\n",
                    routine);
        return 0;
    }

    saved.eax = g_eax; saved.ecx = g_ecx; saved.edx = g_edx;
    saved.ebx = g_ebx; saved.esi = g_esi; saved.edi = g_edi;
    saved.seh_ebp = g_seh_ebp;
    saved_ebp = g_ebp;      /* frameless callees read this global mid-body */
    saved_esp = g_esp;

    recomp_irqt(vector, 6u, routine);
    g_isr_in_progress = 1;
    PUSH32(g_esp, context);      /* arg2 ServiceContext -> g_esp+8 */
    PUSH32(g_esp, kinterrupt);   /* arg1 Interrupt      -> g_esp+4 */
    PUSH32(g_esp, 0xDEADBEEFu);  /* return address      -> g_esp+0 */
    fn();
    g_isr_in_progress = 0;

    g_esp = saved_esp;
    g_ebp = saved_ebp;
    g_eax = saved.eax; g_ecx = saved.ecx; g_edx = saved.edx;
    g_ebx = saved.ebx; g_esi = saved.esi; g_edi = saved.edi;
    g_seh_ebp = saved.seh_ebp;

    if (vector < 64u) ++g_vec_delivered[vector];
    { static unsigned logged;
      if (logged++ < 200u)
          fprintf(stderr, "[INTR] delivered vector %u to 0x%08X\n",
                  vector, routine); }
    return 1;
}

/*
 * The first system thread is still executed cooperatively because the lifted
 * CPU register file is global.  Its original code eventually reaches a
 * deliberate infinite idle loop.  At that point it is safe to return control
 * to the caller which created the thread, allowing the title's main startup
 * path to continue without running two emulated contexts concurrently.
 */
void recomp_bootstrap_thread_reached_idle(void)
{
    /* On a real thread an idle loop is just an idle loop -- let it run, but
     * yield so it does not spin a core against the threads doing work. */
    if (t_guest_thread_active) {
        Sleep(1);
        return;
    }
    if (g_bootstrap_thread_active)
        longjmp(g_thread_exit_buf, 2);
}

static void thunk_PsCreateSystemThreadEx(void)
{
    uint32_t thread_handle_ptr = MEM32(g_esp + 4);
    uint32_t thread_id_ptr     = MEM32(g_esp + 20);
    uint32_t start_context1    = MEM32(g_esp + 24); /* real StartAddress */
    uint32_t start_context2    = MEM32(g_esp + 28); /* real ArgList */
    uint32_t start_routine_va  = MEM32(g_esp + 40); /* CRT trampoline */


    if (thread_handle_ptr) MEM32(thread_handle_ptr) = 1; /* fake non-null handle */
    if (thread_id_ptr) MEM32(thread_id_ptr) = 0x1234;    /* fake thread id */

    /* A real thread, unless asked for the old inline behaviour. */
    if (!getenv("CONKER_SYNC_THREADS") &&
        recomp_spawn_guest_thread(start_routine_va, start_context1,
                                  start_context2)) {
        g_eax = 0;        /* STATUS_SUCCESS */
        g_esp += 44;      /* stdcall: return address + 10 args */
        return;
    }

    fprintf(stderr, "[kthunk] PsCreateSystemThreadEx: trampoline=0x%08X "
            "real_start=0x%08X real_arg=0x%08X (running SYNCHRONOUSLY)\n",
            start_routine_va, start_context1, start_context2);

    recomp_func_t trampoline = recomp_lookup_manual(start_routine_va);
    if (!trampoline) trampoline = recomp_lookup(start_routine_va);

    if (trampoline) {
        uint32_t saved_esp = g_esp - 44; /* stack level our own caller expects back */
        recomp_register_context_t parent = {
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_seh_ebp
        };
        int thread_exit_reason;

        /* Emulate the real kernel handing the new "thread" its stack.
         * PUSH32 decrements-then-stores, and per RECOMP_ICALL's own
         * convention: args are pushed first (rightmost/last-arg first,
         * per stdcall), and the dummy return address is pushed LAST -
         * ending up at g_esp+0, with arg1 at g_esp+4, arg2 at g_esp+8.
         * (Confirmed from recomp_types.h's RECOMP_ICALL doc comment -
         * earlier version of this code had the push order backwards.) */
        PUSH32(g_esp, start_context2);  /* arg2 (ArgList)     -> g_esp+8 */
        PUSH32(g_esp, start_context1);  /* arg1 (StartAddress)-> g_esp+4 */
        PUSH32(g_esp, 0xDEADC0DE);      /* dummy return addr  -> g_esp+0 */

        g_bootstrap_thread_active = 1;
        thread_exit_reason = setjmp(g_thread_exit_buf);
        if (thread_exit_reason == 0) {
            trampoline();
            /* If it somehow returns normally instead of terminating via
             * PsTerminateSystemThread, fall through here too. */
        }
        g_bootstrap_thread_active = 0;

        if (thread_exit_reason == 2)
            fprintf(stderr, "[kthunk] bootstrap system thread reached idle; resuming creator\n");

        g_esp = saved_esp; /* restore OUR caller's stack level, discarding
                             * whatever the simulated "thread" did to g_esp */
        g_eax = parent.eax;
        g_ecx = parent.ecx;
        g_edx = parent.edx;
        g_ebx = parent.ebx;
        g_esi = parent.esi;
        g_edi = parent.edi;
        g_seh_ebp = parent.seh_ebp;
    } else {
        fprintf(stderr, "[kthunk] PsCreateSystemThreadEx: trampoline "
                "0x%08X not found in dispatch table\n", start_routine_va);
    }

    g_eax = 0; /* STATUS_SUCCESS */
    g_esp += 44; /* stdcall: ret addr + 10 args */
}

/* Ordinal 258: PsTerminateSystemThread(NTSTATUS ExitStatus) - 1 arg.
 *
 * Deliberately does NOT call kstub_PsTerminateSystemThread - that
 * template calls ExitThread(), which under the synchronous-thread
 * simplification above would kill your real (only) process thread.
 * Instead, jumps back to right after the trampoline() call in
 * thunk_PsCreateSystemThreadEx, simulating "the thread has exited"
 * without tearing down the whole program.
 */
static void thunk_PsTerminateSystemThread(void)
{
    uint32_t exit_status = MEM32(g_esp + 4);

    /* On a real guest thread this ends that thread and nothing else. */
    if (t_guest_thread_active) {
        fprintf(stderr, "[kthunk] PsTerminateSystemThread(%u) - guest "
                "thread exiting\n", exit_status);
        longjmp(t_guest_thread_exit, 1);
    }

    fprintf(stderr, "[kthunk] PsTerminateSystemThread(%u) - simulated exit, "
            "unwinding synchronous thread call\n", exit_status);
    if (g_bootstrap_thread_active)
        longjmp(g_thread_exit_buf, 1);
    /* A later thread may be stubbed rather than cooperatively active. */
    g_eax = 0;
    g_esp += 8;
}

/* Ordinal 327: XeLoadSection(PXBE_SECTION_HEADER).
 * The recompiled executable is resident already, so section loading is a
 * successful no-op.  This direct thunk is still required to consume the
 * synthetic return address and the single stdcall argument. */
static void thunk_XeLoadSection(void)
{
    uint32_t section = MEM32(g_esp + 4);
    if (section == 0) {
        fprintf(stderr, "[kthunk] XeLoadSection(NULL) - invalid parameter\\n");
        g_eax = 0xC000000Du; /* STATUS_INVALID_PARAMETER */
        g_esp += 8;
        return;
    }

    if (section < 0x00010000u || section >= 0x04000000u) {
        fprintf(stderr, "[kthunk] XeLoadSection(section=0x%08X) - invalid header\\n", section);
        g_eax = 0xC000000Du;
        g_esp += 8;
        return;
    }

    /* All XBE data is pre-mapped by xbox_memory_init, but callers still use
     * this count as the section's loaded-state indicator. */
    MEM32(section + 0x18) = MEM32(section + 0x18) + 1u;
    fprintf(stderr, "[kthunk] XeLoadSection(section=0x%08X, va=0x%08X, refs=%u) - resident\\n",
            section, MEM32(section + 0x04), MEM32(section + 0x18));
    g_eax = 0; /* STATUS_SUCCESS */
    g_esp += 8;
}

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va < KTHUNK_BASE)
        return NULL;

    uint32_t ordinal = xbox_va - KTHUNK_BASE;

    /*
     * Special trampolines that need custom ABI handling.
     */
    if (ordinal == 15)
        return thunk_ExAllocatePoolWithTag;

    if (ordinal == 187)
        return thunk_NtClose;

    if (ordinal == 202)
        return thunk_NtOpenFile;

    if (ordinal == 190)
        return thunk_NtCreateFile;

    if (ordinal == 219)
        return thunk_NtReadFile;

    if (ordinal == 236)
        return thunk_NtWriteFile;

    if (ordinal == 166)
        return thunk_MmAllocateContiguousMemoryEx;

    if (ordinal == 218)
        return thunk_NtQueryVolumeInformationFile;

    if (ordinal == 67)
        return thunk_IoCreateSymbolicLink;

    if (ordinal == 37)
        return thunk_FscSetCacheSize;

    if (ordinal == 1)
        return thunk_AvGetSavedDataAddress;

    if (ordinal == 47)
        return thunk_HalRegisterShutdownNotification;

    if (ordinal == 107)
        return thunk_KeInitializeDpc;

    if (ordinal == 113)
        return thunk_KeInitializeTimerEx;

    if (ordinal == 149)
        return thunk_KeSetTimer;

    if (ordinal == 150)
        return thunk_KeSetTimerEx;

    if (ordinal == 97)
        return thunk_KeCancelTimer;

    if (ordinal == 24)
        return thunk_ExQueryNonVolatileSetting;

    if (ordinal == 301)
        return thunk_RtlNtStatusToDosError;

    if (ordinal == 277)
        return thunk_RtlEnterCriticalSection;

    if (ordinal == 289)
        return thunk_RtlInitAnsiString;

    if (ordinal == 95)
        return thunk_KeBugCheck;

    if (ordinal == 184)
        return thunk_NtAllocateVirtualMemory;

    if (ordinal == 255)
        return thunk_PsCreateSystemThreadEx;

    if (ordinal == 258)
        return thunk_PsTerminateSystemThread;

    if (ordinal == 23)
        return thunk_ExQueryPoolBlockSize;

    if (ordinal == 327)
        return thunk_XeLoadSection;

    /*
     * Most kernel functions already have bridge implementations.
     */
    recomp_func_t bridge = xbox_kernel_bridge_direct(ordinal);

    if (bridge)
        return bridge;

    /*
     * No implementation yet.
     */
    for (size_t i = 0; i < g_kthunks_count; i++) {
        if (g_kthunks[i].ordinal == ordinal) {
            fprintf(stderr,
                "[kthunk] ordinal %u (%s) has no implementation\n",
                ordinal,
                g_kthunks[i].name);
            return NULL;
        }
    }

    /* A malformed indirect target can retry every frame.  Keep the first
     * report actionable without turning a diagnostic run into a giant log. */
    {
        static uint32_t last_unknown_ordinal;
        static uint32_t unknown_repeat_count;
        if (ordinal != last_unknown_ordinal) {
            last_unknown_ordinal = ordinal;
            unknown_repeat_count = 0;
        }
        if (unknown_repeat_count++ == 0) {
            fprintf(stderr,
                "[kthunk] unknown ordinal %u (target=%08X esp=%08X eax=%08X ecx=%08X edx=%08X)\n",
                ordinal, xbox_va, g_esp, g_eax, g_ecx, g_edx);
        } else if (unknown_repeat_count == 2) {
            fprintf(stderr, "[kthunk] suppressing repeated unknown ordinal %u\n", ordinal);
        }
    }

    return NULL;
}
