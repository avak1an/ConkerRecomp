/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Reuses the same x86-64 instruction decoder pattern as nv2a_mmio_hook.c
 * but routes reads/writes through the MCPX APU register handlers.
 */

#include <stdlib.h>
#include "apu.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "apu_memory.h"

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* The MMIO hook is a Win32-VEH x86-64 instruction decoder. On Linux the
 * equivalent goes through sigaction + ucontext_t (Stage 2 / main.c). For
 * now the whole body is Windows-only so apu_emu links on Debian. */
#if defined(_WIN32)
#include <windows.h>

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

/* AC'97 controller interface, a separate window from the APU's. */
#define ACI_MMIO_BASE  0xFEC00000u
#define ACI_MMIO_SIZE  0x00001000u

/* (g_apu_state is defined above, outside the Win32 guard) */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

/* ============================================================
 * x86-64 register access helpers (same as NV2A hook)
 * ============================================================ */

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1; /* SIB */
    if (mod == 0 && (rm & 7) == 5) len += 4; /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* ============================================================
 * Instruction decoder for APU MMIO access
 * ============================================================ */

/* ============================================================
 * ACI (AC'97 controller) register file
 *
 * 0xFEC00000 is a separate MMIO window from the APU's, and nothing modelled
 * it -- it fell through to the generic "back it with zeroed pages" path in
 * main.c, which turns every register into a latch.
 *
 * DSOUND's stream reset writes RR (bit 1) to a NABM channel control register,
 * reads it back exactly once, and then `test cl,cl; jne $-4`. On hardware RR
 * is self-clearing and has already cleared by the time the read completes, so
 * the loop falls straight through; it is an assertion, not a retry. Backed by
 * RAM the read returned the 2 the title had just written and the loop became
 * the infinite loop it looks like.
 *
 * There is no DMA engine behind this, so a reset completes instantly: store
 * writes, but drop the self-clearing bits on the way in. That is the whole
 * model -- everything else behaves as ordinary storage, which is what the
 * zeroed page already gave.
 * ============================================================ */

#define ACI_MMIO_SIZE_BYTES 0x1000u

/* NABM per-channel control registers live at 0x100 + 0x10*n + 0x0B. */
#define ACI_NABM_BASE   0x100u
#define ACI_NABM_CR_LOW 0x0Bu
#define ACI_CR_RR       0x02u   /* Reset Registers: self-clearing */

/* AC'97 global control and status.
 *
 * A codec that is physically present reports itself ready, and nothing here
 * ever did, so the primary-codec-ready bit read back zero forever.  The title
 * polls it 0x3E8 times in sub_004CE08E, gives up, and returns a DirectSound
 * error that abandons the whole audio stack -- which is why the APU global
 * interrupt was never enabled, the audio ISR never ran, audio packets never
 * retired from E_PENDING, and the XMV decoder was never allowed to publish a
 * decoded frame.  One bit, and the boot video at the end of it.
 *
 * Measured on a console: control reads 0, status reads 0x100.  The warm reset
 * in control is self-clearing like the NABM one above, which is why it reads
 * back zero there after the title sets it. */
#define ACI_GLOBAL_CONTROL  0x12Cu
#define ACI_GLOBAL_STATUS   0x130u
#define ACI_GC_WARM_RESET   0x02u   /* self-clearing */
#define ACI_GS_PCR          0x0100u /* primary codec ready */

static uint8_t g_aci_regs[ACI_MMIO_SIZE_BYTES];
static bool    g_target_aci;

static bool aci_is_channel_control(uint32_t off)
{
    /* The NABM window is 0x80 bytes -- eight channels at 0x10 apart, so the
     * last control byte is at 0x17B.  This stopped at 0x140 and covered only
     * four, which was enough while the codec never reported ready and the
     * stream reset for the upper channels was unreachable.  With the codec
     * present the title resets channel 7, read its RR bit back set, and spun:
     * sub_004CE34C loads that byte ONCE into cl and then `test cl,cl; jne $-4`
     * without reloading, so a stale set bit is not a retry, it is forever. */
    return off >= ACI_NABM_BASE &&
           off < ACI_NABM_BASE + 0x80u &&
           (off & 0x0Fu) == ACI_NABM_CR_LOW;
}

/* ============================================================
 * AC97 NABM buffer-descriptor DMA progress
 *
 * The comment above says "There is no DMA engine behind this", and that turned
 * out to be the thing standing between here and a playing boot video.
 *
 * DirectSound does not learn that an audio packet is finished from the APU
 * voice notifiers.  It polls this controller.  Watching the packet status word
 * on a console shows it cleared from 0x0055945C, reached through sub_004C7FFF
 * -- the DirectSound service loop -- which sub_004C5248 calls, which the XMV
 * update calls at loc_004E2D78 in the same call that then decides whether it
 * may publish a video frame.  Synchronous polling by the guest, start to
 * finish.  sub_004C7FFF runs here exactly as often as it does there (1,000
 * calls, never skipped); it simply never saw any progress to report.
 *
 * Per channel, offsets from its base:
 *
 *     +0x00  BDBAR  descriptor list base
 *     +0x04  CIV    current index value
 *     +0x05  LVI    last valid index
 *     +0x06  SR     status
 *     +0x08  PICB   position in current buffer, in samples, counts DOWN
 *     +0x0A  PIV    prefetched index
 *     +0x0B  CR     control
 *
 * A descriptor is 8 bytes: a buffer address, then a 16-bit length in samples
 * and 16 bits of flags.  While RPBM is set the engine walks the list, draining
 * PICB at the sample rate and stepping CIV at each buffer boundary.
 *
 * Advanced lazily on read from the wall clock rather than on a timer thread:
 * the guest only observes these through reads, so there is nothing for a
 * thread to be more correct about, and it cannot drift against a clock it
 * never samples.
 *
 * CONKER_APU_DMA enables it, off by default until the result is checked.
 */

extern uint8_t *g_apu_ram_ptr;   /* Xbox 64MB RAM base */

#define ACI_CIV   0x04u
#define ACI_LVI   0x05u
#define ACI_SR    0x06u
#define ACI_PICB  0x08u
#define ACI_PIV   0x0Au

#define ACI_SR_DCH    0x0001u   /* DMA controller halted */
#define ACI_SR_CELV   0x0002u   /* current equals last valid */
#define ACI_SR_LVBCI  0x0004u   /* last valid buffer completed */
#define ACI_SR_BCIS   0x0008u   /* buffer completion */

#define ACI_CR_RPBM   0x01u     /* run/pause bus master */
#define ACI_CR_LVBIE  0x04u     /* last-valid-buffer interrupt enable */
#define ACI_CR_FEIE   0x08u     /* FIFO error interrupt enable        */
#define ACI_CR_IOCE   0x10u     /* interrupt on completion enable     */

#define ACI_SAMPLE_RATE 48000u
#define ACI_CHANNELS    8u

static long long g_aci_last_us[ACI_CHANNELS];
static unsigned  g_aci_frac[ACI_CHANNELS];
static int       g_aci_dma_on = -1;

static int aci_dma_enabled(void)
{
    if (g_aci_dma_on < 0)
        /* On by default.  The title programs PCM-Out channel 1 with a
         * real BDBAR and CR=0x1D on both a console and here, and waits
         * on the completion interrupt rather than polling the channel,
         * so with the engine off nothing in the audio path advances.
         * CONKER_NO_APU_DMA turns it off for isolation; CONKER_APU_DMA
         * is still accepted and now redundant. */
        g_aci_dma_on = (getenv("CONKER_NO_APU_DMA") == NULL);
    return g_aci_dma_on;
}

static uint16_t aci_ld16(uint32_t off);
static uint32_t aci_ld32(uint32_t off);

/* The global-status bit each NABM channel contributes.
 *
 * Only PCM-Out is evidence-backed: a console reads GLOB_STA = 0x140 at the
 * audio ISR -- codec-ready plus bit 6 -- and channel 1, the block at +0x10, is
 * PCM-Out.  The ISR masks with 0x51, so bits 0 and 4 are audio sources too,
 * but nothing measured says which channels own them and guessing an MCPX
 * mapping out of the ICH datasheet would be inventing it.  They stay 0 until
 * something measures them.
 */
static uint32_t aci_channel_summary_bit(unsigned ch)
{
    return (ch == 1u) ? 0x40u : 0u;     /* PCM-Out -> GLOB_STA bit 6 */
}

/* The global status is derived, never stored.
 *
 * Each channel contributes its summary bit while it has an interrupt condition
 * latched in SR *and* that condition enabled in CR.  Because the value is
 * computed from the per-channel status, the guest clearing SR -- which the
 * write path already handles as write-one-to-clear -- deasserts the summary as
 * a consequence, with no second copy of the state to keep in step.  It is not
 * cleared on read, and it is never latched permanently.
 */
static uint32_t aci_global_status(void)
{
    uint32_t v = ACI_GS_PCR;            /* primary codec ready, always */
    unsigned ch;

    for (ch = 0; ch < ACI_CHANNELS; ch++) {
        uint32_t base = ACI_NABM_BASE + ch * 0x10u;
        uint16_t sr = aci_ld16(base + ACI_SR);
        uint8_t  cr = g_aci_regs[base + ACI_NABM_CR_LOW];
        int pending = 0;

        if ((sr & ACI_SR_BCIS)  && (cr & ACI_CR_IOCE))  pending = 1;
        if ((sr & ACI_SR_LVBCI) && (cr & ACI_CR_LVBIE)) pending = 1;
        if (pending)
            v |= aci_channel_summary_bit(ch);
    }
    return v;
}

static long long aci_now_us(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (long long)((c.QuadPart * 1000000LL) / f.QuadPart);
}

static uint32_t aci_ld32(uint32_t off)
{
    return (uint32_t)g_aci_regs[off] | ((uint32_t)g_aci_regs[off + 1] << 8) |
           ((uint32_t)g_aci_regs[off + 2] << 16) |
           ((uint32_t)g_aci_regs[off + 3] << 24);
}

static uint16_t aci_ld16(uint32_t off)
{
    return (uint16_t)(g_aci_regs[off] | (g_aci_regs[off + 1] << 8));
}

static void aci_st16(uint32_t off, uint16_t v)
{
    g_aci_regs[off] = (uint8_t)v;
    g_aci_regs[off + 1] = (uint8_t)(v >> 8);
}

/* Length in samples of descriptor `index` of this channel, from guest RAM. */
static uint16_t aci_desc_len(uint32_t base, uint8_t index)
{
    uint32_t bdbar = aci_ld32(base + 0x00u);
    uint32_t addr;

    /* MCPXAPUState is opaque here; apu_shim.h exports the RAM base. */
    if (g_apu_ram_ptr == NULL || bdbar == 0)
        return 0;
    addr = bdbar + (uint32_t)index * 8u + 4u;
    return apu_dma_read16(addr);
}

static void aci_dma_advance(uint32_t base, unsigned ch)
{
    long long now, elapsed;
    unsigned samples;
    uint16_t sr, picb;
    uint8_t civ, lvi;

    if (!aci_dma_enabled())
        return;
    /* TEMPORARY: is any channel actually running, and does anything advance? */
    {
        static unsigned n, seen_run;
        unsigned cr = g_aci_regs[base + ACI_NABM_CR_LOW];
        if (n < 12u) {
            if ((cr & ACI_CR_RPBM) && !seen_run) {
                seen_run = 1; ++n;
                fprintf(stderr, "[NABM] ch%u RUNNING cr=%02X bdbar=%08X "
                        "civ=%u lvi=%u picb=%u sr=%04X" "\n",
                        ch, cr, aci_ld32(base), g_aci_regs[base + ACI_CIV],
                        g_aci_regs[base + ACI_LVI], aci_ld16(base + ACI_PICB),
                        aci_ld16(base + ACI_SR));
                fflush(stderr);
            } else if (!(cr & ACI_CR_RPBM) && n < 4u) {
                ++n;
                fprintf(stderr, "[NABM] ch%u idle cr=%02X bdbar=%08X" "\n",
                        ch, cr, aci_ld32(base));
                fflush(stderr);
            }
        }
    }

    if (!(g_aci_regs[base + ACI_NABM_CR_LOW] & ACI_CR_RPBM)) {
        g_aci_last_us[ch] = 0;          /* stopped: restart the clock cleanly */
        return;
    }

    now = aci_now_us();
    if (g_aci_last_us[ch] == 0) {
        g_aci_last_us[ch] = now;
        return;
    }
    elapsed = now - g_aci_last_us[ch];
    if (elapsed <= 0)
        return;
    g_aci_last_us[ch] = now;

    /* Samples consumed, keeping the sub-sample remainder so a slow poll rate
     * does not quietly lose time. */
    {
        unsigned long long num = (unsigned long long)elapsed * ACI_SAMPLE_RATE
                                 + g_aci_frac[ch];
        samples = (unsigned)(num / 1000000ull);
        g_aci_frac[ch] = (unsigned)(num % 1000000ull);
    }
    if (samples == 0)
        return;

    sr   = aci_ld16(base + ACI_SR);
    picb = aci_ld16(base + ACI_PICB);
    civ  = g_aci_regs[base + ACI_CIV];
    lvi  = g_aci_regs[base + ACI_LVI];

    if (sr & ACI_SR_DCH)
        return;

    while (samples > 0) {
        if (picb == 0) {
            picb = aci_desc_len(base, civ);
            if (picb == 0) {           /* nothing described here: halt */
                sr |= ACI_SR_DCH;
                break;
            }
        }
        if (samples < picb) {
            picb = (uint16_t)(picb - samples);
            samples = 0;
        } else {
            samples -= picb;
            picb = 0;
            /* This buffer is finished.  Report it and step on. */
            sr |= ACI_SR_BCIS;
            if (civ == lvi) {
                sr |= ACI_SR_LVBCI | ACI_SR_CELV | ACI_SR_DCH;
                break;
            }
            civ = (uint8_t)((civ + 1u) & 0x1Fu);
            g_aci_regs[base + ACI_PIV] = (uint8_t)((civ + 1u) & 0x1Fu);
        }
    }

    g_aci_regs[base + ACI_CIV] = civ;
    aci_st16(base + ACI_PICB, picb);
    aci_st16(base + ACI_SR, sr);
}

/* Service every running channel on the audio clock.
 *
 * Advancement used to happen only for the channel whose registers the guest
 * was reading, which is fine for a driver that polls and useless for one that
 * waits on the completion interrupt -- which is what this title does.  The
 * engine now ticks from the APU frame loop for every active channel, and the
 * interrupt is asserted while any channel has an enabled condition latched.
 *
 * The vector is the audio one the APU already uses (Xbox IRQ 6 + 48).  Raising
 * it here is the ACI reporting its own condition; it is not derived from the
 * APU's ISTS, which was measured on a console reading 0 at the very moment the
 * ACI reported an interrupt.
 */
/* IRQ 6 -> vector 54.  The ACI is a different device from the APU, whose
 * interrupt is IRQ 5 / vector 53; see the note in apu_core.c. */
#define ACI_IRQ_VECTOR 54u   /* Xbox IRQ 6 + 48 */
extern void recomp_irq_raise(unsigned vector);

unsigned long long g_aci_ticks, g_aci_irq_raises;

void mcpx_aci_tick(void)
{
    unsigned ch;

    if (!aci_dma_enabled())
        return;
    ++g_aci_ticks;
    for (ch = 0; ch < ACI_CHANNELS; ch++)
        aci_dma_advance(ACI_NABM_BASE + ch * 0x10u, ch);

    /* Level-triggered: assert while the condition stands.  The guest clears it
     * by writing one to the channel's SR bits, which the write path already
     * honours, and the derived summary follows. */
    if (aci_global_status() & ~(uint32_t)ACI_GS_PCR) {
        ++g_aci_irq_raises;
        recomp_irq_raise(ACI_IRQ_VECTOR);
    }
}

/* Bring the addressed channel up to date before the guest reads it. */
static void aci_dma_sync(uint32_t off)
{
    unsigned ch;
    for (ch = 0; ch < ACI_CHANNELS; ch++) {
        uint32_t base = ACI_NABM_BASE + ch * 0x10u;
        if (off >= base && off < base + 0x10u) {
            aci_dma_advance(base, ch);
            return;
        }
    }
}

static void aci_write(uint32_t off, uint64_t val, unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        uint32_t a = off + i;
        uint8_t  b = (uint8_t)(val >> (8 * i));

        if (a >= ACI_MMIO_SIZE_BYTES)
            return;
        if (aci_is_channel_control(a))
            b &= (uint8_t)~ACI_CR_RR;   /* the reset has already completed */
        if (a == ACI_GLOBAL_CONTROL)
            b &= (uint8_t)~ACI_GC_WARM_RESET;   /* likewise, and instantly */
        if ((a & 0x0Fu) == ACI_SR && a >= ACI_NABM_BASE &&
            a < ACI_NABM_BASE + 0x80u) {
            /* Status interrupt bits are write-one-to-clear. */
            b = (uint8_t)(g_aci_regs[a] & ~b);
        }
        if (aci_is_channel_control(a) && (b & ACI_CR_RPBM) &&
            !(g_aci_regs[a] & ACI_CR_RPBM)) {
            unsigned ch = (a - ACI_NABM_BASE) / 0x10u;
            if (ch < ACI_CHANNELS) {
                g_aci_last_us[ch] = 0;
                g_aci_frac[ch] = 0;
            }
        }
        if (a < 0x130u) {
            extern void recomp_aci_wlog(uint32_t off, uint32_t val,
                                        uint32_t old);
            recomp_aci_wlog(a, b, g_aci_regs[a]);
        }
        g_aci_regs[a] = b;
    }
}

static uint64_t aci_read(uint32_t off, unsigned size)
{
    uint64_t val = 0;
    unsigned i;

    aci_dma_sync(off);

    for (i = 0; i < size; i++) {
        uint32_t a = off + i;
        uint8_t b;
        if (a >= ACI_MMIO_SIZE_BYTES)
            break;
        b = g_aci_regs[a];
        /* The whole global-status dword is derived, not stored. */
        if (a >= ACI_GLOBAL_STATUS && a < ACI_GLOBAL_STATUS + 4u)
            b = (uint8_t)(aci_global_status() >>
                          (8u * (a - ACI_GLOBAL_STATUS)));
        val |= (uint64_t)b << (8 * i);
    }
    return val;
}

static void apu_mmio_dispatch_write(uint32_t off, uint64_t val, unsigned size)
{
    if (g_target_aci)
        aci_write(off, val, size);
    else
        mcpx_apu_mmio_write(g_apu_state, off, val, size);
}

static uint64_t apu_mmio_dispatch_read(uint32_t off, unsigned size)
{
    if (g_target_aci)
        return aci_read(off, size);
    return mcpx_apu_mmio_read(g_apu_state, off, size);
}

static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    if (!g_target_aci && !g_apu_state) return false;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0, has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* MOV r/m, r (write: 88/89) */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);
        apu_mmio_dispatch_write(mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m, imm32 (write: C7 /0) */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint32_t imm = *(uint32_t *)(opcode + 1 + modrm_len);
        apu_mmio_dispatch_write(mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + 4;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m8, imm8 (write: C6 /0) */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);
        apu_mmio_dispatch_write(mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r, r/m (read: 8A/8B) */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = apu_mmio_dispatch_read(mmio_offset, access_size);
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (access_size == 1) *dst = (*dst & ~0xFFULL) | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFF;
        else *dst = val;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m8 (0F B6) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = apu_mmio_dispatch_read(mmio_offset, 1);
        *ctx_reg64(ctx, reg) = val & 0xFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m16 (0F B7) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        access_size = 2;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = apu_mmio_dispatch_read(mmio_offset, 2);
        *ctx_reg64(ctx, reg) = val & 0xFFFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* TEST r/m, r (84/85) - read */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = apu_mmio_dispatch_read(mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* CMP r/m, r (38/39) */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = apu_mmio_dispatch_read(mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }
        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (mem_val < reg_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* OR r/m, r (08/09) */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = apu_mmio_dispatch_read(mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        apu_mmio_dispatch_write(mmio_offset, mem_val | reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* AND r/m, r (20/21) */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = apu_mmio_dispatch_read(mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        apu_mmio_dispatch_write(mmio_offset, mem_val & reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* Unrecognized */
    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    g_target_aci = false;
    return apu_decode_and_handle(ctx, mmio_offset, is_write);
}

bool aci_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - ACI_MMIO_BASE;
    bool handled;

    (void)fault_addr;
    g_target_aci = true;
    handled = apu_decode_and_handle(ctx, mmio_offset, is_write);
    g_target_aci = false;
    return handled;
}

#endif /* _WIN32 */
