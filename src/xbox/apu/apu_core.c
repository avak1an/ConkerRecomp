/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
/* getenv returns char*.  Without this header C treats it as returning
 * int, which on x64 truncates the pointer to 32 bits and sign-extends
 * it -- a wild address that only faults once something dereferences
 * the result rather than just testing it against NULL. */
#include <stdlib.h>
#include "apu.h"
#include "apu_xaudio2.h"
#include "fpconv.h"

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
/* not static: recomp_dump_as reports it alongside the output content */
volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

/* The title connects two device interrupts: vector 51 for the GPU and vector
 * 49, routine 0x0055FB7A, for this device.  49 had never fired once, because
 * pci_irq_assert() is a no-op in standalone and nothing consumed d->set_irq.
 *
 * That is what stalls the boot video.  The XMV update refuses to publish a
 * decoded frame while any audio packet is still E_PENDING, the packet is
 * retired by the guest's own audio ISR, and the ISR only runs when this
 * interrupt is delivered.  On a console the status retires between one check
 * and the next -- E_PENDING on 2 of 90 reads -- against 1,167 of 1,171 here.
 *
 * Nothing about that is XMV-specific and nothing here knows about video: the
 * APU raises its interrupt exactly when its own ISTS/IEN condition says to,
 * the guest's ISR does the retiring, and no host code touches a guest
 * structure.  Delivery rides the safe points already built for vblank.
 *
 * CONKER_APU_IRQ enables it, off by default until the result is checked. */
extern void recomp_irq_raise(unsigned vector);

/* Xbox device interrupts are vector = IRQ + 48.
 *
 * The MCPX is two separate devices behind two separate IRQs, and they have
 * separate handlers:
 *
 *   IRQ 5 -> vector 53   APU   (the audio processor: voice engine, DSPs,
 *                               NV_PAPU_ISTS at 0xFE801000)
 *   IRQ 6 -> vector 54   ACI   (the AC97 codec interface: NABM channels,
 *                               GLOB_STA at 0xFEC00130)
 *
 * This file models the APU, so its interrupt is 53.  It was 54, which
 * delivered every APU interrupt to the ACI handler -- a different device
 * object with a different ISR -- so the APU handler was never entered and
 * nothing it drives ever ran.  Conker connects the two exactly this way,
 * routine 0x004C86A0 on 53 and 0x004CE6DA on 54, but nothing here depends on
 * that: it is the MCPX IRQ mapping, not a per-title arrangement.
 *
 * (49 was an earlier wrong guess.  The title connects 0x0055FB7A there and it
 * looked like audio because it was the only other device interrupt in the
 * log, but it reads HcInterruptStatus at 0xFED00000 -- it is the OHCI USB
 * handler, and its constant traffic is 1 kHz OHCI frame interrupts.)
 */
#define XBOX_IRQ_TO_VECTOR(irq) ((irq) + 48u)
#define XBOX_IRQ_MCPX_APU  5u
#define APU_IRQ_VECTOR     XBOX_IRQ_TO_VECTOR(XBOX_IRQ_MCPX_APU)   /* 53 */

/* On by default now that the delivery path is complete.  CONKER_APU_IRQ=0
 * turns it off again, which is worth keeping: it gates the sequencer unpause
 * below as well, so it is the single switch that reverts the APU to the old
 * parked behaviour if something regresses. */
static int apu_irq_enabled(void)
{
    static int checked, enabled;
    if (!checked) {
        const char *e = getenv("CONKER_APU_IRQ");
        checked = 1;
        enabled = !(e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N'));
    }
    return enabled;
}

unsigned long long g_apu_irq_calls, g_apu_irq_asserts, g_apu_notifies;

/* d->frame_count is zeroed every second by the debug FPS calc, so it cannot
 * be differenced.  This one only ever counts up. */
unsigned long long g_apu_vp_frames;

/* The device drives a level, the host machinery takes an edge.
 *
 * recomp_irq_raise() sets a bit that one safe-point drain consumes, so it is
 * an edge.  NV_PAPU_ISTS is a level: it stays set until the guest's ISR writes
 * the bits back to acknowledge.  Raising on every update_irq() while the line
 * is still asserted would re-enter the ISR forever, so the transition is
 * latched here and the line is only re-raised after it has gone away.
 *
 * Nothing else acknowledges: the deassert comes from the ISTS write in
 * mcpx_apu_write() calling back into update_irq(), which is the guest's own
 * acknowledge path. */
static int g_apu_irq_level;
/* Re-arm point: the sequence number of the vector-53 delivery that consumed
 * the last raise.  See update_irq() for why the latch cannot key off the
 * condition alone. */
static unsigned long long g_apu_irq_armed_at;
extern unsigned long long g_vec_delivered[64];
int g_apu_irq_level_get(void) { return g_apu_irq_level; }
unsigned long long g_apu_irq_raises, g_apu_irq_edges;

/* Measurement only: where the frame thread's wall time goes.  All in
 * QPC ticks, converted once at report time. */
unsigned long long g_apu_iters, g_apu_t_iter, g_apu_t_locked;
unsigned long long g_apu_t_lockwait, g_apu_t_backpressure;
unsigned long long g_apu_backpressure_loops;
unsigned long long g_apu_idle_entries, g_apu_t_idle;

/* Identity and coarse phase of the frame thread, for the APU stall
 * watchdog.  Phase is a plain integer store with no ordering requirements:
 * it is only ever read by another thread after that thread has suspended
 * this one, so a torn or stale value is not possible in the way that
 * matters.  Nothing here changes what the thread does. */
unsigned long long g_apu_thread_id;
unsigned long long g_apu_last_frame_qpc;
unsigned g_apu_phase;      /* see apu_phase_name() in main.c */
#define APU_PHASE(n) do { g_apu_phase = (n); } while (0)

#include "../platform/hr_period.h"

static unsigned long long apu_qpc(void)
{
    LARGE_INTEGER v; QueryPerformanceCounter(&v);
    return (unsigned long long)v.QuadPart;
}

double apu_qpc_ms_scale(void)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return f.QuadPart ? 1000.0 / (double)f.QuadPart : 0.0;
}

static void update_irq(MCPXAPUState *d)
{
    ++g_apu_irq_calls;
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        pci_irq_assert(PCI_DEVICE(d));   /* no-op without a PCI bus */
        ++g_apu_irq_asserts;
        /* Latching purely on "the condition became true" deadlocks.
         *
         * The guest ISR acknowledges by writing back the ISTS bits it read,
         * but this thread sets FENINTSTS/FEVINTSTS whenever a voice finishes a
         * buffer.  A voice that finishes between the ISR's read and its write
         * leaves a bit standing, so ISTS never returns to zero, the condition
         * never goes false, the latch never cleared -- and the line was never
         * raised again.  Measured: lvl stuck at 1 with ISTS=00000061 while
         * update_irq kept asserting, deliveries frozen, every packet left
         * pending and video decode stopped.
         *
         * A level-triggered line re-interrupts while it is still asserted, so
         * re-arm once the previous raise has actually been delivered.  That is
         * at most one raise per delivery -- still no storm, because delivery
         * only happens at safe points -- and it stops as soon as the guest's
         * acknowledge really does clear ISTS. */
        if (!g_apu_irq_level ||
            g_vec_delivered[APU_IRQ_VECTOR] != g_apu_irq_armed_at) {
            g_apu_irq_level = 1;
            g_apu_irq_armed_at = g_vec_delivered[APU_IRQ_VECTOR];
            ++g_apu_irq_edges;
            if (apu_irq_enabled()) {
                ++g_apu_irq_raises;
                recomp_irq_raise(APU_IRQ_VECTOR);
            }
        }
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        pci_irq_deassert(PCI_DEVICE(d));
        g_apu_irq_level = 0;
    }
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    if (addr == NV_PAPU_IEN || addr == NV_PAPU_ISTS) {
        static unsigned n;
        if (n < 16u && apu_irq_enabled()) {
            ++n;
            fprintf(stderr, "[APU-REG] %s <- %08X   (ISTS=%08X IEN=%08X)" "\n",
                    addr == NV_PAPU_IEN ? "IEN " : "ISTS", (unsigned)val,
                    (unsigned)qatomic_read(&d->regs[NV_PAPU_ISTS]),
                    (unsigned)qatomic_read(&d->regs[NV_PAPU_IEN]));
            fflush(stderr);
        }
    }

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Writing these is how the guest starts the sequencer, and it is the
         * same condition the frame thread already calls apu_active.  Until now
         * only the debug test tone ever cleared pause_requested, so the thread
         * parked itself at init and stayed there: se_frame never ran, no voice
         * ever finished a buffer, set_notify_status was never called, and the
         * interrupt this device exists to raise had nothing to raise it for.
         * The broadcast below woke the thread, which re-checked the flag and
         * went straight back to sleep. */
        if (apu_irq_enabled()) {
            int xcnt = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
            uint32_t fec = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            if (xcnt != NV_PAPU_SECTL_XCNTMODE_OFF &&
                !(fec & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                !(fec & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
                d->pause_requested = false;
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 12
#define WAVEOUT_BUF_SAMPLES APU_MONITOR_SAMPLES /* 256 samples per 8 VP frames */
#define XA2_SAMPLE_RATE_HZ  48000

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

/* Submit either a completed guest block or an independent host-only block. */
static void apu_submit_audio(MCPXAPUState *d, int16_t output[APU_MONITOR_SAMPLES][2])
{
    const size_t output_bytes = APU_MONITOR_SAMPLES * 2u * sizeof(int16_t);
    if (g_audio_muted) {
        memset(output, 0, output_bytes);
    } else {
        if (g_test_tone.active) {
            for (int i = 0; i < APU_MONITOR_SAMPLES; i++) {
                int s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                for (int ch = 0; ch < 2; ch++) {
                    int mixed = output[i][ch] + s;
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    output[i][ch] = (int16_t)mixed;
                }
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }
        mixer_render(output, APU_MONITOR_SAMPLES);
    }

    /* Both backends submit exactly the interval advanced by the VP. */
    if (xa2_is_active()) {

        /* Pace the frame loop against the audio device, and wait
         * outside the lock -- the same shape as the waveOut path below.
         *
         * xa2_submit_samples drops the frame and returns 0 the moment
         * its queue is full, so on its own it applies no back-pressure
         * at all: the frame thread then spun as fast as the CPU allowed
         * with d->lock held, and every guest APU MMIO write -- which
         * needs that same lock through voice_lock -- starved.  That is
         * the wedge the stall watchdog kept catching.
         *
         * Releasing the lock while waiting is the point: holding it
         * across the sleep would pace the loop and still starve the
         * guest.  The 50-iteration cap matches waveOut, so a stalled or
         * absent audio device slows playback rather than hanging the
         * emulator. */
        { int wait_loops = 0;
          APU_PHASE(6);
          while (!xa2_submit_samples((const int16_t *)output,
                                     APU_MONITOR_SAMPLES)) {
              APU_PHASE(7);   /* in back-pressure wait */
              unsigned long long b0 = apu_qpc();
              qemu_mutex_unlock(&d->lock);
              Sleep(1);
              qemu_mutex_lock(&d->lock);
              g_apu_t_backpressure += apu_qpc() - b0;
              ++g_apu_backpressure_loops;
              if (++wait_loops > 50) break;
          } }
        return;
    }

    if (!g_waveout.initialized) return;

    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];

    /* Wait if this buffer is still playing (with timeout) */
    int wait_loops = 0;
    while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
        qemu_mutex_unlock(&d->lock);
        Sleep(1);
        qemu_mutex_lock(&d->lock);
        if (++wait_loops > 50) return; /* Never overwrite a queued buffer. */
    }

    memcpy(g_waveout.bufs[idx], output, output_bytes);

    /* Submit to waveOut */
    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));

    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % APU_MONITOR_FRAME_DIV) {
        return;
    }

    /* The VP/DSP has already assembled eight 32-sample slices. Preserve
     * that block, mix optional host voices into it, then clear the assembly
     * buffer for the next interval (including VP's additive monitor path).
     * Clearing before the copy discarded every guest audio sample. */
    int16_t output[APU_MONITOR_SAMPLES][2];
    memcpy(output, d->monitor.frame_buf, sizeof(output));
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    apu_submit_audio(d, output);
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

/* TEMPORARY: is the throttle actually pacing at EP_FRAME_US?
 *
 * The wait below is SleepConditionVariableCS with a millisecond timeout and
 * nothing in the tree raises the system timer resolution, so a request for
 * 1-5 ms can return anywhere up to a scheduler tick later.  When it does, the
 * "more than one frame behind" test at the top rebases next_frame_time_us to
 * now instead of catching up, and the overshoot becomes permanent.
 *
 * These count that without altering a single timing decision.
 */
unsigned long long g_thr_calls, g_thr_rebases, g_thr_waits;
unsigned long long g_thr_req_us, g_thr_act_us;
unsigned long long g_thr_spins;
unsigned g_thr_worst_us;

/* The monitor stage emits APU_MONITOR_SAMPLES every time it runs, so the
 * frame loop has to run at exactly that many samples per second. */
#define APU_MONITOR_RATE_HZ ((double)XA2_SAMPLE_RATE_HZ / (double)APU_MONITOR_SAMPLES)

/* Pace the frame loop on an absolute deadline.
 *
 * This used to compute a millisecond remainder and hand it to
 * qemu_cond_timedwait -- SleepConditionVariableCS, whose timeout rounds up to
 * the scheduler tick.  Asking for the 5.33 ms an EP frame is worth returned
 * far later, and because a tick more than one EP_FRAME_US late rebased the
 * target to "now" instead of catching up, the overshoot became permanent:
 * 1.71x measured, so the VP settled near 1376 Hz instead of 1500.
 *
 * hr_period keeps the deadline absolute, so jitter on one tick comes out of
 * the next, and waits on a timer that is not quantised to the scheduler tick.
 * It still resyncs rather than bursting when a whole period is missed.
 *
 * The lock is dropped around each wait: holding d->lock across the sleep is
 * what starved guest MMIO writes and wedged the boot, so the wait is driven a
 * step at a time here rather than by hr_period_wait().
 */
static hr_period g_apu_period;
static int g_apu_period_ready;

static void throttle(MCPXAPUState *d, unsigned phase)
{
    long long remaining;
    int64_t t0_us;
    unsigned long long waited = 0;   /* spins in the tail margin */

    if (phase % APU_MONITOR_FRAME_DIV) {
        return;
    }
    ++g_thr_calls;

    if (!g_apu_period_ready) {
        hr_period_init(&g_apu_period, APU_MONITOR_RATE_HZ);
        g_apu_period_ready = 1;
        fprintf(stderr, "[APU] frame pacing %.4f ms (%.1f Hz), %s timer" "\n",
                1000.0 / APU_MONITOR_RATE_HZ, APU_MONITOR_RATE_HZ,
                g_apu_period.high_res ? "high-resolution"
                                      : "DEFAULT-RESOLUTION (drifts)");
    }

    t0_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    hr_period_advance(&g_apu_period);

    while (!d->pause_requested &&
           (remaining = hr_period_remaining(&g_apu_period)) > 0) {
        if (!waited) {
            ++g_thr_waits;
            g_thr_req_us += (unsigned long long)
                            ((remaining * 1000000LL) / g_apu_period.qpc_freq);
        }
        ++waited;
        qemu_mutex_unlock(&d->lock);
        hr_period_wait_once(&g_apu_period, remaining);
        qemu_mutex_lock(&d->lock);
    }
    g_thr_rebases = g_apu_period.resyncs;
    g_thr_spins += waited ? waited - 1u : 0u;

    {   int64_t slept = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0_us;
        if (slept > 0) {
            g_thr_act_us += (unsigned long long)slept;
            if ((unsigned)slept > g_thr_worst_us) g_thr_worst_us = (unsigned)slept;
            d->sleep_acc_us += (int)slept;
        } }
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

/* TEMPORARY: is the voice processor actually consuming samples at 48 kHz?
 *
 * sub_004C8EED, the DirectSound retirement routine, has no timing source of
 * its own.  It spins on NV1BA0_PIO_FREE (0xFE820010) for FIFO space, writes
 * SET_CURRENT_VOICE / SET_VOICE_SSL_A / SET_VOICE_SSL_B, and completes the
 * packets belonging to the buffer half that just finished.  It is called
 * from the DPC that the APU interrupt queues, so the retirement rate is
 * exactly the rate at which voices finish SSL buffer halves -- which is set
 * by how fast the VP consumes samples, and nothing else.
 *
 * Design intent: throttle() waits once every APU_MONITOR_FRAME_DIV (8) VP
 * frames at APU_MONITOR_RATE_HZ (187.5 Hz), so 8 x 32 = 256 samples per
 * 5.3333 ms = 48000 samples/s.  This measures what actually happens. */
static void apu_rate_report(MCPXAPUState *d)
{
    static double freq;
    static unsigned long long t0, f0, n0, r0, c0, i0, u0, a0;
    unsigned long long now = apu_qpc();
    double dt;
    if (freq == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (double)f.QuadPart;
        t0 = now; f0 = g_apu_vp_frames;
        n0 = g_apu_notifies; r0 = g_apu_irq_raises;
        c0 = g_thr_calls; i0 = g_apu_iters;
        u0 = g_apu_irq_calls; a0 = g_apu_irq_asserts;
        return;
    }
    dt = (double)(now - t0) / freq;
    if (dt < 2.0) return;
    {
        unsigned long long fr = g_apu_vp_frames;
        double vp   = (double)(fr - f0) / dt;
        double smp  = vp * (double)NUM_SAMPLES_PER_FRAME;
        fprintf(stderr,
                "[APU-RATE] VP %.1f frames/s = %.0f samples/s (expected %d, ratio %.2fx)" "\n"
                "[APU-RATE]   loop %.1f/s  throttle %.1f/s  notifies %.2f/s  raises %.2f/s" "\n"
                "[APU-RATE]   update_irq %.1f/s  asserts %.1f/s  level=%d  ISTS=%08X IEN=%08X" "\n",
                vp, smp, XA2_SAMPLE_RATE_HZ,
                smp > 0.0 ? (double)XA2_SAMPLE_RATE_HZ / smp : 0.0,
                (double)(g_apu_iters - i0) / dt,
                (double)(g_thr_calls - c0) / dt,
                (double)(g_apu_notifies - n0) / dt,
                (double)(g_apu_irq_raises - r0) / dt,
                (double)(g_apu_irq_calls - u0) / dt,
                (double)(g_apu_irq_asserts - a0) / dt,
                g_apu_irq_level,
                (unsigned)qatomic_read(&d->regs[NV_PAPU_ISTS]),
                (unsigned)qatomic_read(&d->regs[NV_PAPU_IEN]));
        fflush(stderr);
        t0 = now; f0 = fr; n0 = g_apu_notifies; r0 = g_apu_irq_raises;
        c0 = g_thr_calls; i0 = g_apu_iters;
        u0 = g_apu_irq_calls; a0 = g_apu_irq_asserts;
    }
}
static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;
    ++g_apu_vp_frames;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

static bool apu_pipeline_active(MCPXAPUState *d)
{
    int mode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]), NV_PAPU_SECTL_XCNTMODE);
    uint32_t fe = qatomic_read(&d->regs[NV_PAPU_FECTL]);
    return mode != NV_PAPU_SECTL_XCNTMODE_OFF &&
           !(fe & (NV_PAPU_FECTL_FEMETHMODE_TRAPPED | NV_PAPU_FECTL_FEMETHMODE_HALTED));
}

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    g_apu_thread_id = (unsigned long long)GetCurrentThreadId();
    qemu_mutex_lock(&d->lock);
    bool frame_paced = false;

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            unsigned long long i0 = apu_qpc();
            ++g_apu_idle_entries;
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            g_apu_t_idle += apu_qpc() - i0;
            continue;
        }

        bool apu_active = apu_pipeline_active(d);

        /* The ACI is a separate device from the VP: it runs whether or
         * not the APU pipeline is active, so it ticks unconditionally
         * here rather than inside the apu_active branch below. */
        apu_rate_report(d);
        { extern void mcpx_aci_tick(void); mcpx_aci_tick(); }
        { extern void recomp_gen_tick(void); recomp_gen_tick(); }

        /* TEMPORARY: is the pipeline that would notify even running? */
        { static unsigned nf, nact, nirq, nrep;
          ++nf;
          if (apu_active) ++nact;
          if (d->set_irq) ++nirq;
          if (nrep < 20u && (nf % 500u) == 0u && apu_irq_enabled()) {
              ++nrep;
              fprintf(stderr, "[APU-IRQ] frames=%u active=%u set_irq=%u  "
                      "SECTL=%08X FECTL=%08X ISTS=%08X IEN=%08X  "
                      "voicelists 2D=%04X 3D=%04X MP=%04X" "\n",
                      nf, nact, nirq,
                      (unsigned)qatomic_read(&d->regs[NV_PAPU_SECTL]),
                      (unsigned)qatomic_read(&d->regs[NV_PAPU_FECTL]),
                      (unsigned)qatomic_read(&d->regs[NV_PAPU_ISTS]),
                      (unsigned)qatomic_read(&d->regs[NV_PAPU_IEN]),
                      (unsigned)qatomic_read(&d->regs[NV_PAPU_TVL2D]),
                      (unsigned)qatomic_read(&d->regs[NV_PAPU_TVL3D]),
                      (unsigned)qatomic_read(&d->regs[NV_PAPU_TVLMP]));
              fprintf(stderr, "[APU-IRQ]   update_irq calls=%llu asserts=%llu  "
                      "notifies=%llu raises=%llu edges=%llu level=%d" "\n",
                      g_apu_irq_calls, g_apu_irq_asserts, g_apu_notifies,
                      g_apu_irq_raises, g_apu_irq_edges, g_apu_irq_level);
              fflush(stderr);
          } }

        if (apu_active && !g_test_tone.active) {
            if (!frame_paced) {
                throttle(d, (unsigned)d->ep_frame_div);
                frame_paced = true;
            }
            /* throttle releases the lock. A guest halt during that wait must
             * not consume a DSP slice or advance the ping-pong buffer phase. */
            if (d->pause_requested || !apu_pipeline_active(d)) continue;
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            APU_PHASE(3); se_frame(d); APU_PHASE(2);
            frame_paced = false;
            /* set_notify_status() raises d->set_irq when a voice finishes a
             * buffer and it has written the notification into guest memory.
             * Nothing consumed that flag, so the condition update_irq()
             * computes was never evaluated and the interrupt never asserted --
             * the notification sat in memory with no one told to read it. */
            if (d->set_irq) {
                d->set_irq = false;
                update_irq(d);
            }
        } else if (g_test_tone.active || g_mixer_active_count) {
            /* Host diagnostics have their own completed block. Never clear
             * a partial guest block or advance stopped GP/EP programs. */
            int16_t host_output[APU_MONITOR_SAMPLES][2] = {0};
            throttle(d, 0);
            apu_submit_audio(d, host_output);
            frame_paced = false;
        } else {
            if (d->set_irq) {
                d->set_irq = false;
                update_irq(d);
            }
            qemu_cond_timedwait(&d->cond, &d->lock, 1);
        }

        /* Give everyone else a turn at the device lock.
         *
         * This thread takes d->lock once, before the loop, and holds it
         * for the rest of its life.  The only releases are the idle wait
         * above and the waveOut backend's "buffer still playing" sleep --
         * and the XAudio2 path returns before reaching that, so with
         * XAudio2 selected there is no release at all.  A guest thread
         * blocked in voice_lock -> qemu_mutex_lock then never got in:
         * Windows critical sections are not FIFO, so a holder that
         * unlocks and immediately re-locks keeps on winning.
         *
         * The stall watchdog caught exactly that -- this thread inside
         * voice_get_samples, the guest waiting in mcpx_apu_vp_write --
         * and it is why the boot wedged in roughly one run in three.
         *
         * Yielding between the unlock and the re-lock is what makes the
         * window real rather than nominal: without it the waiter is woken
         * but loses the race back to the lock.
         */
        { unsigned long long t0, t1;
          t0 = apu_qpc();
          qemu_mutex_unlock(&d->lock);
          SwitchToThread();
          qemu_mutex_lock(&d->lock);
          t1 = apu_qpc();
          g_apu_t_lockwait += t1 - t0;
          g_apu_last_frame_qpc = t1;
          APU_PHASE(1);       /* top of loop */
          ++g_apu_iters;
          { static unsigned long long prev;
            if (prev) {
                g_apu_t_iter += t1 - prev;
                g_apu_t_locked += t0 - prev;
            }
            prev = t1; } }
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        dsp_reset(d->gp.dsp);
        dsp_invalidate_opcache(d->gp.dsp);
        memset(d->gp.regs, 0, sizeof(d->gp.regs));
    }
    if (d->ep.dsp) {
        dsp_reset(d->ep.dsp);
        dsp_invalidate_opcache(d->ep.dsp);
        memset(d->ep.regs, 0, sizeof(d->ep.regs));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init programmable GP and EP. Their bootstrap comes from guest RAM. */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    mcpx_apu_update_dsp_preference(d);

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);
    dsp_destroy(d->gp.dsp);
    dsp_destroy(d->ep.dsp);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    }
    if (is_write && addr >= 0x30000 && addr < 0x40000) {
        mcpx_apu_dsp_write(d, true, (uint32_t)addr - 0x30000, (uint32_t)val, size);
    } else if (is_write && addr >= 0x50000 && addr < 0x60000) {
        mcpx_apu_dsp_write(d, false, (uint32_t)addr - 0x50000, (uint32_t)val, size);
    }
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

/* TEMPORARY: which APU registers the guest polls, and how often.
 *
 * The boot video is gated on an audio packet retiring from E_PENDING, and the
 * DSP path makes 109 references to APU MMIO, so the guest drives the hardware
 * itself rather than being handed completions.  This says which register it
 * waits on -- the one to make advance. */
static void apu_trace(uint64_t addr, uint64_t val, int is_write)
{
    static struct { unsigned off, reads, writes; } t[64];
    static unsigned n, calls;
    unsigned i;

    /* Cached: this runs on every APU MMIO access with the device lock
     * held, and an uncached Debug-CRT getenv there is what wedged the
     * guest.  See the note on tv_on() in apu_vp.c. */
    { static int on = -1;
      if (on < 0) on = getenv("CONKER_APU_TRACE") != NULL;
      if (!on) return; }
    for (i = 0; i < n; ++i)
        if (t[i].off == (unsigned)addr) break;
    if (i == n && n < 64) { t[n].off = (unsigned)addr; ++n; }
    if (i < 64) { if (is_write) ++t[i].writes; else ++t[i].reads; }

    if ((++calls % 20000u) == 0u) {
        fprintf(stderr, "[APU-TRACE] %u accesses, %u distinct registers:" "\n", calls, n);
        for (i = 0; i < n; ++i)
            fprintf(stderr, "[APU-TRACE]   +0x%05X  r=%-8u w=%u" "\n",
                    t[i].off, t[i].reads, t[i].writes);
        fflush(stderr);
    }
}

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    apu_trace(addr, 0, 0);
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    } else if (addr >= 0x30000 && addr < 0x40000) {
        return mcpx_apu_dsp_read(d, true, (uint32_t)addr - 0x30000, size);
    } else if (addr >= 0x50000 && addr < 0x60000) {
        return mcpx_apu_dsp_read(d, false, (uint32_t)addr - 0x50000, size);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    apu_trace(addr, val, 1);
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    g_mixer_voices[slot].active = 0;
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || v->pcm_bytes == 0) return;
    v->looping = looping;
    v->play_offset = 0;
    v->active = 1;
    InterlockedIncrement((volatile LONG *)&g_mixer_active_count);

    /* Wake up APU thread if it was paused */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * play_offset is stored as a 16.16 fixed-point source frame position. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint32_t inc = (uint32_t)(((uint64_t)voice->sample_rate << 16) / 48000);
        uint32_t pos = voice->play_offset; /* 16.16 fixed-point */
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos = 0;
                    src_frame = 0;
                } else {
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = 0;
            } else if (voice->active) {
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
}
