/*
 * kernel_memory.c - Xbox Memory Management
 *
 * Implements Mm* and NtAllocateVirtualMemory/NtFreeVirtualMemory/NtQueryVirtualMemory
 * using Win32 VirtualAlloc/VirtualFree/VirtualQuery.
 *
 * Xbox contiguous memory (MmAllocateContiguousMemory) is used for GPU-accessible
 * buffers. On Windows, actual GPU resources are handled by our D3D11 layer;
 * these allocations just need to return valid CPU-accessible memory.
 */

#include "kernel.h"
#include "xbox_memory.h"   /* XBOX_MEM_SIZE, xbox_heap_free() */
#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>

extern uint32_t xbox_heap_alloc(uint32_t size, uint32_t alignment);
extern void recomp_init_graphics_after_heap(void);

void xbox_log(int level, const char* subsystem, const char* fmt, ...)
{
    static const char* level_names[] = {
        "ERROR",
        "WARN",
        "INFO",
        "DEBUG",
        "TRACE"
    };

    const char* level_name =
        (level >= 0 && level <= 4) ? level_names[level] : "UNKNOWN";

    fprintf(stderr, "[%s][%s] ",
        level_name,
        subsystem ? subsystem : "XBOX");

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
}

/* ============================================================================
 * Helper: Xbox protect flags → Win32 protect flags
 * ============================================================================ */

static DWORD xbox_protect_to_win32(ULONG xbox_protect)
{
    /* Xbox uses the same PAGE_* constants as Windows NT */
    switch (xbox_protect & 0xFF) {
        case 0x01: return PAGE_NOACCESS;
        case 0x02: return PAGE_READONLY;
        case 0x04: return PAGE_READWRITE;
        case 0x08: return PAGE_WRITECOPY;
        case 0x10: return PAGE_EXECUTE;
        case 0x20: return PAGE_EXECUTE_READ;
        case 0x40: return PAGE_EXECUTE_READWRITE;
        default:   return PAGE_READWRITE;
    }
}

/* ============================================================================
 * Contiguous Memory (GPU-accessible on Xbox)
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateContiguousMemory(ULONG NumberOfBytes)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

PVOID __stdcall xbox_MmAllocateContiguousMemoryEx(
    ULONG NumberOfBytes,
    ULONG_PTR LowestAcceptableAddress,
    ULONG_PTR HighestAcceptableAddress,
    ULONG Alignment,
    ULONG Protect)
{
    /*
     * Xbox requests physically contiguous, aligned memory for GPU use.
     * We can't guarantee physical contiguity on Windows, but the game's
     * CPU-side code just needs a valid pointer. GPU resources are handled
     * separately by our D3D11 layer.
     *
     * Use _aligned_malloc for alignment, then VirtualAlloc for a fallback.
     */
    PVOID p = NULL;

    if (Alignment > 0 && (Alignment & (Alignment - 1)) == 0) {
        /* Power-of-2 alignment: use _aligned_malloc */
        p = _aligned_malloc(NumberOfBytes, Alignment);
        if (p)
            memset(p, 0, NumberOfBytes);
    }

    if (!p) {
        /* Fallback: page-aligned VirtualAlloc */
        p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                         xbox_protect_to_win32(Protect));
    }

    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateContiguousMemoryEx(%u, align=%u) = %p",
        NumberOfBytes, Alignment, p);
    return p;
}

VOID __stdcall xbox_MmFreeContiguousMemory(PVOID BaseAddress)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeContiguousMemory(%p)", BaseAddress);
    if (!BaseAddress)
        return;

    /*
     * Determine if this was allocated with _aligned_malloc or VirtualAlloc.
     * We use VirtualQuery to check: if it's a VirtualAlloc'd region,
     * AllocationBase will equal the pointer (page-aligned).
     */
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi)) &&
        mbi.AllocationBase == BaseAddress &&
        mbi.State == MEM_COMMIT) {
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
    } else {
        _aligned_free(BaseAddress);
    }
}

/* ============================================================================
 * System Memory
 * ============================================================================ */

PVOID __stdcall xbox_MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
{
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE,
                           xbox_protect_to_win32(Protect));
    XBOX_TRACE(XBOX_LOG_MEM, "MmAllocateSystemMemory(%u) = %p", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmFreeSystemMemory(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

/* ============================================================================
 * Memory Query & Protection
 * ============================================================================ */

NTSTATUS __stdcall xbox_MmQueryStatistics(PXBOX_MM_STATISTICS MemoryStatistics)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);

    if (!MemoryStatistics)
        return STATUS_INVALID_PARAMETER;

    if (!GlobalMemoryStatusEx(&ms))
        return STATUS_UNSUCCESSFUL;

    memset(MemoryStatistics, 0, sizeof(XBOX_MM_STATISTICS));
    MemoryStatistics->Length = sizeof(XBOX_MM_STATISTICS);

    /* Xbox has 64MB RAM. Report plausible values. */
    ULONG page_size = 4096;
    MemoryStatistics->TotalPhysicalPages = 64 * 1024 * 1024 / page_size; /* 16384 pages */
    MemoryStatistics->AvailablePages = (ULONG)(ms.ullAvailPhys / page_size);
    if (MemoryStatistics->AvailablePages > MemoryStatistics->TotalPhysicalPages)
        MemoryStatistics->AvailablePages = MemoryStatistics->TotalPhysicalPages / 2;

    return STATUS_SUCCESS;
}

PVOID __stdcall xbox_MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
{
    /* GPU register access - handled by our D3D11 layer. Return a dummy buffer. */
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmMapIoSpace(0x%08X, %u) = %p (stub)", (ULONG)PhysicalAddress, NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmUnmapIoSpace(PVOID BaseAddress, ULONG NumberOfBytes)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmUnmapIoSpace(%p, %u)", BaseAddress, NumberOfBytes);
    if (BaseAddress)
        VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

ULONG_PTR __stdcall xbox_MmGetPhysicalAddress(PVOID BaseAddress)
{
    /* No physical address translation on Windows - return the VA as a placeholder */
    return (ULONG_PTR)BaseAddress;
}

VOID __stdcall xbox_MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
{
    /* Xbox: mark memory to survive soft-reboot. No-op on Windows. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmPersistContiguousMemory(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, Persist);
}

ULONG __stdcall xbox_MmQueryAddressProtect(PVOID VirtualAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(VirtualAddress, &mbi, sizeof(mbi))) {
        /* Return the Xbox-equivalent protection */
        return mbi.Protect;
    }
    return PAGE_NOACCESS;
}

VOID __stdcall xbox_MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)
{
    DWORD old_protect;
    VirtualProtect(BaseAddress, NumberOfBytes, xbox_protect_to_win32(NewProtect), &old_protect);
    XBOX_TRACE(XBOX_LOG_MEM, "MmSetAddressProtect(%p, %u, 0x%X)", BaseAddress, NumberOfBytes, NewProtect);
}

ULONG __stdcall xbox_MmQueryAllocationSize(PVOID BaseAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(BaseAddress, &mbi, sizeof(mbi))) {
        return (ULONG)mbi.RegionSize;
    }
    return 0;
}

PVOID __stdcall xbox_MmClaimGpuInstanceMemory(ULONG NumberOfBytes, PULONG NumberOfPaddingBytes)
{
    /* GPU instance memory - handled by D3D11 layer */
    if (NumberOfPaddingBytes)
        *NumberOfPaddingBytes = 0;
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XBOX_TRACE(XBOX_LOG_MEM, "MmClaimGpuInstanceMemory(%u) = %p (stub)", NumberOfBytes, p);
    return p;
}

VOID __stdcall xbox_MmLockUnlockBufferPages(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN UnlockPages)
{
    /* Page locking is not meaningful in user mode. No-op. */
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockBufferPages(%p, %u, %d) - stub", BaseAddress, NumberOfBytes, UnlockPages);
}

VOID __stdcall xbox_MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN UnlockPage)
{
    XBOX_TRACE(XBOX_LOG_MEM, "MmLockUnlockPhysicalPage(0x%08X, %d) - stub", (ULONG)PhysicalAddress, UnlockPage);
}

/* ============================================================================
 * Kernel Stack
 * ============================================================================ */

PVOID __stdcall xbox_MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)
{
    /* Allocate a stack-like region. Return the TOP of the stack (high address). */
    PVOID base = VirtualAlloc(NULL, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base)
        return NULL;

    /* Xbox convention: return pointer to top of stack */
    PVOID stack_top = (PUCHAR)base + NumberOfBytes;
    XBOX_TRACE(XBOX_LOG_MEM, "MmCreateKernelStack(%u) = %p (base=%p)", NumberOfBytes, stack_top, base);
    return stack_top;
}

VOID __stdcall xbox_MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
{
    /* StackLimit is the low address (base of VirtualAlloc), StackBase is the high address */
    XBOX_TRACE(XBOX_LOG_MEM, "MmDeleteKernelStack(base=%p, limit=%p)", StackBase, StackLimit);
    if (StackLimit)
        VirtualFree(StackLimit, 0, MEM_RELEASE);
}

/* ============================================================================
 * Virtual Memory (Nt API)
 * ============================================================================ */

/*NTSTATUS __stdcall xbox_NtAllocateVirtualMemory(
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect)
{
    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;

    PVOID result = VirtualAlloc(*BaseAddress, *RegionSize,
                                AllocationType, xbox_protect_to_win32(Protect));
    if (!result) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_MEM,
            "NtAllocateVirtualMemory failed: base=%p size=%u type=0x%X err=%u",
            *BaseAddress, (ULONG)*RegionSize, AllocationType, GetLastError());
        return STATUS_NO_MEMORY;
    }

    *BaseAddress = result;
    XBOX_TRACE(XBOX_LOG_MEM, "NtAllocateVirtualMemory(%p, %u) = %p",
        *BaseAddress, (ULONG)*RegionSize, result);
    return STATUS_SUCCESS;
}*/

NTSTATUS __stdcall xbox_NtAllocateVirtualMemory(
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect)
{
    static int startup_heap_reserved = 0;
    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;

    uint32_t xbox_base = (uint32_t)(uintptr_t)(*BaseAddress);
    SIZE_T size = *RegionSize;

    /* The full Xbox address space is already backed by the runtime mapping.
     * Once the fixed startup heap reservation exists, anonymous game VM
     * requests need a stable Xbox VA, not an arbitrary host VirtualAlloc VA
     * translated through g_xbox_mem_offset. */
    if (xbox_base == 0 && startup_heap_reserved) {
        uint32_t xbox_result = xbox_heap_alloc((uint32_t)(size ? size : 1), 0x1000);
        if (!xbox_result)
            return STATUS_NO_MEMORY;
        *BaseAddress = (PVOID)(uintptr_t)xbox_result;
        XBOX_TRACE(XBOX_LOG_MEM,
            "NtAllocateVirtualMemory(xbox_base=0, size=%u) = xbox_result=0x%08X (bump)",
            (ULONG)size, xbox_result);
        return STATUS_SUCCESS;
    }

    PVOID native_hint = NULL;

    if (xbox_base != 0) {
        native_hint = (PVOID)(
            (uintptr_t)xbox_base + (uintptr_t)g_xbox_mem_offset
            );
    } else if (AllocationType != MEM_COMMIT && !startup_heap_reserved) {
        /* Conker's reconstructed startup heap retains a 32-bit block size
         * calculation that assumes its first VM reservation is low.  Asking
         * Windows for any address makes the resulting Xbox VA vary per run
         * (and can overflow that arithmetic).  Keep this first anonymous
         * reservation at the title's stable 0x04000000 heap region. */
        native_hint = (PVOID)(
            (uintptr_t)0x04000000u + (uintptr_t)g_xbox_mem_offset
            );
    }


    fprintf(stderr,
        "[VM DEBUG] xbox_base=0x%08X native_hint=%p size=%zu type=0x%X\n",
        xbox_base,
        native_hint,
        size,
        AllocationType
    );

    if (native_hint) {
        MEMORY_BASIC_INFORMATION mbi = { 0 };

        SIZE_T q = VirtualQuery(
            native_hint,
            &mbi,
            sizeof(mbi)
        );

        fprintf(stderr,
            "[VM DEBUG] VirtualQuery=%zu Base=%p AllocationBase=%p "
            "RegionSize=%zu State=0x%lX Protect=0x%lX Type=0x%lX\n",
            q,
            mbi.BaseAddress,
            mbi.AllocationBase,
            mbi.RegionSize,
            mbi.State,
            mbi.Protect,
            mbi.Type
        );
    }

    PVOID result;

    if (AllocationType == MEM_COMMIT) {
        /*
         * Commit pages inside an existing Xbox reservation.
         * native_hint is already the native address corresponding
         * to the Xbox VA supplied by the caller.
         */
        result = VirtualAlloc(
            native_hint,
            size,
            MEM_COMMIT,
            xbox_protect_to_win32(Protect)
        );
    }
    else {
        /*
         * Back a reservation immediately instead of leaving it reserve-only.
         *
         * Every Xbox VA below 64MB is already fully backed by the runtime's
         * file mapping, so the recompiled code is written against memory that
         * is simply there.  This path is the one exception: the title's startup
         * heap sits at Xbox VA 0x04000000, past the end of that mapping, so it
         * gets its own host allocation.  Left reserve-only it behaves unlike
         * every other guest address -- sub_003E6550 initialises its bucket
         * free-lists across the whole arena and faulted at 0x04001280, just
         * past the single 4KB page the title had committed so far.
         *
         * Committing up front keeps this region consistent with the rest of
         * guest memory.  A later MEM_COMMIT for a sub-range still succeeds:
         * committing already-committed pages is a no-op on Windows.
         */
        DWORD alloc_type = AllocationType;
        if (alloc_type & MEM_RESERVE)
            alloc_type |= MEM_COMMIT;

        /* xbox_memory_init already reserved the guest window above 64MB, so
         * nothing on the host can take the address the title wants. Inside it,
         * MEM_RESERVE would fail with ERROR_INVALID_ADDRESS -- the region is
         * reserved, by us -- so commit instead, which is what the request
         * actually means here. */
        if (g_guest_window_base && native_hint &&
            (uint8_t *)native_hint >= g_guest_window_base &&
            (uint8_t *)native_hint + size <=
                g_guest_window_base + g_guest_window_size) {
            alloc_type = (alloc_type & ~(DWORD)MEM_RESERVE) | MEM_COMMIT;
        }

        result = VirtualAlloc(
            native_hint,
            size,
            alloc_type,
            xbox_protect_to_win32(Protect)
        );
    }

    if (!result) {
        xbox_log(
            XBOX_LOG_WARN,
            XBOX_LOG_MEM,
            "NtAllocateVirtualMemory failed: xbox_base=0x%08X "
            "native_hint=%p size=%u type=0x%X err=%lu",
            xbox_base,
            native_hint,
            (ULONG)size,
            AllocationType,
            GetLastError()
        );

        return STATUS_NO_MEMORY;
    }

    if (xbox_base == 0 && AllocationType != MEM_COMMIT)
        startup_heap_reserved = 1;

    uint32_t xbox_result =
        (uint32_t)((uintptr_t)result - (uintptr_t)g_xbox_mem_offset);

    *BaseAddress = (PVOID)(uintptr_t)xbox_result;

    XBOX_TRACE(
        XBOX_LOG_MEM,
        "NtAllocateVirtualMemory(xbox_base=0x%08X, size=%u) "
        "= xbox_result=0x%08X native=%p",
        xbox_base,
        (ULONG)size,
        xbox_result,
        result
    );

    /* The initial game heap has its required fixed host address now.  It is
     * safe for the native graphics layer to allocate its own resources. */
    if (xbox_base == 0 && AllocationType != MEM_COMMIT)
        recomp_init_graphics_after_heap();

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtFreeVirtualMemory(
    PVOID* BaseAddress,
    PSIZE_T RegionSize,
    ULONG FreeType)
{
    if (!BaseAddress || !*BaseAddress)
        return STATUS_INVALID_PARAMETER;

    /* The caller's variable holds a 32-bit Xbox VA.  Reading it as a whole
     * PVOID pulled in the next four bytes as well, which is why every free
     * arrived as a value like 00065000C05BF000 and was refused with err=87 --
     * two guest words glued together, belonging to no allocation at all. */
    uint32_t xbox_base = (uint32_t)(uintptr_t)(*BaseAddress);
    SIZE_T size = (FreeType & MEM_RELEASE) ? 0 : (RegionSize ? *RegionSize : 0);

    /* Anything inside the 64 MB guest space came from the guest heap, which
     * now recycles.  Handing those addresses to VirtualFree was never going to
     * work: they are offsets into one large mapping, not separate host
     * reservations, so nothing was ever given back and the heap only grew. */
    if (xbox_base != 0 && xbox_base < XBOX_MEM_SIZE) {
        xbox_heap_free(xbox_base);
        XBOX_TRACE(XBOX_LOG_MEM, "NtFreeVirtualMemory(0x%08X, 0x%X) -> heap",
                   xbox_base, FreeType);
        if (FreeType & MEM_RELEASE)
            *BaseAddress = NULL;
        return STATUS_SUCCESS;
    }

    /* Above that the allocation really was a host reservation, placed at the
     * Xbox VA plus the mapping offset.  Free it where it actually lives. */
    {
        PVOID native = (PVOID)((uintptr_t)xbox_base +
                               (uintptr_t)g_xbox_mem_offset);
        if (!VirtualFree(native, size, FreeType)) {
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_MEM,
                "NtFreeVirtualMemory failed: xbox=0x%08X native=%p type=0x%X "
                "err=%u", xbox_base, native, FreeType, GetLastError());
            return STATUS_UNSUCCESSFUL;
        }
    }

    XBOX_TRACE(XBOX_LOG_MEM, "NtFreeVirtualMemory(0x%08X, 0x%X)",
               xbox_base, FreeType);

    if (FreeType & MEM_RELEASE)
        *BaseAddress = NULL;

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryVirtualMemory(
    PVOID BaseAddress,
    PVOID MemoryInformation,
    ULONG MemoryInformationLength,
    PULONG ReturnLength)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!VirtualQuery(BaseAddress, &mbi, sizeof(mbi)))
        return STATUS_INVALID_PARAMETER;

    /*
     * Xbox NtQueryVirtualMemory returns a MEMORY_BASIC_INFORMATION-like struct.
     * Copy what fits into the caller's buffer.
     */
    ULONG copy_size = (MemoryInformationLength < sizeof(mbi)) ? MemoryInformationLength : (ULONG)sizeof(mbi);
    memcpy(MemoryInformation, &mbi, copy_size);

    if (ReturnLength)
        *ReturnLength = copy_size;

    return STATUS_SUCCESS;
}
