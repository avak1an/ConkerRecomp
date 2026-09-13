#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "recomp_types.h"
#include "kernel_guest_timer.h"

extern int recomp_dpc_insert(uint32_t address);

typedef struct guest_timer {
    struct guest_timer *next;
    uint32_t address, dpc;
    uint64_t deadline, period;
    int absolute;
} guest_timer;

static guest_timer *timers;
static SRWLOCK timer_lock = SRWLOCK_INIT;
unsigned long long g_timer_sets, g_timer_expirations, g_timer_dpcs;

static uint64_t system_time(void)
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    return ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

static uint64_t monotonic_time(void)
{
    ULONGLONG time;
    QueryUnbiasedInterruptTime(&time);
    return time;
}

static guest_timer **find_timer(uint32_t address)
{
    guest_timer **link = &timers;
    while (*link && (*link)->address != address) link = &(*link)->next;
    return link;
}

int recomp_timer_cancel(uint32_t address)
{
    AcquireSRWLockExclusive(&timer_lock);
    guest_timer **link = find_timer(address);
    guest_timer *entry = *link;
    int was_set = entry != NULL;
    if (entry) { *link = entry->next; free(entry); }
    MEM8(address + 3) = 0; /* Header.Inserted; signal state is unchanged. */
    ReleaseSRWLockExclusive(&timer_lock);
    return was_set;
}

void recomp_timer_init(uint32_t address, unsigned type)
{
    recomp_timer_cancel(address);
    for (unsigned i = 0; i < 40; ++i) MEM8(address + i) = 0;
    MEM8(address) = (uint8_t)(8 + (type & 1));
    MEM8(address + 2) = 10; /* Xbox KTIMER is 40 bytes. */
    MEM32(address + 8) = MEM32(address + 12) = address + 8;
}

int recomp_timer_set(uint32_t address, int64_t due, int32_t period_ms, uint32_t dpc)
{
    AcquireSRWLockExclusive(&timer_lock);
    guest_timer **link = find_timer(address);
    guest_timer *entry = *link;
    int was_set = entry != NULL;
    if (!entry) {
        entry = (guest_timer *)calloc(1, sizeof(*entry));
        if (!entry) { fputs("[timer] out of host memory\n", stderr); abort(); }
        entry->address = address;
        *link = entry;
    }
    entry->absolute = due >= 0;
    /* Unsigned subtraction handles INT64_MIN without signed overflow. */
    entry->deadline = due >= 0 ? (uint64_t)due : monotonic_time() + (0u - (uint64_t)due);
    entry->period = period_ms > 0 ? (uint64_t)period_ms * 10000u : 0;
    entry->dpc = dpc;
    MEM8(address + 1) = (uint8_t)entry->absolute;
    MEM8(address + 3) = 1;
    MEM32(address + 4) = 0;
    MEM64(address + 16) = entry->deadline;
    MEM32(address + 32) = dpc;
    MEM32(address + 36) = (uint32_t)period_ms;
    ++g_timer_sets;
    ReleaseSRWLockExclusive(&timer_lock);
    return was_set;
}

void recomp_timer_poll(void)
{
    AcquireSRWLockExclusive(&timer_lock);
    if (!timers) { ReleaseSRWLockExclusive(&timer_lock); return; }
    uint64_t wall = system_time(), monotonic = monotonic_time();
    guest_timer **link = &timers;
    while (*link) {
        guest_timer *entry = *link;
        uint64_t now = entry->absolute ? wall : monotonic;
        if (entry->deadline > now) { link = &entry->next; continue; }
        uint32_t address = entry->address, dpc = entry->dpc;
        MEM32(address + 4) = 1;
        ++g_timer_expirations;
        if (entry->period) {
            /* Missed periods coalesce to one DPC, never a catch-up storm. */
            entry->deadline = monotonic + entry->period;
            entry->absolute = 0;
            MEM8(address + 1) = 0;
            MEM64(address + 16) = entry->deadline;
            link = &entry->next;
        } else {
            MEM8(address + 3) = 0;
            *link = entry->next;
            free(entry);
        }
        if (dpc && !MEM8(dpc + 2)) {
            MEM32(dpc + 20) = (uint32_t)wall;
            MEM32(dpc + 24) = (uint32_t)(wall >> 32);
            if (recomp_dpc_insert(dpc)) ++g_timer_dpcs;
        }
    }
    ReleaseSRWLockExclusive(&timer_lock);
}
