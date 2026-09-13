/**
 * D3D8→D3D11 Compatibility Device Implementation
 *
 * Implements the Xbox D3D8 IDirect3DDevice8 interface using D3D11.
 * The game's translated RenderWare code calls D3D8 methods through
 * COM vtables; this layer translates those calls to D3D11 equivalents.
 *
 * Architecture:
 * - D3D11 device and swap chain created during initialization
 * - Render state tracking: D3D8 states mapped to D3D11 state objects
 * - Texture/buffer management: D3D8 resource handles wrap D3D11 resources
 * - Fixed-function pipeline: emulated via D3D11 shaders (the Xbox D3D8
 *   pipeline is configurable but not fully programmable)
 *
 * Build: Requires Windows SDK with d3d11.h and dxgi.h
 */

#include "d3d8_internal.h"
#include "d3d8_nv2a_gpu.h"
#include <d3dcompiler.h>
#include "../nv2a/nv2a_pgraph_d3d11.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* Recompiled game code passes Xbox virtual addresses to the D3D8 API.  The
 * D3D11 backend, however, runs in the host address space and must translate
 * low 64 MB Xbox RAM pointers before copying vertex/index data. */
extern ptrdiff_t g_xbox_mem_offset;

static const void *d3d8_host_data_ptr(const void *ptr)
{
    uintptr_t address = (uintptr_t)ptr;
    if (address != 0 && address < 0x04000000u)
        return (const void *)(address + (uintptr_t)g_xbox_mem_offset);
    return ptr;
}

/* ================================================================
 * Internal device state
 * ================================================================ */

/* Maximum tracked render states, texture stages, and transforms */
#define MAX_RENDER_STATES    256
#define MAX_TEXTURE_STAGES   4
#define MAX_TSS_STATES       32
#define MAX_TRANSFORMS       512
#define MAX_LIGHTS           8

typedef struct D3D8DeviceState {
    /* D3D11 objects */
    ID3D11Device            *d3d11_device;
    ID3D11DeviceContext     *d3d11_context;
    IDXGISwapChain          *swap_chain;

    /* Default render targets */
    ID3D11RenderTargetView  *default_rtv;
    ID3D11DepthStencilView  *default_dsv;
    ID3D11Texture2D         *default_depth;

    /* Window */
    HWND                    hwnd;
    UINT                    width;
    UINT                    height;
    D3DFORMAT               backbuffer_format;

    /* State tracking */
    DWORD                   render_states[MAX_RENDER_STATES];
    DWORD                   tss[MAX_TEXTURE_STAGES][MAX_TSS_STATES];
    D3DMATRIX               transforms[MAX_TRANSFORMS];
    D3DVIEWPORT8            viewport;
    D3DMATERIAL8            material;
    D3DLIGHT8               lights[MAX_LIGHTS];
    BOOL                    light_enable[MAX_LIGHTS];

    /* Current shader/FVF */
    DWORD                   vertex_shader;
    DWORD                   pixel_shader;

    /* Scene state */
    BOOL                    in_scene;

    /* Reference count */
    LONG                    ref_count;
} D3D8DeviceState;

/* Global device instance (Xbox has a single D3D device) */
static D3D8DeviceState g_device_state;
static IDirect3DDevice8 g_device;
static BOOL g_device_initialized = FALSE;

/* Current resource bindings */
static IDirect3DVertexBuffer8 *g_cur_vb = NULL;
static UINT                    g_cur_vb_stride = 0;
static IDirect3DIndexBuffer8  *g_cur_ib = NULL;
static UINT                    g_cur_ib_base_vertex = 0;
static IDirect3DBaseTexture8  *g_cur_textures[4] = { NULL };

/* Forward declarations */
static const IDirect3DDevice8Vtbl g_device_vtbl;
static void up_ring_shutdown(void);

/* ================================================================
 * Public frame pump (called from recompiled game code)
 * ================================================================ */
/*
 * Service the window's message queue.
 *
 * This used to happen only inside d3d8_PresentFrame(), which is reached from
 * the title's FLIP_STALL.  That was fine while the title was drawing a menu.
 * Once it got past the boot video it started rendering a real scene -- tens of
 * thousands of draws against fewer than six hundred flips in a whole run -- so
 * whole seconds pass between pumps and Windows marks the window unresponsive.
 * The process is drawing the entire time, which is why the draw counters never
 * showed it.
 *
 * Pumping on a clock instead of on flips keeps the window alive however long a
 * frame takes.  Cheap enough to call from the draw path: it does nothing at all
 * unless the interval has elapsed.
 */
void d3d8_PumpWindowMessages(int force)
{
    static ULONGLONG next_tick;
    ULONGLONG now = GetTickCount64();
    MSG msg;

    if (!force && now < next_tick)
        return;
    next_tick = now + 16u;    /* about once a display frame */

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr, "[INFO WINDOW] WM_QUIT observed by the message "
                    "pump wParam=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void d3d8_PresentFrame(void)
{
    /* Pump Windows messages */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr, "[INFO WINDOW] WM_QUIT observed by PresentFrame wParam=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Present without waiting on the host refresh rate. The command processor
     * separately enforces FLIP_STALL using NV_PGRAPH_SURFACE and the guest's
     * vblank handler. Because submission is synchronous, that wait services
     * guest interrupts on this thread; a blocking DXGI Present cannot do so.
     * Notify PGRAPH here so diagnostics describe the displayed image. */
    /* Temporary: watch the title's video player state.
     *
     * sub_001F6B70 is its per-frame pump: a singleton pointer at guest
     * 0x8497F0, whose first dword is a state initialised to 1.  At 3 the pump
     * tears the player down and clears the singleton, which is what lets the
     * frontend move on.  Report every change so it is visible whether that
     * ever happens. */
    {
        extern ptrdiff_t g_xbox_mem_offset;
        static uint32_t last_player, last_state, last_playing, last_w58;
        static float last_clock = -12345.0f;
        static uint32_t last_ts;
        const uint8_t *guest = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
        uint32_t player = *(const uint32_t *)(guest + 0x8497F0u);
        uint32_t state = player >= 0x1000u && player < 0x04000000u
            ? *(const uint32_t *)(guest + player) : 0xFFFFFFFFu;
        /* 0x849C60 is the pump's "still playing" flag: while it is non-zero
         * sub_001F6C50 returns early and the state stays at 1 forever.  Zero
         * is what moves it to 2, and from there to 3. */
        uint32_t playing = *(const uint32_t *)(guest + 0x849C60u);
        /* The rest of the video controller state, from sub_002527F0: it is
         * time-driven -- rdtsc, two divides, then a float at 0x849C6C against
         * a constant -- and these are the words it steps through.  Watching
         * them says which one stops moving. */
        float    clock = *(const float *)(guest + 0x849C6Cu);
        uint32_t w4C   = *(const uint32_t *)(guest + 0x849C4Cu);
        uint32_t w58   = *(const uint32_t *)(guest + 0x849C58u);
        uint32_t w5C   = *(const uint32_t *)(guest + 0x849C5Cu);
        /* The constant the clock is compared against, and initialised from.
         * On hardware it is -1.0, a "not started" sentinel: sub_002527F0 opens
         * with ucomiss [0x849C6C], [0x648D34].  If ours is not -1.0 the state
         * machine branches the other way from its very first update. */
        float sentinel = *(const float *)(guest + 0x648D34u);
        /* The frame period the clock is consumed in.  The consume loop at
         * loc_00252909 only counts a frame while clock > this; the frame count
         * is what gates the decode call, and the decode call returning negative
         * is what ends playback.  A wrong value here stalls the whole chain and
         * leaves the clock growing without bound, which is what ours does. */
        float period = *(const float *)(guest + 0x64A720u);
        float scale  = *(const float *)(guest + 0x648D2Cu);
        /* Wall time alongside it.  The clock is in milliseconds and the frame
         * period is 33.3333, so a correct clock gains 1000 per second.  The
         * ratio of the two is the timebase error, and it is the whole bug. */
        static ULONGLONG t0;
        ULONGLONG t_ms;
        if (t0 == 0) t0 = GetTickCount64();
        t_ms = GetTickCount64() - t0;
        /* The video object itself.  sub_004E2615 copies obj[0x40], obj[0x44]
         * and obj[0x48] out of it, and the caller turns the 64-bit pair at
         * +0x40 into the playback clock.  So the clock is driven by the
         * decoder's presentation timestamp; if that never advances, playback
         * never reaches its end however much wall time passes. */
        uint32_t vobj  = w4C;
        uint32_t ts_lo = 0u, ts_hi = 0u, vstatus = 0u;
        if (vobj >= 0x1000u && vobj < 0x04000000u - 0x50u) {
            ts_lo   = *(const uint32_t *)(guest + vobj + 0x40u);
            ts_hi   = *(const uint32_t *)(guest + vobj + 0x44u);
            vstatus = *(const uint32_t *)(guest + vobj + 0x48u);
        }
        /* Include the clock in the change test.  Leaving it out meant it was
         * only ever sampled at moments when something else moved, so "clock
         * stays 0.000" was a property of the sampling, not of the clock. */
        if (player != last_player || state != last_state ||
            playing != last_playing || w58 != last_w58 ||
            clock != last_clock || ts_lo != last_ts) {
            last_player = player;
            last_state = state;
            last_playing = playing;
            last_w58 = w58;
            last_clock = clock;
            last_ts = ts_lo;
            static unsigned logged;
            if (logged++ < 40u)
            fprintf(stderr, "[INFO VIDEO] player=%08X state=%u playing=%08X "
                    "clock=%.3f sentinel=%.3f 4C=%08X 58=%08X 5C=%08X ts=%08X%08X st=%08X period=%g wall=%llums ratio=%.1f\n",
                    player, state, playing, clock, sentinel, w4C, w58, w5C, ts_hi, ts_lo, vstatus, period, (unsigned long long)t_ms,
                    clock > 0.0f ? (double)t_ms / (double)clock : 0.0);
        }

        /* Tell the title its boot video is over.
         *
         * sub_001F6C50 leaves the player at state 1 for as long as this global
         * is non-zero; zero is what moves it to 2 and then to 3, which is what
         * releases the frontend.  Nothing here decodes an XMV frame, so nothing
         * ever clears it yet, so the title waits on playback that never
         * reaches its end of stream.
         *
         * Opt-in via CONKER_FORCE_VIDEO_END, because of what it costs.  It does
         * get the title past the gate, and everything behind the gate was found
         * this way -- the array draw path, the missing two-channel texture
         * formats, the four texture stages, the register combiners.  But the
         * title is not healthy afterwards: it runs post-video code whose
         * prerequisites real playback would have set up.  The first attempt
         * faulted on a null pointer, and with that fixed it draws tens of
         * thousands of primitives while completing barely a frame -- fewer than
         * one flip sample in a whole run -- so the window shows a burst of
         * geometry and then never updates again.
         *
         * The durable fix is for the guest's own playback to reach its end of
         * stream.  Until then the default build stays on the responsive side of
         * the gate, and this flag is how to go past it deliberately.
         */
        {
            static int cleared;
            static int allowed = -1;
            if (allowed < 0)
                allowed = getenv("CONKER_FORCE_VIDEO_END") != NULL;
            if (allowed && !cleared && playing != 0u) {
                *(uint32_t *)((uint8_t *)(uintptr_t)g_xbox_mem_offset +
                              0x849C60u) = 0u;
                cleared = 1;
                fprintf(stderr, "[INFO VIDEO] CONKER_FORCE_VIDEO_END: forced "
                        "the title's playing flag clear\n");
            }
        }
    }

    if (g_device_state.swap_chain) {
        pgraph_d3d11_present_notify();
        /* Sample here, immediately before the bits go to the display, rather
         * than at the caller's frame boundary.  A corner that is on screen but
         * absent from an earlier sample means something writes between the two
         * points, and only this one can say so. */
        d3d8_DebugSampleBackbuffer("at-present");
        /* CONKER_VSYNC=1 restores the blocking present.  Removing it made the
         * title run 7.6x more work per second, but it also unthrottled the loop
         * that drives the video clock -- and that clock is accumulated from an
         * integer tick delta, so a loop spinning far faster than the console's
         * floors most deltas to zero and loses time.  Worth being able to
         * compare the two directly. */
        IDXGISwapChain_Present(g_device_state.swap_chain,
                               getenv("CONKER_VSYNC") ? 1u : 0u, 0);
    }
}

void d3d8_DebugSampleBackbuffer(const char *tag)
{
    static unsigned sample_count;
    static int dumped;
    static ULONGLONG dump_epoch, last_dump;
    { static unsigned at_present;
      const char *path = getenv("CONKER_DUMP_FRAME");
      const char *after = getenv("CONKER_DUMP_AFTER");
      int due_frame = path && dumped < 16 && dump_epoch &&
          (!last_dump || GetTickCount64() - last_dump >= 5000u) &&
          (!after || (double)(GetTickCount64() - dump_epoch) >= atof(after) * 1000.0);
      /* Slow scenes may take minutes to reach the next hundredth present.
       * Capture the first eligible frame, then keep the usual sample rate. */
      if (tag && tag[0] == 'a' && (at_present++ % 100u) != 0u && !due_frame)
          return; }
    ID3D11Texture2D *back_buffer = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    uint64_t nonblack = 0;
    uint64_t nonzero_alpha = 0;
    uint32_t center = 0;
    UINT x;
    UINT y;

    if ((sample_count++ >= 8u && !getenv("CONKER_DUMP_FRAME")) ||
        !g_device_state.swap_chain ||
        !g_device_state.d3d11_device || !g_device_state.d3d11_context)
        return;

    hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                  &IID_ID3D11Texture2D,
                                  (void **)&back_buffer);
    if (FAILED(hr) || !back_buffer) {
        fprintf(stderr,
                "[WARN PGRAPH-PIXELS] tag=%s GetBuffer hr=%08X\n",
                tag ? tag : "?", (uint32_t)hr);
        return;
    }

    ID3D11Texture2D_GetDesc(back_buffer, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                                      &desc, NULL, &staging);
    if (SUCCEEDED(hr) && staging) {
        ID3D11DeviceContext_CopyResource(
            g_device_state.d3d11_context,
            (ID3D11Resource *)staging,
            (ID3D11Resource *)back_buffer);
        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(
            g_device_state.d3d11_context, (ID3D11Resource *)staging,
            0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < desc.Height; ++y) {
                const uint32_t *row = (const uint32_t *)(
                    (const uint8_t *)mapped.pData +
                    (size_t)y * mapped.RowPitch);
                for (x = 0; x < desc.Width; ++x) {
                    const uint32_t pixel = row[x];
                    if (pixel & 0x00FFFFFFu)
                        ++nonblack;
                    if (pixel & 0xFF000000u)
                        ++nonzero_alpha;
                }
            }
            if (desc.Width && desc.Height) {
                const uint32_t *center_row = (const uint32_t *)(
                    (const uint8_t *)mapped.pData +
                    (size_t)(desc.Height / 2u) * mapped.RowPitch);
                center = center_row[desc.Width / 2u];
            }
            /* One frame to disk, when asked.  A pixel count says whether
             * anything reached the screen; it cannot say whether that
             * something is the menu.  Opt-in, so ordinary runs write no
             * files: set CONKER_DUMP_FRAME=<path>. */
            {
                const char *dump_path = getenv("CONKER_DUMP_FRAME");
                const char *dump_after = getenv("CONKER_DUMP_AFTER");
                ULONGLONG dump_now = GetTickCount64();
                if (!dump_epoch) dump_epoch = dump_now;
                /* A short sequence, late in the run.
                 *
                 * Dumping the first frames captured the loading screen while
                 * what was actually on the window seconds later was something
                 * else entirely -- which is why these dumps and what could be
                 * seen on screen disagreed for so long. */
                char dump_name[512];
                if (dump_path && dumped < 16 &&
                    (!dump_after || (double)(dump_now - dump_epoch) >=
                                        atof(dump_after) * 1000.0) &&
                    (!last_dump || dump_now - last_dump >= 5000u)) {
                    FILE *bmp;
                    snprintf(dump_name, sizeof(dump_name), "%s.%d.bmp",
                             dump_path, dumped);
                    bmp = fopen(dump_name, "wb");
                    if (bmp) {
                        uint32_t row_bytes = desc.Width * 4u;
                        uint32_t image = row_bytes * desc.Height;
                        uint8_t header[54];
                        int32_t signed_height = -(int32_t)desc.Height;
                        memset(header, 0, sizeof(header));
                        header[0] = 'B'; header[1] = 'M';
                        *(uint32_t *)(header + 2) = 54u + image;
                        *(uint32_t *)(header + 10) = 54u;
                        *(uint32_t *)(header + 14) = 40u;
                        *(int32_t *)(header + 18) = (int32_t)desc.Width;
                        *(int32_t *)(header + 22) = signed_height; /* top-down */
                        *(uint16_t *)(header + 26) = 1u;
                        *(uint16_t *)(header + 28) = 32u;
                        *(uint32_t *)(header + 34) = image;
                        fwrite(header, 1, sizeof(header), bmp);
                        for (y = 0; y < desc.Height; ++y) {
                            const uint8_t *row = (const uint8_t *)mapped.pData +
                                                (size_t)y * mapped.RowPitch;
                            /* BMP pixels are BGRA; the swapchain is RGBA. */
                            for (UINT x = 0; x < desc.Width; ++x) {
                                uint8_t pixel[4];
                                memcpy(pixel, row + x * 4u, 4);
                                if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                                    pixel[0] = row[x * 4u + 2u];
                                    pixel[2] = row[x * 4u];
                                }
                                fwrite(pixel, 1, 4, bmp);
                            }
                        }
                        fclose(bmp);
                        ++dumped;
                        last_dump = dump_now;
                        fprintf(stderr,
                                "[INFO PGRAPH-PIXELS] wrote %ux%u frame to %s\n",
                                (unsigned)desc.Width, (unsigned)desc.Height,
                                dump_name);
                    }
                }
            }
            ID3D11DeviceContext_Unmap(
                g_device_state.d3d11_context,
                (ID3D11Resource *)staging, 0);
            fprintf(stderr,
                    "[INFO PGRAPH-PIXELS] tag=%s size=%ux%u "
                    "nonblack=%llu alpha=%llu center=%08X\n",
                    tag ? tag : "?", (unsigned)desc.Width,
                    (unsigned)desc.Height,
                    (unsigned long long)nonblack,
                    (unsigned long long)nonzero_alpha, center);
        } else {
            fprintf(stderr,
                    "[WARN PGRAPH-PIXELS] tag=%s Map hr=%08X\n",
                    tag ? tag : "?", (uint32_t)hr);
        }
        ID3D11Texture2D_Release(staging);
    } else {
        fprintf(stderr,
                "[WARN PGRAPH-PIXELS] tag=%s staging hr=%08X\n",
                tag ? tag : "?", (uint32_t)hr);
    }

    /* CopyResource may force a write-bound source out of the OM slots on
     * some D3D11 runtimes.  Restore the compatibility device's canonical
     * target so the diagnostic cannot perturb the draw that follows it. */
    ID3D11DeviceContext_OMSetRenderTargets(
        g_device_state.d3d11_context, 1,
        &g_device_state.default_rtv, g_device_state.default_dsv);
    ID3D11Texture2D_Release(back_buffer);
}

/*
 * One runtime target to a BMP.
 *
 * Pixel counts said the frame contained a miniature copy of itself and a block
 * of noise, but not which surface they were in -- and the bloom chain writes
 * four of them per frame.  Seeing each surface separately is the only way to
 * tell content that arrived wrong from content that was composited wrong.
 */
void d3d8_DebugDumpRuntimeTexture(IDirect3DTexture8 *texture, const char *path)
{
    D3D8Texture *rt = (D3D8Texture *)texture;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint8_t header[54];
    FILE *bmp;
    UINT y;

    if (!rt || !rt->d3d11_texture || !g_device_state.d3d11_device ||
        !g_device_state.d3d11_context || !path)
        return;

    ID3D11Texture2D_GetDesc(rt->d3d11_texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                                            &desc, NULL, &staging)))
        return;
    ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                                     (ID3D11Resource *)staging,
                                     (ID3D11Resource *)rt->d3d11_texture);
    if (FAILED(ID3D11DeviceContext_Map(g_device_state.d3d11_context,
                                       (ID3D11Resource *)staging, 0,
                                       D3D11_MAP_READ, 0, &mapped))) {
        ID3D11Texture2D_Release(staging);
        return;
    }
    /* Preserve block-compressed SRVs as DDS for offline inspection. */
    if (desc.Format == DXGI_FORMAT_BC1_UNORM ||
        desc.Format == DXGI_FORMAT_BC2_UNORM ||
        desc.Format == DXGI_FORMAT_BC3_UNORM) {
        char dds_path[600];
        uint32_t dds[32] = {0};
        UINT block_bytes = desc.Format == DXGI_FORMAT_BC1_UNORM ? 8u : 16u;
        UINT row_bytes = ((desc.Width + 3u) / 4u) * block_bytes;
        UINT rows = (desc.Height + 3u) / 4u;
        snprintf(dds_path, sizeof(dds_path), "%s.dds", path);
        dds[0] = 0x20534444u; dds[1] = 124; dds[2] = 0x81007u;
        dds[3] = desc.Height; dds[4] = desc.Width; dds[5] = row_bytes * rows;
        dds[19] = 32; dds[20] = 4;
        dds[21] = desc.Format == DXGI_FORMAT_BC1_UNORM ? 0x31545844u :
                  desc.Format == DXGI_FORMAT_BC2_UNORM ? 0x33545844u : 0x35545844u;
        dds[27] = 0x1000;
        bmp = row_bytes <= mapped.RowPitch ? fopen(dds_path, "wb") : NULL;
        if (bmp) {
            fwrite(dds, 1, sizeof(dds), bmp);
            for (y = 0; y < rows; ++y)
                fwrite((const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch,
                       1, row_bytes, bmp);
            fclose(bmp);
            fprintf(stderr, "[INFO D3D-DUMP] %ux%u BC texture to %s\n",
                    desc.Width, desc.Height, dds_path);
        }
        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                  (ID3D11Resource *)staging, 0);
        ID3D11Texture2D_Release(staging);
        return;
    }
    /* 32-bit formats only.  The writer below assumes four bytes per pixel, and
     * on an 8-bit or block-compressed texture that reads past the end of each
     * mapped row -- which crashed inside fwrite rather than producing a wrong
     * picture. */
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        fprintf(stderr, "[INFO D3D-DUMP] skipped %s: format %u"
                        " is not 32-bit\n",
                path, (unsigned)desc.Format);
        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                  (ID3D11Resource *)staging, 0);
        ID3D11Texture2D_Release(staging);
        return;
    }

    bmp = fopen(path, "wb");
    if (bmp) {
        uint32_t row_bytes = desc.Width * 4u;
        if (row_bytes > mapped.RowPitch)
            row_bytes = mapped.RowPitch;
        uint32_t image = row_bytes * desc.Height;
        memset(header, 0, sizeof(header));
        header[0] = 'B'; header[1] = 'M';
        *(uint32_t *)(header + 2) = 54u + image;
        *(uint32_t *)(header + 10) = 54u;
        *(uint32_t *)(header + 14) = 40u;
        *(int32_t *)(header + 18) = (int32_t)desc.Width;
        *(int32_t *)(header + 22) = -(int32_t)desc.Height;
        *(uint16_t *)(header + 26) = 1u;
        *(uint16_t *)(header + 28) = 32u;
        *(uint32_t *)(header + 34) = image;
        fwrite(header, 1, sizeof(header), bmp);
        for (y = 0; y < desc.Height; ++y) {
            const uint8_t *row = (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch;
            for (UINT x = 0; x < row_bytes / 4u; ++x) {
                uint8_t pixel[4];
                memcpy(pixel, row + x * 4u, 4);
                if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    pixel[0] = row[x * 4u + 2u];
                    pixel[2] = row[x * 4u];
                }
                fwrite(pixel, 1, 4, bmp);
            }
        }
        fclose(bmp);
        fprintf(stderr, "[INFO D3D-DUMP] %ux%u to %s\n",
                (unsigned)desc.Width, (unsigned)desc.Height, path);
    }
    ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                              (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
}

/* Read the actual D3D11 SRV, including bindings detached by render-target
 * hazards. The D3D8 GetTexture API is still a stub and cannot answer this. */
void d3d8_DebugDumpBoundTexture(unsigned stage, const char *path)
{
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Resource *resource = NULL;
    D3D8Texture wrapper = {0};
    if (stage >= 4 || !g_device_state.d3d11_context) return;
    ID3D11DeviceContext_PSGetShaderResources(g_device_state.d3d11_context, stage, 1, &srv);
    fprintf(stderr, "[PIXEL-CAPTURE] actual SRV%u=%p\n", stage, srv);
    if (!srv) return;
    ID3D11ShaderResourceView_GetResource(srv, &resource);
    if (resource && SUCCEEDED(ID3D11Resource_QueryInterface(resource,
            &IID_ID3D11Texture2D, (void **)&wrapper.d3d11_texture))) {
        d3d8_DebugDumpRuntimeTexture(&wrapper.iface, path);
        ID3D11Texture2D_Release(wrapper.d3d11_texture);
    }
    if (resource) ID3D11Resource_Release(resource);
    ID3D11ShaderResourceView_Release(srv);
}

/* Uncapped content probe used by the present-path trace.  Returns stats
 * instead of printing, so one caller can build a single per-Present line
 * comparing several surfaces.  Samples on a grid: enough to tell a black
 * surface from a picture and to notice the picture changing, without paying
 * for a full 640x480 readback per frame. */
void d3d8_DebugProbeTexture(IDirect3DTexture8 *texture, unsigned *out_w,
                            unsigned *out_h, unsigned *out_nonblack,
                            unsigned *out_sampled, uint32_t *out_hash)
{
    D3D8Texture *rt = (D3D8Texture *)texture;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    unsigned nonblack = 0, sampled = 0;
    uint32_t hash = 2166136261u;
    UINT x, y, step;

    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_nonblack) *out_nonblack = 0;
    if (out_sampled) *out_sampled = 0;
    if (out_hash) *out_hash = 0;

    if (!rt || !rt->d3d11_texture || !g_device_state.d3d11_device ||
        !g_device_state.d3d11_context)
        return;

    ID3D11Texture2D_GetDesc(rt->d3d11_texture, &desc);
    if (out_w) *out_w = (unsigned)desc.Width;
    if (out_h) *out_h = (unsigned)desc.Height;

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device, &desc,
                                      NULL, &staging);
    if (FAILED(hr) || !staging)
        return;

    ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                                     (ID3D11Resource *)staging,
                                     (ID3D11Resource *)rt->d3d11_texture);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
                                 (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        step = (desc.Width >= 256u) ? 4u : 1u;
        for (y = 0; y < desc.Height; y += step) {
            const uint32_t *row = (const uint32_t *)(
                (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch);
            for (x = 0; x < desc.Width; x += step) {
                uint32_t pixel = row[x];
                ++sampled;
                if (pixel & 0x00FFFFFFu) ++nonblack;
                hash = (hash ^ pixel) * 16777619u;
            }
        }
        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                  (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);

    if (out_nonblack) *out_nonblack = nonblack;
    if (out_sampled) *out_sampled = sampled;
    if (out_hash) *out_hash = hash;
}

/* Same, for the swap chain's back buffer. */
void d3d8_DebugDumpBackbuffer(const char *path)
{
    D3D8Texture wrapper = {0};
    if (!g_device_state.swap_chain || !path) return;
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
            &IID_ID3D11Texture2D, (void **)&wrapper.d3d11_texture))) {
        d3d8_DebugDumpRuntimeTexture(&wrapper.iface, path);
        ID3D11Texture2D_Release(wrapper.d3d11_texture);
    }
}

void d3d8_DebugProbeBackbuffer(unsigned *out_nonblack, unsigned *out_sampled,
                               uint32_t *out_hash)
{
    ID3D11Texture2D *back_buffer = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    unsigned nonblack = 0, sampled = 0;
    uint32_t hash = 2166136261u;
    UINT x, y, step;

    if (out_nonblack) *out_nonblack = 0;
    if (out_sampled) *out_sampled = 0;
    if (out_hash) *out_hash = 0;

    if (!g_device_state.swap_chain || !g_device_state.d3d11_device ||
        !g_device_state.d3d11_context)
        return;
    hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                  &IID_ID3D11Texture2D,
                                  (void **)&back_buffer);
    if (FAILED(hr) || !back_buffer)
        return;

    ID3D11Texture2D_GetDesc(back_buffer, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device, &desc,
                                      NULL, &staging);
    if (SUCCEEDED(hr) && staging) {
        ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                                         (ID3D11Resource *)staging,
                                         (ID3D11Resource *)back_buffer);
        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
                                     (ID3D11Resource *)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            step = (desc.Width >= 256u) ? 4u : 1u;
            for (y = 0; y < desc.Height; y += step) {
                const uint32_t *row = (const uint32_t *)(
                    (const uint8_t *)mapped.pData +
                    (size_t)y * mapped.RowPitch);
                for (x = 0; x < desc.Width; x += step) {
                    uint32_t pixel = row[x];
                    ++sampled;
                    if (pixel & 0x00FFFFFFu) ++nonblack;
                    hash = (hash ^ pixel) * 16777619u;
                }
            }
            ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                      (ID3D11Resource *)staging, 0);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11Texture2D_Release(back_buffer);

    if (out_nonblack) *out_nonblack = nonblack;
    if (out_sampled) *out_sampled = sampled;
    if (out_hash) *out_hash = hash;
}

void d3d8_DebugSampleRuntimeTexture(IDirect3DTexture8 *texture,
                                    const char *tag,
                                    uint32_t guest_offset)
{
    static unsigned sample_count;
    D3D8Texture *runtime_texture = (D3D8Texture *)texture;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    uint64_t nonblack = 0;
    uint64_t nonzero_alpha = 0;
    uint32_t center = 0;
    UINT x;
    UINT y;

    if (sample_count++ >= 8u || !runtime_texture ||
        !runtime_texture->d3d11_texture ||
        !g_device_state.d3d11_device || !g_device_state.d3d11_context)
        return;

    ID3D11Texture2D_GetDesc(runtime_texture->d3d11_texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                                      &desc, NULL, &staging);
    if (SUCCEEDED(hr) && staging) {
        ID3D11DeviceContext_CopyResource(
            g_device_state.d3d11_context,
            (ID3D11Resource *)staging,
            (ID3D11Resource *)runtime_texture->d3d11_texture);
        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(
            g_device_state.d3d11_context, (ID3D11Resource *)staging,
            0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < desc.Height; ++y) {
                const uint32_t *row = (const uint32_t *)(
                    (const uint8_t *)mapped.pData +
                    (size_t)y * mapped.RowPitch);
                for (x = 0; x < desc.Width; ++x) {
                    const uint32_t pixel = row[x];
                    if (pixel & 0x00FFFFFFu)
                        ++nonblack;
                    if (pixel & 0xFF000000u)
                        ++nonzero_alpha;
                }
            }
            if (desc.Width && desc.Height) {
                const uint32_t *center_row = (const uint32_t *)(
                    (const uint8_t *)mapped.pData +
                    (size_t)(desc.Height / 2u) * mapped.RowPitch);
                center = center_row[desc.Width / 2u];
            }
            ID3D11DeviceContext_Unmap(
                g_device_state.d3d11_context,
                (ID3D11Resource *)staging, 0);
            fprintf(stderr,
                    "[INFO PGRAPH-RUNTIME-PIXELS] tag=%s target=%08X "
                    "size=%ux%u nonblack=%llu alpha=%llu center=%08X\n",
                    tag ? tag : "?", guest_offset,
                    (unsigned)desc.Width, (unsigned)desc.Height,
                    (unsigned long long)nonblack,
                    (unsigned long long)nonzero_alpha, center);
        } else {
            fprintf(stderr,
                    "[WARN PGRAPH-RUNTIME-PIXELS] tag=%s target=%08X "
                    "Map hr=%08X\n",
                    tag ? tag : "?", guest_offset, (uint32_t)hr);
        }
        ID3D11Texture2D_Release(staging);
    } else {
        fprintf(stderr,
                "[WARN PGRAPH-RUNTIME-PIXELS] tag=%s target=%08X "
                "staging hr=%08X\n",
                tag ? tag : "?", guest_offset, (uint32_t)hr);
    }

    d3d8_RestoreDefaultRenderTarget();
}

/* ================================================================
 * Internal accessors (used by d3d8_resources/shaders/states)
 * ================================================================ */

IDirect3DDevice8    *d3d8_GetDevice(void) { return &g_device; }
ID3D11Device        *d3d8_GetD3D11Device(void) { return g_device_state.d3d11_device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return g_device_state.d3d11_context; }
IDXGISwapChain      *d3d8_GetSwapChain(void) { return g_device_state.swap_chain; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return g_device_state.default_rtv; }
HWND                 d3d8_GetHWND(void) { return g_device_state.hwnd; }
UINT                 d3d8_GetBackbufferWidth(void) { return g_device_state.width; }
UINT                 d3d8_GetBackbufferHeight(void) { return g_device_state.height; }
const DWORD         *d3d8_GetRenderStates(void) { return g_device_state.render_states; }
const DWORD         *d3d8_GetTSS(DWORD stage) { return (stage < MAX_TEXTURE_STAGES) ? g_device_state.tss[stage] : NULL; }
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) {
    return ((DWORD)type < MAX_TRANSFORMS) ? &g_device_state.transforms[(DWORD)type] : NULL;
}

const D3DLIGHT8     *d3d8_GetLight(DWORD index) {
    return (index < MAX_LIGHTS) ? &g_device_state.lights[index] : NULL;
}

BOOL                 d3d8_GetLightEnable(DWORD index) {
    return (index < MAX_LIGHTS) ? g_device_state.light_enable[index] : FALSE;
}

const D3DMATERIAL8  *d3d8_GetMaterial(void) {
    return &g_device_state.material;
}

UINT                 d3d8_GetNumLights(void) {
    return MAX_LIGHTS;
}

/* Tracked rather than queried.
 *
 * The query version cost an OMGetRenderTargets per draw, so it had to be
 * sampled -- every 256th draw -- and with only a couple of escapes a frame it
 * reported zero for a fault that was happening constantly.  A flag maintained
 * by the two functions that bind is free, so every draw can be checked. */
static int g_default_rtv_bound = 1;

/* The viewport the two bind functions last installed, tracked so a draw can
 * report it without an RSGetViewports call. */
static float g_bound_vp_w, g_bound_vp_h;
static ID3D11RenderTargetView *g_bound_rtv;
static ID3D11DepthStencilView *g_bound_dsv;

/* Depth belongs to the guest zeta allocation, not to a color target. Two
 * color surfaces can share it, and rebinding must preserve prior writes. */
typedef struct RuntimeDepthTarget {
    struct RuntimeDepthTarget *next;
    uint32_t guest_offset;
    UINT width, height;
    D3DFORMAT format;
    ID3D11Texture2D *texture;
    ID3D11DepthStencilView *view;
    ID3D11ShaderResourceView *depth_srv, *stencil_srv;
    IDirect3DTexture8 *color_texture;
    ID3D11UnorderedAccessView *color_uav;
    UINT pitch;
    BOOL swizzled;
    uint64_t write_serial, imported_serial;
} RuntimeDepthTarget;
static RuntimeDepthTarget *g_runtime_depth_targets;
static ID3D11ComputeShader *g_depth_color_shader;
static uint64_t g_depth_write_serial;
static ID3D11VertexShader *g_depth_import_vs;
static ID3D11PixelShader *g_depth_import_ps;
static ID3D11Buffer *g_depth_import_constants;
static ID3D11DepthStencilState *g_depth_import_state;
static ID3D11RasterizerState *g_depth_import_raster;

static void runtime_depth_written(void)
{
    for (RuntimeDepthTarget *t = g_runtime_depth_targets; t; t = t->next)
        if (t->view == g_bound_dsv) {
            t->write_serial = ++g_depth_write_serial;
            return;
        }
}

static void runtime_depth_drawn(void)
{
    const DWORD *s = d3d8_GetRenderStates();
    if ((s[D3DRS_ZENABLE] && s[D3DRS_ZWRITEENABLE]) ||
        (s[D3DRS_STENCILENABLE] && s[D3DRS_STENCILWRITEMASK]))
        runtime_depth_written();
}

/* Read-only subviews still refer to the scene's Xbox depth bytes. In
 * particular, a swizzled lighting target can read a small part of a linear
 * scene zeta surface. A newly allocated host DSV has none of those pixels.
 * Import from the most recent covering writer, entirely on the GPU. */
#include "d3d8_depth_alias.h"

static void runtime_depth_shutdown(void)
{
    while (g_runtime_depth_targets) {
        RuntimeDepthTarget *target = g_runtime_depth_targets;
        g_runtime_depth_targets = target->next;
        if (target->color_uav) ID3D11UnorderedAccessView_Release(target->color_uav);
        if (target->color_texture) target->color_texture->lpVtbl->Release(target->color_texture);
        if (target->depth_srv) ID3D11ShaderResourceView_Release(target->depth_srv);
        if (target->stencil_srv) ID3D11ShaderResourceView_Release(target->stencil_srv);
        ID3D11DepthStencilView_Release(target->view);
        ID3D11Texture2D_Release(target->texture);
        free(target);
    }
    g_bound_dsv = NULL;
    if (g_depth_color_shader) ID3D11ComputeShader_Release(g_depth_color_shader);
    g_depth_color_shader = NULL;
    if (g_depth_import_vs) ID3D11VertexShader_Release(g_depth_import_vs);
    if (g_depth_import_ps) ID3D11PixelShader_Release(g_depth_import_ps);
    if (g_depth_import_constants) ID3D11Buffer_Release(g_depth_import_constants);
    if (g_depth_import_state) ID3D11DepthStencilState_Release(g_depth_import_state);
    if (g_depth_import_raster) ID3D11RasterizerState_Release(g_depth_import_raster);
    g_depth_import_vs = NULL; g_depth_import_ps = NULL;
    g_depth_import_constants = NULL; g_depth_import_state = NULL;
    g_depth_import_raster = NULL; g_depth_write_serial = 0;
}

HRESULT d3d8_BindRuntimeDepthStencilLayout(uint32_t guest_offset, UINT width,
        UINT height, D3DFORMAT format, UINT pitch, BOOL swizzled, BOOL readonly)
{
    RuntimeDepthTarget *target;
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;
    if (!g_device_state.d3d11_context || !g_bound_rtv || !width || !height ||
        (format != D3DFMT_D16 && format != D3DFMT_D24S8))
        return E_INVALIDARG;
    for (target = g_runtime_depth_targets; target; target = target->next)
        if (target->guest_offset == guest_offset && target->width == width &&
            target->height == height && target->format == format &&
            target->pitch == pitch && target->swizzled == swizzled)
            break;
    if (!target) {
        target = (RuntimeDepthTarget *)calloc(1, sizeof(*target));
        if (!target) return E_OUTOFMEMORY;
        memset(&desc, 0, sizeof(desc));
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format == D3DFMT_D16 ? DXGI_FORMAT_R16_TYPELESS
                                         : DXGI_FORMAT_R24G8_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                                         &desc, NULL, &target->texture);
        if (SUCCEEDED(hr)) {
            D3D11_DEPTH_STENCIL_VIEW_DESC vd = {0};
            vd.Format = format == D3DFMT_D16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            hr = ID3D11Device_CreateDepthStencilView(g_device_state.d3d11_device,
                (ID3D11Resource *)target->texture, &vd, &target->view);
        }
        if (FAILED(hr)) {
            if (target->texture) ID3D11Texture2D_Release(target->texture);
            free(target);
            return hr;
        }
        target->guest_offset = guest_offset;
        target->width = width;
        target->height = height;
        target->format = format;
        target->pitch = pitch;
        target->swizzled = swizzled;
        target->next = g_runtime_depth_targets;
        g_runtime_depth_targets = target;
        fprintf(stderr, "[D3D8-ZETA] created %08X %ux%u format=%u\n",
                guest_offset, width, height, (unsigned)format);
    }
    if (readonly) {
        hr = runtime_depth_import(target);
        if (FAILED(hr)) return hr;
    }
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1,
                                          &g_bound_rtv, target->view);
    g_bound_dsv = target->view;
    return S_OK;
}

HRESULT d3d8_BindRuntimeDepthStencil(uint32_t guest_offset, UINT width,
                                   UINT height, D3DFORMAT format)
{
    return d3d8_BindRuntimeDepthStencilLayout(guest_offset, width, height,
        format, width * (format == D3DFMT_D16 ? 2u : 4u), FALSE, FALSE);
}

IDirect3DTexture8 *d3d8_ResolveDepthColorTexture(uint32_t guest_offset,
                                               uint32_t color, UINT requested_width,
                                               UINT requested_height, UINT *width, UINT *height)
{
    /* NV2A can reinterpret zeta bytes as color. D24S8 in guest memory is
     * depth[31:8], stencil[7:0]; LU_IMAGE_R8G8B8A8 therefore exposes the
     * three depth bytes as RGB and stencil as alpha. Host D24S8 has a
     * different packing, so copying its bits into an RGBA resource is wrong.
     * Repack the two typed views on the GPU, then use ordinary filtering. */
    RuntimeDepthTarget *target;
    if (color != 0x41u) return NULL;
    /* Small lighting passes share the scene's zeta address. Their host views
     * must not replace the full-size stencil source requested by postprocessing. */
    for (target = g_runtime_depth_targets; target; target = target->next)
        if (target->guest_offset == guest_offset && target->format == D3DFMT_D24S8 &&
            target->width == requested_width && target->height == requested_height) break;
    if (!target) return NULL;
    ID3D11Device *dev = g_device_state.d3d11_device;
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    HRESULT hr = S_OK;
    if (!g_depth_color_shader) {
        static const char source[] =
            "Texture2D<float> z:register(t0);Texture2D<uint2> s:register(t1);"
            "RWTexture2D<float4> o:register(u0);"
            "[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){"
            "uint w,h;o.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;"
            "uint d=(uint)round(saturate(z.Load(int3(p.xy,0)))*16777215.0);"
            "uint a=s.Load(int3(p.xy,0)).y;"
            "o[p.xy]=float4((d>>16)&255,(d>>8)&255,d&255,a&255)/255.0;}";
        ID3DBlob *code = NULL, *errors = NULL;
        hr = D3DCompile(source, sizeof(source)-1, NULL, NULL, NULL, "main", "cs_5_0",
                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &g_depth_color_shader);
        if (errors) ID3D10Blob_Release(errors);
        if (code) ID3D10Blob_Release(code);
        if (FAILED(hr)) return NULL;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    if (!target->depth_srv) {
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)target->texture,
                                                  &sd, &target->depth_srv);
        if (FAILED(hr)) return NULL;
    }
    if (!target->stencil_srv) {
        sd.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)target->texture,
                                                  &sd, &target->stencil_srv);
        if (FAILED(hr)) return NULL;
    }
    if (!target->color_texture && FAILED(d3d8_CreateShaderOutputTexture(
        target->width, target->height, &target->color_texture))) return NULL;
    if (!target->color_uav) {
        D3D8Texture *tex = (D3D8Texture *)target->color_texture;
        hr = ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)tex->d3d11_texture,
                                                  NULL, &target->color_uav);
        if (FAILED(hr)) return NULL;
    }

    /* Preserve graphics bindings: this resolve can happen between setting
     * texture stages, so unbinding other inputs without restoring them
     * would lose the scene color that stage zero just selected. */
    ID3D11ShaderResourceView *old_ps[4] = {0}, *old_cs[2] = {0}, *nulls[4] = {0};
    ID3D11UnorderedAccessView *old_uav = NULL, *null_uav = NULL;
    ID3D11ComputeShader *old_shader = NULL;
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 4, old_ps);
    ID3D11DeviceContext_CSGetShaderResources(ctx, 0, 2, old_cs);
    ID3D11DeviceContext_CSGetUnorderedAccessViews(ctx, 0, 1, &old_uav);
    ID3D11DeviceContext_CSGetShader(ctx, &old_shader, NULL, NULL);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, nulls);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_bound_rtv, NULL);
    ID3D11ShaderResourceView *inputs[2] = {target->depth_srv, target->stencil_srv};
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 2, inputs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &target->color_uav, NULL);
    ID3D11DeviceContext_CSSetShader(ctx, g_depth_color_shader, NULL, 0);
    ID3D11DeviceContext_Dispatch(ctx, (target->width+7)/8, (target->height+7)/8, 1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &null_uav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 2, nulls);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_bound_rtv, g_bound_dsv);
    ID3D11DeviceContext_CSSetShader(ctx, old_shader, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 2, old_cs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &old_uav, NULL);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, old_ps);
    for (int i=0;i<4;i++) if(old_ps[i]) ID3D11ShaderResourceView_Release(old_ps[i]);
    for (int i=0;i<2;i++) if(old_cs[i]) ID3D11ShaderResourceView_Release(old_cs[i]);
    if(old_uav) ID3D11UnorderedAccessView_Release(old_uav);
    if(old_shader) ID3D11ComputeShader_Release(old_shader);
    if(width) *width=target->width;
    if(height) *height=target->height;
    return target->color_texture;
}

void d3d8_DebugGetViewportSize(float *w, float *h)
{
    if (w) *w = g_bound_vp_w;
    if (h) *h = g_bound_vp_h;
}

/*
 * The dimensions a pre-transformed vertex is in screen space of.
 *
 * XYZRHW coordinates are in pixels of whatever is being rendered into, which
 * is not always the back buffer: a bloom chain draws a 256x256 quad into a
 * 256x256 target.  Converting those with the back buffer's 640x480 put the
 * quad in the top-left 40% x 53% of its target, and since those targets are
 * composited back onto the frame, the result was a miniature copy of the
 * picture in the corner of the picture.
 */
void d3d8_GetRenderTargetSize(float *w, float *h)
{
    if (w) *w = g_bound_vp_w > 0.0f ? g_bound_vp_w
                                    : (float)d3d8_GetBackbufferWidth();
    if (h) *h = g_bound_vp_h > 0.0f ? g_bound_vp_h
                                    : (float)d3d8_GetBackbufferHeight();
}

/* Resource identity of a draw, for the render-target trace.
 *
 * The XMV quad has valid pixels, correct geometry and a pixel state that
 * provably does not matter -- bypassing it entirely changed nothing -- so
 * the remaining question is whether the resource the draw writes is the
 * resource Present reads.  This prints both, plus the SRV, so A-vs-B can
 * be read off directly instead of inferred. */
void d3d8_DebugIdentities(IDirect3DTexture8 *texture, const char *tag,
                          uint32_t guest_offset)
{
    D3D8Texture *tx = (D3D8Texture *)texture;
    ID3D11RenderTargetView *om_rtv = NULL;
    ID3D11DepthStencilView *om_dsv = NULL;
    ID3D11ShaderResourceView *ps_srv = NULL;
    ID3D11Resource *tx_rtv_res = NULL, *om_rtv_res = NULL, *srv_res = NULL;

    if (!g_device_state.d3d11_context) return;

    ID3D11DeviceContext_OMGetRenderTargets(g_device_state.d3d11_context, 1,
                                           &om_rtv, &om_dsv);
    ID3D11DeviceContext_PSGetShaderResources(g_device_state.d3d11_context,
                                             0, 1, &ps_srv);
    if (tx && tx->rtv)
        ID3D11RenderTargetView_GetResource(tx->rtv, &tx_rtv_res);
    if (om_rtv)
        ID3D11RenderTargetView_GetResource(om_rtv, &om_rtv_res);
    if (ps_srv)
        ID3D11ShaderResourceView_GetResource(ps_srv, &srv_res);

    fprintf(stderr,
        "[RID] %-14s guest=%08X  tex8=%p tex2D=%p rtv=%p rtvRes=%p srvOfTex=%p %ux%u" "\n"
        "[RID]                OM rtv=%p OMrtvRes=%p   PS srv=%p srvRes=%p   MATCH=%s" "\n",
        tag ? tag : "?", guest_offset, (void *)tx,
        (void *)(tx ? tx->d3d11_texture : NULL),
        (void *)(tx ? tx->rtv : NULL), (void *)tx_rtv_res,
        (void *)(tx ? tx->srv : NULL),
        tx ? (unsigned)tx->width : 0u, tx ? (unsigned)tx->height : 0u,
        (void *)om_rtv, (void *)om_rtv_res, (void *)ps_srv, (void *)srv_res,
        (tx_rtv_res != NULL && tx_rtv_res == om_rtv_res) ? "yes"
            : (om_rtv == NULL ? "NO RENDER TARGET BOUND" : "DIFFERENT"));
    fflush(stderr);

    if (tx_rtv_res) ID3D11Resource_Release(tx_rtv_res);
    if (om_rtv_res) ID3D11Resource_Release(om_rtv_res);
    if (srv_res) ID3D11Resource_Release(srv_res);
    if (om_rtv) ID3D11RenderTargetView_Release(om_rtv);
    if (om_dsv) ID3D11DepthStencilView_Release(om_dsv);
    if (ps_srv) ID3D11ShaderResourceView_Release(ps_srv);
}
HRESULT d3d8_BindRuntimeRenderTexture(IDirect3DTexture8 *texture,
                                      BOOL clear, D3DCOLOR clear_color)
{
    D3D8Texture *runtime_texture = (D3D8Texture *)texture;

    if (!runtime_texture || !runtime_texture->rtv ||
        !g_device_state.d3d11_context)
        return E_INVALIDARG;

    ID3D11DeviceContext_OMSetRenderTargets(
        g_device_state.d3d11_context, 1, &runtime_texture->rtv, NULL);
    g_bound_dsv = NULL;
    g_default_rtv_bound = 0;

    /* The viewport belongs to the target, not to the swap chain.
     *
     * Binding a render target does not change it, so a pass drawing into a
     * 128x128 surface kept the 640x480 viewport and landed in the top-left
     * corner at 1:1 instead of filling its target.  Every compositor pass was
     * built that way, which is why one of them was visible on screen as a
     * square in the corner containing a miniature of the frame. */
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = (FLOAT)runtime_texture->width;
        vp.Height = (FLOAT)runtime_texture->height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
        g_bound_vp_w = vp.Width;
        g_bound_vp_h = vp.Height;
        g_bound_rtv = runtime_texture->rtv;
    }

    if (clear) {
        float color[4] = {
            ((clear_color >> 16) & 0xFF) / 255.0f,
            ((clear_color >> 8) & 0xFF) / 255.0f,
            ((clear_color >> 0) & 0xFF) / 255.0f,
            ((clear_color >> 24) & 0xFF) / 255.0f,
        };
        ID3D11DeviceContext_ClearRenderTargetView(
            g_device_state.d3d11_context, runtime_texture->rtv, color);
    }
    return S_OK;
}

HRESULT d3d8_CopyRuntimeRenderTexture(IDirect3DTexture8 *destination,
                                      IDirect3DTexture8 *source)
{
    D3D8Texture *destination_texture = (D3D8Texture *)destination;
    D3D8Texture *source_texture = (D3D8Texture *)source;
    D3D11_TEXTURE2D_DESC destination_desc;
    D3D11_TEXTURE2D_DESC source_desc;
    ID3D11ShaderResourceView *null_srv = NULL;

    if (!destination_texture || !source_texture ||
        !destination_texture->d3d11_texture ||
        !source_texture->d3d11_texture ||
        !g_device_state.d3d11_context)
        return E_INVALIDARG;

    ID3D11Texture2D_GetDesc(
        destination_texture->d3d11_texture, &destination_desc);
    ID3D11Texture2D_GetDesc(source_texture->d3d11_texture, &source_desc);
    if (destination_desc.Width != source_desc.Width ||
        destination_desc.Height != source_desc.Height ||
        destination_desc.MipLevels != source_desc.MipLevels ||
        destination_desc.ArraySize != source_desc.ArraySize ||
        destination_desc.Format != source_desc.Format ||
        destination_desc.SampleDesc.Count != source_desc.SampleDesc.Count ||
        destination_desc.SampleDesc.Quality != source_desc.SampleDesc.Quality)
        return E_INVALIDARG;

    /* The source is normally still the active Frontend render target, while
     * the destination may have been sampled by the preceding frame.  Unbind
     * both roles before issuing the live GPU-to-GPU resolve. */
    ID3D11DeviceContext_OMSetRenderTargets(
        g_device_state.d3d11_context, 0, NULL, NULL);
    ID3D11DeviceContext_PSSetShaderResources(
        g_device_state.d3d11_context, 0, 1, &null_srv);
    ID3D11DeviceContext_CopyResource(
        g_device_state.d3d11_context,
        (ID3D11Resource *)destination_texture->d3d11_texture,
        (ID3D11Resource *)source_texture->d3d11_texture);
    return S_OK;
}

/* Which render target is actually bound right now, and is it the swap chain's?
 *
 * Draws that look perfectly healthy in the log land nowhere visible if an
 * offscreen target was bound and never restored, and nothing else can tell
 * those two cases apart. */
/* Non-black pixel count of an arbitrary texture.
 *
 * Sampling the back buffer says whether anything reached the screen; when the
 * title renders into its own surfaces the interesting question moves one step
 * earlier -- is the surface itself empty, or is the copy to the screen wrong?
 * Nothing else can tell those apart. */
void d3d8_DebugSampleTexture(IDirect3DTexture8 *texture, const char *tag)
{
    D3D8Texture *tex = (D3D8Texture *)texture;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint64_t nonblack = 0;
    UINT x, y;

    if (!tex || !tex->d3d11_texture || !g_device_state.d3d11_context)
        return;

    ID3D11Texture2D_GetDesc(tex->d3d11_texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                                            &desc, NULL, &staging)) || !staging)
        return;

    ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
                                     (ID3D11Resource *)staging,
                                     (ID3D11Resource *)tex->d3d11_texture);
    memset(&mapped, 0, sizeof(mapped));
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_device_state.d3d11_context,
                                          (ID3D11Resource *)staging, 0,
                                          D3D11_MAP_READ, 0, &mapped))) {
        for (y = 0; y < desc.Height; ++y) {
            const uint32_t *row = (const uint32_t *)(
                (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch);
            for (x = 0; x < desc.Width; ++x)
                if (row[x] & 0x00FFFFFFu)
                    ++nonblack;
        }
        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                  (ID3D11Resource *)staging, 0);
        fprintf(stderr, "[INFO D3D-TEXTURE] tag=%s %ux%u nonblack=%llu\n",
                tag ? tag : "?", (unsigned)desc.Width, (unsigned)desc.Height,
                (unsigned long long)nonblack);
    }
    ID3D11Texture2D_Release(staging);
}

void d3d8_DebugReportCurrentTarget(const char *tag)
{
    ID3D11RenderTargetView *current = NULL;
    ID3D11DepthStencilView *depth = NULL;
    D3D11_VIEWPORT vp[8];
    UINT vp_count = 8;

    if (!g_device_state.d3d11_context)
        return;

    ID3D11DeviceContext_OMGetRenderTargets(g_device_state.d3d11_context,
                                           1, &current, &depth);
    ID3D11DeviceContext_RSGetViewports(g_device_state.d3d11_context,
                                       &vp_count, vp);
    fprintf(stderr,
            "[INFO D3D-TARGET] tag=%s rtv=%p default=%p %s depth=%p "
            "viewports=%u first=%.0fx%.0f at %.0f,%.0f depth %.2f..%.2f\n",
            tag ? tag : "?", (void *)current,
            (void *)g_device_state.default_rtv,
            current == g_device_state.default_rtv ? "(swap chain)"
                                                  : "(OFFSCREEN)",
            (void *)depth, vp_count,
            vp_count ? vp[0].Width : 0.0f, vp_count ? vp[0].Height : 0.0f,
            vp_count ? vp[0].TopLeftX : 0.0f, vp_count ? vp[0].TopLeftY : 0.0f,
            vp_count ? vp[0].MinDepth : 0.0f, vp_count ? vp[0].MaxDepth : 0.0f);
    if (current) ID3D11RenderTargetView_Release(current);
    if (depth) ID3D11DepthStencilView_Release(depth);
}

/* ================================================================
 * Read/write hazard resolution
 *
 * NV2A has no notion of a resource view: a draw may read and write the same
 * framebuffer memory, and titles rely on it -- a compositor samples the
 * surface it is compositing onto.  D3D11 refuses, and refuses quietly:
 * binding a resource as a shader input unbinds it as the render target, so
 * the draw produces nothing at all and nothing reports why.
 *
 * The resolution is to sample a copy of the target taken before the draw.
 * The draw then reads the surface as it stood beforehand and writes its
 * result into the surface, which is the behaviour the title is written
 * against.
 *
 * This is a host-renderer compatibility mechanism and nothing else.  It knows
 * no addresses, no title and no game state; it is keyed entirely on the
 * texture handle the caller is about to sample.  A D3D12 or Vulkan backend
 * would replace the body -- those APIs express the same hazard through
 * barriers and may not need a copy at all -- without the translator or any
 * recompiled code changing.
 *
 * Deliberately unoptimised for now: every hazard takes a fresh copy.  No
 * dirty tracking, no skipping, no reuse heuristics.  Correctness first, and a
 * measurement of what the naive version costs before deciding what to trade.
 * ================================================================ */

#define D3D8_HAZARD_SHADOW_SLOTS 16u

static struct {
    IDirect3DTexture8 *target;
    IDirect3DTexture8 *shadow;
} g_hazard_shadow[D3D8_HAZARD_SHADOW_SLOTS];
static unsigned g_hazard_shadow_count;
static unsigned g_hazard_copies;

IDirect3DTexture8 *d3d8_ResolveReadWriteHazard(IDirect3DDevice8 *dev,
                                               IDirect3DTexture8 *target)
{
    D3D8Texture *tex = (D3D8Texture *)target;
    IDirect3DTexture8 *shadow = NULL;
    D3D11_TEXTURE2D_DESC desc;
    unsigned i;

    if (!dev || !tex || !tex->d3d11_texture)
        return NULL;

    for (i = 0; i < g_hazard_shadow_count; ++i) {
        if (g_hazard_shadow[i].target == target) {
            shadow = g_hazard_shadow[i].shadow;
            break;
        }
    }

    if (shadow == NULL) {
        if (g_hazard_shadow_count >= D3D8_HAZARD_SHADOW_SLOTS)
            return NULL;
        /* Same dimensions and format as the target, which is what
         * d3d8_CopyRuntimeRenderTexture() requires.  No render-target usage:
         * a shadow is only ever read.  Nothing here assumes a resolution, so
         * a wider or taller surface simply gets a wider or taller shadow. */
        ID3D11Texture2D_GetDesc(tex->d3d11_texture, &desc);
        if (FAILED(dev->lpVtbl->CreateTexture(dev, desc.Width, desc.Height, 1,
                                              0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_DEFAULT, &shadow)) ||
            shadow == NULL)
            return NULL;
        g_hazard_shadow[g_hazard_shadow_count].target = target;
        g_hazard_shadow[g_hazard_shadow_count].shadow = shadow;
        ++g_hazard_shadow_count;
        fprintf(stderr, "[INFO D3D-HAZARD] shadow %u created, %ux%u\n",
                g_hazard_shadow_count, (unsigned)desc.Width,
                (unsigned)desc.Height);
    }

    if (FAILED(d3d8_CopyRuntimeRenderTexture(shadow, target)))
        return NULL;
    ++g_hazard_copies;

    /* The copy unbinds the render target and the shader inputs -- it cannot
     * write into a bound resource.  Put the target back, or this draw and
     * every one after it lands nowhere. */
    if (FAILED(d3d8_BindRuntimeRenderTexture(target, FALSE, 0)))
        return NULL;

    /* Temporary, while the behaviour is being verified: enough to see the
     * hazard firing and how often, without flooding a three-minute run. */
    {
        static unsigned logged;
        if (logged < 8u || (g_hazard_copies % 20000u) == 0u) {
            ++logged;
            fprintf(stderr, "[INFO D3D-HAZARD] resolved by copy #%u\n",
                    g_hazard_copies);
        }
    }
    return shadow;
}

void d3d8_ReleaseHazardShadows(void)
{
    unsigned i;

    for (i = 0; i < g_hazard_shadow_count; ++i) {
        if (g_hazard_shadow[i].shadow)
            g_hazard_shadow[i].shadow->lpVtbl->Release(
                g_hazard_shadow[i].shadow);
        g_hazard_shadow[i].target = NULL;
        g_hazard_shadow[i].shadow = NULL;
    }
    g_hazard_shadow_count = 0;
}

/* Is the swap chain itself the current render target?
 *
 * The title never renders straight to the front buffer -- it draws into its
 * own surfaces and the flip copies one across.  A draw that lands here is a
 * pass escaping onto the screen, which is visible but invisible to any
 * back-buffer sampling, since it happens after the copy. */
int d3d8_IsDefaultRenderTargetBound(void)
{
    return g_default_rtv_bound;
}

void d3d8_RestoreDefaultRenderTarget(void)
{
    D3D11_VIEWPORT vp;

    if (!g_device_state.d3d11_context || !g_device_state.default_rtv)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(
        g_device_state.d3d11_context, 1,
        &g_device_state.default_rtv, g_device_state.default_dsv);
    g_default_rtv_bound = 1;

    /* And the viewport back with it, or the next draw inherits whatever
     * offscreen target was bound last. */
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)d3d8_GetBackbufferWidth();
    vp.Height = (FLOAT)d3d8_GetBackbufferHeight();
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
    g_bound_vp_w = vp.Width;
    g_bound_vp_h = vp.Height;
    g_bound_rtv = g_device_state.default_rtv;
    g_bound_dsv = g_device_state.default_dsv;
}

/* ================================================================
 * D3D11 initialization helpers
 * ================================================================ */

static HRESULT d3d11_create_device_and_swap_chain(
    D3D8DeviceState *state,
    D3DPRESENT_PARAMETERS *pp)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    UINT create_flags = 0;
    HRESULT hr;

    /* The validation layer is useful for graphics diagnosis but expensive
     * for the title's thousands of small draws, even in a debug-symbol build. */
    if (getenv("CONKER_D3D_DEBUG")) create_flags |= D3D11_CREATE_DEVICE_DEBUG;

    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = pp->BackBufferCount ? pp->BackBufferCount : 1;
    scd.BufferDesc.Width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    scd.BufferDesc.Height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = pp->hDeviceWindow;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = pp->Windowed;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        create_flags,
        NULL, 0,
        D3D11_SDK_VERSION,
        &scd,
        &state->swap_chain,
        &state->d3d11_device,
        &feature_level,
        &state->d3d11_context
    );

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Failed to create D3D11 device: 0x%08lX\n", hr);
        return hr;
    }

    state->hwnd = pp->hDeviceWindow;
    state->width = scd.BufferDesc.Width;
    state->height = scd.BufferDesc.Height;

    /* Validation uses a fixed native-size window. DXGI's automatic Alt+Enter
     * would otherwise stretch it to the monitor before a launcher exists. */
    if (pp->Windowed) {
        IDXGIFactory *factory = NULL;
        hr = IDXGISwapChain_GetParent(state->swap_chain, &IID_IDXGIFactory,
                                      (void **)&factory);
        if (SUCCEEDED(hr)) {
            hr = IDXGIFactory_MakeWindowAssociation(factory, pp->hDeviceWindow,
                    DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            IDXGIFactory_Release(factory);
        }
        if (FAILED(hr))
            fprintf(stderr, "D3D8: Cannot disable automatic fullscreen: 0x%08lX\n", hr);
    }

    return S_OK;
}

static HRESULT d3d11_create_render_targets(D3D8DeviceState *state)
{
    ID3D11Texture2D *back_buffer = NULL;
    D3D11_TEXTURE2D_DESC depth_desc;
    HRESULT hr;

    /* Create render target view from swap chain back buffer */
    hr = IDXGISwapChain_GetBuffer(state->swap_chain, 0,
                                   &IID_ID3D11Texture2D,
                                   (void **)&back_buffer);
    if (FAILED(hr)) return hr;

    hr = ID3D11Device_CreateRenderTargetView(state->d3d11_device,
                                              (ID3D11Resource *)back_buffer,
                                              NULL, &state->default_rtv);
    ID3D11Texture2D_Release(back_buffer);
    if (FAILED(hr)) return hr;

    /* Create depth stencil */
    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.Width = state->width;
    depth_desc.Height = state->height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &depth_desc,
                                       NULL, &state->default_depth);
    if (FAILED(hr)) return hr;

    hr = ID3D11Device_CreateDepthStencilView(state->d3d11_device,
                                              (ID3D11Resource *)state->default_depth,
                                              NULL, &state->default_dsv);
    if (FAILED(hr)) return hr;

    /* Bind default render targets */
    ID3D11DeviceContext_OMSetRenderTargets(state->d3d11_context, 1,
                                            &state->default_rtv,
                                            state->default_dsv);
    g_bound_rtv = state->default_rtv;
    g_bound_dsv = state->default_dsv;

    return S_OK;
}

static void d3d8_init_default_states(D3D8DeviceState *state)
{
    /* Set Xbox D3D8 default render states */
    memset(state->render_states, 0, sizeof(state->render_states));
    state->render_states[D3DRS_ZENABLE]           = 1;
    state->render_states[D3DRS_FILLMODE]          = D3DFILL_SOLID;
    state->render_states[D3DRS_SHADEMODE]         = 2; /* D3DSHADE_GOURAUD */
    state->render_states[D3DRS_ZWRITEENABLE]      = TRUE;
    state->render_states[D3DRS_ALPHATESTENABLE]    = FALSE;
    state->render_states[D3DRS_SRCBLEND]          = D3DBLEND_ONE;
    state->render_states[D3DRS_DESTBLEND]         = D3DBLEND_ZERO;
    state->render_states[D3DRS_CULLMODE]          = D3DCULL_CCW;
    state->render_states[D3DRS_ZFUNC]             = D3DCMP_LESSEQUAL;
    state->render_states[D3DRS_ALPHAREF]          = 0;
    state->render_states[D3DRS_ALPHAFUNC]         = D3DCMP_ALWAYS;
    state->render_states[D3DRS_ALPHABLENDENABLE]   = FALSE;
    state->render_states[D3DRS_FOGENABLE]         = FALSE;
    state->render_states[D3DRS_STENCILENABLE]     = FALSE;
    state->render_states[D3DRS_COLORWRITEENABLE]  = 0x0F;

    /* Texture arguments are enums in which D3DTA_DIFFUSE is legitimately
     * zero.  Give every stage explicit defaults so the shader uploader never
     * has to use zero as an "unset" sentinel. */
    state->tss[0][D3DTSS_COLOROP]   = D3DTOP_MODULATE;
    state->tss[0][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    state->tss[0][D3DTSS_COLORARG2] = D3DTA_CURRENT;
    state->tss[0][D3DTSS_ALPHAOP]   = D3DTOP_SELECTARG1;
    state->tss[0][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    state->tss[0][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    for (int stage = 1; stage < MAX_TEXTURE_STAGES; ++stage) {
        state->tss[stage][D3DTSS_COLOROP]   = D3DTOP_DISABLE;
        state->tss[stage][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        state->tss[stage][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        state->tss[stage][D3DTSS_ALPHAOP]   = D3DTOP_DISABLE;
        state->tss[stage][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        state->tss[stage][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    }

    /* Default viewport */
    state->viewport.X = 0;
    state->viewport.Y = 0;
    state->viewport.Width = state->width;
    state->viewport.Height = state->height;
    state->viewport.MinZ = 0.0f;
    state->viewport.MaxZ = 1.0f;

    /* Identity matrices */
    for (int i = 0; i < MAX_TRANSFORMS; i++) {
        memset(&state->transforms[i], 0, sizeof(D3DMATRIX));
        state->transforms[i]._11 = 1.0f;
        state->transforms[i]._22 = 1.0f;
        state->transforms[i]._33 = 1.0f;
        state->transforms[i]._44 = 1.0f;
    }

    state->vertex_shader = 0;
    state->pixel_shader = 0;
    state->in_scene = FALSE;
}

/* ================================================================
 * IDirect3DDevice8 method implementations
 * ================================================================ */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{
    (void)self;
    return InterlockedIncrement(&g_device_state.ref_count);
}

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    (void)self;
    LONG ref = InterlockedDecrement(&g_device_state.ref_count);
    if (ref <= 0) {
        for (unsigned stage = 0; stage < 4; ++stage) {
            IDirect3DTexture8 *texture = (IDirect3DTexture8 *)g_cur_textures[stage];
            if (texture) texture->lpVtbl->Release(texture);
            g_cur_textures[stage] = NULL;
        }
        /* Cleanup subsystems first */
        up_ring_shutdown();
        d3d8_vsh_shutdown();
        d3d8_nv2a_gpu_shutdown();
        d3d8_combiners_shutdown();
        d3d8_states_shutdown();
        d3d8_shaders_shutdown();

        /* Cleanup D3D11 resources */
        D3D8DeviceState *s = &g_device_state;
        runtime_depth_shutdown();
        g_bound_rtv = NULL;
        if (s->default_dsv) { ID3D11DepthStencilView_Release(s->default_dsv); s->default_dsv = NULL; }
        if (s->default_depth) { ID3D11Texture2D_Release(s->default_depth); s->default_depth = NULL; }
        if (s->default_rtv) { ID3D11RenderTargetView_Release(s->default_rtv); s->default_rtv = NULL; }
        if (s->swap_chain) { IDXGISwapChain_Release(s->swap_chain); s->swap_chain = NULL; }
        if (s->d3d11_context) { ID3D11DeviceContext_Release(s->d3d11_context); s->d3d11_context = NULL; }
        if (s->d3d11_device) { ID3D11Device_Release(s->d3d11_device); s->d3d11_device = NULL; }
        g_device_initialized = FALSE;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8)
{
    (void)self; (void)ppD3D8;
    /* TODO: return the factory */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *pCaps)
{
    (void)self; (void)pCaps;
    /* TODO: fill with Xbox NV2A capabilities */
    return S_OK;
}

static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *pMode)
{
    (void)self; (void)pMode;
    return S_OK;
}

static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *pParams)
{
    (void)self; (void)pParams;
    return S_OK;
}

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP)
{
    (void)self; (void)pPP;
    /* TODO: resize swap chain */
    return S_OK;
}

static DWORD g_d3d_begin_count = 0;
static DWORD g_d3d_end_count = 0;
static DWORD g_d3d_clear_count = 0;
static DWORD g_d3d_draw_count = 0;
static DWORD g_d3d_settransform_count = 0;
static DWORD g_d3d_setrs_count = 0;
static DWORD g_d3d_settexture_count = 0;

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty)
{
    static DWORD frame_count = 0;
    static DWORD last_tick = 0;
    (void)self; (void)src; (void)dst; (void)hWnd; (void)pDirty;

    frame_count++;
    DWORD now = GetTickCount();
    if (last_tick == 0) last_tick = now;
    if (now - last_tick >= 2000) {
        fprintf(stderr, "  [D3D] %.1fs: %u present (%.1f fps), %u begin, %u end, "
                "%u clear, %u draw, %u xform, %u rs, %u tex\n",
                (now - last_tick) / 1000.0, frame_count,
                frame_count * 1000.0 / (now - last_tick),
                g_d3d_begin_count, g_d3d_end_count,
                g_d3d_clear_count, g_d3d_draw_count,
                g_d3d_settransform_count, g_d3d_setrs_count,
                g_d3d_settexture_count);
        fflush(stderr);
        frame_count = 0;
        g_d3d_begin_count = g_d3d_end_count = 0;
        g_d3d_clear_count = g_d3d_draw_count = 0;
        g_d3d_settransform_count = g_d3d_setrs_count = 0;
        g_d3d_settexture_count = 0;
        last_tick = now;
    }

    /* Pump Windows messages: the game's internal main loop drives rendering,
     * so our external message pump never runs. Process messages here to keep
     * the window responsive and handle input. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr, "[INFO WINDOW] WM_QUIT observed by device Present wParam=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    pgraph_d3d11_present_notify();
    return IDXGISwapChain_Present(g_device_state.swap_chain, 1, 0);
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)iBackBuffer; (void)Type; (void)ppSurface;
    /* TODO: wrap back buffer as D3D8 surface */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = TRUE;
    g_d3d_begin_count++;
    return S_OK;
}

static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = FALSE;
    g_d3d_end_count++;
    return S_OK;
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    (void)self; (void)Count; (void)pRects; (void)Stencil;
    g_d3d_clear_count++;

    /* Clear the active attachments. Clearing the swap chain during an
     * offscreen pass leaves stale scene pixels and erases unrelated output. */
    if (Flags & D3DCLEAR_TARGET) {
        float clear_color[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,  /* R */
            ((Color >>  8) & 0xFF) / 255.0f,  /* G */
            ((Color >>  0) & 0xFF) / 255.0f,  /* B */
            ((Color >> 24) & 0xFF) / 255.0f,  /* A */
        };
        ID3D11RenderTargetView *target = g_bound_rtv
                ? g_bound_rtv : g_device_state.default_rtv;
        ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context,
                                                  target, clear_color);
    }

    if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        UINT clear_flags = 0;
        if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= D3D11_CLEAR_DEPTH;
        if (Flags & D3DCLEAR_STENCIL) clear_flags |= D3D11_CLEAR_STENCIL;

        if (g_bound_dsv)
            ID3D11DeviceContext_ClearDepthStencilView(g_device_state.d3d11_context,
                                                     g_bound_dsv, clear_flags,
                                                     Z, (UINT8)Stencil);
        if (g_bound_dsv) runtime_depth_written();
    }

    return S_OK;
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
    (void)self;
    g_d3d_settransform_count++;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        g_device_state.transforms[(DWORD)State] = *pMatrix;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
    (void)self;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        *pMatrix = g_device_state.transforms[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value)
{
    (void)self;
    g_d3d_setrs_count++;
    if ((DWORD)State < MAX_RENDER_STATES) {
        g_device_state.render_states[(DWORD)State] = Value;
    }
    /* Mark combiner state dirty if any PS register combiner state changed */
    if ((DWORD)State >= D3DRS_PSALPHAINPUTS0 && (DWORD)State <= D3DRS_PSINPUTTEXTURE) {
        d3d8_combiners_mark_dirty();
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue)
{
    (void)self;
    if ((DWORD)State < MAX_RENDER_STATES && pValue) {
        *pValue = g_device_state.render_states[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES) {
        g_device_state.tss[Stage][(DWORD)Type] = Value;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES && pValue) {
        *pValue = g_device_state.tss[Stage][(DWORD)Type];
    }
    return S_OK;
}

/* One texture stage state, for code inside the layer that needs to read the
 * stage back -- the shader builder does, to decide whether a texture matrix is
 * live. */
DWORD d3d8_GetTextureStageStateValue(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type)
{
    if (Stage >= MAX_TEXTURE_STAGES || (DWORD)Type >= MAX_TSS_STATES)
        return 0;
    return g_device_state.tss[Stage][(DWORD)Type];
}

DWORD d3d8_GetDrawCount(void) { return g_d3d_draw_count; }

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
    (void)self;
    g_d3d_settexture_count++;
    if (Stage >= 4) return E_INVALIDARG;
    if (g_cur_textures[Stage] != pTexture) {
        IDirect3DTexture8 *next = (IDirect3DTexture8 *)pTexture;
        IDirect3DTexture8 *previous = (IDirect3DTexture8 *)g_cur_textures[Stage];
        if (next) next->lpVtbl->AddRef(next);
        if (previous) previous->lpVtbl->Release(previous);
    }
    g_cur_textures[Stage] = pTexture;

    /* Bind SRV to pixel shader */
    if (pTexture) {
        D3D8Texture *tex = (D3D8Texture *)pTexture;
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
            Stage, 1, &tex->srv);
        /* Mark texture stage as active */
        if (g_device_state.tss[Stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    } else {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
            Stage, 1, &null_srv);
        g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
    (void)self; (void)Stage; (void)ppTexture;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
    (void)self;
    if (StreamNumber != 0) return S_OK; /* Only stream 0 supported */
    g_cur_vb = pStreamData;
    g_cur_vb_stride = Stride;

    if (pStreamData) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)pStreamData;
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &Stride, &offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
    (void)self; (void)StreamNumber; (void)ppStreamData; (void)pStride;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
    (void)self;
    g_cur_ib = pIndexData;
    g_cur_ib_base_vertex = BaseVertexIndex;

    if (pIndexData) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)pIndexData;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32)
            ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
    (void)self; (void)ppIndexData; (void)pBaseVertexIndex;
    return E_NOTIMPL;
}

static D3D11_PRIMITIVE_TOPOLOGY map_primitive_type(D3DPRIMITIVETYPE pt, UINT count, UINT *out_count)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: *out_count = count + 2; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case D3DPT_TRIANGLEFAN:   *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_LINELIST:      *out_count = count * 2; return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     *out_count = count + 1; return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_POINTLIST:     *out_count = count;     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_QUADLIST:      *out_count = count * 6; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                  *out_count = 0;          return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* ================================================================
 * Triangle fan / quad list → triangle list conversion
 *
 * D3D11 doesn't support triangle fans or quad lists.
 * Convert vertex data in-place to triangle list.
 * Returns malloc'd buffer (caller must free) or NULL if no conversion needed.
 * ================================================================ */

static void *convert_fan_or_quad(D3DPRIMITIVETYPE pt, const void *src,
                                  UINT prim_count, UINT stride,
                                  UINT *out_vertex_count)
{
    BYTE *dst;
    const BYTE *s = (const BYTE *)src;
    UINT i;

    if (pt == D3DPT_TRIANGLEFAN) {
        /* Fan: vertex 0 is the hub, each triangle is (0, i+1, i+2) */
        UINT tri_verts = prim_count * 3;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            memcpy(dst + (i * 3 + 0) * stride, s, stride);                      /* v0 (hub) */
            memcpy(dst + (i * 3 + 1) * stride, s + (i + 1) * stride, stride);   /* v[i+1] */
            memcpy(dst + (i * 3 + 2) * stride, s + (i + 2) * stride, stride);   /* v[i+2] */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    if (pt == D3DPT_QUADLIST) {
        /* Quad list: each quad (v0,v1,v2,v3) → 2 triangles (v0,v1,v2), (v0,v2,v3) */
        UINT tri_verts = prim_count * 6;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            const BYTE *q = s + i * 4 * stride;
            memcpy(dst + (i * 6 + 0) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 1) * stride, q + 1 * stride, stride);  /* v1 */
            memcpy(dst + (i * 6 + 2) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 3) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 4) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 5) * stride, q + 3 * stride, stride);  /* v3 */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    return NULL; /* no conversion needed */
}

/* ================================================================
 * DrawPrimitiveUP ring buffer
 *
 * Instead of creating and destroying a D3D11 buffer on every
 * DrawPrimitiveUP call, use a persistent ring buffer.
 * ================================================================ */

#define UP_RING_BUFFER_SIZE (4 * 1024 * 1024)  /* 4MB ring buffer */

static ID3D11Buffer *g_up_ring_buffer = NULL;
static UINT          g_up_ring_offset = 0;

static HRESULT up_ring_init(void)
{
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = UP_RING_BUFFER_SIZE;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, NULL, &g_up_ring_buffer);
}

static void up_ring_shutdown(void)
{
    if (g_up_ring_buffer) {
        ID3D11Buffer_Release(g_up_ring_buffer);
        g_up_ring_buffer = NULL;
    }
    g_up_ring_offset = 0;
}

/* Upload vertex data to ring buffer, returns offset. Returns (UINT)-1 on failure. */
static UINT up_ring_upload(const void *data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_MAP map_type;
    HRESULT hr;
    UINT offset;

    if (!g_up_ring_buffer) {
        if (FAILED(up_ring_init())) return (UINT)-1;
    }

    if (size > UP_RING_BUFFER_SIZE) return (UINT)-1;

    /* Wrap around if not enough space */
    if (g_up_ring_offset + size > UP_RING_BUFFER_SIZE) {
        g_up_ring_offset = 0;
        map_type = D3D11_MAP_WRITE_DISCARD;
    } else {
        map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
    }

    hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0, map_type, 0, &mapped);
    if (FAILED(hr)) return (UINT)-1;

    offset = g_up_ring_offset;
    memcpy((BYTE *)mapped.pData + offset, data, size);

    ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0);

    g_up_ring_offset = (offset + size + 15) & ~15;  /* 16-byte align */
    return offset;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    static unsigned native_draw_trace_count;
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    if (native_draw_trace_count++ < 16) {
        fprintf(stderr,
                "[D3D8-DRAW] primitive=%u start=%u primitives=%u "
                "vertices=%u vb=%p stride=%u shader=%08X scene=%u\n",
                (unsigned)PrimitiveType, StartVertex, PrimitiveCount,
                vertex_count, (void *)g_cur_vb, g_cur_vb_stride,
                g_device_state.vertex_shader,
                (unsigned)g_device_state.in_scene);
    }

    /* Prepare pipeline: shaders, input layout, constant buffers, render states */
    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, StartVertex);
    runtime_depth_drawn();
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    (void)self; (void)MinVertexIndex; (void)NumVertices;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT index_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, StartIndex, (INT)g_cur_ib_base_vertex);
    runtime_depth_drawn();
    return S_OK;
}

/* Vertex-path trace: pgraph raises this for the one draw under study, so
 * the vertices dumped here are exactly the bytes handed to D3D11 after all
 * runtime conversion. */
int g_vtx_trace;
typedef struct { float x, y, z, rhw; uint32_t color; float u, v; } OutVtxDbg;

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count, vb_size, ring_offset;
    const void *draw_data;
    void *converted = NULL;

    if (!pVertexData || !VertexStreamZeroStride) return E_INVALIDARG;

    draw_data = d3d8_host_data_ptr(pVertexData);

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Convert triangle fans and quad lists to triangle lists */
    if (PrimitiveType == D3DPT_TRIANGLEFAN || PrimitiveType == D3DPT_QUADLIST) {
        converted = convert_fan_or_quad(PrimitiveType, draw_data,
                                         PrimitiveCount, VertexStreamZeroStride,
                                         &vertex_count);
        if (converted) draw_data = converted;
    }

    vb_size = vertex_count * VertexStreamZeroStride;

    /* Upload to ring buffer */
    ring_offset = up_ring_upload(draw_data, vb_size);
    if (converted) free(converted);

    if (ring_offset == (UINT)-1) return E_OUTOFMEMORY;

    /* Bind ring buffer at the right offset */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &g_up_ring_buffer, &VertexStreamZeroStride, &ring_offset);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    { BOOL prog = d3d8_vsh_prepare_draw(g_device_state.vertex_shader);
      if (!prog) d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
      d3d8_nv2a_gpu_bind();
      if (g_vtx_trace) {
          const OutVtxDbg *v = (const OutVtxDbg *)draw_data;
          UINT vi, n = vertex_count < 6u ? vertex_count : 6u;
          fprintf(stderr, "[VTX]   D3D11: verts=%u stride=%u topo=%d  shader=%08X path=%s" "\n",
                  vertex_count, VertexStreamZeroStride, (int)topology,
                  (unsigned)g_device_state.vertex_shader,
                  prog ? "PROGRAMMABLE VS" : "FVF fixed-function");
          if (VertexStreamZeroStride == sizeof(OutVtxDbg))
              for (vi = 0; vi < n; ++vi)
                  fprintf(stderr, "[VTX]     v%u  xyz=%.2f,%.2f,%.2f rhw=%.4f  uv=%.4f,%.4f  c=%08X" "\n",
                          vi, v[vi].x, v[vi].y, v[vi].z, v[vi].rhw,
                          v[vi].u, v[vi].v, v[vi].color);
          fflush(stderr);
      } }
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, 0);
    runtime_depth_drawn();

    /* Restore previous VB binding if any */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        UINT restore_offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &restore_offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self; (void)MinVertexIndex;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *tmp_vb = NULL, *tmp_ib = NULL;
    UINT index_count, vb_size, ib_size, offset = 0;
    UINT idx_bytes;
    DXGI_FORMAT ib_fmt;
    HRESULT hr;

    if (!pVertexData || !pIndexData || !VertexStreamZeroStride) return E_INVALIDARG;

    pVertexData = d3d8_host_data_ptr(pVertexData);
    pIndexData = d3d8_host_data_ptr(pIndexData);

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    idx_bytes = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
    ib_fmt = (IndexDataFormat == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    vb_size = NumVertices * VertexStreamZeroStride;
    ib_size = index_count * idx_bytes;

    /* Create temp vertex buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = vb_size;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = pVertexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_vb);
    if (FAILED(hr)) return hr;

    /* Create temp index buffer */
    bd.ByteWidth = ib_size;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = pIndexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_ib);
    if (FAILED(hr)) { ID3D11Buffer_Release(tmp_vb); return hr; }

    /* Bind, prepare, draw */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &tmp_vb, &VertexStreamZeroStride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
        tmp_ib, ib_fmt, 0);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    if (!d3d8_vsh_prepare_draw(g_device_state.vertex_shader))
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, 0, 0);
    runtime_depth_drawn();

    /* Cleanup temp buffers */
    ID3D11Buffer_Release(tmp_ib);
    ID3D11Buffer_Release(tmp_vb);

    /* Restore previous bindings */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &offset);
    }
    if (g_cur_ib) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)g_cur_ib;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateTextureImpl(Width, Height, Levels, Usage, Format, ppTexture);
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateVertexBufferImpl(Length, Usage, FVF, ppVertexBuffer);
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateIndexBufferImpl(Length, Usage, Format, ppIndexBuffer);
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)Lockable; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface)
{
    (void)self; (void)pRenderTarget; (void)pZStencilSurface;
    /* TODO: resolve D3D8 surface to D3D11 RTV/DSV */
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget)
{
    (void)self; (void)ppRenderTarget;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface)
{
    (void)self; (void)ppZStencilSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) {
        g_device_state.viewport = *pViewport;

        D3D11_VIEWPORT d3d11_vp;
        d3d11_vp.TopLeftX = (FLOAT)pViewport->X;
        d3d11_vp.TopLeftY = (FLOAT)pViewport->Y;
        d3d11_vp.Width    = (FLOAT)pViewport->Width;
        d3d11_vp.Height   = (FLOAT)pViewport->Height;
        d3d11_vp.MinDepth = pViewport->MinZ;
        d3d11_vp.MaxDepth = pViewport->MaxZ;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &d3d11_vp);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) *pViewport = g_device_state.viewport;
    return S_OK;
}

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) g_device_state.material = *pMaterial;
    return S_OK;
}

static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) *pMaterial = g_device_state.material;
    return S_OK;
}

static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) g_device_state.lights[Index] = *pLight;
    return S_OK;
}

static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) *pLight = g_device_state.lights[Index];
    return S_OK;
}

static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD Index, BOOL Enable)
{
    (void)self;
    if (Index < MAX_LIGHTS) g_device_state.light_enable[Index] = Enable;
    return S_OK;
}

static HRESULT __stdcall dev_CreateVertexShader(IDirect3DDevice8 *self, const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
    (void)self; (void)pDeclaration; (void)Usage;
    if (!pHandle) return E_INVALIDARG;
    if (!pFunction) return E_INVALIDARG;
    /* Count instructions: each is 4 DWORDs, last has bit 0 of word[3] set (END flag) */
    {
        int i, num_insns = 0;
        for (i = 0; i < 136; i++) {
            num_insns++;
            if (pFunction[i * 4 + 3] & 1) break;  /* END bit in last word */
        }
        return d3d8_vsh_create_shader(pFunction, num_insns, pHandle);
    }
}

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.vertex_shader = Handle;
    return S_OK;
}

static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.vertex_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self;
    d3d8_vsh_set_constant(Register, pConstantData, ConstantCount);
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.pixel_shader = Handle;
    d3d8_combiners_set_pixel_shader(Handle);
    return S_OK;
}

static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.pixel_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self; (void)Register; (void)pConstantData; (void)ConstantCount;
    return S_OK;
}

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp)
{
    (void)self; (void)Flags; (void)pRamp;
}

static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp)
{
    (void)self; (void)pRamp;
}

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries)
{
    (void)self; (void)PaletteNumber; (void)pEntries;
    return S_OK;
}

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush)
{
    (void)self; (void)Count; (void)ppPush;
    /* TODO: Xbox push buffer emulation */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *pPush)
{
    (void)self; (void)pPush;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD Flags)
{
    (void)self; (void)Flags;

    /* Pump Windows messages (same as dev_Present) */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return IDXGISwapChain_Present(g_device_state.swap_chain, 1, 0);
}

/* ================================================================
 * Vtable
 * ================================================================ */

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetDirect3D,
    dev_GetDeviceCaps,
    dev_GetDisplayMode,
    dev_GetCreationParameters,
    dev_Reset,
    dev_Present,
    dev_GetBackBuffer,
    dev_BeginScene,
    dev_EndScene,
    dev_Clear,
    dev_SetTransform,
    dev_GetTransform,
    dev_SetRenderState,
    dev_GetRenderState,
    dev_SetTextureStageState,
    dev_GetTextureStageState,
    dev_SetTexture,
    dev_GetTexture,
    dev_SetStreamSource,
    dev_GetStreamSource,
    dev_SetIndices,
    dev_GetIndices,
    dev_DrawPrimitive,
    dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture,
    dev_CreateVertexBuffer,
    dev_CreateIndexBuffer,
    dev_CreateRenderTarget,
    dev_CreateDepthStencilSurface,
    dev_SetRenderTarget,
    dev_GetRenderTarget,
    dev_GetDepthStencilSurface,
    dev_SetViewport,
    dev_GetViewport,
    dev_SetMaterial,
    dev_GetMaterial,
    dev_SetLight,
    dev_GetLight,
    dev_LightEnable,
    dev_SetVertexShader,
    dev_GetVertexShader,
    dev_SetVertexShaderConstant,
    dev_SetPixelShader,
    dev_GetPixelShader,
    dev_SetPixelShaderConstant,
    dev_SetGammaRamp,
    dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush,
    dev_EndPush,
    dev_Swap,
};

/* ================================================================
 * Public API
 * ================================================================ */

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_device_initialized ? &g_device : NULL;
}

/* ================================================================
 * IDirect3D8 factory implementation
 * ================================================================ */

static IDirect3D8 g_d3d8;
static LONG g_d3d8_ref = 0;

static HRESULT __stdcall d3d8_QueryInterface(IDirect3D8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall d3d8_AddRef(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_d3d8_ref);
}

static ULONG __stdcall d3d8_Release(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedDecrement(&g_d3d8_ref);
}

static HRESULT __stdcall d3d8_CreateDevice(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice)
{
    (void)self; (void)Adapter; (void)DeviceType; (void)BehaviorFlags;
    HRESULT hr;

    if (!pPP || !ppDevice) return E_INVALIDARG;

    memset(&g_device_state, 0, sizeof(g_device_state));
    g_device_state.ref_count = 1;

    if (!pPP->hDeviceWindow) pPP->hDeviceWindow = hFocusWindow;

    hr = d3d11_create_device_and_swap_chain(&g_device_state, pPP);
    if (FAILED(hr)) return hr;

    hr = d3d11_create_render_targets(&g_device_state);
    if (FAILED(hr)) return hr;

    d3d8_init_default_states(&g_device_state);

    /* Set initial viewport (D3D11 requires explicit viewport) */
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = (FLOAT)g_device_state.width;
        vp.Height   = (FLOAT)g_device_state.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
    }

    /* Initialize shader and state subsystems */
    hr = d3d8_shaders_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Shader init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_states_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: State init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_combiners_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Combiner init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to fixed-function pixel shaders */
    }

    hr = d3d8_vsh_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VSH init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to FVF vertex shaders */
    }

    g_device.lpVtbl = &g_device_vtbl;
    g_device_initialized = TRUE;

    *ppDevice = &g_device;
    fprintf(stderr, "D3D8: Device created (%ux%u)\n", g_device_state.width, g_device_state.height);
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d8_QueryInterface,
    d3d8_AddRef,
    d3d8_Release,
    d3d8_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}
