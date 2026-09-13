#include <windows.h>
#include <stdint.h>
#include "kernel_guest_clock.h"

static HANDLE clock_stop, clock_thread;
static volatile uint32_t *guest_tick;
static LARGE_INTEGER clock_origin, clock_frequency;
static uint64_t clock_origin_ms;

static void publish_tick(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t elapsed = (uint64_t)(now.QuadPart - clock_origin.QuadPart);
    uint64_t frequency = (uint64_t)clock_frequency.QuadPart;
    uint64_t ms = clock_origin_ms + (elapsed / frequency) * 1000u
                + (elapsed % frequency) * 1000u / frequency;
    /* KeTickCount is read directly by guest code, including busy waits. */
    InterlockedExchange((volatile LONG *)guest_tick, (LONG)(uint32_t)ms);
}

static DWORD WINAPI clock_main(void *unused)
{
    (void)unused;
    while (WaitForSingleObject(clock_stop, 1) == WAIT_TIMEOUT)
        publish_tick();
    return 0;
}

void recomp_clock_stop(void)
{
    if (clock_thread) {
        SetEvent(clock_stop);
        WaitForSingleObject(clock_thread, INFINITE);
        CloseHandle(clock_thread);
        clock_thread = NULL;
    }
    if (clock_stop) { CloseHandle(clock_stop); clock_stop = NULL; }
    guest_tick = NULL;
}

int recomp_clock_start(volatile uint32_t *tick_count)
{
    recomp_clock_stop();
    if (!tick_count || ((uintptr_t)tick_count & 3u)) return 0;
    QueryPerformanceFrequency(&clock_frequency);
    QueryPerformanceCounter(&clock_origin);
    clock_origin_ms = GetTickCount64();
    guest_tick = tick_count;
    publish_tick();
    clock_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (clock_stop) clock_thread = CreateThread(NULL, 0, clock_main, NULL, 0, NULL);
    if (!clock_thread) { recomp_clock_stop(); return 0; }
    return 1;
}
