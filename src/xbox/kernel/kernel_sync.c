/*
 * kernel_sync.c - Xbox Synchronization Primitives
 *
 * Implements events, semaphores, wait functions, kernel timers, and DPCs
 * using Win32 synchronization objects.
 *
 * Xbox synchronization model:
 *   - Events: notification (manual-reset) or synchronization (auto-reset)
 *   - Semaphores: standard counting semaphores
 *   - Wait functions: single and multiple, with optional timeout
 *   - Timers: kernel timers with optional DPC callback on expiry
 *   - DPCs: deferred procedure calls, executed via thread pool on Windows
 *
 * Time values use NT 100-nanosecond units (negative = relative).
 */

#include "kernel.h"

/* ============================================================================
 * Helper: Convert NT 100ns interval to Win32 milliseconds
 *
 * NT time intervals:
 *   - Negative = relative (most common), in 100ns units
 *   - Positive = absolute FILETIME
 *   - NULL = infinite wait
 * ============================================================================ */

static DWORD xbox_nt_timeout_to_ms(PLARGE_INTEGER Timeout)
{
    if (!Timeout)
        return INFINITE;

    if (Timeout->QuadPart == 0)
        return 0;

    if (Timeout->QuadPart < 0) {
        /* Relative: negative 100ns units */
        LONGLONG relative_100ns = -Timeout->QuadPart;
        DWORD ms = (DWORD)(relative_100ns / 10000);
        if (ms == 0 && relative_100ns > 0)
            ms = 1;
        return ms;
    }

    /* Absolute: compute delta from now */
    LARGE_INTEGER now;
    GetSystemTimeAsFileTime((LPFILETIME)&now);
    LONGLONG diff = Timeout->QuadPart - now.QuadPart;
    if (diff <= 0)
        return 0;
    return (DWORD)(diff / 10000);
}

/* ============================================================================
 * Events
 *
 * Xbox event types:
 *   XboxNotificationEvent (0) = manual-reset (Win32: TRUE)
 *   XboxSynchronizationEvent (1) = auto-reset (Win32: FALSE)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateEvent(
    PHANDLE EventHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    ULONG EventType,
    BOOLEAN InitialState)
{
    HANDLE hEvent;
    BOOL bManualReset;

    (void)ObjectAttributes;

    if (!EventHandle)
        return STATUS_INVALID_PARAMETER;

    /* XboxNotificationEvent = manual-reset, XboxSynchronizationEvent = auto-reset */
    bManualReset = (EventType == XboxNotificationEvent) ? TRUE : FALSE;

    hEvent = CreateEventW(NULL, bManualReset, InitialState, NULL);
    if (!hEvent) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateEvent: CreateEventW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *EventHandle = hEvent;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateEvent: handle=%p, type=%s, initial=%d",
        hEvent, bManualReset ? "notification" : "synchronization", InitialState);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtSetEvent(HANDLE EventHandle, PLONG PreviousState)
{
    if (PreviousState)
        *PreviousState = 0; /* We don't track previous state */

    if (!SetEvent(EventHandle)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtSetEvent: SetEvent failed for handle %p (error %u)",
            EventHandle, GetLastError());
        return STATUS_INVALID_HANDLE;
    }

    return STATUS_SUCCESS;
}


/* ============================================================================
 * Guest dispatcher objects
 *
 * KeWaitForSingleObject / KeSetEvent take a POINTER TO A DISPATCHER OBJECT in
 * guest memory, not a handle.  This title never imports KeInitializeEvent -- it
 * builds the header inline -- so everything is derived from guest memory:
 *
 *     +0x00 Type        0 NotificationEvent (manual reset)
 *                       1 SynchronizationEvent (auto reset)
 *     +0x02 Size        4 dwords for a KEVENT
 *     +0x04 SignalState LONG
 *
 * Two rules, both learned the hard way.
 *
 * 1. The wait must be cooperative, never a blocking host wait.
 *
 *    Guest interrupts and DPCs are delivered only on the guest thread, at
 *    recomp_safe_point() and in the MMIO vectored handler, because the lifted
 *    register file is per-thread.  A host thread that raises an IRQ only sets a
 *    bit.  So parking the guest thread in WaitForSingleObjectEx() stops the
 *    whole emulated machine: no vblank is delivered, no DPC drains, and the
 *    routine that would signal the object can never run.
 *
 *    That is not hypothetical.  D3D bring-up in sub_002A6D90 does
 *
 *        MEM32(dev + 0x1DC0) = 0
 *        KeWaitForSingleObject(dev + 0x1DBC, 6, 1, 0, NULL)
 *
 *    and dev+0x1DBC is signalled only by the vblank DPC (sub_00540DC0 ->
 *    sub_00540F70 -> KeSetEvent), queued by ISR 0x00540CA0 on vector 51.  With
 *    a blocking host wait, roughly one boot in seven reached that wait before
 *    the first vblank had been delivered and hung there forever: frames=1,
 *    safepoints=0, "delivered vector 51" = 0, "[DPC] queued" = 0.
 *
 *    So the wait polls guest memory and services the machine between polls.
 *
 * 2. Guest memory is authoritative, and it is the ONLY authority.
 *
 *    The title clears SignalState with a plain store (the MEM32 above) and
 *    never calls KeResetEvent or KeClearEvent -- neither is even imported.  A
 *    host event mirroring the state therefore drifts permanently out of step:
 *    the earlier implementation created a manual-reset host event, set it on
 *    the first KeSetEvent and never reset it, after which every wait on the
 *    object returned in under a millisecond without waiting.  Good boots were
 *    good only because they consumed that stale signal.
 *
 *    There is no host event here at all.  Each wait re-reads Type and
 *    SignalState from guest RAM, so a direct guest store takes effect with no
 *    kernel call needed, and KeSetEvent is a read-modify-write of the same
 *    word.  The registry below is bookkeeping: counters and the "is this
 *    really an event" check, nothing the wait depends on.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
extern ptrdiff_t g_xbox_mem_offset;

/* Servicing hooks, all on this thread.  recomp_safe_point() delivers pending
 * interrupt vectors and then drains; recomp_dpc_drain() is called again
 * directly because safe points have a re-entrancy guard and a wait entered
 * from inside a service routine would otherwise never see its own DPC.  Both
 * save and restore the guest register file. */
extern void recomp_safe_point(void);
extern void recomp_dpc_drain(void);
extern unsigned long long g_sp_hits, g_dpc_runs_total;
extern unsigned long long g_vec_delivered[64];

#define KDISP_MAX 256u
#define KDISP_VBLANK_VECTOR 51u

struct kdisp_entry {
    uint32_t va;
    uint8_t  type;      /* last seen; the wait re-reads it every time */
    unsigned long long waits, sets, blocked, timeouts;
};

static struct kdisp_entry g_kdisp[KDISP_MAX];
static unsigned g_kdisp_n;
static CRITICAL_SECTION g_kdisp_cs;
static int g_kdisp_cs_ready;

unsigned long long g_kdisp_waits, g_kdisp_sets, g_kdisp_created;
unsigned long long g_kdisp_immediate, g_kdisp_blocked, g_kdisp_bad;
unsigned long long g_kdisp_timeouts, g_kdisp_services;

static void kdisp_lock(void)
{
    if (!g_kdisp_cs_ready) {           /* first touch is on the guest thread */
        InitializeCriticalSection(&g_kdisp_cs);
        g_kdisp_cs_ready = 1;
    }
    EnterCriticalSection(&g_kdisp_cs);
}

static void kdisp_unlock(void) { LeaveCriticalSection(&g_kdisp_cs); }

static volatile uint32_t *kdisp_guest(uint32_t va)
{
    return (volatile uint32_t *)(uintptr_t)((uintptr_t)va +
                                            (uintptr_t)g_xbox_mem_offset);
}

/* Bookkeeping only.  Nothing the wait or the set depends on lives here, so
 * there is no adoption race to lose a signal to. */
static struct kdisp_entry *kdisp_note(uint32_t va, uint8_t type)
{
    unsigned i;
    struct kdisp_entry *e;

    kdisp_lock();
    for (i = 0; i < g_kdisp_n; ++i) {
        if (g_kdisp[i].va == va) {
            e = &g_kdisp[i];
            e->type = type;
            kdisp_unlock();
            return e;
        }
    }
    if (g_kdisp_n >= KDISP_MAX) { kdisp_unlock(); return NULL; }
    e = &g_kdisp[g_kdisp_n++];
    e->va = va;
    e->type = type;
    ++g_kdisp_created;
    kdisp_unlock();
    return e;
}

/* Events and timers share dispatcher signal/consume semantics. */
static int kdisp_is_event(uint32_t header, uint8_t *type)
{
    uint8_t t = (uint8_t)(header & 0xFFu);
    uint8_t size = (uint8_t)((header >> 16) & 0xFFu);

    if ((t > 1u && t != 8u && t != 9u) || size == 0u)
        return 0;
    *type = t;
    return 1;
}

static double kdisp_seconds(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;

    if (f.QuadPart == 0)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static int kdisp_log_enabled(void)
{
    static int state;   /* 0 unknown, 1 on, 2 off */

    if (!state) {
        const char *v = getenv("CONKER_KDISP_LOG");
        state = (v && v[0] == '0') ? 2 : 1;
    }
    return state == 1;
}

/*
 * KeWaitForSingleObject on a guest dispatcher object.
 *
 * Timeout follows the NT convention: NULL infinite, 0 a poll, negative a
 * relative 100ns interval, positive an absolute FILETIME.  The deadline is
 * taken once at entry so servicing time counts against it, and STATUS_TIMEOUT
 * is returned only once that deadline has actually passed.
 */
NTSTATUS xbox_kdisp_wait_va(uint32_t va, PLARGE_INTEGER Timeout,
                            BOOLEAN Alertable)
{
    volatile uint32_t *obj = kdisp_guest(va);
    struct kdisp_entry *e;
    uint8_t type;
    DWORD ms;
    int finite;
    double deadline = 0.0, t_in;
    unsigned long long spins = 0, sp0, dpc0, vbl0;

    ++g_kdisp_waits;

    if (!kdisp_is_event(obj[0], &type)) {
        ++g_kdisp_bad;
        return STATUS_UNSUCCESSFUL;
    }
    e = kdisp_note(va, type);
    if (e) ++e->waits;

    /* NULL is the only infinite form; a 0xFFFFFFFF millisecond count
     * from a 49-day relative interval is still a finite wait. */
    finite = (Timeout != NULL);
    ms = finite ? xbox_nt_timeout_to_ms(Timeout) : INFINITE;
    t_in = kdisp_seconds();
    if (finite)
        deadline = t_in + (double)ms / 1000.0;

    sp0 = g_sp_hits;
    dpc0 = g_dpc_runs_total;
    vbl0 = g_vec_delivered[KDISP_VBLANK_VECTOR];

    for (;;) {
        /* Guest memory, every time round: a direct store by the title is as
         * good as a KeSetEvent and must be seen without one. */
        if (obj[1]) {
            /* Synchronization events are consumed by the waiter; notification
             * events stay signalled until something clears them. */
            if (type == 1u || type == 9u)
                obj[1] = 0u;
            if (spins == 0) {
                ++g_kdisp_immediate;
            } else if (kdisp_log_enabled()) {
                fprintf(stderr, "[KDISP] wait %08X satisfied after %.1f ms: "
                        "%llu safe points, %llu DPCs, %llu vblanks\n", va,
                        (kdisp_seconds() - t_in) * 1000.0,
                        g_sp_hits - sp0, g_dpc_runs_total - dpc0,
                        g_vec_delivered[KDISP_VBLANK_VECTOR] - vbl0);
                fflush(stderr);
            }
            return STATUS_SUCCESS;
        }

        if (finite && ms == 0) {    /* a zero timeout is a poll, not a wait */
            ++g_kdisp_timeouts;
            if (e) ++e->timeouts;
            return STATUS_TIMEOUT;
        }
        if (finite && kdisp_seconds() >= deadline) {
            ++g_kdisp_timeouts;
            if (e) ++e->timeouts;
            return STATUS_TIMEOUT;
        }

        if (spins == 0) {
            ++g_kdisp_blocked;
            if (e) ++e->blocked;
            if (kdisp_log_enabled()) {
                fprintf(stderr, "[KDISP] wait %08X type=%u timeout=%s: "
                        "unsignalled, servicing\n", va, type,
                        finite ? "finite" : "INFINITE");
                fflush(stderr);
            }
        }
        ++spins;
        ++g_kdisp_services;

        /* Run the machine.  This is the whole point: the object is signalled
         * by an ISR/DPC that can only execute on this thread. */
        recomp_safe_point();
        recomp_dpc_drain();
        if (obj[1])
            continue;               /* re-check before giving up the CPU */

        /* Yield first -- most waits here are satisfied by a vblank inside one
         * field -- then sleep, never past the deadline.  SleepEx carries the
         * alertable bit so a queued APC still ends the wait. */
        if (spins < 64ull) {
            if (Alertable) {
                if (SleepEx(0, TRUE) == WAIT_IO_COMPLETION)
                    return STATUS_ALERTED;
            } else {
                SwitchToThread();
            }
        } else {
            DWORD nap = 1;
            if (finite) {
                double left = (deadline - kdisp_seconds()) * 1000.0;
                if (left <= 0.0)
                    continue;       /* the deadline test above reports it */
                if (left < 1.0)
                    nap = 0;
            }
            if (SleepEx(nap, Alertable) == WAIT_IO_COMPLETION)
                return STATUS_ALERTED;
        }
    }
}

/*
 * KeSetEvent on a guest dispatcher object.  Returns the previous state, which
 * the real kernel does and the handle version could not.  A waiter sees this
 * on its next poll, so there is no separate wake to get wrong.
 */
LONG xbox_kdisp_set_va(uint32_t va)
{
    volatile uint32_t *obj = kdisp_guest(va);
    struct kdisp_entry *e;
    uint8_t type;
    LONG prev;

    ++g_kdisp_sets;
    if (!kdisp_is_event(obj[0], &type)) {
        ++g_kdisp_bad;
        return 0;
    }
    e = kdisp_note(va, type);
    if (e) ++e->sets;

    prev = (LONG)obj[1];
    obj[1] = 1u;
    return prev;
}

void xbox_kdisp_report(void)
{
    unsigned i;

    fprintf(stderr, "[KDISP] objects=%llu waits=%llu (immediate %llu, serviced "
            "%llu, timed out %llu) service iterations=%llu sets=%llu "
            "rejected=%llu\n",
            g_kdisp_created, g_kdisp_waits, g_kdisp_immediate,
            g_kdisp_blocked, g_kdisp_timeouts, g_kdisp_services,
            g_kdisp_sets, g_kdisp_bad);
    for (i = 0; i < g_kdisp_n; ++i)
        fprintf(stderr, "[KDISP]   obj %08X type %u  waits=%llu (serviced %llu,"
                " timed out %llu) sets=%llu  signal now %u\n",
                g_kdisp[i].va, g_kdisp[i].type, g_kdisp[i].waits,
                g_kdisp[i].blocked, g_kdisp[i].timeouts, g_kdisp[i].sets,
                (unsigned)kdisp_guest(g_kdisp[i].va)[1]);
    fflush(stderr);
}

/*
 * KeSetEvent - kernel-mode event signal.
 * On Xbox, this is the kernel-mode equivalent of NtSetEvent.
 * The Object parameter is treated as a Win32 event HANDLE.
 * Returns the previous signal state.
 */
LONG __stdcall xbox_KeSetEvent(PVOID Event, LONG Increment, BOOLEAN Wait)
{
    HANDLE hEvent = (HANDLE)Event;

    (void)Increment;
    (void)Wait;

    /* We can't easily query previous state, so just set and return 0 */
    SetEvent(hEvent);
    return 0;
}

/* ============================================================================
 * Semaphores
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateSemaphore(
    PHANDLE SemaphoreHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    LONG InitialCount,
    LONG MaximumCount)
{
    HANDLE hSemaphore;

    (void)ObjectAttributes;

    if (!SemaphoreHandle)
        return STATUS_INVALID_PARAMETER;

    hSemaphore = CreateSemaphoreW(NULL, InitialCount, MaximumCount, NULL);
    if (!hSemaphore) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateSemaphore: CreateSemaphoreW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *SemaphoreHandle = hSemaphore;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateSemaphore: handle=%p, initial=%d, max=%d",
        hSemaphore, InitialCount, MaximumCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReleaseSemaphore(
    HANDLE SemaphoreHandle,
    LONG ReleaseCount,
    PLONG PreviousCount)
{
    if (!ReleaseSemaphore(SemaphoreHandle, ReleaseCount, PreviousCount)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtReleaseSemaphore: failed for handle %p (error %u)",
            SemaphoreHandle, GetLastError());
        return STATUS_INVALID_HANDLE;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Wait Functions
 * ============================================================================ */

/*
 * Map Win32 WaitFor* return codes to NTSTATUS.
 */
static NTSTATUS xbox_wait_result_to_ntstatus(DWORD result, ULONG count)
{
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
        return (NTSTATUS)(STATUS_SUCCESS + (result - WAIT_OBJECT_0));

    switch (result) {
        case WAIT_TIMEOUT:          return STATUS_TIMEOUT;
        case WAIT_IO_COMPLETION:    return STATUS_ALERTED;
        case WAIT_ABANDONED_0:      return STATUS_ABANDONED;
        case WAIT_FAILED:           return STATUS_UNSUCCESSFUL;
        default:                    return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS __stdcall xbox_NtWaitForSingleObject(
    HANDLE Handle,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result;
    result = WaitForSingleObjectEx(Handle, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_NtWaitForSingleObjectEx(
    HANDLE Handle,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result;

    /*
     * Same as NtWaitForSingleObject but with an explicit wait mode. We run
     * everything in one host process with no kernel/user split, so WaitMode
     * has nothing to select and is ignored.
     */
    (void)WaitMode;

    result = WaitForSingleObjectEx(Handle, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_NtWaitForMultipleObjectsEx(
    ULONG Count,
    HANDLE Handles[],
    ULONG WaitType,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    BOOL bWaitAll;
    DWORD result;

    /* WaitType: 0 = WaitAll, 1 = WaitAny (matches NT definitions) */
    bWaitAll = (WaitType == 0) ? TRUE : FALSE;

    result = WaitForMultipleObjectsEx(Count, Handles, bWaitAll, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, Count);
}

/*
 * KeWaitForSingleObject - kernel-mode wait on a dispatcher object.
 * On Xbox, Objects can be events, timers, threads, etc.
 * We treat the object pointer as a Win32 HANDLE.
 */
NTSTATUS __stdcall xbox_KeWaitForSingleObject(
    PVOID Object,
    ULONG WaitReason,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    (void)WaitReason;
    (void)WaitMode;

    HANDLE hObject = (HANDLE)Object;
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);
    DWORD result = WaitForSingleObjectEx(hObject, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, 1);
}

NTSTATUS __stdcall xbox_KeWaitForMultipleObjects(
    ULONG Count,
    PVOID Objects[],
    ULONG WaitType,
    ULONG WaitReason,
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout,
    PVOID WaitBlockArray)
{
    (void)WaitReason;
    (void)WaitMode;
    (void)WaitBlockArray;

    BOOL bWaitAll = (WaitType == 0) ? TRUE : FALSE;
    DWORD ms = xbox_nt_timeout_to_ms(Timeout);

    /* Objects[] is an array of PVOID which we treat as HANDLE[] */
    DWORD result = WaitForMultipleObjectsEx(Count, (HANDLE*)Objects,
                                            bWaitAll, ms, Alertable);
    return xbox_wait_result_to_ntstatus(result, Count);
}

/* ============================================================================
 * Kernel Timers
 *
 * Xbox kernel timers are dispatcher objects that can be waited on and
 * optionally queue a DPC when they expire.
 *
 * Implementation: Each XBOX_KTIMER contains a Win32 event (for waitable
 * behavior) and uses CreateTimerQueueTimer for the timing mechanism.
 * When the timer fires, it signals the event and optionally invokes the DPC.
 * ============================================================================ */

/* Global timer queue - created lazily on first timer use */
static HANDLE g_timer_queue = NULL;
static CRITICAL_SECTION g_timer_cs;
static BOOL g_timer_cs_init = FALSE;

static void xbox_ensure_timer_queue(void)
{
    if (!g_timer_cs_init) {
        InitializeCriticalSection(&g_timer_cs);
        g_timer_cs_init = TRUE;
    }

    if (!g_timer_queue) {
        EnterCriticalSection(&g_timer_cs);
        if (!g_timer_queue) {
            g_timer_queue = CreateTimerQueue();
            if (!g_timer_queue) {
                xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
                    "Failed to create timer queue (error %u)", GetLastError());
            }
        }
        LeaveCriticalSection(&g_timer_cs);
    }
}

/*
 * Timer callback - called by the Windows timer queue thread.
 * Signals the event (for KeWaitForSingleObject) and fires the DPC if set.
 */
static VOID CALLBACK xbox_timer_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
    PXBOX_KTIMER timer = (PXBOX_KTIMER)lpParameter;

    (void)TimerOrWaitFired;

    /* Signal the event so waiters wake up */
    if (timer->win32_event)
        SetEvent(timer->win32_event);

    /* Fire the DPC if one is associated */
    if (timer->Dpc && timer->Dpc->DeferredRoutine) {
        xbox_log(XBOX_LOG_TRACE, XBOX_LOG_SYNC, "Timer DPC firing: routine=%p",
            timer->Dpc->DeferredRoutine);
        timer->Dpc->DeferredRoutine(
            timer->Dpc,
            timer->Dpc->DeferredContext,
            timer->Dpc->SystemArgument1,
            timer->Dpc->SystemArgument2);
    }

    /* If not periodic, mark as no longer inserted */
    if (timer->Period == 0)
        timer->Inserted = FALSE;
}

VOID __stdcall xbox_KeInitializeTimerEx(PXBOX_KTIMER Timer, XBOX_TIMER_TYPE Type)
{
    (void)Type;

    if (!Timer)
        return;

    memset(Timer, 0, sizeof(XBOX_KTIMER));

    /* Create a manual-reset event for notification timers,
     * auto-reset for synchronization timers */
    BOOL manual_reset = (Type == XboxNotificationTimer) ? TRUE : FALSE;
    Timer->win32_event = CreateEventW(NULL, manual_reset, FALSE, NULL);
    Timer->Inserted = FALSE;
    Timer->Period = 0;
    Timer->Dpc = NULL;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeInitializeTimerEx: timer=%p, type=%d, event=%p",
        Timer, Type, Timer->win32_event);
}

BOOLEAN __stdcall xbox_KeSetTimer(PXBOX_KTIMER Timer, LARGE_INTEGER DueTime, PXBOX_KDPC Dpc)
{
    return xbox_KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

BOOLEAN __stdcall xbox_KeSetTimerEx(
    PXBOX_KTIMER Timer,
    LARGE_INTEGER DueTime,
    LONG Period,
    PXBOX_KDPC Dpc)
{
    BOOLEAN was_inserted;
    DWORD due_ms;
    DWORD period_ms;

    if (!Timer)
        return FALSE;

    xbox_ensure_timer_queue();

    was_inserted = Timer->Inserted;

    /* Cancel existing timer if re-arming */
    if (was_inserted && Timer->win32_timer) {
        DeleteTimerQueueTimer(g_timer_queue, Timer->win32_timer, NULL);
        Timer->win32_timer = NULL;
    }

    /* Reset the event */
    if (Timer->win32_event)
        ResetEvent(Timer->win32_event);

    Timer->Dpc = Dpc;
    Timer->Period = Period;

    /* Convert DueTime (100ns units) to milliseconds */
    if (DueTime.QuadPart < 0) {
        LONGLONG relative_100ns = -DueTime.QuadPart;
        due_ms = (DWORD)(relative_100ns / 10000);
        if (due_ms == 0 && relative_100ns > 0)
            due_ms = 1;
    } else if (DueTime.QuadPart == 0) {
        due_ms = 0;
    } else {
        LARGE_INTEGER now;
        GetSystemTimeAsFileTime((LPFILETIME)&now);
        LONGLONG diff = DueTime.QuadPart - now.QuadPart;
        due_ms = (diff > 0) ? (DWORD)(diff / 10000) : 0;
    }

    period_ms = (Period > 0) ? (DWORD)Period : 0;

    DWORD flags = 0;
    if (period_ms == 0)
        flags |= WT_EXECUTEONLYONCE;

    if (!CreateTimerQueueTimer(&Timer->win32_timer, g_timer_queue,
                               xbox_timer_callback, Timer,
                               due_ms, period_ms, flags)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "KeSetTimerEx: CreateTimerQueueTimer failed (error %u)", GetLastError());
        return was_inserted;
    }

    Timer->Inserted = TRUE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeSetTimerEx: timer=%p, due=%ums, period=%ums, dpc=%p",
        Timer, due_ms, period_ms, Dpc);

    return was_inserted;
}

BOOLEAN __stdcall xbox_KeCancelTimer(PXBOX_KTIMER Timer)
{
    BOOLEAN was_inserted;

    if (!Timer)
        return FALSE;

    was_inserted = Timer->Inserted;

    if (was_inserted && Timer->win32_timer) {
        DeleteTimerQueueTimer(g_timer_queue, Timer->win32_timer, INVALID_HANDLE_VALUE);
        Timer->win32_timer = NULL;
        Timer->Inserted = FALSE;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeCancelTimer: timer=%p, was_inserted=%d", Timer, was_inserted);

    return was_inserted;
}

/* ============================================================================
 * Deferred Procedure Calls (DPCs)
 *
 * Xbox DPCs are typically queued from ISRs or timer callbacks to run at
 * DISPATCH_LEVEL. On Windows, we execute them immediately or via thread pool
 * since we don't have real IRQL levels.
 * ============================================================================ */

VOID __stdcall xbox_KeInitializeDpc(
    PXBOX_KDPC Dpc,
    PKDEFERRED_ROUTINE DeferredRoutine,
    PVOID DeferredContext)
{
    if (!Dpc)
        return;

    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    Dpc->SystemArgument1 = NULL;
    Dpc->SystemArgument2 = NULL;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "KeInitializeDpc: dpc=%p, routine=%p", Dpc, DeferredRoutine);
}

/*
 * Thread pool callback for executing DPCs.
 */
static VOID CALLBACK xbox_dpc_work_callback(PTP_CALLBACK_INSTANCE Instance,
                                            PVOID Context)
{
    PXBOX_KDPC dpc = (PXBOX_KDPC)Context;

    (void)Instance;

    if (dpc && dpc->DeferredRoutine) {
        dpc->DeferredRoutine(dpc, dpc->DeferredContext,
                            dpc->SystemArgument1, dpc->SystemArgument2);
    }
}

BOOLEAN __stdcall xbox_KeInsertQueueDpc(
    PXBOX_KDPC Dpc,
    PVOID SystemArgument1,
    PVOID SystemArgument2)
{
    if (!Dpc || !Dpc->DeferredRoutine)
        return FALSE;

    Dpc->SystemArgument1 = SystemArgument1;
    Dpc->SystemArgument2 = SystemArgument2;

    /* Submit to the Windows thread pool for async execution */
    if (!TrySubmitThreadpoolCallback(xbox_dpc_work_callback, Dpc, NULL)) {
        /* Fallback: execute synchronously */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_SYNC,
            "KeInsertQueueDpc: threadpool submit failed, executing synchronously");
        Dpc->DeferredRoutine(Dpc, Dpc->DeferredContext,
                            SystemArgument1, SystemArgument2);
    }

    return TRUE;
}

BOOLEAN __stdcall xbox_KeRemoveQueueDpc(PXBOX_KDPC Dpc)
{
    /*
     * On Windows, once submitted to the thread pool we can't easily cancel.
     * DPCs are typically very short-lived, so this is rarely called.
     * Return FALSE to indicate the DPC was not in the queue (may have already run).
     */
    (void)Dpc;
    return FALSE;
}

/* ============================================================================
 * KeSynchronizeExecution
 *
 * BOOLEAN KeSynchronizeExecution(PKINTERRUPT Interrupt,
 *                                PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
 *                                PVOID SynchronizeContext)
 *
 * Raises to the interrupt's IRQL, takes its spinlock, runs the routine, and
 * returns what the routine returned.  The point of it is the exclusion: the
 * caller is reaching into state its ISR also touches.
 *
 * SynchronizeRoutine is a *guest* code address.  This used to cast it to a
 * native function pointer and call it, which would jump to whatever lives at
 * that host address -- so it could never have worked; it went unnoticed only
 * because ordinal 153 had no bridge entry and the call was skipped entirely.
 * It goes through the recomp dispatcher instead, which also gives the routine
 * the guest register file and stack it expects.
 *
 * There is no IRQL in this model, so the exclusion is expressed by holding
 * off interrupt delivery for the duration, which recomp_call_guest_stdcall1
 * does.
 * ============================================================================ */
extern int recomp_call_guest_stdcall1(uint32_t routine_va, uint32_t arg,
                                      uint32_t *out_eax);

BOOLEAN __stdcall xbox_KeSynchronizeExecution(
    PXBOX_KINTERRUPT Interrupt,
    PVOID SynchronizeRoutine,
    PVOID SynchronizeContext)
{
    uint32_t routine_va = (uint32_t)(uintptr_t)SynchronizeRoutine;
    uint32_t context_va = (uint32_t)(uintptr_t)SynchronizeContext;
    uint32_t result = 0;

    (void)Interrupt;

    if (!routine_va)
        return FALSE;
    if (!recomp_call_guest_stdcall1(routine_va, context_va, &result))
        return FALSE;

    /* BOOLEAN is a byte: the routine sets AL and leaves the rest of EAX
     * holding whatever it had. */
    return (BOOLEAN)(result & 0xFFu);
}

/* ============================================================================
 * Events (continued)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtClearEvent(HANDLE EventHandle)
{
    if (!ResetEvent(EventHandle)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtClearEvent: ResetEvent failed (error %u)", GetLastError());
        return STATUS_INVALID_HANDLE;
    }
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Mutants (mutexes)
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtCreateMutant(
    PHANDLE MutantHandle,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    BOOLEAN InitialOwner)
{
    HANDLE hMutex;

    (void)ObjectAttributes;

    if (!MutantHandle)
        return STATUS_INVALID_PARAMETER;

    hMutex = CreateMutexW(NULL, InitialOwner, NULL);
    if (!hMutex) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtCreateMutant: CreateMutexW failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *MutantHandle = hMutex;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_SYNC,
        "NtCreateMutant: handle=%p initial_owner=%d", hMutex, InitialOwner);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount)
{
    /*
     * ReleaseMutex reports the previous count only through its own bookkeeping,
     * which Win32 does not expose. Callers overwhelmingly use PreviousCount as
     * an ignore-me out-param; report 0 (the count before this release took it
     * to unheld) rather than leaving the caller's storage uninitialised.
     */
    if (!ReleaseMutex(MutantHandle)) {
        DWORD err = GetLastError();
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_SYNC,
            "NtReleaseMutant: ReleaseMutex failed (error %u)", err);
        /* Releasing a mutex this thread does not own is the common failure. */
        return (err == ERROR_NOT_OWNER) ? STATUS_MUTANT_NOT_OWNED
                                        : STATUS_INVALID_HANDLE;
    }

    if (PreviousCount)
        *PreviousCount = 0;

    return STATUS_SUCCESS;
}
