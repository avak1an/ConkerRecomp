/*
 * NV2A GPU Core - Standalone register handlers
 *
 * Adapted from xemu (Copyright (c) 2012 espes, 2015 Jannik Vogel,
 * 2018-2025 Matt Borgerson) - LGPL v2+
 *
 * Contains: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC register handlers,
 * nv2a_update_irq, DMA helpers, block dispatch table, and standalone init.
 */

#include "nv2a_state.h"
/* getenv returns char*.  Without this header C treats it as returning
 * int, which on x64 truncates the pointer to 32 bits and sign-extends
 * it -- a wild address that only faults once something dereferences
 * the result rather than just testing it against NULL. */
#include <stdlib.h>
#include <windows.h>
#include <dbghelp.h>
extern ptrdiff_t g_xbox_mem_offset;
#include "nv2a_pgraph_d3d11.h"
#include "nv2a_pushbuffer.h"

/* ============================================================
 * Global state
 * ============================================================ */

static NV2AState *g_nv2a = NULL;
static MemoryRegion g_vram_region;
static MemoryRegion g_ramin_region;
/* The synchronous FIFO wait must wake when the guest can retire a flip.
 * Sleep(1) takes about 15.6 ms on a default-resolution Windows timer and
 * can both miss a vblank and sleep again after retirement has completed. */
static HANDLE g_nv2a_vblank_event;

NV2AState *nv2a_get_state(void) {
    return g_nv2a;
}

/* Where the display is scanning out from, or 0 before the title sets it. */
uint32_t nv2a_display_scanout_start(void)
{
    return g_nv2a ? (uint32_t)g_nv2a->pcrtc.start : 0u;
}

/* ============================================================
 * IRQ aggregation (from xemu nv2a.c)
 * ============================================================ */

/*
 * Vertical blank.
 *
 * The Xbox display raises this sixty times a second and the title services it
 * through the routine it connects with KeConnectInterrupt.  Nothing here ever
 * raised it, and pci_irq_assert() is a no-op, so the routine had never run --
 * which is why the title arms the interrupt during display init and then
 * waits forever for anything driven off it.
 *
 * On by default.  This is not an optional embellishment: the display field
 * counter at [dev+0x1DE8] only advances while the reported field parity
 * alternates -- sub_0053D4D0 gates its increment on exactly that -- and the
 * XMV decoder paces itself off that counter.  With no vblank the counter is
 * frozen, the decoder's packet deadline at [decoder+0xF8] is never reached,
 * and video decode stops after the first packet.  Measured: frozen at 1 for
 * all 1192 decoder calls in a run, against 28..1359 with vblank running.
 *
 * CONKER_NO_VBLANK=1 turns it off again, for isolating whether a fault is
 * vblank-related.  That is a diagnostic, not a supported mode.
 */
#define NV2A_GPU_INTERRUPT_VECTOR 51u   /* Xbox IRQ 3 */

extern int recomp_deliver_interrupt(uint32_t vector);

#include "../platform/hr_period.h"

static int nv2a_vblank_enabled(void)
{
    static int checked, enabled;

    if (!checked) {
        checked = 1;
        /* Default on; CONKER_NO_VBLANK is the diagnostic escape hatch.
         * CONKER_VBLANK is still accepted so existing scripts and the
         * notes that mention it keep working, but it is now redundant. */
        enabled = getenv("CONKER_NO_VBLANK") == NULL;
    }
    return enabled;
}

extern volatile int g_recomp_irq_pending;
extern void recomp_irq_raise(unsigned vector);
extern void recomp_safe_point(void);

/*
 * The display interrupt, raised on its own cadence.
 *
 * Raising is deliberately separate from delivery, and on its own thread.  The
 * pending bit is set here sixty times a second whatever the guest is doing;
 * delivery happens later, at a safe point the lifter emitted, where the lifted
 * register state is coherent.  Tying the two together is what made a
 * compute-bound guest impossible to interrupt.
 */
/*
 * Port I/O.
 *
 * The lifter used to drop "in" and "out" with only a comment, which left the
 * destination register holding whatever it happened to hold.  That is not a
 * harmless gap: Conker's vblank DPC at 0x00540F70 does
 *
 *     in   al, 0x80C0
 *     shr  eax, 5
 *     not  eax
 *     and  eax, ecx
 *     mov  [esi+0x1D0], eax     ; the flip-pending flag
 *
 * so pending = ~(port >> 5) & ecx.  With the read missing, eax stayed 1 and
 * the DPC wrote "flip pending" every time -- 1,295 sets against 1 clear in a
 * 40 second run -- and the XMV decoder, which will not resample its clock
 * while a flip is pending, never decoded a frame of the boot video.
 *
 * Sampled at 0x00540FEE on a console during that video, the port alternates
 * exactly: 0x20, 0x00, 0x20, 0x00, 90 of each in 180 consecutive DPC entries.
 * Bit 5 is the interlaced field parity -- NTSC 480i has two fields per frame,
 * the flip lands on one of them -- so it toggles once per vertical blank and
 * pending clears every other field.  Every other bit read back zero.
 *
 * Unknown ports read back zero.  That is a guess, but a defined one, and it
 * beats a stale register: 114 of these sites exist across the title and this
 * is the first proven to matter.
 */
unsigned g_nv2a_field;

uint32_t recomp_port_in(uint16_t port, unsigned size)
{
    uint32_t v = 0;

    switch (port) {
    case 0x80C0:
        /* Bit 5 = field parity, toggled by the vblank raiser. */
        v = g_nv2a_field ? 0x20u : 0x00u;
        break;
    default:
        v = 0;
        break;
    }

    if (size == 1) return v & 0xFFu;
    if (size == 2) return v & 0xFFFFu;
    return v;
}

void recomp_port_out(uint16_t port, unsigned size, uint32_t value)
{
    (void)port; (void)size; (void)value;
}

unsigned long long g_vbl_ticks, g_vbl_qpc0, g_vbl_qpc1, g_vbl_resyncs;

/* NTSC field rate: 60000/1001 Hz = 16.6833 ms.  The XMV decoder's [+0xBC]
 * carries 0x0010AEF0 = 16.6684 ms per presentation tick, so this is the rate
 * the title's own timing constants assume. */
#define NV2A_FIELD_RATE_HZ (60000.0 / 1001.0)

static DWORD WINAPI nv2a_vblank_thread(LPVOID param)
{
    hr_period period;

    (void)param;
    hr_period_init(&period, NV2A_FIELD_RATE_HZ);
    fprintf(stderr, "[INTR] vblank period %.4f ms, %s timer\n",
            1000.0 / NV2A_FIELD_RATE_HZ,
            period.high_res ? "high-resolution" : "DEFAULT-RESOLUTION (drifts)");
    for (;;) {
        { /* measurement: the achieved period, see recomp_dump_as */
          extern unsigned long long g_vbl_ticks, g_vbl_qpc0, g_vbl_qpc1;
          extern unsigned long long g_vbl_resyncs;
          LARGE_INTEGER _v; QueryPerformanceCounter(&_v);
          if (!g_vbl_qpc0) g_vbl_qpc0 = (unsigned long long)_v.QuadPart;
          g_vbl_qpc1 = (unsigned long long)_v.QuadPart;
          ++g_vbl_ticks; g_vbl_resyncs = period.resyncs; }
        hr_period_wait(&period);
        if (g_nv2a == NULL)
            continue;
        g_nv2a->pcrtc.pending_interrupts |= NV_PCRTC_INTR_0_VBLANK;
        g_nv2a_field ^= 1u;   /* see recomp_port_in: port 0x80C0 bit 5 */
        nv2a_update_irq(g_nv2a);
        /* Only flag it when the masks would actually let it through, so a
         * masked title does not spend a safe point on every back-edge. */
        if ((g_nv2a->pcrtc.pending_interrupts & g_nv2a->pcrtc.enabled_interrupts)
            && g_nv2a->pmc.enabled_interrupts)
            recomp_irq_raise(51u);   /* Xbox IRQ 3, the GPU */
        if (g_nv2a_vblank_event) SetEvent(g_nv2a_vblank_event);
    }
}

void nv2a_vblank_start(void)
{
    if (!nv2a_vblank_enabled())
        return;
    fprintf(stderr, "[INTR] vblank raiser started (59.94 Hz, delivery at safe points)\n");
    g_nv2a_vblank_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, nv2a_vblank_thread, NULL, 0, NULL));
}

/* Kept as an extra delivery opportunity on the MMIO path.  Harmless when the
 * safe points are doing the work, and it costs one branch. */
void nv2a_vblank_pump(void)
{
    if (g_nv2a == NULL || !nv2a_vblank_enabled() || !g_recomp_irq_pending)
        return;
    if ((g_nv2a->pcrtc.pending_interrupts & g_nv2a->pcrtc.enabled_interrupts)
        && g_nv2a->pmc.enabled_interrupts) {
        /* Share the normal drain so MMIO cannot consume the notification
         * flag while an audio vector remains queued, or nest another ISR. */
        recomp_safe_point();
    }
}

void nv2a_update_irq(NV2AState *d)
{
    /* PFIFO */
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;
    }

    /* PCRTC */
    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;
    }

    /* PGRAPH */
    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;
    }

    if (d->pmc.pending_interrupts && d->pmc.enabled_interrupts) {
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

/* A nonzero Kelvin NO_OPERATION is a software-method trap. Execution must
 * wait for the guest ISR: RunPushBuffer uses it to patch the return jump and
 * apply the current fixup data before entering a reused command buffer. */
int nv2a_pgraph_software_method(unsigned subchannel, uint32_t parameter)
{
    NV2AState *d = g_nv2a;
    static unsigned logs;
    if (!parameter) return 1;
    if (!d || subchannel >= 8u) return -1;
    d->pgraph.regs[NV_PGRAPH_TRAPPED_ADDR] = 0x100u | (subchannel << 16);
    d->pgraph.regs[NV_PGRAPH_TRAPPED_DATA_LOW] = parameter;
    d->pgraph.regs[NV_PGRAPH_NSOURCE] = NV_PGRAPH_NSOURCE_NOTIFICATION;
    d->pgraph.pending_interrupts |= NV_PGRAPH_INTR_ERROR;
    nv2a_update_irq(d);
    int delivered = 0;
    if ((d->pgraph.enabled_interrupts & NV_PGRAPH_INTR_ERROR) &&
        d->pmc.enabled_interrupts)
        delivered = recomp_deliver_interrupt(NV2A_GPU_INTERRUPT_VECTOR);
    if (delivered && (d->pgraph.pending_interrupts & NV_PGRAPH_INTR_ERROR)) {
        /* This driver's ISR queues its PGRAPH service as a DPC. Complete
         * that deferred handler while the command processor is stopped. */
        extern void recomp_dpc_drain(void);
        recomp_dpc_drain();
    }
    int acknowledged = !(d->pgraph.pending_interrupts & NV_PGRAPH_INTR_ERROR);
    if (logs++ < 24u)
        fprintf(stderr, "[PGRAPH-SOFTWARE] param=%08X delivered=%d ack=%d\n",
                parameter, delivered, acknowledged);
    return delivered && acknowledged ? 1 : -1;
}

/* The flip ring is full when its read and write indices meet. Only the
 * driver's NV_PGRAPH_INCREMENT write retires a displayed buffer; a host
 * Present does not. Keep this in the MMIO register so the CPU and command
 * processor observe the same state. */
static void nv2a_flip_increment(NV2AState *d, uint32_t mask)
{
    uint32_t surface = d->pgraph.regs[NV_PGRAPH_SURFACE];
    uint32_t modulo = GET_MASK(surface, NV_PGRAPH_SURFACE_MODULO_3D);
    if (modulo) {
        uint32_t next = (GET_MASK(surface, mask) + 1u) % modulo;
        SET_MASK(surface, mask, next);
        d->pgraph.regs[NV_PGRAPH_SURFACE] = surface;
    }
}

int nv2a_pgraph_flip_method(uint32_t method, uint32_t parameter)
{
    NV2AState *d = g_nv2a;
    if (!d) return -1;
    switch (method) {
    case NV097_SET_FLIP_READ:
        SET_MASK(d->pgraph.regs[NV_PGRAPH_SURFACE],
                 NV_PGRAPH_SURFACE_READ_3D, parameter);
        return 1;
    case NV097_SET_FLIP_WRITE:
        SET_MASK(d->pgraph.regs[NV_PGRAPH_SURFACE],
                 NV_PGRAPH_SURFACE_WRITE_3D, parameter);
        return 1;
    case NV097_SET_FLIP_MODULO:
        SET_MASK(d->pgraph.regs[NV_PGRAPH_SURFACE],
                 NV_PGRAPH_SURFACE_MODULO_3D, parameter);
        return 1;
    case NV097_FLIP_INCREMENT_WRITE:
        nv2a_flip_increment(d, NV_PGRAPH_SURFACE_WRITE_3D);
        return 1;
    case NV097_FLIP_STALL: {
        extern void recomp_safe_point(void);
        extern void d3d8_PumpWindowMessages(int force);
        ULONGLONG start = GetTickCount64(), report = start + 5000u;
        unsigned waited = 0;
        static unsigned logs;
        /* Submission is synchronous in this runtime. Stop consuming commands
         * here, but service the real device interrupts and guest DPCs that
         * release the ring. Sleeping on host Present or advancing the index
         * on a timer would bypass the guest's display interval decisions. */
        for (;;) {
            uint32_t surface = d->pgraph.regs[NV_PGRAPH_SURFACE];
            if (!GET_MASK(surface, NV_PGRAPH_SURFACE_MODULO_3D)) {
                fprintf(stderr, "[PGRAPH-FLIP] stall with zero modulo\n");
                return -1;
            }
            if (GET_MASK(surface, NV_PGRAPH_SURFACE_READ_3D) !=
                GET_MASK(surface, NV_PGRAPH_SURFACE_WRITE_3D)) break;
            if (g_recomp_irq_pending) {
                recomp_safe_point();
                /* Recheck capacity before waiting: the ISR may just have
                 * retired the buffer we need. */
                continue;
            }
            d3d8_PumpWindowMessages(0);
            if (g_nv2a_vblank_event)
                WaitForSingleObject(g_nv2a_vblank_event, 16);
            else
                Sleep(1);
            ++waited;
            ULONGLONG now = GetTickCount64();
            if (now >= report) {
                fprintf(stderr, "[PGRAPH-FLIP] waiting %llums surface=%08X\n",
                        (unsigned long long)(now - start), surface);
                report = now + 5000u;
            }
        }
        if (waited && logs++ < 16u)
            fprintf(stderr, "[PGRAPH-FLIP] retired after %llums surface=%08X\n",
                    (unsigned long long)(GetTickCount64() - start),
                    d->pgraph.regs[NV_PGRAPH_SURFACE]);
        return 1;
    }
    default: return 0;
    }
}

/* ============================================================
 * DMA helpers (from xemu nv2a.c)
 * ============================================================ */

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);
    dma.address &= 0x07FFFFFF;

    if (dma.address >= memory_region_size(d->vram)) {
        fprintf(stderr, "[NV2A] DMA map address 0x%llx out of VRAM range\n",
                (unsigned long long)dma.address);
        *len = 0;
        return NULL;
    }

    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

/* ============================================================
 * PMC - card master control (from xemu pmc.c)
 * ============================================================ */

uint64_t pmc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PMC_BOOT_0:
        /* NV2A, A03, Rev 0 */
        r = 0x02A000A3;
        break;
    case NV_PMC_INTR_0:
        r = d->pmc.pending_interrupts;
        break;
    case NV_PMC_INTR_EN_0:
        r = d->pmc.enabled_interrupts;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PMC, addr, size, r);
    return r;
}

void pmc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PMC, addr, size, val);

    switch (addr) {
    case NV_PMC_INTR_0:
        d->pmc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PMC_INTR_EN_0:
        /* The master enable.  PCRTC arming alone delivers nothing without
         * this, so both have to be seen before the vblank theory holds. */
        { static unsigned logged;
          if (logged++ < 200u)
              fprintf(stderr, "[INTR] PMC_INTR_EN_0 <- 0x%08X%s\n",
                      (unsigned)val,
                      val ? "   (master interrupt enable ON)" : "   (off)"); }
        d->pmc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PBUS - bus control (from xemu pbus.c)
 * ============================================================ */

uint64_t pbus_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    PCIDevice *d = PCI_DEVICE(s);

    uint64_t r = 0;
    switch (addr) {
    case NV_PBUS_PCI_NV_0:
        r = pci_get_long(d->config + PCI_VENDOR_ID);
        break;
    case NV_PBUS_PCI_NV_1:
        r = pci_get_long(d->config + PCI_COMMAND);
        break;
    case NV_PBUS_PCI_NV_2:
        r = pci_get_long(d->config + PCI_CLASS_REVISION);
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PBUS, addr, size, r);
    return r;
}

void pbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    PCIDevice *d = PCI_DEVICE(s);

    nv2a_reg_log_write(NV_PBUS, addr, size, val);

    switch (addr) {
    case NV_PBUS_PCI_NV_1:
        pci_set_long(d->config + PCI_COMMAND, val);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PTIMER - time measurement (from xemu ptimer.c)
 * ============================================================ */

static uint64_t ptimer_get_clock(NV2AState *d)
{
    if (d->ptimer.numerator == 0) return 0;
    return muldiv64(muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                             d->pramdac.core_clock_freq,
                             NANOSECONDS_PER_SECOND),
                    d->ptimer.denominator,
                    d->ptimer.numerator);
}

uint64_t ptimer_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PTIMER_INTR_0:
        r = d->ptimer.pending_interrupts;
        break;
    case NV_PTIMER_INTR_EN_0:
        r = d->ptimer.enabled_interrupts;
        break;
    case NV_PTIMER_NUMERATOR:
        r = d->ptimer.numerator;
        break;
    case NV_PTIMER_DENOMINATOR:
        r = d->ptimer.denominator;
        break;
    case NV_PTIMER_TIME_0:
        r = (ptimer_get_clock(d) & 0x7ffffff) << 5;
        break;
    case NV_PTIMER_TIME_1:
        r = (ptimer_get_clock(d) >> 27) & 0x1fffffff;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PTIMER, addr, size, r);
    return r;
}

void ptimer_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PTIMER, addr, size, val);

    switch (addr) {
    case NV_PTIMER_INTR_0:
        d->ptimer.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_INTR_EN_0:
        { static unsigned lg;
          if (lg++ < 12u)
              fprintf(stderr, "[INTR] PTIMER_INTR_EN_0 <- 0x%08X\n",
                      (unsigned)val); }
        d->ptimer.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_DENOMINATOR:
        d->ptimer.denominator = val;
        break;
    case NV_PTIMER_NUMERATOR:
        d->ptimer.numerator = val;
        break;
    case NV_PTIMER_ALARM_0:
        { static unsigned lg;
          if (lg++ < 6u)
              fprintf(stderr, "[INTR] PTIMER_ALARM_0 <- 0x%08X  (num=%u den=%u)\n",
                      (unsigned)val, (unsigned)d->ptimer.numerator,
                      (unsigned)d->ptimer.denominator); }
        d->ptimer.alarm_time = val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * PFB - framebuffer / memory control (from xemu pfb.c)
 * ============================================================ */

uint64_t pfb_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFB_CSTATUS:
        r = memory_region_size(d->vram);
        break;
    case NV_PFB_WBC:
        r = 0; /* Flush not pending */
        break;
    default:
        r = d->pfb.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFB, addr, size, r);
    return r;
}

void pfb_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFB, addr, size, val);

    switch (addr) {
    default:
        d->pfb.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * PCRTC - CRT controller (from xemu pcrtc.c)
 * ============================================================ */

uint64_t pcrtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PCRTC_INTR_0:
        r = d->pcrtc.pending_interrupts;
        break;
    case NV_PCRTC_INTR_EN_0:
        r = d->pcrtc.enabled_interrupts;
        break;
    case NV_PCRTC_START:
        r = d->pcrtc.start;
        break;
    case NV_PCRTC_RASTER:
        r = d->pcrtc.raster++;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PCRTC, addr, size, r);
    return r;
}

void pcrtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PCRTC, addr, size, val);

    switch (addr) {
    case NV_PCRTC_INTR_0:
        /* An acknowledge means the title believes it is servicing one. */
        { static unsigned logged;
          if (logged++ < 200u)
              fprintf(stderr, "[INTR] PCRTC_INTR_0 ack <- 0x%08X\n",
                      (unsigned)val); }
        d->pcrtc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_INTR_EN_0:
        /* Does the title arm the display interrupt?  Everything the Xbox
         * drives off vertical blank -- the D3D vblank and swap callbacks
         * among them -- depends on this being enabled and then delivered,
         * and delivery here is a no-op stub. */
        { static unsigned logged;
          if (logged++ < 200u)
              fprintf(stderr, "[INTR] PCRTC_INTR_EN_0 <- 0x%08X%s\n",
                      (unsigned)val,
                      val ? "   (vblank interrupt ARMED)" : "   (disabled)"); }
        d->pcrtc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_START:
        val &= 0x07FFFFFF;
        /* The address the display scans out.  This is the only register that
         * says which surface is actually on screen: the title renders into
         * two and composites one onto the other, and the flip methods carry
         * fixed indices rather than a per-frame choice, so nothing in pgraph
         * can tell the finished surface from the work surface. */
        { static uint32_t last; static unsigned logged;
          if (val != last) {
              last = val;
              if (logged++ < 12u)
                  fprintf(stderr, "[INFO NV2A-DISPLAY] scanout start=%08X\n",
                          (unsigned)val);
          } }
        d->pcrtc.start = val;
        NV2A_DPRINTF("PCRTC_START - %x %x %x %x\n",
                d->vram_ptr[val+64], d->vram_ptr[val+64+1],
                d->vram_ptr[val+64+2], d->vram_ptr[val+64+3]);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PRAMDAC - RAMDAC / PLL control (from xemu pramdac.c)
 * ============================================================ */

uint64_t pramdac_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr & ~3) {
    case NV_PRAMDAC_NVPLL_COEFF:
        r = d->pramdac.core_clock_coeff;
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        r = d->pramdac.memory_clock_coeff;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        r = d->pramdac.video_clock_coeff;
        break;
    case NV_PRAMDAC_PLL_TEST_COUNTER:
        /* emulated PLLs locked instantly */
        r = NV_PRAMDAC_PLL_TEST_COUNTER_VPLL2_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_NVPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_MPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_VPLL_LOCK;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        r = d->pramdac.general_control;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        r = d->pramdac.fp_vdisplay_end;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        r = d->pramdac.fp_vcrtc;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        r = d->pramdac.fp_vsync_end;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        r = d->pramdac.fp_vvalid_end;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        r = d->pramdac.fp_hdisplay_end;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        r = d->pramdac.fp_hcrtc;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        r = d->pramdac.fp_hvalid_end;
        break;
    default:
        break;
    }

    /* Handle unaligned access */
    r >>= 32 - 8 * size - 8 * (addr & 3);

    nv2a_reg_log_read(NV_PRAMDAC, addr, size, r);
    return r;
}

void pramdac_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint32_t m, n, p;

    nv2a_reg_log_write(NV_PRAMDAC, addr, size, val);

    switch (addr) {
    case NV_PRAMDAC_NVPLL_COEFF:
        d->pramdac.core_clock_coeff = val;

        m = val & NV_PRAMDAC_NVPLL_COEFF_MDIV;
        n = (val & NV_PRAMDAC_NVPLL_COEFF_NDIV) >> 8;
        p = (val & NV_PRAMDAC_NVPLL_COEFF_PDIV) >> 16;

        if (m == 0) {
            d->pramdac.core_clock_freq = 0;
        } else {
            d->pramdac.core_clock_freq = (NV2A_CRYSTAL_FREQ * n)
                                          / (1 << p) / m;
        }
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        d->pramdac.memory_clock_coeff = val;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        d->pramdac.video_clock_coeff = val;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        d->pramdac.general_control = val;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        d->pramdac.fp_vdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        d->pramdac.fp_vcrtc = val;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        d->pramdac.fp_vsync_end = val;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        d->pramdac.fp_vvalid_end = val;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        d->pramdac.fp_hdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        d->pramdac.fp_hcrtc = val;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        d->pramdac.fp_hvalid_end = val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * PVIDEO - video overlay (stub)
 * ============================================================ */

uint64_t pvideo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = d->pvideo.regs[addr];
    nv2a_reg_log_read(NV_PVIDEO, addr, size, r);
    return r;
}

void pvideo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PVIDEO, addr, size, val);
    /* TEMPORARY: does the title present the XMV through the video overlay?
     * PVIDEO is a stub here -- it stores the register and does nothing -- so
     * if the boot video is an overlay rather than a textured quad, decoding it
     * correctly would still leave the screen black. */
    { static unsigned n;
      if (n < 40u) {
          ++n;
          fprintf(stderr, "[PVIDEO] write +0x%04X = 0x%08X (size %u)\n",
                  (unsigned)addr, (unsigned)val, size);
          fflush(stderr);
      } }
    d->pvideo.regs[addr] = val;
}

/* ============================================================
 * PGRAPH - graphics engine (stub for Phase 1)
 * ============================================================ */

/* PGRAPH 0xB10 is a second window onto the channel's consumed push-buffer
 * position, alongside NV_USER DMA_GET.  The USER state that backs both lives
 * further down this file, so reach it through a small accessor. */
#define NV_PGRAPH_DMA_GET_SHADOW 0x00000B10
unsigned long long g_pgraph_get_reads;
static uint32_t nv2a_user_consumed_position(void);
static uint32_t nv2a_user_consumed_offset(void);

/* One canonical consumed position, published to the two progress interfaces
 * in the units each of them uses.
 *
 * The two are NOT the same integer.  NV_USER DMA_GET is an absolute address in
 * the same space the guest writes to DMA_PUT -- the channel init spin in
 * sub_0053E5D0 compares the two directly, so it has to stay absolute.  PGRAPH
 * 0xB10 is compared by sub_0053C120 against an in-memory dword index shifted
 * left by two, so it is a ring-relative BYTE offset.
 *
 * Both are advanced only after nv2a_pushbuffer_submit_guest() has returned,
 * i.e. after the commands have actually been translated and submitted, never
 * merely because DMA_PUT was written. */
static uint32_t g_consumed_abs;      /* absolute, for NV_USER DMA_GET */
static uint32_t g_consumed_offset;   /* ring-relative bytes, for PGRAPH 0xB10 */
static unsigned long long g_consume_completions;
static uint32_t g_user_dma_put;
static uint32_t g_user_ring_base;

uint64_t pgraph_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r;

    switch (addr) {
    case NV_PGRAPH_INTR:
        r = d->pgraph.pending_interrupts;
        break;
    case NV_PGRAPH_INTR_EN:
        r = d->pgraph.enabled_interrupts;
        break;
    case NV_PGRAPH_DMA_GET_SHADOW:
        /* How far the GPU has consumed the push buffer.
         *
         * D3D's push-buffer wait (sub_0053C120) spins on this comparing it
         * against its own PUT pointer, and it was reading back the never-
         * written register array -- zero -- so the wait could never clear and
         * rendering stopped for good after the intro.
         *
         * This runtime translates push-buffer work synchronously in
         * nv2a_pushbuffer_submit_guest(), so there is no asynchronous GPU that
         * can be behind: by the time the title is able to read this register,
         * the work really has been consumed.  Reporting the submitted position
         * is what hardware reports once the channel has drained, and it is the
         * same reasoning and the same underlying state that NV_USER DMA_GET
         * already uses -- not a value invented for one title. */
        /* Progress in the upper bits, the handshake tag in 2..6. */
        r = (nv2a_user_consumed_offset() & ~(uint32_t)0x7C)
          | nv2a_progress_tag();
        break;
    default:
        r = d->pgraph.regs[addr];
        break;
    }

    if (addr == NV_PGRAPH_DMA_GET_SHADOW) {
        static unsigned n;
        ++g_pgraph_get_reads;
        if (n++ < 12u)
            fprintf(stderr, "[PGRAPH] get=0x%08X dma_put=0x%08X ring_base=0x%08X\n",
                    (unsigned)r, (unsigned)g_user_dma_put,
                    (unsigned)g_user_ring_base);
    }

    nv2a_reg_log_read(NV_PGRAPH, addr, size, r);
    return r;
}

void pgraph_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PGRAPH, addr, size, val);

    switch (addr) {
    case NV_PGRAPH_INCREMENT:
        if (val & NV_PGRAPH_INCREMENT_READ_3D)
            nv2a_flip_increment(d, NV_PGRAPH_SURFACE_READ_3D);
        break;
    case NV_PGRAPH_INTR:
        /* Write-one-to-clear, exactly as pmc_write and ptimer_write already
         * model their own interrupt registers.  Storing the written value
         * instead -- which is what the generic register path did -- turns the
         * driver's acknowledgement into a re-arm: it writes the bits it wants
         * cleared, reads them straight back as still pending, and re-enters
         * its trap handler forever.  That was ~1863 identical passes over
         * INTR / NSOURCE / TRAPPED_ADDR / FIFO before the boot died. */
        d->pgraph.pending_interrupts &= ~(uint32_t)val;
        nv2a_update_irq(d);
        break;
    case NV_PGRAPH_INTR_EN:
        { static unsigned lg;
          if (lg++ < 12u)
              fprintf(stderr, "[INTR] PGRAPH_INTR_EN <- 0x%08X\n",
                      (unsigned)val); }
        d->pgraph.enabled_interrupts = (uint32_t)val;
        nv2a_update_irq(d);
        break;
    default:
        d->pgraph.regs[addr] = (uint32_t)val;
        break;
    }
}

/* ============================================================
 * PGRAPH method dispatch
 * Called when push buffer commands are parsed.
 * Routes method calls into PGRAPH register writes.
 * ============================================================ */

static uint32_t g_pgraph_method_count = 0;
static uint32_t g_pgraph_draw_count = 0;
static uint32_t g_pgraph_clear_count = 0;
static uint32_t g_pgraph_flip_count = 0;
static uint32_t g_pgraph_inline_verts = 0;
static int g_pgraph_in_begin = 0;

/* NV097 method constants for dispatch */
#define M_NO_OPERATION          0x0100
#define M_SET_SURFACE_FORMAT    0x0208
#define M_SET_SURFACE_PITCH     0x020C
#define M_SET_SURFACE_COLOR_OFF 0x0210
#define M_SET_SURFACE_ZETA_OFF  0x0214
#define M_SET_SURFACE_CLIP_H    0x0200
#define M_SET_SURFACE_CLIP_V    0x0204
#define M_CLEAR_SURFACE         0x01D0
#define M_SET_COLOR_CLEAR_VALUE 0x01D4
#define M_SET_BEGIN_END         0x17FC
#define M_INLINE_ARRAY          0x1818
#define M_FLIP_INCREMENT_WRITE  0x0114
#define M_FLIP_STALL            0x0118
#define M_SET_VIEWPORT_OFFSET   0x0A20
#define M_SET_VIEWPORT_SCALE    0x0AF0

void pgraph_method(NV2AState *d, uint32_t subchannel,
                   uint32_t method, uint32_t param)
{
    g_pgraph_method_count++;


    /* Route through D3D11 translator first */
    if (pgraph_d3d11_method(subchannel, method, param)) {
        /* Handled by D3D11 translator — still store in regs for state queries */
        if (method < 0x2000 * 4) {
            d->pgraph.regs[method / 4] = param;
        }
        return;
    }

    /* Log unhandled methods (first 20 + periodic) */
    if (g_pgraph_method_count <= 20 || (g_pgraph_method_count % 5000) == 0) {
        fprintf(stderr, "[PGRAPH] #%u UNHANDLED sub=%u 0x%04X = 0x%08X\n",
                g_pgraph_method_count, subchannel, method, param);
    }

    /* Store method parameters in PGRAPH register space */
    if (method < 0x2000 * 4) {
        d->pgraph.regs[method / 4] = param;
    }

    /* Track high-level operations (legacy counters) */
    switch (method) {
    case M_CLEAR_SURFACE:
        g_pgraph_clear_count++;
        break;

    case M_SET_BEGIN_END:
        if (param != 0) {
            g_pgraph_in_begin = 1;
            g_pgraph_draw_count++;
        } else {
            g_pgraph_in_begin = 0;
        }
        break;

    case M_INLINE_ARRAY:
        if (g_pgraph_in_begin) {
            g_pgraph_inline_verts++;
        }
        break;

    case M_FLIP_INCREMENT_WRITE:
        g_pgraph_flip_count++;
        if (g_pgraph_flip_count <= 5 || (g_pgraph_flip_count % 300) == 0) {
            fprintf(stderr, "[PGRAPH] Frame %u: %u methods, %u draws, %u clears, %u inline verts\n",
                    g_pgraph_flip_count, g_pgraph_method_count,
                    g_pgraph_draw_count, g_pgraph_clear_count,
                    g_pgraph_inline_verts);
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * PFIFO - command FIFO (stub for Phase 1)
 * Full PFIFO with push buffer processing comes in Phase 2-3.
 * ============================================================ */

uint64_t pfifo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFIFO_INTR_0:
        r = d->pfifo.pending_interrupts;
        break;
    case NV_PFIFO_INTR_EN_0:
        r = d->pfifo.enabled_interrupts;
        break;

    /* Drain status. sub_00540B13 spins until the caches and the runout queue
     * report empty and the DMA push reports idle; with the generic register
     * path these echoed whatever had been written and it looped forever
     * (~4,300 passes over INTR_0 / CACHE1_STATUS / RUNOUT_STATUS before the
     * boot was killed).
     *
     * Reporting empty and idle is the truthful answer rather than a fudge:
     * there is no FIFO engine behind these registers, so nothing is ever
     * queued and no push is ever mid-transfer.  Push buffers are translated
     * synchronously by the D3D8 submit path instead. */
    case NV_PFIFO_CACHE1_STATUS:
        r = NV_PFIFO_CACHE1_STATUS_LOW_MARK;      /* empty, never full */
        break;
    case NV_PFIFO_RUNOUT_STATUS:
        r = NV_PFIFO_RUNOUT_STATUS_LOW_MARK;      /* empty, nothing ran out */
        break;
    case NV_PFIFO_CACHE1_DMA_PUSH:
        /* Keep the bits the driver owns (ACCESS, BUFFER) and clear STATE:
         * the push is never in progress because it completes on the write. */
        r = d->pfifo.regs[addr] & ~(uint32_t)NV_PFIFO_CACHE1_DMA_PUSH_STATE;
        break;

    default:
        r = d->pfifo.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFIFO, addr, size, r);
    return r;
}

void pfifo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFIFO, addr, size, val);

    switch (addr) {
    case NV_PFIFO_INTR_0:
        d->pfifo.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PFIFO_INTR_EN_0:
        { static unsigned lg;
          if (lg++ < 12u)
              fprintf(stderr, "[INTR] PFIFO_INTR_EN_0 <- 0x%08X\n",
                      (unsigned)val); }
        d->pfifo.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        d->pfifo.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * Stub handler for unimplemented blocks
 * ============================================================ */

uint64_t nv2a_stub_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub read: addr=0x%llx size=%d\n",
                 (unsigned long long)addr, size);
    return 0;
}

void nv2a_stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub write: addr=0x%llx val=0x%llx size=%d\n",
                 (unsigned long long)addr, (unsigned long long)val, size);
}

/* ============================================================
 * Block dispatch table (from xemu nv2a.c)
 * ============================================================ */

#define ENTRY(NAME, LNAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                      \
    .offset = OFFSET,                                     \
    .size   = SIZE,                                       \
    .ops    = { .read = LNAME##_read, .write = LNAME##_write }, \
}
#define STUB_ENTRY(NAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                    \
    .offset = OFFSET,                                   \
    .size   = SIZE,                                     \
    .ops    = { .read = nv2a_stub_read, .write = nv2a_stub_write }, \
}

/* ============================================================
 * NV_PRAMIN - instance memory
 * ============================================================
 *
 * PRAMIN is not a register block: it is a window onto the GPU's own 1MB of
 * instance memory, where the driver builds DMA objects, graphics contexts and
 * the channel's RAMHT.  xemu maps it as a RAM region, which is why the block
 * table carried a NULL entry for it -- and why every write the title made to
 * 0xFD700000+ was silently discarded here, leaving the DMA objects the push
 * buffer depends on full of zeros.
 *
 * Back it with the RAMIN allocation nv2a_hook_init already makes.  Accesses
 * past the end are dropped rather than allowed to run off the block, since a
 * bad instance offset should not become a host memory corruption.
 */
uint64_t pramin_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    if (!d->ramin_ptr || addr + size > memory_region_size(&d->ramin))
        return 0;

    switch (size) {
    case 1:  return *(uint8_t  *)(d->ramin_ptr + addr);
    case 2:  return *(uint16_t *)(d->ramin_ptr + addr);
    default: return *(uint32_t *)(d->ramin_ptr + addr);
    }
}

void pramin_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    if (!d->ramin_ptr || addr + size > memory_region_size(&d->ramin))
        return;

    switch (size) {
    case 1:  *(uint8_t  *)(d->ramin_ptr + addr) = (uint8_t)val;  break;
    case 2:  *(uint16_t *)(d->ramin_ptr + addr) = (uint16_t)val; break;
    default: *(uint32_t *)(d->ramin_ptr + addr) = (uint32_t)val; break;
    }
}

/* ============================================================
 * RAMHT - handle resolution
 * ============================================================
 *
 * Push-buffer methods refer to graphics objects and DMA contexts by handle,
 * never by address.  The channel's hash table in PRAMIN maps a handle to the
 * instance address of the object it names, and the object itself carries the
 * class (for SET_OBJECT) or the DMA descriptor (for the SET_CONTEXT_DMA_*
 * family).  Without this, every one of those methods is uninterpretable and the
 * translator can only report them unhandled.
 *
 * The table is walked linearly rather than by hash.  It is at most a few
 * thousand entries, this runs once per binding rather than per draw, and a
 * linear scan cannot disagree with the hardware's hash function -- which is
 * derived from the channel id and table size and is easy to get subtly wrong.
 */

/* RAMHT base, as a PRAMIN byte offset.  NV_PFIFO_RAMHT stores it shifted:
 * the driver computes (offset >> 8) & 0x1F0 when it programs the register. */
static uint32_t ramht_base(NV2AState *d)
{
    return (d->pfifo.regs[NV_PFIFO_RAMHT] & 0x1F0u) << 8;
}

/**
 * Resolve a handle to the PRAMIN byte offset of its object.
 *
 * Returns 0 when the handle is not in the table.  `out_context`, when given,
 * receives the raw context dword, whose low bits carry the engine.
 */
uint32_t nv2a_ramht_lookup(uint32_t handle, uint32_t *out_context)
{
    NV2AState *d = g_nv2a;
    uint32_t base, limit, off;

    if (out_context)
        *out_context = 0;
    if (!d || !d->ramin_ptr || handle == 0)
        return 0;

    base = ramht_base(d);
    limit = (uint32_t)memory_region_size(&d->ramin);
    if (base >= limit)
        return 0;

    /* Entries are handle/context pairs.  Cap the walk at the table's nominal
     * 4KB so a bad base cannot run the whole of instance memory. */
    for (off = base; off + 8u <= limit && off < base + 0x1000u; off += 8u) {
        uint32_t entry_handle = *(const uint32_t *)(d->ramin_ptr + off);
        uint32_t context      = *(const uint32_t *)(d->ramin_ptr + off + 4u);

        if (entry_handle != handle || context == 0)
            continue;
        if (out_context)
            *out_context = context;
        /* Instance address is stored in units of 16 bytes. */
        return (context & 0x0000FFFFu) << 4;
    }
    return 0;
}

/**
 * Resolve a context-DMA handle to the guest address range it describes.
 *
 * Returns 1 on success.  The address is masked to the 64MB the console has, so
 * a descriptor naming AGP or PCI space cannot produce an out-of-range base.
 */
int nv2a_dma_from_handle(uint32_t handle, uint32_t *out_base, uint32_t *out_limit)
{
    NV2AState *d = g_nv2a;
    uint32_t instance;
    DMAObject dma;

    if (out_base)  *out_base = 0;
    if (out_limit) *out_limit = 0;
    if (!d || !d->ramin_ptr)
        return 0;

    instance = nv2a_ramht_lookup(handle, NULL);
    if (instance == 0 || instance + 12u > memory_region_size(&d->ramin))
        return 0;

    dma = nv_dma_load(d, instance);
    if (out_base)  *out_base  = (uint32_t)dma.address & 0x03FFFFFFu;
    if (out_limit) *out_limit = (uint32_t)dma.limit;
    return 1;
}

/* ============================================================
 * NV_USER - per-channel DMA submission window
 * ============================================================
 *
 * The title submits work by writing its push-buffer end address to DMA_PUT and
 * then spinning until DMA_GET reports the same value, i.e. until the GPU has
 * consumed everything queued.  sub_0053E5D0 does exactly that during device
 * init and hung forever while this block was a plain stub: DMA_GET read back
 * whatever had last been stored there and never reached DMA_PUT.
 *
 * Push buffers are translated synchronously by the D3D8 submit path, so there
 * is no asynchronous GPU to fall behind.  Reporting GET == PUT is what real
 * hardware reports once the channel has drained, and it is honest here: by the
 * time the title can observe the register, the work really has been consumed.
 *
 * Channels repeat every 0x2000 bytes across this 8MB window.  Only channel 0 is
 * used, so the address is folded to its channel-relative offset rather than
 * modelling per-channel state.
 */
static uint32_t g_user_ref;
/* Lowest DMA_PUT seen in the push buffer currently in use -- its base.
 * See the DMA_PUT case below for why this has to be observed. */

/* The consumed position both DMA_GET windows report.  Synchronous submission
 * means consumed == submitted; see the NV_USER note above. */
static uint32_t nv2a_user_consumed_position(void)
{
    return g_consumed_abs;
}

static uint32_t nv2a_user_consumed_offset(void)
{
    return g_consumed_offset;
}

/* The five-bit progress tag PGRAPH 0xB10 carries in bits 2..6.
 *
 * Captured on hardware over 400 samples of the D3D push-buffer wait: bits 2..6
 * of 0xB10 equal bits 2..6 of (S << 2) in every single one, while bits 0,1 and
 * everything above bit 7 are unrelated -- the samples read as ordinary float
 * pipeline data, 0.45 to 3.5.  So `test cl, 0x7C` is a handshake on a modulo-32
 * sequence number, not a comparison of ring positions, and on hardware it is
 * always already satisfied: the wait exited on the first read all 400 times.
 *
 * S is the guest's allocation counter, and it lives one page below the ring
 * base.  That has now been observed on three rings at three different
 * addresses -- 01201000/01202000 and 01CBC000/01CBD000 here, 81F71000/81F72000
 * on hardware -- so the address is derived from the ring base this runtime
 * already observes rather than assumed or hooked out of guest code.
 *
 * Because every submitted command is translated before the DMA_PUT write
 * returns, this renderer is never behind, so the tag it reports is the
 * requester's own: that is the literal truth here, not a value chosen to
 * unblock the loop. */
static uint32_t nv2a_progress_tag(void)
{
    uint32_t base = g_user_ring_base;
    const uint32_t *shadow;

    if (base < 0x1000u || base >= 0x04000000u)
        return 0u;
    shadow = (const uint32_t *)(uintptr_t)((base - 0x1000u)
                                           + (uintptr_t)g_xbox_mem_offset);
    return ((*shadow) << 2) & 0x7Cu;
}

/* Called once the submitted range has been translated.  Ring wrap needs no
 * special case: DMA_PUT is already an address inside the current ring, so the
 * offset it yields walks back to near zero on its own when the pusher wraps.
 * A channel that is torn down and rebuilt re-observes its base through
 * nv2a_user_dma_put_step, and the offset follows it. */
uint32_t nv2a_get_dma_put(void) { return g_user_dma_put; }

/* Read-only views of the same state the 0xB10 model uses, for the allocator
 * trace.  No side effects: the trace must not perturb what it measures. */
uint32_t nv2a_get_consumed_abs(void)    { return g_consumed_abs; }
uint32_t nv2a_get_consumed_offset(void) { return g_consumed_offset; }
uint32_t nv2a_get_ring_base(void)       { return g_user_ring_base; }
uint32_t nv2a_peek_b10(void)
{
    return (nv2a_user_consumed_offset() & ~(uint32_t)0x7C)
         | nv2a_progress_tag();
}

static void nv2a_consume_completed(void)
{
    uint32_t base = g_user_ring_base;
    uint32_t put  = g_user_dma_put;

    g_consumed_abs = put;
    g_consumed_offset = (base && put >= base) ? (put - base) : put;
    ++g_consume_completions;
}

/* TEMPORARY: who publishes DMA_PUT, and what the last publication was.
 *
 * sub_0053C630, which sub_0053C190 calls immediately before the wait, is
 * NOT the publisher -- NV_PFB_WBC bit 16 is the write-back cache flush and
 * this runtime already answers "flush not pending" for it.  So the batch
 * has to be published somewhere else, and this records where.
 *
 * A ring of the last few writes with their native frames, symbolised only
 * when the ring is dumped, so the write path itself stays cheap. */
#define PUTLOG_N      8u
#define PUTLOG_FRAMES 26u

struct put_rec {
    unsigned long long seq;
    uint32_t value, prev, base;
    void *frames[PUTLOG_FRAMES];
    USHORT nframes;
};
static struct put_rec g_putlog[PUTLOG_N];
static unsigned long long g_put_seq;

static void nv2a_put_record(uint32_t value, uint32_t prev, uint32_t base)
{
    struct put_rec *r = &g_putlog[g_put_seq % PUTLOG_N];
    r->seq = ++g_put_seq;
    r->value = value; r->prev = prev; r->base = base;
    r->nframes = CaptureStackBackTrace(1, PUTLOG_FRAMES, r->frames, NULL);
}

void nv2a_dump_putlog(void)
{
    unsigned i;
    char sbuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
    SYMBOL_INFO *sym = (SYMBOL_INFO *)sbuf;
    HANDLE proc = GetCurrentProcess();
    if (!g_put_seq) return;
    sym->SizeOfStruct = sizeof(*sym);
    sym->MaxNameLen = MAX_SYM_NAME;
    fprintf(stderr, "[PUTLOG] consume completions=%llu  PGRAPH 0xB10 reads=%llu"
            "  consumed abs=%08X offset=%08X  ring base=%08X" "\n",
            g_consume_completions, g_pgraph_get_reads,
            g_consumed_abs, g_consumed_offset, g_user_ring_base);
    fprintf(stderr, "[PUTLOG] %llu DMA_PUT publications, last %u:" "\n",
            g_put_seq, (unsigned)(g_put_seq < PUTLOG_N ? g_put_seq : PUTLOG_N));
    for (i = 0; i < PUTLOG_N; ++i) {
        struct put_rec *r = &g_putlog[(g_put_seq + i) % PUTLOG_N];
        USHORT f;
        if (!r->seq) continue;
        fprintf(stderr, "[PUTLOG]   #%llu put=%08X prev=%08X base=%08X advance=%d" "\n",
                r->seq, r->value, r->prev, r->base,
                (int)(r->value - r->prev));
        for (f = 0; f < r->nframes; ++f) {
            DWORD64 disp = 0;
            if (SymFromAddr(proc, (DWORD64)(uintptr_t)r->frames[f], &disp, sym))
                fprintf(stderr, "[PUTLOG]       %s+0x%llX" "\n",
                        sym->Name, (unsigned long long)disp);
        }
    }
    fflush(stderr);
}
uint64_t user_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint64_t r = 0;
    (void)opaque;

    switch (addr & 0x1FFF) {
    case NV_USER_DMA_PUT:
        r = g_user_dma_put;
        break;
    case NV_USER_DMA_GET:
        r = nv2a_user_consumed_position();  /* always caught up; see above */
        break;
    case NV_USER_REF:
        r = g_user_ref;
        break;
    default:
        r = 0;
        break;
    }

    { static unsigned n; if (n++ < 24)
        fprintf(stderr, "[USER] read  0x%03X -> 0x%08X\n",
                (unsigned)(addr & 0x1FFF), (unsigned)r); }

    nv2a_reg_log_read(NV_USER, addr, size, r);
    return r;
}

void user_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    (void)opaque;
    { static unsigned n; if (n++ < 24)
        fprintf(stderr, "[USER] write 0x%03X <- 0x%08X\n",
                (unsigned)(addr & 0x1FFF), (unsigned)val); }
    nv2a_reg_log_write(NV_USER, addr, size, val);

    switch (addr & 0x1FFF) {
    case NV_USER_DMA_PUT: {
        uint32_t prev = g_user_dma_put;
        uint32_t begin, end;
        int submit = nv2a_user_dma_put_step(&g_user_dma_put,
                                            &g_user_ring_base,
                                            (uint32_t)val, &begin, &end);
        nv2a_put_record((uint32_t)val, prev, g_user_ring_base);

        /* Ring geometry is worth seeing once. It is observed, not read out of
         * a register, so a title that lays its push buffer out differently
         * shows up here rather than as silently missing draws. */
        if ((uint32_t)val < prev) {
            static unsigned wraps;
            if (wraps++ < 8u)
                fprintf(stderr, "[USER] ring wrapped 0x%08X -> 0x%08X, "
                        "base 0x%08X: %s\n",
                        prev, (uint32_t)val, g_user_ring_base,
                        submit ? "following queued tail to new PUT" : "buffer relocation");
            else if (wraps == 9u)
                fprintf(stderr, "[USER] further ring wraps not logged\n");
        }

        /* Record unsubmitted forward relocations. Wrapped tails now reach
         * the linked walker and are no longer skipped intervals. */
        {   extern void nv2a_pb_note_skipped(uint32_t, uint32_t, const char *);
            uint32_t v = (uint32_t)val;
            if (!submit) {
                if (v > prev)
                    nv2a_pb_note_skipped(prev, v, "oversize forward step");
            }
        }

        if (submit)
            nv2a_pushbuffer_submit_guest(begin, end);

        /* Only now, with the work translated, is the position consumed. */
        nv2a_consume_completed();
        break;
    }
    case NV_USER_REF:
        g_user_ref = (uint32_t)val;
        break;
    default:
        break;
    }
}

const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    STUB_ENTRY(PFIFO_CACHE,   0x003000, 0x001000),
    STUB_ENTRY(PRMA,          0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    STUB_ENTRY(PCOUNTER,      0x00a000, 0x001000),
    STUB_ENTRY(PVPE,          0x00b000, 0x001000),
    STUB_ENTRY(PTV,           0x00d000, 0x001000),
    STUB_ENTRY(PRMFB,         0x0a0000, 0x020000),
    STUB_ENTRY(PRMVIO,        0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    STUB_ENTRY(PSTRAPS,       0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    STUB_ENTRY(PRMCIO,        0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    STUB_ENTRY(PRMDIO,        0x681000, 0x001000),
    ENTRY(PRAMIN,   pramin,   0x700000, 0x100000),
    /* NV_USER = 20 */
    ENTRY(USER,     user,     0x800000, 0x800000),
};

#undef ENTRY
#undef STUB_ENTRY

/* ============================================================
 * MMIO dispatch (for VEH handler integration)
 * ============================================================ */

uint64_t nv2a_mmio_read(NV2AState *d, hwaddr addr, unsigned int size)
{
    /* Find which block handles this address */
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            uint64_t _v = blocktable[i].ops.read(d, block_addr, size);
            /* TEMPORARY: the driver's interrupt-service loop never drains.
             * Log what it actually sees so the bit it waits on is visible. */
            { static unsigned _n;
              int _want = (i == NV_PFIFO  && (block_addr == 0x080 ||
                              block_addr == 0x100 || block_addr == 0x400 ||
                              block_addr == 0x500 || block_addr == 0x1214 ||
                              block_addr == 0x1220 || block_addr == 0x1250)) ||
                          (i == NV_PGRAPH && block_addr == 0x100) ||
                          (i == NV_PMC    && block_addr == 0x100);
              if (_want && _n < 48u) { _n++;
                fprintf(stderr, "[RD] %-6s +0x%04X -> 0x%08X\n",
                        blocktable[i].name, (unsigned)block_addr,
                        (unsigned)_v); } }
            return _v;
        }
    }
    NV2A_DPRINTF("MMIO read unmapped: addr=0x%llx\n", (unsigned long long)addr);
    return 0;
}

void nv2a_mmio_write(NV2AState *d, hwaddr addr, uint64_t val, unsigned int size)
{
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            { static unsigned _n;
              int _want = (i == NV_PFIFO  && (block_addr == 0x080 ||
                              block_addr == 0x100 || block_addr == 0x400 ||
                              block_addr == 0x500 || block_addr == 0x1214 ||
                              block_addr == 0x1220 || block_addr == 0x1250)) ||
                          (i == NV_PGRAPH && block_addr == 0x100) ||
                          (i == NV_PMC    && block_addr == 0x100);
              if (_want && _n < 48u) { _n++;
                fprintf(stderr, "[WR] %-6s +0x%04X <- 0x%08X\n",
                        blocktable[i].name, (unsigned)block_addr,
                        (unsigned)val); } }
            blocktable[i].ops.write(d, block_addr, val, size);
            return;
        }
    }
    NV2A_DPRINTF("MMIO write unmapped: addr=0x%llx val=0x%llx\n",
                 (unsigned long long)addr, (unsigned long long)val);
}

/* ============================================================
 * Standalone initialization
 * ============================================================ */

NV2AState *nv2a_init_standalone(uint8_t *vram_ptr, uint32_t vram_size,
                                 uint8_t *ramin_ptr, uint32_t ramin_size)
{
    if (g_nv2a) return g_nv2a;

    NV2AState *d = (NV2AState *)calloc(1, sizeof(NV2AState));
    if (!d) return NULL;

    /* Set up VRAM */
    g_vram_region.size = vram_size;
    d->vram = &g_vram_region;
    d->vram_ptr = vram_ptr;
    d->vram_pci.size = vram_size;

    /* Set up RAMIN */
    g_ramin_region.size = ramin_size;
    d->ramin.size = ramin_size;
    d->ramin_ptr = ramin_ptr;

    /* PCI config space: NV2A vendor/device */
    pci_set_long(d->parent_obj.config + PCI_VENDOR_ID, 0x02A010DE); /* NVIDIA NV2A */
    pci_set_long(d->parent_obj.config + PCI_CLASS_REVISION, 0x030000A1);

    /* Default PLL: 233 MHz core clock (Xbox default) */
    d->pramdac.core_clock_coeff = 0x00011C01; /* n=0x1C, m=1, p=0 */
    d->pramdac.core_clock_freq = NV2A_CRYSTAL_FREQ * 0x1C; /* ~233 MHz */

    /* Default timer divisors */
    d->ptimer.numerator = 1;
    d->ptimer.denominator = 1;

    /* Initialize PFIFO mutex */
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    g_nv2a = d;

    fprintf(stderr, "[NV2A] Standalone GPU initialized: VRAM=%uMB RAMIN=%uKB\n",
            vram_size / (1024*1024), ramin_size / 1024);

    return d;
}
