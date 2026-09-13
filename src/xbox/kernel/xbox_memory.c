/**
 * Xbox Memory Layout - Conker: Live and Reloaded
 *
 * Implements the API declared in templates/runtime/xbox_memory.h,
 * customized with the section layout from:
 *   py -3 -m tools.xbe_parser game_files/default.xbe
 *
 * Base Address: 0x00010000
 * Image Size:   0x00881D20
 * Sections end (.XTLID): 0x008912E0 + 0x00000A30 = 0x00891D10
 */

#include <stdlib.h>
#include "xbox_memory.h"
#include "recomp_types.h"
#include "kernel_guest_clock.h"
#include <stdio.h>

/* ---- Conker-specific section layout (from xbe_parser output) ---- */
/* NOTE: .text raw size (0x0047BF3C) differs slightly from a couple
 *       other sections' raw vs virtual size (e.g. XONLINE, DSOUND) -
 *       when copying sections below we always copy min(virt,raw) bytes
 *       and zero-fill the remainder, matching how the Xbox loader
 *       actually maps XBE sections. */

#define CONKER_TEXT_VA          0x00011000
#define CONKER_TEXT_SIZE        0x0047BF3C
#define CONKER_TEXT_RAW_OFFSET  0x00001000

#define CONKER_RDATA_VA         0x00561000
#define CONKER_RDATA_SIZE       0x000EE964
#define CONKER_RDATA_RAW_OFFSET 0x00555000

#define CONKER_DATA_VA          0x0064F980
#define CONKER_DATA_SIZE        0x002320E4   /* total virtual size incl. BSS */
#define CONKER_DATA_INIT_SIZE   0x0010F668   /* raw (initialized) size */
#define CONKER_DATA_RAW_OFFSET  0x00644000

/* Everything after .XTLID (0x00891D10) is free VA space we control. */
#define CONKER_STACK_BASE        0x00900000
#define CONKER_KERNEL_DATA_BASE  0x00892000

/* XBE image-header and section-header fields. */
#define XBE_BASE_ADDR_OFFSET             0x0104u
#define XBE_SECTION_COUNT_OFFSET         0x011Cu
#define XBE_SECTION_HEADERS_OFFSET       0x0120u
#define XBE_SECTION_HEADER_SIZE          56u
#define XBE_SECTION_VA_OFFSET            0x04u
#define XBE_SECTION_VSIZE_OFFSET         0x08u
#define XBE_SECTION_RAW_OFFSET           0x0Cu
#define XBE_SECTION_RAW_SIZE_OFFSET      0x10u
#define XBE_SECTION_NAME_OFFSET          0x14u

static HANDLE g_mapping = NULL;
static void  *g_base_ptr = NULL;

/* Top of the guest VA range the runtime reserves for the title's own
 * allocations. Everything from XBOX_MEM_SIZE up to here is address space we
 * hold so no host allocator can take it.
 *
 * 512MB was a guess and it was too low: with the frame loop actually running,
 * Conker's virtual allocations reach 0x2DCB0000. Those still succeeded --
 * outside the window NtAllocateVirtualMemory reserves normally -- but they
 * succeeded unprotected, which is the state that let the APU take
 * base + 0x04000000 in the first place.
 *
 * 0x80000000 is the real boundary, not another guess: that is where the
 * physical alias lives (and 0xA0000000 above it), so it is the top of the
 * range the title can use for ordinary allocations. Reserving to there costs
 * address space and nothing else. */
#define XBOX_GUEST_WINDOW_END 0x80000000u

/* Size of each physical-alias window. The console repeats RAM every 64MB
 * inside it, so the window is filled with that many views of the one mapping;
 * 512MB covers 0x80000000-0x9FFFFFFF and 0xA0000000-0xBFFFFFFF exactly. */
#define XBOX_ALIAS_WINDOW 0x20000000u

uint8_t *g_guest_window_base = NULL;
size_t   g_guest_window_size = 0;
static uint32_t g_heap_cursor = 0;

#define XBOX_HEAP_RESERVED_RANGE_COUNT 8
typedef struct xbox_heap_reserved_range {
    uint32_t start;
    uint32_t end;
} xbox_heap_reserved_range;

static xbox_heap_reserved_range
    g_heap_reserved_ranges[XBOX_HEAP_RESERVED_RANGE_COUNT];
static uint32_t g_heap_reserved_range_count = 0;

ptrdiff_t g_xbox_mem_offset = 0;

static void copy_section(void *base, const uint8_t *xbe_data, size_t xbe_size,
                          uint32_t va, uint32_t virt_size,
                          uint32_t raw_off, uint32_t raw_size)
{
    uint8_t *dst = (uint8_t *)base + va;
    uint32_t copy_size = (raw_size < virt_size) ? raw_size : virt_size;

    if ((size_t)raw_off + copy_size > xbe_size) {
        fprintf(stderr, "[xbox_memory] WARNING: section at VA 0x%08X "
                "reads past end of XBE file (raw_off=0x%X copy_size=0x%X "
                "xbe_size=0x%zX) - truncating\n",
                va, raw_off, copy_size, xbe_size);
        if ((size_t)raw_off >= xbe_size) return;
        copy_size = (uint32_t)(xbe_size - raw_off);
    }

    memcpy(dst, xbe_data + raw_off, copy_size);
    if (copy_size < virt_size) {
        memset(dst + copy_size, 0, virt_size - copy_size);
    }
}

/* Load every section described by the XBE instead of only the three core
 * sections.  Conker's later startup stages fetch named library sections
 * such as D3D and DSOUND through this same directory. */
static void copy_xbe_sections(void *base, const uint8_t *xbe, size_t xbe_size)
{
    uint32_t image_base;
    uint32_t section_count;
    uint32_t headers_va;
    uint32_t headers_offset;
    uint32_t loaded = 0;

    if (xbe_size < XBE_SECTION_HEADERS_OFFSET + sizeof(uint32_t))
        return;

    image_base = *(const uint32_t *)(xbe + XBE_BASE_ADDR_OFFSET);
    section_count = *(const uint32_t *)(xbe + XBE_SECTION_COUNT_OFFSET);
    headers_va = *(const uint32_t *)(xbe + XBE_SECTION_HEADERS_OFFSET);
    if (image_base != XBOX_BASE_ADDRESS || headers_va < image_base)
        return;

    headers_offset = headers_va - image_base;
    if (section_count > 64u)
        section_count = 64u;

    for (uint32_t index = 0; index < section_count; ++index) {
        const uint8_t *header;
        uint32_t section_va, virtual_size, raw_offset, raw_size, name_va;
        uint32_t copy_size;
        const char *name = "?";

        if ((size_t)headers_offset + (size_t)(index + 1u) * XBE_SECTION_HEADER_SIZE > xbe_size)
            break;

        header = xbe + headers_offset + index * XBE_SECTION_HEADER_SIZE;
        section_va = *(const uint32_t *)(header + XBE_SECTION_VA_OFFSET);
        virtual_size = *(const uint32_t *)(header + XBE_SECTION_VSIZE_OFFSET);
        raw_offset = *(const uint32_t *)(header + XBE_SECTION_RAW_OFFSET);
        raw_size = *(const uint32_t *)(header + XBE_SECTION_RAW_SIZE_OFFSET);
        name_va = *(const uint32_t *)(header + XBE_SECTION_NAME_OFFSET);

        if (name_va >= image_base && (size_t)(name_va - image_base) < xbe_size)
            name = (const char *)(xbe + (name_va - image_base));

        if (section_va >= XBOX_MEM_SIZE || virtual_size > XBOX_MEM_SIZE - section_va) {
            fprintf(stderr, "[xbox_memory] skipping invalid section %u (%s) VA=0x%08X size=0x%X\n",
                    index, name, section_va, virtual_size);
            continue;
        }

        memset((uint8_t *)base + section_va, 0, virtual_size);
        copy_size = raw_size < virtual_size ? raw_size : virtual_size;
        if (copy_size && (size_t)raw_offset + copy_size <= xbe_size)
            memcpy((uint8_t *)base + section_va, xbe + raw_offset, copy_size);

        ++loaded;
    }

    fprintf(stderr, "[xbox_memory] loaded %u/%u XBE sections from directory\n",
            loaded, section_count);
}

/* ── Kernel data exports ──────────────────────────────────────────────────
 *
 * Byte offsets into the data area at CONKER_KERNEL_DATA_BASE.  The layout
 * is private to this loader; several offsets differ from the legacy bridge's
 * KDATA_* layout in src/kernel/xbox_memory_layout.h. That
 * header's XBOX_KERNEL_DATA_BASE (0x00740000) lands inside this title's .data
 * section (0x0064F980-0x00881A64), so writing there would corrupt game state.
 * CONKER_KERNEL_DATA_BASE sits above the last section and below the stack.
 */
#define KD_HARDWARE_INFO        0x000  /* XboxHardwareInfo (8 bytes)        */
#define KD_KRNL_VERSION         0x010  /* XboxKrnlVersion (8 bytes)         */
#define KD_DISK_CACHE_PARTS     0x020  /* HalDiskCachePartitionCount (4)    */
#define KD_LAUNCH_DATA_PAGE     0x030  /* LaunchDataPage (pointer)          */
#define KD_TICK_COUNT           0x040  /* KeTickCount (4 bytes)             */
#define KD_EVENT_OBJ_TYPE       0x050  /* ExEventObjectType                 */
#define KD_THREAD_OBJ_TYPE      0x060  /* PsThreadObjectType                */
#define KD_IO_COMPLETION_TYPE   0x070  /* IoCompletionObjectType            */
#define KD_IO_DEVICE_TYPE       0x080  /* IoDeviceObjectType                */
#define KD_HD_KEY               0x100  /* XboxHDKey (16 bytes)              */
#define KD_SIGNATURE_KEY        0x110  /* XboxSignatureKey (16 bytes)       */
#define KD_LAN_KEY              0x120  /* XboxLANKey (16 bytes)             */
#define KD_ALT_SIGNATURE_KEYS   0x130  /* XboxAlternateSignatureKeys (256)  */
#define KD_XE_IMAGE_FILENAME    0x240  /* XeImageFileName (ANSI_STRING)     */
#define KD_XE_PUBLIC_KEY        0x300  /* XePublicKeyData (284 bytes)       */
#define KD_KERNEL_PE_STUB       0x500  /* synthetic kernel COFF FILE_HEADER */
#define KD_AREA_SIZE            0x600

/**
 * Xbox VA backing a kernel DATA-export ordinal, or 0 if the ordinal names a
 * function -- those need no patching, since calls through them are intercepted.
 */
static uint32_t kernel_data_va_for_ordinal(uint32_t ordinal)
{
    switch (ordinal) {
    /* 16, not 17: ordinal 17 is ExFreePool, a function the title calls with
     * one argument at 11 sites. Resolving it to this data page turned every
     * one into a failed indirect call that leaked its argument. */
    case  16: return CONKER_KERNEL_DATA_BASE + KD_EVENT_OBJ_TYPE;
    case  40: return CONKER_KERNEL_DATA_BASE + KD_DISK_CACHE_PARTS;
    case  65: return CONKER_KERNEL_DATA_BASE + KD_IO_COMPLETION_TYPE;
    case  71: return CONKER_KERNEL_DATA_BASE + KD_IO_DEVICE_TYPE;
    case 156: return CONKER_KERNEL_DATA_BASE + KD_TICK_COUNT;
    case 164: return CONKER_KERNEL_DATA_BASE + KD_LAUNCH_DATA_PAGE;
    case 259: return CONKER_KERNEL_DATA_BASE + KD_THREAD_OBJ_TYPE;
    case 322: return CONKER_KERNEL_DATA_BASE + KD_HARDWARE_INFO;
    case 323: return CONKER_KERNEL_DATA_BASE + KD_HD_KEY;
    case 324: return CONKER_KERNEL_DATA_BASE + KD_KRNL_VERSION;
    case 325: return CONKER_KERNEL_DATA_BASE + KD_SIGNATURE_KEY;
    case 326: return CONKER_KERNEL_DATA_BASE + KD_LAN_KEY;
    case 327: return CONKER_KERNEL_DATA_BASE + KD_ALT_SIGNATURE_KEYS;
    case 328: return CONKER_KERNEL_DATA_BASE + KD_XE_IMAGE_FILENAME;
    case 355: return CONKER_KERNEL_DATA_BASE + KD_LAN_KEY;            /* alias */
    case 356: return CONKER_KERNEL_DATA_BASE + KD_ALT_SIGNATURE_KEYS; /* alias */
    case 357: return CONKER_KERNEL_DATA_BASE + KD_XE_PUBLIC_KEY;
    default:  return 0;
    }
}

/** Give every data export a plausible retail value before anything reads it. */
static void kernel_data_area_init(void)
{
    memset((uint8_t *)g_base_ptr + CONKER_KERNEL_DATA_BASE, 0, KD_AREA_SIZE);

    /* XboxHardwareInfo: Flags=0 (retail), GpuRevision=A1, McpRevision=B1. */
    MEM32(CONKER_KERNEL_DATA_BASE + KD_HARDWARE_INFO + 0) = 0;
    MEM8 (CONKER_KERNEL_DATA_BASE + KD_HARDWARE_INFO + 4) = 0xA1;
    MEM8 (CONKER_KERNEL_DATA_BASE + KD_HARDWARE_INFO + 5) = 0xB1;

    /* XboxKrnlVersion 1.0.5849.0, matching this title's XDK libraries. */
    MEM32(CONKER_KERNEL_DATA_BASE + KD_KRNL_VERSION + 0) = 0x00000001;
    MEM32(CONKER_KERNEL_DATA_BASE + KD_KRNL_VERSION + 4) = 0x000016D9;

    /* Retail exposes the three cache partitions X:, Y: and Z:. */
    MEM32(CONKER_KERNEL_DATA_BASE + KD_DISK_CACHE_PARTS) = 3;

    /* No launch data: this title was started directly, not chained into. */
    MEM32(CONKER_KERNEL_DATA_BASE + KD_LAUNCH_DATA_PAGE) = 0;

    MEM32(CONKER_KERNEL_DATA_BASE + KD_TICK_COUNT) = GetTickCount();

    /* Key material stays zeroed: this build verifies no signatures, and real
     * per-console keys are neither available nor wanted here. */

    /*
     * A synthetic, empty kernel COFF header for sub_004306DE.
     *
     * That routine reads [0x8001003C] -- the e_lfanew of the Xbox kernel image
     * mapped at 0x80010000 -- subtracts 0x7FFF0000, and walks the COFF header
     * it lands on, looking for a section named "INIT":
     *
     *     hdr    = MEM32(0x8001003C) - 0x7FFF0000
     *     count  = MEM16(hdr + 6)      // NumberOfSections
     *     opt    = MEM16(hdr + 0x14)   // SizeOfOptionalHeader
     *     probe  = hdr + opt + count * 40 - 16
     *     if (MEM32(probe) == 'INIT') ...
     *
     * There is no kernel image here, so point it at a header declaring zero
     * sections.  The probe then lands 16 bytes below the header, inside this
     * zeroed area, reads 0 instead of 'INIT', and the routine takes its own
     * designed "not found" path.  Nothing is fabricated beyond an empty
     * header, and no address outside the mapping is ever formed.
     *
     * This replaces a hand-placed page at 0x80010000 holding a self-pointer.
     * That page could not survive the physical-memory aliasing below, since
     * 0x80010000 now legitimately aliases the XBE header at 0x00010000.
     */
    MEM32(CONKER_KERNEL_DATA_BASE + KD_KERNEL_PE_STUB + 0x06) = 0; /* sections */
    MEM32(CONKER_KERNEL_DATA_BASE + KD_KERNEL_PE_STUB + 0x14) = 0; /* opt size */

    /* 0x8001003C aliases 0x0001003C, which is inside the XBE's 256-byte
     * digital signature (0x0004..0x0104).  Nothing reads that at runtime, so
     * it is free to carry the pointer this scan needs. */
    MEM32(0x0001003Cu) = 0x7FFF0000u
                       + (uint32_t)(CONKER_KERNEL_DATA_BASE + KD_KERNEL_PE_STUB);
}

/**
 * Patch every data-export slot in the title's kernel import thunk table.
 *
 * The table VA lives in the XBE header at 0x0158, XOR-encrypted with a key that
 * differs between retail and debug builds, with no flag saying which was used.
 * Decode with both and keep whichever lands in mapped memory.
 */
static void resolve_kernel_data_exports(const uint8_t *xbe, size_t xbe_size)
{
    if (xbe_size < 0x015Cu) {
        fprintf(stderr, "[kdata] XBE too small to hold a thunk address\n");
        return;
    }

    uint32_t raw    = *(const uint32_t *)(xbe + 0x0158);
    uint32_t retail = raw ^ 0x5B6D40B6u;   /* retail XOR key */
    uint32_t debugk = raw ^ 0xEFB1F152u;   /* debug XOR key  */
    uint32_t base   = (retail >= 0x00010000u && retail < XBOX_MEM_SIZE)
                      ? retail : debugk;

    if (base < 0x00010000u || base >= XBOX_MEM_SIZE) {
        fprintf(stderr, "[kdata] thunk table VA 0x%08X out of range "
                "(raw=0x%08X) - data exports left unresolved\n", base, raw);
        return;
    }

    unsigned patched = 0, slots = 0;
    for (uint32_t i = 0; i < 512u; i++) {
        uint32_t slot = base + i * 4u;
        uint32_t cur  = MEM32(slot);
        if (cur == 0)
            break;
        slots++;
        if ((cur & 0x80000000u) == 0)
            continue;               /* already resolved, not a placeholder */
        uint32_t va = kernel_data_va_for_ordinal(cur & 0x7FFFFFFFu);
        if (!va)
            continue;               /* function import: leave for RECOMP_ICALL */
        MEM32(slot) = va;
        patched++;
    }

    fprintf(stderr, "[kdata] thunk table 0x%08X: %u slots, %u data exports "
            "resolved into 0x%08X\n", base, slots, patched,
            (uint32_t)CONKER_KERNEL_DATA_BASE);
}

BOOL xbox_memory_init(const void *xbe_data, size_t xbe_size)
{
    /* Total reserved region: 0x0 .. 64MB, so low-memory TIB reads at
     * 0x20/0x28 and everything else lands in the same mapping. */
    size_t total_size = XBOX_MEM_SIZE;

    g_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                    PAGE_READWRITE, 0, (DWORD)total_size, NULL);
    if (!g_mapping) {
        fprintf(stderr, "[xbox_memory] CreateFileMapping failed: %lu\n", GetLastError());
        return FALSE;
    }

    /* Place the 64MB view so that the guest window ABOVE it is also free.
     *
     * Letting the OS pick the view address and then hoping the next 2GB
     * happened to be free is what made startup crash intermittently: about
     * one run in three, Windows put another file mapping immediately after
     * ours, the guest-window reservation failed, and guest VA 0x04000000 --
     * where Conker's startup heap lives -- was never committable.  The title
     * wrote there anyway and took an access violation at 0x04000280.
     *
     * The two placements are one decision, so they are made together: probe
     * for a hole big enough for BOTH, drop the probe, and immediately claim
     * it as the view plus the window.  If anything slips into the gap the
     * whole attempt is unwound and retried, so a lost race costs an iteration
     * rather than a corrupted layout.
     *
     * The identity/low addresses are still tried first, since g_xbox_mem_offset
     * of zero makes every guest pointer its own host pointer -- but only if
     * the window above them can be reserved too. */
    {
        const size_t window_size = XBOX_GUEST_WINDOW_END - XBOX_MEM_SIZE;
        const size_t span = (size_t)XBOX_GUEST_WINDOW_END;
        void *desired[] = { (void *)0x00000000, (void *)0x10000000 };
        unsigned attempt;

        for (unsigned i = 0; i < sizeof(desired) / sizeof(desired[0]); ++i) {
            void *view = MapViewOfFileEx(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                         total_size, desired[i]);
            if (!view)
                continue;
            if (VirtualAlloc((uint8_t *)view + XBOX_MEM_SIZE, window_size,
                             MEM_RESERVE, PAGE_NOACCESS)) {
                g_base_ptr = view;
                g_guest_window_base = (uint8_t *)view + XBOX_MEM_SIZE;
                g_guest_window_size = window_size;
                break;
            }
            UnmapViewOfFile(view);      /* no room above it; keep looking */
        }

        for (attempt = 0; !g_base_ptr && attempt < 16u; ++attempt) {
            /* One reservation proves the whole span is free and contiguous. */
            void *probe = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
            void *view;
            if (!probe)
                break;
            VirtualFree(probe, 0, MEM_RELEASE);

            view = MapViewOfFileEx(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                   total_size, probe);
            if (!view)
                continue;             /* someone took it; try again */
            if (!VirtualAlloc((uint8_t *)view + XBOX_MEM_SIZE, window_size,
                              MEM_RESERVE, PAGE_NOACCESS)) {
                UnmapViewOfFile(view);
                continue;
            }
            g_base_ptr = view;
            g_guest_window_base = (uint8_t *)view + XBOX_MEM_SIZE;
            g_guest_window_size = window_size;
            fprintf(stderr, "[xbox_memory] placed view and guest window "
                    "together on attempt %u\n", attempt + 1u);
        }

        if (!g_base_ptr) {
            /* Last resort: map anywhere.  The window reservation below will
             * report if it could not follow, which is the state that caused
             * the 0x04000280 crash. */
            fprintf(stderr, "[xbox_memory] WARNING: could not place the view "
                    "with its guest window; falling back\n");
            g_base_ptr = MapViewOfFileEx(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                         total_size, NULL);
        }
    }
    if (!g_base_ptr) {
        fprintf(stderr, "[xbox_memory] MapViewOfFileEx failed: %lu\n", GetLastError());
        CloseHandle(g_mapping);
        g_mapping = NULL;
        return FALSE;
    }

    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)g_base_ptr - 0x00000000);
    fprintf(stderr, "[xbox_memory] mapped at %p, offset=0x%tX\n",
            g_base_ptr, g_xbox_mem_offset);

    /* Reserve the guest address space above the 64MB mapping.
     *
     * Guest VAs past 64MB are not part of the file mapping: the title
     * reserves them itself (its startup heap sits at 0x04000000) and the
     * runtime satisfies that with a host allocation at base + VA. That only
     * works while the host address space right after the mapping is free,
     * and nothing was keeping it free -- it simply happened to be, because
     * little else allocated before the title ran.
     *
     * Wiring up the APU broke exactly that: its state block landed at
     * base + 0x04000000, the heap reservation came back ERROR_INVALID_ADDRESS,
     * and the title carried on to write at guest 0x08000000. Claim the window
     * here so the layout no longer depends on host allocation order.
     *
     * 512MB is far past anything a 64MB retail title asks for (Conker reaches
     * 0x08000000) and stays well clear of the physical aliases at 0x80000000
     * and 0xA0000000, which get their own views. Address space only -- no
     * pages are committed until the title asks. */
    if (g_guest_window_base) {
        /* Already claimed together with the view above. */
        fprintf(stderr, "[xbox_memory] guest window reserved %p..%p "
                "(guest 0x%08X..0x%08X)\n",
                g_guest_window_base,
                g_guest_window_base + g_guest_window_size,
                (unsigned)XBOX_MEM_SIZE, (unsigned)XBOX_GUEST_WINDOW_END);
    } else if (g_guest_window_base = (uint8_t *)g_base_ptr + XBOX_MEM_SIZE,
               g_guest_window_size = XBOX_GUEST_WINDOW_END - XBOX_MEM_SIZE,
               !VirtualAlloc(g_guest_window_base, g_guest_window_size,
                             MEM_RESERVE, PAGE_NOACCESS)) {
        fprintf(stderr, "[xbox_memory] WARNING: could not reserve the guest\n"
                "               window at %p (%lu); guest allocations above\n"
                "               64MB may collide with host allocations\n",
                g_guest_window_base, GetLastError());
        g_guest_window_base = NULL;
        g_guest_window_size = 0;
    } else {
        fprintf(stderr, "[xbox_memory] guest window reserved %p..%p "
                "(guest 0x%08X..0x%08X)\n",
                g_guest_window_base,
                g_guest_window_base + g_guest_window_size,
                (unsigned)XBOX_MEM_SIZE, (unsigned)XBOX_GUEST_WINDOW_END);
    }

    const uint8_t *xbe = (const uint8_t *)xbe_data;

    /* The Xbox loader maps the image header at the title's image base.  In
     * addition to certificates and import metadata, it contains the section
     * directory used by the title's named-section loader. */
    if (xbe_size >= 0x10Cu) {
        uint32_t header_size = *(const uint32_t *)(xbe + 0x108);
        if (header_size == 0 || header_size > 0x10000u)
            header_size = 0x1000u;
        if (header_size > xbe_size)
            header_size = (uint32_t)xbe_size;

        memcpy((uint8_t *)g_base_ptr + 0x00010000u, xbe, header_size);
        fprintf(stderr, "[xbox_memory] XBE header mapped at 0x00010000 (%u bytes)\n",
                header_size);
    }

    copy_xbe_sections(g_base_ptr, xbe, xbe_size);

    /* Emulated thread block, addressed through fs: by the recompiled code.
     *
     * This used to sit at guest VA 0, because the lifter dropped the fs:
     * prefix and turned fs:[N] into a plain read of address N. VA 0 is not
     * spare: it is the first page of Xbox RAM, and this title's XPP pool
     * allocator carves blocks out of its physical alias (0x80000020 upward)
     * and fills each with 0xCC. It duly allocated over this block, and the
     * title then dispatched through the 0xCCCCCCCC it read back from
     * [fs:[0x20] + 0x250].
     *
     * The lifter now relocates fs: to RECOMP_FS_BASE (see recomp_types.h), so
     * the block sits on a page no guest allocator can reach. */
    MEM32(RECOMP_FS_BASE + 0x00) = 0xFFFFFFFF;                 /* SEH end of chain */
    MEM32(RECOMP_FS_BASE + 0x04) = CONKER_STACK_BASE + XBOX_STACK_SIZE - 16;
    MEM32(RECOMP_FS_BASE + 0x08) = CONKER_STACK_BASE;          /* stack limit */
    MEM32(RECOMP_FS_BASE + 0x18) = RECOMP_FS_BASE;             /* self pointer */
    /* fs:[0x20] is the KPCR Prcb pointer. Eight sites read [Prcb + 0x250] --
     * a D3D cache structure -- and every one of them just tests the result
     * against zero, so a zeroed Prcb keeps the title on the "no cache" path,
     * which is what this runtime wants, having no such cache.
     *
     * It must be a real zeroed page, not NULL. A null Prcb makes the title
     * read guest 0x250 instead, and that address belongs to the XPP pool
     * described above: by the time these run it holds 0xCC filler, the test
     * against zero passes, and the title dispatches through 0xCCCCCCCC. */
    MEM32(RECOMP_FS_BASE + 0x20) = RECOMP_FS_PRCB;

    g_esp = CONKER_STACK_BASE + XBOX_STACK_SIZE - 16;
    g_heap_cursor = CONKER_STACK_BASE + XBOX_STACK_SIZE;
    g_heap_reserved_range_count = 0;

    /* ---- Kernel data-export resolution ----
     *
     * The kernel thunk table holds BOTH function-import and data-export
     * placeholders side by side, each written as 0x80000000|ordinal.  Function
     * slots can be left alone: the recompiled code calls through them and
     * RECOMP_ICALL intercepts the synthetic VA.  Data slots are never called --
     * the game just dereferences them with a plain read -- so interception
     * cannot catch them and they must be patched up front, as a real loader
     * would.
     *
     * This used to be three hand-written patches, each added after the crash it
     * caused (XboxHardwareInfo, XboxKrnlVersion, HalDiskCachePartitionCount),
     * which only ever fixed the export that had already crashed.  Ordinal 164
     * (LaunchDataPage) then took down the CRT thread-start routine as soon as
     * the disassembler extent fix made that path reachable.  Resolve every data
     * export in one pass instead, so the next one does not need a crash first.
     */
    kernel_data_area_init();
    resolve_kernel_data_exports(xbe, xbe_size);
    if (!recomp_clock_start((volatile uint32_t *)
            ((uint8_t *)g_base_ptr + CONKER_KERNEL_DATA_BASE + KD_TICK_COUNT))) {
        fprintf(stderr, "[xbox_memory] failed to start kernel clock\n");
        return FALSE;
    }

    /* ---- Physical-memory aliases ----
     *
     * The Xbox maps its 64MB of RAM three times: cached at 0x00000000, again
     * at 0x80000000, and uncached at 0xA0000000.  Address 0x80000000+N and
     * 0xA0000000+N are the same bytes as N.  Drivers use the high windows
     * deliberately -- the D3D8 layer writes 0x80000000 directly before a
     * wbinvd (sub_0053E5D0 at 0x0053E5D0+0x6BF5), which faulted here because
     * only the low window was backed.
     *
     * Mapping a second and third view of the same file mapping reproduces the
     * aliasing exactly, and needs no change to MEM32: the macro adds
     * g_xbox_mem_offset, so Xbox VA 0x80000000+N lands at
     * g_base_ptr + 0x80000000 + N, which is offset N of the second view.
     *
     * This replaces a single hand-placed page at 0x80010000 that existed only
     * so sub_004306DE could read [0x8001003C] without faulting.  Under real
     * aliasing that address is the XBE header at 0x0001003C, whose dword is
     * 0xFB819780 -- not the "INIT" magic the scan looks for -- so the check
     * still fails and the routine still takes its designed skip path, now for
     * the right reason rather than because of a fabricated page.
     */
    /* Each window repeats every 64MB, because the memory controller has a
     * 26-bit address bus: physical 0x8513D121 and 0x8113D121 are the same
     * byte. One view per window was not enough -- sub_002B45F0 read
     * 0x8513D121, 81MB into a 64MB view, and faulted.
     *
     * This is where the "mirrors" belong, and only here. docs/technical/
     * memory-layout.md describes mirroring the whole address space at 64MB
     * intervals, which would put a mirror over 0x04000000 -- and that is the
     * title's own heap: it reserves 1MB there through NtAllocateVirtualMemory
     * and commits pages inside it. A mirror there would alias the heap onto
     * low RAM and corrupt both. The wrap is a property of *physical*
     * addressing, so it applies to the physical windows and not to the
     * virtual addresses the kernel hands out.
     *
     * XBOX_ALIAS_WINDOW covers 0x80000000-0x9FFFFFFF and 0xA0000000-0xBFFFFFFF
     * completely, so any address in either window resolves rather than
     * depending on how far past the base the title happens to reach. */
    /* 0xF0000000 is the third alias, and it is not one of the physical
     * windows: it is the aperture the title uses for GPU surfaces.  It
     * aliases RAM byte for byte -- the NV2A tiles through its own tile
     * registers when it reads, not through this aperture -- so the rule is
     * simply 0xF0000000 + X -> X.
     *
     * It gets exactly one view, of RAM size, rather than the 64MB-wrapped
     * window the physical aliases get.  A console maps it as an alias region
     * of exactly ram_size, and wrapping it further would run over the NV2A,
     * APU and ACI register spaces at 0xFD000000 and above, turning modelled
     * MMIO into silent RAM.
     *
     * Measured before adding it: unmapped, the title faulted on this aperture
     * in about half of all runs and the guest thread died in the loading
     * screen.  The addresses moved run to run -- 0xF1D80000 in one,
     * 0xF34DF218..0xF3505E18 in others -- because they are whatever surface
     * the allocator handed out, which is why nothing here keys off an
     * address.  Every underlying address seen was below XBOX_MEM_SIZE. */
    {
        static const struct { uint32_t base, window; } alias_map[] = {
            { 0x80000000u, XBOX_ALIAS_WINDOW },  /* physical, cached   */
            { 0xA0000000u, XBOX_ALIAS_WINDOW },  /* physical, uncached */
            { 0xF0000000u, XBOX_MEM_SIZE     },  /* GPU aperture       */
        };
        unsigned i, v, mapped = 0, failed = 0;

        for (i = 0; i < sizeof(alias_map) / sizeof(alias_map[0]); i++) {
            const uint32_t views = alias_map[i].window / XBOX_MEM_SIZE;
            for (v = 0; v < views; v++) {
                uint32_t va = alias_map[i].base + v * XBOX_MEM_SIZE;
                void *want = (void *)((uintptr_t)g_base_ptr + va);
                void *got = MapViewOfFileEx(g_mapping, FILE_MAP_ALL_ACCESS,
                                            0, 0, XBOX_MEM_SIZE, want);
                if (got == want) {
                    mapped++;
                } else {
                    if (got)
                        UnmapViewOfFile(got);
                    failed++;
                    if (failed <= 4)
                        fprintf(stderr, "[xbox_memory] WARNING: could not "
                                "alias 0x%08X at %p (error %lu); accesses "
                                "through it will fault\n",
                                va, want, GetLastError());
                }
            }
            fprintf(stderr, "[xbox_memory] alias 0x%08X..0x%08X "
                    "(%u x %u MB views)\n",
                    alias_map[i].base,
                    alias_map[i].base + alias_map[i].window - 1u,
                    views, (unsigned)(XBOX_MEM_SIZE / (1024 * 1024)));
        }
        if (failed)
            fprintf(stderr, "[xbox_memory] %u of %u alias views failed\n",
                    failed, mapped + failed);
    }

    return TRUE;
}

void xbox_memory_shutdown(void)
{
    recomp_clock_stop();
    if (g_base_ptr) { UnmapViewOfFile(g_base_ptr); g_base_ptr = NULL; }
    if (g_mapping)  { CloseHandle(g_mapping); g_mapping = NULL; }
}

BOOL xbox_is_xbox_address(uintptr_t address)
{
    return address < XBOX_MEM_SIZE;
}

void *xbox_get_memory_base(void) { return g_base_ptr; }
ptrdiff_t xbox_get_memory_offset(void) { return g_xbox_mem_offset; }
HANDLE xbox_get_mapping_handle(void) { return g_mapping; }

/* ================================================================
 * Guest heap recycling
 *
 * This was a bump allocator with no free path: g_heap_cursor only moved
 * forward and xbox_heap_free() was a no-op.  Conker outlives that.  It streams
 * audio banks and packages continuously, and once the cursor reaches the top of
 * the 64 MB guest space every further request fails -- including the ~1 MB
 * buffer it wants for the boot video, which is where the title stopped.  Four
 * allocations failed that way in a three-minute run.
 *
 * So: remember the size of every live allocation, and put a freed block back on
 * a free list that coalesces with its neighbours.  Allocation takes the first
 * free block that fits and splits the remainder; only when none does is the
 * cursor advanced.  A block freed at the very top rolls the cursor back
 * instead, which keeps a stream of same-sized allocations from walking upward.
 *
 * Host-side bookkeeping, so it is ordinary malloc/realloc.  Nothing here is
 * visible in guest address space.
 * ================================================================ */

typedef struct { uint32_t start; uint32_t size; } xbox_heap_block;

static xbox_heap_block *g_heap_live;
static uint32_t g_heap_live_count, g_heap_live_cap;
static xbox_heap_block *g_heap_hole;
static uint32_t g_heap_hole_count, g_heap_hole_cap;
static uint64_t g_heap_reused_bytes;
static uint32_t g_heap_reused_count;

static int heap_vec_room(xbox_heap_block **vec, uint32_t *cap, uint32_t need)
{
    xbox_heap_block *grown;
    uint32_t want;

    if (need <= *cap)
        return 1;
    want = *cap ? *cap * 2u : 256u;
    while (want < need)
        want *= 2u;
    grown = (xbox_heap_block *)realloc(*vec, (size_t)want * sizeof(**vec));
    if (!grown)
        return 0;
    *vec = grown;
    *cap = want;
    return 1;
}

/* Insert a block, keeping the list sorted by address and merging neighbours. */
static void heap_hole_insert(uint32_t start, uint32_t size)
{
    uint32_t i, at;

    if (size == 0u)
        return;
    if (!heap_vec_room(&g_heap_hole, &g_heap_hole_cap, g_heap_hole_count + 1u))
        return;   /* the block is simply lost; never corrupt the list */

    for (at = 0u; at < g_heap_hole_count; ++at)
        if (g_heap_hole[at].start > start)
            break;
    for (i = g_heap_hole_count; i > at; --i)
        g_heap_hole[i] = g_heap_hole[i - 1u];
    g_heap_hole[at].start = start;
    g_heap_hole[at].size = size;
    ++g_heap_hole_count;

    /* Merge forward, then backward. */
    if (at + 1u < g_heap_hole_count &&
        g_heap_hole[at].start + g_heap_hole[at].size ==
            g_heap_hole[at + 1u].start) {
        g_heap_hole[at].size += g_heap_hole[at + 1u].size;
        for (i = at + 1u; i + 1u < g_heap_hole_count; ++i)
            g_heap_hole[i] = g_heap_hole[i + 1u];
        --g_heap_hole_count;
    }
    if (at > 0u &&
        g_heap_hole[at - 1u].start + g_heap_hole[at - 1u].size ==
            g_heap_hole[at].start) {
        g_heap_hole[at - 1u].size += g_heap_hole[at].size;
        for (i = at; i + 1u < g_heap_hole_count; ++i)
            g_heap_hole[i] = g_heap_hole[i + 1u];
        --g_heap_hole_count;
    }
}

/* Does [start,start+size) overlap a range the title expects to stay put? */
static int heap_range_is_reserved(uint32_t start, uint32_t size)
{
    uint32_t i;
    for (i = 0u; i < g_heap_reserved_range_count; ++i)
        if (start < g_heap_reserved_ranges[i].end &&
            start + size > g_heap_reserved_ranges[i].start)
            return 1;
    return 0;
}

/* First free block that can satisfy size at alignment, split if larger. */
static uint32_t heap_hole_take(uint32_t size, uint32_t alignment)
{
    uint32_t i;

    for (i = 0u; i < g_heap_hole_count; ++i) {
        uint32_t start = g_heap_hole[i].start;
        uint32_t end = start + g_heap_hole[i].size;
        uint32_t aligned = (start + (alignment - 1u)) & ~(alignment - 1u);

        if (aligned < start || aligned > end || size > end - aligned)
            continue;
        if (heap_range_is_reserved(aligned, size))
            continue;

        {
            uint32_t head = aligned - start;
            uint32_t tail = end - (aligned + size);
            uint32_t j;

            for (j = i; j + 1u < g_heap_hole_count; ++j)
                g_heap_hole[j] = g_heap_hole[j + 1u];
            --g_heap_hole_count;

            if (head)
                heap_hole_insert(start, head);
            if (tail)
                heap_hole_insert(aligned + size, tail);
        }
        ++g_heap_reused_count;
        g_heap_reused_bytes += size;
        return aligned;
    }
    return 0u;
}

static void heap_live_record(uint32_t start, uint32_t size)
{
    if (!heap_vec_room(&g_heap_live, &g_heap_live_cap, g_heap_live_count + 1u))
        return;
    g_heap_live[g_heap_live_count].start = start;
    g_heap_live[g_heap_live_count].size = size;
    ++g_heap_live_count;
}

/*
 * Size of a live guest allocation, or 0 when the address is not one.
 *
 * MmQueryAllocationSize answers this on hardware.  The host-side
 * implementation asks VirtualQuery, which describes the whole 64 MB guest
 * mapping rather than the block the title allocated, so the live table is the
 * only thing here that knows the real size.
 */
uint32_t xbox_heap_size_of(uint32_t va)
{
    uint32_t i;

    for (i = 0u; i < g_heap_live_count; ++i)
        if (g_heap_live[i].start == va)
            return g_heap_live[i].size;
    return 0u;
}

uint32_t xbox_heap_alloc(uint32_t size, uint32_t alignment)
{
    uint32_t aligned;
    uint32_t end;
    int moved;

    if (alignment < 4) alignment = 4;
    if (size == 0u) size = 1u;

    /* A recycled block before fresh address space, so a title that allocates
     * and frees in a steady rhythm stays in the same region instead of walking
     * to the top of guest memory and stopping there. */
    {
        uint32_t reused = heap_hole_take(size, alignment);
        if (reused) {
            heap_live_record(reused, size);
            return reused;
        }
    }

    aligned = (g_heap_cursor + (alignment - 1)) & ~(alignment - 1);

    /* Captured retail objects occupy fixed guest addresses.  Preserve those
     * islands without discarding the otherwise-free heap below them. */
    do {
        moved = 0;
        if (size > XBOX_MEM_SIZE - aligned) {
            end = XBOX_MEM_SIZE;
        } else {
            end = aligned + size;
        }
        for (uint32_t index = 0; index < g_heap_reserved_range_count;
             ++index) {
            const xbox_heap_reserved_range *range =
                &g_heap_reserved_ranges[index];
            if (aligned < range->end && end > range->start) {
                aligned = (range->end + (alignment - 1)) &
                          ~(alignment - 1);
                moved = 1;
                break;
            }
        }
    } while (moved);

    if (size > XBOX_MEM_SIZE - aligned || aligned + size >= XBOX_MEM_SIZE) {
        xbox_heap_report();
        fprintf(stderr, "[xbox_memory] heap exhausted (requested %u at 0x%08X)\n",
                size, aligned);
        return 0;
    }
    g_heap_cursor = aligned + size;
    heap_live_record(aligned, size);
    return aligned;
}

void xbox_heap_reserve_range(uint32_t start_va, uint32_t end_va)
{
    xbox_heap_reserved_range *range;

    if (start_va >= XBOX_MEM_SIZE || start_va >= end_va)
        return;
    if (end_va > XBOX_MEM_SIZE)
        end_va = XBOX_MEM_SIZE;

    /* Repeated restores may register an identical range. */
    for (uint32_t index = 0; index < g_heap_reserved_range_count; ++index) {
        range = &g_heap_reserved_ranges[index];
        if (range->start == start_va && range->end == end_va)
            return;
    }
    if (g_heap_reserved_range_count >= XBOX_HEAP_RESERVED_RANGE_COUNT) {
        fprintf(stderr,
                "[xbox_memory] too many fixed heap reservations; "
                "reserving through 0x%08X instead\n",
                end_va);
        xbox_heap_reserve_through(end_va);
        return;
    }

    range = &g_heap_reserved_ranges[g_heap_reserved_range_count++];
    range->start = start_va;
    range->end = end_va;
    fprintf(stderr,
            "[xbox_memory] reserved fixed heap island 0x%08X..0x%08X "
            "(heap is 0x%08X)\n",
            start_va, end_va, g_heap_cursor);

    if (g_heap_cursor >= start_va && g_heap_cursor < end_va)
        g_heap_cursor = end_va;
}

void xbox_heap_reserve_through(uint32_t end_va)
{
    if (end_va > XBOX_MEM_SIZE)
        end_va = XBOX_MEM_SIZE;
    if (g_heap_cursor < end_va) {
        fprintf(stderr,
                "[xbox_memory] reserving restored range through 0x%08X "
                "(heap was 0x%08X)\n",
                end_va, g_heap_cursor);
        g_heap_cursor = end_va;
    }
}

void xbox_heap_free(uint32_t xbox_va)
{
    uint32_t i;

    if (xbox_va == 0u)
        return;

    for (i = 0u; i < g_heap_live_count; ++i) {
        if (g_heap_live[i].start != xbox_va)
            continue;
        {
            uint32_t size = g_heap_live[i].size;
            uint32_t j;

            for (j = i; j + 1u < g_heap_live_count; ++j)
                g_heap_live[j] = g_heap_live[j + 1u];
            --g_heap_live_count;

            /* A block at the very top gives its address space straight back to
             * the cursor; anything else joins the free list. */
            if (xbox_va + size == g_heap_cursor)
                g_heap_cursor = xbox_va;
            else
                heap_hole_insert(xbox_va, size);
        }
        return;
    }

    /* Not one of ours -- a fixed address the title placed itself, or already
     * freed.  Saying so once is worth more than silently doing nothing. */
    { static unsigned logged;
      if (logged++ < 8u)
          fprintf(stderr, "[xbox_memory] free of untracked 0x%08X\n",
                  xbox_va); }
}

void xbox_heap_report(void)
{
    uint64_t held = 0, free_bytes = 0;
    uint32_t i;

    for (i = 0u; i < g_heap_live_count; ++i) held += g_heap_live[i].size;
    for (i = 0u; i < g_heap_hole_count; ++i) free_bytes += g_heap_hole[i].size;
    fprintf(stderr,
            "[xbox_memory] heap cursor=0x%08X live=%u (%llu KiB) "
            "holes=%u (%llu KiB) reused=%u (%llu KiB)\n",
            g_heap_cursor, g_heap_live_count,
            (unsigned long long)(held / 1024u),
            g_heap_hole_count, (unsigned long long)(free_bytes / 1024u),
            g_heap_reused_count,
            (unsigned long long)(g_heap_reused_bytes / 1024u));
}
