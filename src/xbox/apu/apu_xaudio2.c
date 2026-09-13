/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#include "apu_regs.h"

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2

/* One submission carries exactly the interval the VP just advanced.
 *
 * This used to be 1024 samples while mcpx_apu_monitor_frame ran once per 8 VP
 * frames -- 8 * NUM_SAMPLES_PER_FRAME = 256 samples of emulated audio.  The
 * device drained the 1024 in real time and refused further submissions until
 * it had, and the frame thread waits on that refusal, so the VP was paced to
 * a quarter of real speed: 392 Hz measured against the 1500 Hz that
 * 48000/NUM_SAMPLES_PER_FRAME requires.  Every downstream symptom -- audio
 * packets retiring 4x late, [decoder+0xC8] growing without bound -- followed
 * from that one ratio. */
#define XA2_BUF_SAMPLES   APU_MONITOR_SAMPLES   /* 256, 5.33 ms */

/* Queue depth is expressed in samples so the submission size can change
 * without changing how much audio is buffered ahead of the device.  3072
 * samples is the ~64 ms the old 3 x 1024 ring held. */
#define XA2_QUEUE_SAMPLES 3072
#define XA2_NUM_BUFS      (XA2_QUEUE_SAMPLES / XA2_BUF_SAMPLES)

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

/* Source-voice starvation is distinct from gaps in submitted PCM. XAudio2
 * reports the bytes missing for its next processing pass in this callback.
 * Counters only: never log, allocate or wait on the audio engine thread. */
volatile LONG g_xa2_short_passes, g_xa2_missing_bytes, g_xa2_device_passes;
static volatile LONG g_xa2_output_started;
static void STDMETHODCALLTYPE xa2_pass_start(IXAudio2VoiceCallback *self, UINT32 required)
{
    (void)self;
    if (!InterlockedCompareExchange(&g_xa2_output_started, 0, 0)) return;
    InterlockedIncrement(&g_xa2_device_passes);
    if (required) {
        InterlockedIncrement(&g_xa2_short_passes);
        InterlockedExchangeAdd(&g_xa2_missing_bytes, (LONG)required);
    }
}
static void STDMETHODCALLTYPE xa2_pass_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE xa2_stream_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE xa2_buffer_event(IXAudio2VoiceCallback *self, void *context) { (void)self; (void)context; }
static void STDMETHODCALLTYPE xa2_voice_error(IXAudio2VoiceCallback *self, void *context, HRESULT error) { (void)self; (void)context; (void)error; }
static IXAudio2VoiceCallbackVtbl g_xa2_callbacks = {
    xa2_pass_start, xa2_pass_end, xa2_stream_end, xa2_buffer_event,
    xa2_buffer_event, xa2_buffer_event, xa2_voice_error
};
static IXAudio2VoiceCallback g_xa2_callback = { &g_xa2_callbacks };

int xa2_init(void)
{
    HRESULT hr;
    WAVEFORMATEX wfx = { 0 };

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != (HRESULT)0x80010106 /* RPC_E_CHANGED_MODE */ && hr != S_FALSE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        return 0;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
        return 0;
    }

    /* Launcher output volume; do not mute the emulated APU or disturb its
     * completion timing. No setting preserves the existing unity gain. */
    {
        const char *setting = getenv("CONKER_VOLUME");
        if (setting && *setting) {
            char *end;
            long percent = strtol(setting, &end, 10);
            if (end != setting && !*end && percent >= 0 && percent <= 100) {
                hr = g_xa2_master->lpVtbl->SetVolume(g_xa2_master,
                    (float)percent / 100.0f, XAUDIO2_COMMIT_NOW);
                if (FAILED(hr))
                    fprintf(stderr, "[XA2] Output volume failed: 0x%08lX\n", hr);
            } else {
                fprintf(stderr, "[XA2] CONKER_VOLUME expects an integer from 0 to 100; using 100\n");
            }
        }
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &g_xa2_callback, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
        return 0;
    }

    IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_xa2_initialized) return;

    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
/* Measurement only: the frame thread paces itself against this queue, so
 * its depth and refusal rate are the first thing to rule in or out. */
unsigned long long g_xa2_attempts, g_xa2_accepted, g_xa2_full;
unsigned long long g_xa2_audible, g_xa2_silent;
int g_xa2_peak;
unsigned g_xa2_depth, g_xa2_depth_max;

unsigned xa2_queue_depth(void) { return g_xa2_depth; }
unsigned xa2_queue_cap(void)   { return (unsigned)XA2_NUM_BUFS; }

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    ++g_xa2_attempts;
    g_xa2_depth = (unsigned)state.BuffersQueued;
    if (g_xa2_depth > g_xa2_depth_max) g_xa2_depth_max = g_xa2_depth;
    { extern void recomp_hz_depth(unsigned);
      recomp_hz_depth(g_xa2_depth); }
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) { ++g_xa2_full; return 0; }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    {   /* Measure the actual VP/DSP + optional host mix sent to the device. */
        int _i, _pk = 0;
        for (_i = 0; _i < copy_samples * XA2_CHANNELS; ++_i) {
            int _v = samples[_i]; if (_v < 0) _v = -_v;
            if (_v > _pk) _pk = _v;
        }
        if (_pk > g_xa2_peak) g_xa2_peak = _pk;
        if (_pk > 64) ++g_xa2_audible; else ++g_xa2_silent;
    }
    IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    ++g_xa2_accepted;
    g_xa2_frames_written++;
    if (g_xa2_frames_written == 128) InterlockedExchange(&g_xa2_output_started, 1);
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }

#endif /* _WIN32 */
