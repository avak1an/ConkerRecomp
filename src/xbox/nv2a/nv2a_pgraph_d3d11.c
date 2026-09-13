/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Translates NV2A push buffer methods into D3D8→D3D11 rendering calls.
 * Designed for Xbox static recompilation (xboxrecomp toolkit).
 *
 * Textures, shaders, and render targets come from live guest memory.
 */

#include "nv2a_pgraph_d3d11.h"
#include "nv2a_vsh.h"
#include "nv2a_vertex_cache.h"
#include "nv2a_fog.h"
#include "nv2a_immediate.h"
#include "runtime_config.h"

/* Fixed constant slots the viewport registers alias.  Read off this
 * title own program: slot 12 is MUL o0.xyz with c58 (scale, paired with
 * the RCC that forms 1/w) and slot 13 is MAD o0.xyz with c59 (offset). */
#define NV2A_VP_CONST_VPSCL 58u
#define NV2A_VP_CONST_VPOFF 59u
#include "nv2a_regs.h"

/* RAMHT resolution lives in nv2a_core.c, which owns PRAMIN and the PFIFO
 * registers.  Declared here rather than including nv2a_state.h, which would
 * drag in the whole QEMU-shim device model for two functions. */
uint32_t nv2a_ramht_lookup(uint32_t handle, uint32_t *out_context);
int nv2a_dma_from_handle(uint32_t handle, uint32_t *out_base,
                         uint32_t *out_limit);
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <malloc.h>
#include <math.h>
#include "../d3d/d3d8_swizzle.h"

/* D3D8 device — we include the full header for COM vtable access */
#include "../d3d/d3d8_xbox.h"
#include "../d3d/d3d8_nv2a_gpu.h"
#include "../d3d/d3d8_combiners.h"
extern IDirect3DDevice8 *xbox_GetD3DDevice(void);
extern UINT d3d8_GetBackbufferWidth(void);
extern UINT d3d8_GetBackbufferHeight(void);
extern ptrdiff_t g_xbox_mem_offset;
extern volatile int g_recomp_frontend_host_carousel_slot;
extern uint32_t nv2a_display_scanout_start(void);
extern int nv2a_pgraph_flip_method(uint32_t method, uint32_t parameter);
extern void d3d8_DebugGetViewportSize(float *w, float *h);
extern void d3d8_PumpWindowMessages(int force);
extern void d3d8_combiners_set_pixel_shader(DWORD token);
extern BOOL d3d8_combiners_active(void);

/* Six words a stage across eight stages, plus control, the two final-combiner
 * words and the shader stage program. */
#define NV2A_COMBINER_SNAPSHOT_WORDS (8 * 6 + 5 + 1 + 4 * 7)
extern void d3d8_DebugDumpRuntimeTexture(IDirect3DTexture8 *texture, const char *path);

enum { FRONTEND_CAPTURE_SLOT_COUNT = 11 };

static int frontend_capture_slot_valid(int slot)
{
    return (slot >= 0 && slot <= 5) || slot == 10;
}

/* ══════════════════════════════════════════════════════════════════════
 * NV2A method constants (from nv2a_regs.h, subset for translator)
 * ══════════════════════════════════════════════════════════════════════ */

/* Method values come exclusively from nv2a_regs.h.  A second local table
 * previously shadowed the authoritative map with stale CLEAR_SURFACE and
 * texture CONTROL0 offsets. */

/* NV2A draw modes → D3D primitive types */
static int nv2a_draw_mode_to_d3d(uint32_t mode) {
    switch (mode) {
        case 1:  return D3DPT_POINTLIST;
        case 2:  return D3DPT_LINELIST;
        case 3:  return D3DPT_LINESTRIP;  /* LINE_LOOP → LINE_STRIP */
        case 4:  return D3DPT_LINESTRIP;
        case 5:  return D3DPT_TRIANGLELIST;
        case 6:  return D3DPT_TRIANGLESTRIP;
        case 7:  return D3DPT_TRIANGLEFAN;
        case 8:  return D3DPT_TRIANGLELIST; /* QUADS → TRI_LIST (needs conversion) */
        default: return D3DPT_TRIANGLELIST;
    }
}

/* NV2A blend factors → D3D blend */
static uint32_t nv2a_blend_to_d3d(uint32_t nv) {
    switch (nv) {
        case 0x0000: return D3DBLEND_ZERO;
        case 0x0001: return D3DBLEND_ONE;
        case 0x0300: return D3DBLEND_SRCCOLOR;
        case 0x0301: return D3DBLEND_INVSRCCOLOR;
        case 0x0302: return D3DBLEND_SRCALPHA;
        case 0x0303: return D3DBLEND_INVSRCALPHA;
        case 0x0304: return D3DBLEND_DESTALPHA;
        case 0x0305: return D3DBLEND_INVDESTALPHA;
        case 0x0306: return D3DBLEND_DESTCOLOR;
        case 0x0307: return D3DBLEND_INVDESTCOLOR;
        case 0x0308: return D3DBLEND_SRCALPHASAT;
        default:     return D3DBLEND_ONE;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Translator State
 * ══════════════════════════════════════════════════════════════════════ */

/* Inline vertex buffer - max 16K vertices per draw */
#define MAX_INLINE_VERTS 16384
#define INLINE_VERT_DWORDS 5  /* X, Y, U, V, Color */

/* Pretransformed FVF vertex with independent, full-precision fog. */
typedef struct {
    float x, y, z, rhw;
    uint32_t color;
    uint32_t specular;
    float u, v, r, q;
    float extra_uv[3][4];
    float fog;
} OutputVertex;

#define FRONTEND_SURFACE_REGISTRATION_COUNT 64
#define FRONTEND_PENDING_SURFACE_COPY_COUNT 16

typedef struct FrontendRuntimeTarget {
    uint32_t guest_offset;
    UINT width;
    UINT height;
    uint32_t last_clear_submission;
    uint32_t last_used_submission;
    IDirect3DTexture8 *texture;
    struct FrontendRuntimeTarget *next;
} FrontendRuntimeTarget;

typedef struct {
    uint32_t guest_offset;
    UINT width;
    UINT height;
    uint32_t serial;
    uint32_t persistent;
} FrontendSurfaceRegistration;

typedef struct {
    uint32_t destination;
    uint32_t source;
    uint32_t serial;
} FrontendPendingSurfaceCopy;

static struct {
    /* Draw state */
    int in_draw;           /* Between BEGIN and END */
    uint32_t draw_mode;    /* NV2A draw mode (0=end, 6=tristrip, etc.) */
    int d3d_prim_type;     /* Translated D3D prim type */

    /* Inline vertex accumulator */
    uint32_t inline_data[MAX_INLINE_VERTS * INLINE_VERT_DWORDS];
    int indexed_draw_invalid;
    uint32_t inline_count; /* Number of dwords accumulated */
    uint32_t fog_enable, fog_mode, fog_color;
    float fog_params[3];
    uint32_t vert_stride;  /* Dwords per vertex, from the format registers */
    /* Dword offset of each attribute within an inline vertex, or -1.  See
     * inline_layout_from_formats(). */
    int      inline_pos_offset;
    uint32_t inline_pos_size;
    int      inline_diffuse_offset;
    int      inline_texcoord_offset;
    uint32_t inline_texcoord_size;
    int      inline_layout_valid;

    /* Live NV097 vertex-array bindings.  These are produced every frame by
     * the native Frontend geometry compiler; retaining them lets the array
     * draw path diagnose and ultimately bind this run's guest buffers rather
     * than relying on a nonexistent host D3D stream. */
    /* Object and memory bindings.
     *
     * The push buffer names graphics objects and memory contexts by handle,
     * resolved through the channel's RAMHT.  subchannel_object holds the raw
     * handle bound by SET_OBJECT and subchannel_instance its PRAMIN offset;
     * dma_context holds the guest base each SET_CONTEXT_DMA_* names, indexed
     * by subchannel and then by (method - 0x0180) / 4.
     *
     * Per subchannel, not shared.  A context DMA belongs to the object
     * bound in a subchannel, so a bind issued on one subchannel says
     * nothing about any other.  While this array was shared, a single
     * SET_CONTEXT_DMA_SEMAPHORE attributed to subchannel 4 replaced the
     * base Kelvin had bound on subchannel 0, and the 37 retire writes that
     * followed were discarded -- 74 reclaim units, which is exactly the
     * shortfall sub_0053C190 then span on forever. */
    uint32_t subchannel_object[8];
    uint32_t subchannel_instance[8];
    uint32_t dma_context[8][10];
    uint32_t dma_context_handle[8][10];
    uint32_t dma_context_limit[8][10];

    /* Vertex program store.
     *
     * The title uploads microcode through the 32-dword window at
     * NV097_SET_TRANSFORM_PROGRAM (0x0B00), four dwords per 128-bit
     * instruction, appending at the pointer that SET_TRANSFORM_PROGRAM_LOAD
     * establishes.  NV2A has 136 instruction slots. */
    uint32_t transform_program[136][4];
    uint32_t transform_program_load;   /* next instruction slot to write */
    uint32_t transform_program_sub;    /* dword within that slot, 0-3 */
    uint32_t transform_program_count;  /* highest slot written, + 1 */
    uint32_t transform_program_start;
    uint32_t transform_execution_mode;
    int      transform_program_dirty;

    /* Semaphore release. The title tracks GPU progress by having the engine
     * write a counter into guest memory, then waits for it to catch up. */
    /* Per subchannel for the same reason as dma_context: the offset is
     * Kelvin state, and a packet attributed to another subchannel must
     * not move where subchannel 0 retires to.  With one shared field a
     * stray SET_SEMAPHORE_OFFSET sent every later release to a valid but
     * wrong address, where it counted as written and advanced nothing. */
    uint32_t semaphore_offset[8];

    uint32_t array_offset[16];
    struct {
        uint32_t address, bytes, stride, offset;
    } gather_active[16];
    unsigned gather_active_count;
    int gather_prepared;
    /* Full per-attribute layout of the inline stream.  The three-field
     * position/diffuse/texcoord summary is enough for the fixed-function
     * path but not for a vertex program, which reads v0..v15 by number. */
    int      inline_attr_off[16];      /* dword offset, -1 if absent */
    uint32_t inline_attr_size[16];
    uint32_t inline_attr_type[16];
    uint32_t array_format[16];
    float immediate_value[16][4];
    int immediate_draw;
    uint32_t transform_constant[192][4];
    uint32_t transform_constant_load;
    uint32_t transform_constant_seen[6];
    int array_drawn_this_frame;
    int array_drawn_since_present;
    int clear_after_array_since_present;
    uint32_t array_draws_since_present;
    uint32_t last_array_submission;
    uint32_t last_clear_submission;

    /* Clear state */
    uint32_t clear_color;
    uint32_t clear_rect_h;  /* (width << 16) | x */
    uint32_t clear_rect_v;  /* (height << 16) | y */

    /* Render state cache */
    int depth_test;
    uint32_t depth_func;
    uint32_t depth_mask;
    int stencil_test;
    uint32_t stencil_func, stencil_ref, stencil_read_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_zfail, stencil_pass;
    uint32_t clear_zstencil;
    uint32_t zmin_max_control;
    uint32_t control0;
    int blend_enable;
    uint32_t blend_sfactor;
    uint32_t blend_dfactor;
    int cull_enable;
    uint32_t cull_face;
    uint32_t front_face;
    int alpha_test;
    uint32_t alpha_func;
    uint32_t alpha_ref;
    uint32_t color_mask;

    /* Viewport */
    float vp_offset[4];
    float vp_scale[4];
    uint32_t surface_clip_h;
    uint32_t surface_clip_v;
    uint32_t surface_format;
    uint32_t surface_pitch;
    uint32_t surface_color_offset;
    uint32_t surface_zeta_offset;

    /* Texture state per stage (4 stages) */
    struct {
        uint32_t offset;     /* NV2A VRAM offset (method 0x1B00) */
        uint32_t format;     /* Format register (method 0x1B04) */
        uint32_t control0;   /* Control0 register (method 0x1B0C) */
        uint32_t address;
        uint32_t filter;
        uint32_t border_color;
        uint32_t image_rect; /* Image rect (method 0x1B1C): linear sizes */
        int enabled;         /* Decoded from control0 bit 30 */
    } tex[4];
    uint32_t bump_env[4][6]; /* MAT00,01,10,11,SCALE,OFFSET */

    /* Raw live register-combiner state.  Keep this separate from the D3D8
     * fixed-function cache: these values arrive through NV097 packets and
     * are needed to reconstruct the Frontend material without importing a
     * captured framebuffer or static native memory. */
    uint32_t combiner_alpha_icw[8];
    uint32_t combiner_alpha_ocw[8];
    uint32_t combiner_color_icw[8];
    uint32_t combiner_color_ocw[8];
    uint32_t combiner_factor0[8];
    uint32_t combiner_factor1[8];
    uint32_t combiner_control;
    uint32_t combiner_final0;
    uint32_t combiner_final1;
    uint32_t shader_stage_program;
    uint32_t dot_rgb_mapping;
    uint32_t shader_other_stage_input;

    /* Raw fixed-function lighting state emitted by the live NV097 command
     * stream.  The native Frontend array contains position + normal only,
     * so its color must come from these methods rather than vertex diffuse.
     * Retain the values from this run; do not source them from a capture. */
    uint32_t light_control;
    uint32_t color_material;
    uint32_t lighting_enable;
    uint32_t material_emission[3];
    uint32_t material_alpha;
    uint32_t specular_enable;
    uint32_t light_enable_mask;
    uint32_t specular_params[7];
    uint32_t scene_ambient[3];
    uint32_t light_state[8][0x20];
    uint32_t back_light_state[8][0x10];
    uint32_t lighting_methods_seen;

    /* Cached texture pointers */
    void *menu_texture;           /* IDirect3DTexture8* from Global.txd */
    FrontendRuntimeTarget *frontend_runtime_target;
    FrontendSurfaceRegistration frontend_surface_registration[
        FRONTEND_SURFACE_REGISTRATION_COUNT];
    uint32_t frontend_surface_registration_serial;
    FrontendPendingSurfaceCopy frontend_pending_surface_copy[
        FRONTEND_PENDING_SURFACE_COPY_COUNT];
    uint32_t frontend_surface_copy_serial;
    IDirect3DTexture8 *frontend_live_texture;
    uint32_t frontend_live_offset;

    /* Stats */
    PgraphD3D11Stats stats;

    /* Chyron scroll */
    float chyron_scroll_offset;  /* Pixels to shift X for chyron text */

    /* Init flag */
    int initialized;
} g_pg;

/* ══════════════════════════════════════════════════════════════════════
 * Float/uint32 conversion
 * ══════════════════════════════════════════════════════════════════════ */
static float u2f(uint32_t u) {
    union { float f; uint32_t i; } x;
    x.i = u;
    return x.f;
}

static void frontend_surface_register(
    uint32_t guest_offset, uint32_t width, uint32_t height,
    uint32_t persistent)
{
    static unsigned registration_log_count;
    FrontendSurfaceRegistration *registration = NULL;
    uint32_t oldest_serial = UINT32_MAX;
    unsigned i;

    if (guest_offset < 0x00010000u || guest_offset >= 0x04000000u ||
        width == 0u || height == 0u || width > 4096u || height > 4096u)
        return;

    for (i = 0; i < FRONTEND_SURFACE_REGISTRATION_COUNT; ++i) {
        FrontendSurfaceRegistration *candidate =
            &g_pg.frontend_surface_registration[i];
        if (candidate->guest_offset == guest_offset) {
            registration = candidate;
            break;
        }
        if (candidate->guest_offset == 0u) {
            registration = candidate;
            break;
        }
        if (!candidate->persistent && candidate->serial < oldest_serial) {
            oldest_serial = candidate->serial;
            registration = candidate;
        }
    }
    if (registration == NULL)
        return;

    registration->guest_offset = guest_offset;
    registration->width = (UINT)width;
    registration->height = (UINT)height;
    registration->serial = ++g_pg.frontend_surface_registration_serial;
    registration->persistent |= persistent;
    if (persistent) {
        fprintf(stderr,
                "[INFO PGRAPH-SURFACE-PIN] live Frontend compositor="
                "%08X size=%ux%u\n",
                guest_offset, width, height);
    }
    if (registration_log_count++ < 16u) {
        fprintf(stderr,
                "[INFO PGRAPH-SURFACE-REGISTER] live Frontend target=%08X "
                "size=%ux%u\n",
                guest_offset, width, height);
    }
}

void pgraph_d3d11_register_frontend_surface(
    uint32_t guest_offset, uint32_t width, uint32_t height)
{
    frontend_surface_register(guest_offset, width, height, 0u);
}

void pgraph_d3d11_register_frontend_compositor_surface(
    uint32_t guest_offset, uint32_t width, uint32_t height)
{
    frontend_surface_register(guest_offset, width, height, 1u);
}

static FrontendRuntimeTarget *frontend_runtime_target_find(
    uint32_t guest_offset)
{
    for (FrontendRuntimeTarget *candidate = g_pg.frontend_runtime_target;
         candidate != NULL; candidate = candidate->next) {
        if (candidate->texture != NULL &&
            candidate->guest_offset == guest_offset)
            return candidate;
    }
    return NULL;
}

static int frontend_runtime_target_dimensions(
    uint32_t guest_offset, UINT *width, UINT *height)
{
    static unsigned dimension_miss_logged;
    static const UINT compositor_width[4] = { 256u, 256u, 128u, 256u };
    static const UINT compositor_height[4] = { 256u, 128u, 128u, 256u };
    unsigned surface;
    unsigned registration_index;

    *width = d3d8_GetBackbufferWidth();
    *height = d3d8_GetBackbufferHeight();

    for (registration_index = 0;
         registration_index < FRONTEND_SURFACE_REGISTRATION_COUNT;
         ++registration_index) {
        const FrontendSurfaceRegistration *registration =
            &g_pg.frontend_surface_registration[registration_index];
        if (registration->guest_offset == guest_offset) {
            *width = registration->width;
            *height = registration->height;
            return 1;
        }
    }

    /* 87A1AC is the live descriptor table published by this run's retail
     * Frontend compositor setup.  Resolve its current backing addresses;
     * no captured framebuffer or static process memory is involved. */
    /* The locally translated constructor publishes these descriptors.
     * Validate the live pointers instead of a flag set by old hand stubs. */
    for (surface = 0; surface < 4u; ++surface) {
        const uint32_t *descriptor_slot =
            (const uint32_t *)(uintptr_t)(
                (uintptr_t)(0x0087A1ACu + surface * 4u) +
                (uintptr_t)g_xbox_mem_offset);
        uint32_t descriptor = *descriptor_slot;
        const uint32_t *backing_slot;

        if (descriptor < 0x00010000u || descriptor > 0x04000000u - 8u)
            continue;
        backing_slot = (const uint32_t *)(uintptr_t)(
            (uintptr_t)(descriptor + 4u) + (uintptr_t)g_xbox_mem_offset);
        if (*backing_slot == guest_offset) {
            *width = compositor_width[surface];
            *height = compositor_height[surface];
            return 1;
        }
    }
    if (dimension_miss_logged++ < 8u) {
        const uint32_t *slots = (const uint32_t *)(uintptr_t)(
            (uintptr_t)0x0087A1ACu + (uintptr_t)g_xbox_mem_offset);
        fprintf(stderr,
                "[WARN PGRAPH-SURFACE-DIMENSIONS] miss=%08X "
                "descriptors=%08X,%08X,%08X,%08X\n",
                guest_offset,
                slots[0], slots[1], slots[2], slots[3]);
    }
    return 0;
}

static FrontendRuntimeTarget *frontend_runtime_target_acquire(
    IDirect3DDevice8 *dev, uint32_t guest_offset, UINT width, UINT height)
{
    FrontendRuntimeTarget *target = frontend_runtime_target_find(guest_offset);
    IDirect3DTexture8 *texture = NULL;
    if (target != NULL && target->width == width && target->height == height) {
        target->last_used_submission = g_pg.stats.frames + 1u;
        return target;
    }
    /* These surfaces contain guest-visible results, not disposable copies of
     * CPU memory. Evicting an LRU entry loses its only current pixels. Keep
     * stable entries until that guest address is reused or the GPU shuts down. */
    if (FAILED(dev->lpVtbl->CreateTexture(
            dev, width, height, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture)) || texture == NULL)
        return NULL;
    if (target == NULL) {
        target = (FrontendRuntimeTarget *)calloc(1, sizeof(*target));
        if (target == NULL) {
            texture->lpVtbl->Release(texture);
            return NULL;
        }
        target->next = g_pg.frontend_runtime_target;
        g_pg.frontend_runtime_target = target;
    }
    if (target->texture != NULL) {
        if (g_pg.frontend_live_texture == target->texture) {
            g_pg.frontend_live_texture = NULL;
            g_pg.frontend_live_offset = 0;
        }
        target->texture->lpVtbl->Release(target->texture);
    }
    target->texture = texture;
    target->last_clear_submission = 0;
    target->guest_offset = guest_offset;
    target->width = width;
    target->height = height;
    target->last_used_submission = g_pg.stats.frames + 1u;
    fprintf(stderr,
            "[INFO PGRAPH-RUNTIME-TARGET] created live target=%08X "
            "size=%ux%u\n",
            guest_offset, (unsigned)width, (unsigned)height);
    return target;
}

static void frontend_runtime_targets_release(void)
{
    FrontendRuntimeTarget *target = g_pg.frontend_runtime_target;
    while (target != NULL) {
        FrontendRuntimeTarget *next = target->next;
        if (target->texture != NULL)
            target->texture->lpVtbl->Release(target->texture);
        free(target);
        target = next;
    }
    g_pg.frontend_runtime_target = NULL;
    g_pg.frontend_live_texture = NULL;
    g_pg.frontend_live_offset = 0;
}

void pgraph_d3d11_queue_frontend_surface_copy(uint32_t destination,
                                              uint32_t source)
{
    FrontendPendingSurfaceCopy *entry = NULL;
    uint32_t oldest_serial = UINT32_MAX;
    unsigned i;

    if (destination == 0u || source == 0u || destination == source)
        return;

    for (i = 0; i < FRONTEND_PENDING_SURFACE_COPY_COUNT; ++i) {
        FrontendPendingSurfaceCopy *candidate =
            &g_pg.frontend_pending_surface_copy[i];
        if (candidate->destination == destination) {
            entry = candidate;
            break;
        }
        if (candidate->destination == 0u) {
            entry = candidate;
            break;
        }
        if (candidate->serial < oldest_serial) {
            oldest_serial = candidate->serial;
            entry = candidate;
        }
    }

    if (entry == NULL)
        return;
    entry->destination = destination;
    entry->source = source;
    entry->serial = ++g_pg.frontend_surface_copy_serial;
}

static FrontendPendingSurfaceCopy *frontend_pending_surface_copy_find(
    uint32_t destination)
{
    FrontendPendingSurfaceCopy *result = NULL;
    unsigned i;

    for (i = 0; i < FRONTEND_PENDING_SURFACE_COPY_COUNT; ++i) {
        FrontendPendingSurfaceCopy *candidate =
            &g_pg.frontend_pending_surface_copy[i];
        if (candidate->destination == destination &&
            (result == NULL || candidate->serial > result->serial))
            result = candidate;
    }
    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * Initialization
 * ══════════════════════════════════════════════════════════════════════ */

void pgraph_d3d11_init(void)
{
    frontend_runtime_targets_release();
    memset(&g_pg, 0, sizeof(g_pg));
    for (unsigned a = 0; a < 16; ++a) g_pg.immediate_value[a][3] = 1.0f;
    g_pg.vert_stride = INLINE_VERT_DWORDS;  /* Default: 5 dwords per vertex */
    g_pg.clear_color = 0xFF000000;
    g_pg.depth_func = 0x0203u; /* LEQUAL */
    g_pg.depth_mask = 1u;
    g_pg.stencil_func = 0x0207u;
    g_pg.stencil_read_mask = g_pg.stencil_write_mask = 0xFFu;
    g_pg.stencil_fail = g_pg.stencil_zfail = g_pg.stencil_pass = NV097_SET_STENCIL_OP_V_KEEP;
    g_pg.alpha_func = 0x0207u; /* ALWAYS */
    g_pg.cull_face = NV097_SET_CULL_FACE_V_BACK;
    g_pg.front_face = NV097_SET_FRONT_FACE_V_CCW;
    for (unsigned stage = 0; stage < 4; ++stage) {
        g_pg.tex[stage].address = 0x00010101u; /* WRAP */
        g_pg.tex[stage].filter = 0x01010000u;  /* point min/mag, level zero */
    }
    g_pg.clear_zstencil = 0xFFFFFF00u;
    g_pg.color_mask = 0x01010101;
    g_pg.initialized = 1;

    fprintf(stderr, "[PGRAPH-D3D11] Translator initialized\n");
}

static uint32_t *g_gpu_inputs;
static size_t g_gpu_input_capacity;
static int g_gpu_draw_failed;

void pgraph_d3d11_shutdown(void)
{
    free(g_gpu_inputs);
    g_gpu_inputs = NULL;
    g_gpu_input_capacity = 0;
    g_gpu_draw_failed = 0;
    frontend_runtime_targets_release();
    g_pg.initialized = 0;
    fprintf(stderr, "[PGRAPH-D3D11] Translator shut down (draws=%u, verts=%u)\n",
            g_pg.stats.draw_calls, g_pg.stats.vertices_submitted);
}

/* ══════════════════════════════════════════════════════════════════════
 * Draw Submission
 * ══════════════════════════════════════════════════════════════════════ */

#define NV2A_VERTEX_ATTR_COUNT 16u

/* Bytes one attribute occupies inside an inline vertex. */
static uint32_t inline_attr_bytes(uint32_t format)
{
    uint32_t type = format & NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE;
    uint32_t size = (format & NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE) >> 4;

    if (size == 0u)
        return 0u;                  /* attribute disabled */
    switch (type) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        return size;                            /* one byte per component */
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        return size * 2u;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        return size * 4u;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        return 4u;                  /* three components packed in one dword */
    default:
        return 0u;
    }
}

/*
 * Work out an inline vertex's layout from the vertex-attribute format
 * registers the title has already set.
 *
 * INLINE_ARRAY pushes the enabled attributes packed back to back in attribute
 * order, so the format registers describe the layout exactly.  This used to be
 * guessed from the primitive mode instead -- 4 dwords for modes 2/3/8, 5 for
 * mode 9, 5 otherwise.  The guess was wrong for the title's own UI: Conker's
 * menu quads are 7 dwords, so every vertex's X came out of the previous
 * vertex's colour.  0xFF66CCFF read as a float is -3.07e38, which is exactly
 * what the geometry showed, and 255,000 draws a frame left 708 non-black
 * pixels on a 640x480 screen.
 *
 * Semantics are inferred structurally rather than by attribute index, because
 * the index only carries meaning under the fixed-function pipeline.  Conker
 * draws its menus with a vertex shader and packs the same three attributes
 * into slots 0, 1, 2, where the fixed-function convention would use 0
 * (position), 3 (diffuse) and 9 (texcoord0).  Reading by index found the
 * position and nothing else, so every quad came out flat white with no
 * texture coordinates.
 *
 * The rules below give the same answer for both conventions:
 *   position  the first enabled attribute -- slot 0 either way
 *   diffuse   the first packed-byte quad, which is what a D3DCOLOR is
 *   texcoord  the first float pair that is not the position
 *
 * Returns the stride in dwords, or 0 when no attributes are enabled -- in
 * which case the caller keeps the old guess rather than dropping the draw.
 */
static uint32_t inline_layout_from_formats(void)
{
    uint32_t offset = 0u;
    unsigned i;

    g_pg.gather_prepared = 0;
    g_pg.inline_pos_offset = -1;
    g_pg.inline_diffuse_offset = -1;
    g_pg.inline_texcoord_offset = -1;
    g_pg.inline_pos_size = 0u;
    g_pg.inline_texcoord_size = 0u;
    for (i = 0u; i < 16u; ++i) g_pg.inline_attr_off[i] = -1;

    for (i = 0u; i < NV2A_VERTEX_ATTR_COUNT; ++i) {
        uint32_t format = g_pg.array_format[i];
        uint32_t type = format & NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE;
        uint32_t size = (format & NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE) >> 4;
        uint32_t bytes = inline_attr_bytes(format);
        int is_float = (type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F);
        int is_packed_bytes =
            (type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D ||
             type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL);

        if (bytes == 0u)
            continue;

        if (i < 16u) {
            g_pg.inline_attr_off[i] = (int)(offset / 4u);
            g_pg.inline_attr_size[i] = size;
            g_pg.inline_attr_type[i] = type;
        }

        if (g_pg.inline_pos_offset < 0) {
            g_pg.inline_pos_offset = (int)(offset / 4u);
            g_pg.inline_pos_size = size;
        } else if (is_packed_bytes && size == 4u &&
                   g_pg.inline_diffuse_offset < 0) {
            g_pg.inline_diffuse_offset = (int)(offset / 4u);
        } else if (is_float && size == 2u &&
                   g_pg.inline_texcoord_offset < 0) {
            g_pg.inline_texcoord_offset = (int)(offset / 4u);
            g_pg.inline_texcoord_size = size;
        }

        /* Each attribute starts on a dword boundary in the inline stream. */
        offset += (bytes + 3u) & ~3u;
    }

    /* Each distinct layout, once.  This is the thing most likely to regress
     * silently -- a wrong stride does not fail, it draws nonsense -- so leave
     * a record of what was actually derived. */
    { static uint32_t seen[8]; static unsigned seen_count; unsigned k;
      uint32_t key = (offset / 4u) | ((uint32_t)(g_pg.inline_pos_size) << 8) |
                     ((uint32_t)(g_pg.inline_diffuse_offset & 0xFF) << 12) |
                     ((uint32_t)(g_pg.inline_texcoord_offset & 0xFF) << 20);
      for (k = 0; k < seen_count; ++k)
          if (seen[k] == key)
              break;
      if (k == seen_count && seen_count < 8u) {
          seen[seen_count++] = key;
          fprintf(stderr, "[INFO PGRAPH-VERTEX] inline layout: stride=%u dwords"
                  " position=+%d x%u diffuse=+%d texcoord=+%d\n",
                  offset / 4u, g_pg.inline_pos_offset, g_pg.inline_pos_size,
                  g_pg.inline_diffuse_offset, g_pg.inline_texcoord_offset);
      } }

    return offset / 4u;
}

/* ══════════════════════════════════════════════════════════════════════
 * Texture upload
 *
 * The title's textures live in guest memory and are described entirely by the
 * texture registers: SET_TEXTURE_OFFSET gives the address, SET_TEXTURE_FORMAT
 * the colour code and log2 dimensions.  Nothing here is title-specific.
 *
 * Before this existed the draw path bound no texture at all unless
 * GAME_HAS_FONT_ATLAS was compiled in, and that selects textures from a
 * hardcoded table of Burnout 3 VRAM addresses.  For Conker every textured quad
 * therefore sampled nothing and rendered as flat vertex colour: the first frame
 * off the swap chain was a 256x256 white square, which was a compositor layer
 * drawn untextured.
 * ══════════════════════════════════════════════════════════════════════ */

#define PGRAPH_TEXTURE_CACHE_SIZE 512u

typedef struct {
    uint32_t offset;
    uint32_t format;
    UINT width;
    UINT height;
    IDirect3DTexture8 *texture;
    /* Coherence with guest memory.  The cache was keyed on the four
     * fields above and never invalidated, so a surface the guest
     * rewrites in place -- an XMV video frame, which keeps the same
     * offset, format and size for the whole clip -- was uploaded once
     * and served stale for the rest of the run.  The hash is sampled,
     * and recomputed at most once per frame per texture, so this costs
     * a few thousand byte reads rather than a full compare per draw. */
    uint32_t source_hash;
    unsigned checked_frame;
    uint64_t last_used;
} PgraphTextureCacheEntry;

/* The target currently bound for rendering.  D3D11 will not let one resource
 * be a render target and a shader input at the same time -- binding it as a
 * texture silently unbinds the RTV, and the draw goes nowhere. */
static IDirect3DTexture8 *g_scene_bound_texture;
/* Actual draw target includes small offscreen effects; the scene pointer
 * above is cleared for those targets by presentation tracking. */
/* Actual draw target, including small offscreen surfaces. */
static FrontendRuntimeTarget *g_scene_bound_target;
static IDirect3DTexture8 *g_draw_feedback_texture;

/* Sampled FNV-1a over the guest source.  Every 64th byte is enough to see
 * a new video frame and cheap enough to run once per texture per frame. */
static uint32_t pgraph_source_hash(uint32_t offset, uint64_t bytes)
{
    const uint8_t *p;
    uint32_t h = 2166136261u;
    uint64_t i;
    if (offset < 0x00001000u || offset + bytes > 0x04000000u) return 0u;
    p = (const uint8_t *)(uintptr_t)((uintptr_t)offset +
                                     (uintptr_t)g_xbox_mem_offset);
    for (i = 0; i < bytes; i += 64u) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Render-phase marker for the frame-8 stall watchdog.
 *
 * One run in two stops rendering while guest safe points keep advancing, so
 * the freeze is on the host side.  These say WHICH host phase owns it; the
 * watchdog samples them alongside the native stacks. */
unsigned g_rp_phase;
unsigned g_rp_draw_seq;
uint32_t g_rp_last_method;
uint32_t g_rp_rt, g_rp_tex;
unsigned long long g_rp_presents;

static PgraphTextureCacheEntry g_texture_cache[PGRAPH_TEXTURE_CACHE_SIZE];
static unsigned g_texture_cache_count;
static uint64_t g_texture_cache_clock;
static unsigned long long g_texture_reuploads;

/*
 * Guest addresses the title has rendered into.
 *
 * It composites by binding a surface it just drew to as a texture.  Those
 * surfaces exist as D3D11 render targets here, not as pixels in guest memory,
 * so uploading their guest bytes samples whatever happens to be there --
 * nothing -- and modulating by it turns the whole frame black.  Recording the
 * addresses lets the draw path fall back to vertex colour for them, which is
 * wrong but visible, instead of right-looking and blank.
 *
 * Sampling them properly needs the scene surfaces to become render targets in
 * their own right; until then this is the honest boundary.
 */
#define PGRAPH_SURFACE_SET_SIZE 32u
static uint32_t g_surface_offsets[PGRAPH_SURFACE_SET_SIZE];
static unsigned g_surface_offset_count;

static void pgraph_note_surface_offset(uint32_t offset)
{
    unsigned i;

    if (offset == 0u)
        return;
    for (i = 0; i < g_surface_offset_count; ++i)
        if (g_surface_offsets[i] == offset)
            return;
    if (g_surface_offset_count < PGRAPH_SURFACE_SET_SIZE)
        g_surface_offsets[g_surface_offset_count++] = offset;
}

static int pgraph_offset_is_surface(uint32_t offset)
{
    unsigned i;

    for (i = 0; i < g_surface_offset_count; ++i)
        if (g_surface_offsets[i] == offset)
            return 1;
    return 0;
}

/*
 * Decode a texture colour code into something our D3D8 layer can create.
 *
 * The Xbox D3DFORMAT values and the NV2A colour codes share a numbering, so
 * most of this is identity -- but only for the codes we can actually upload,
 * and listing them explicitly is what keeps an unsupported format a logged
 * miss rather than a garbled texture.
 */
/* How a texture's bytes are laid out in guest memory. */
typedef enum {
    NV2A_SRC_UNSUPPORTED = 0,
    NV2A_SRC_A8R8G8B8,
    NV2A_SRC_X8R8G8B8,
    NV2A_SRC_R5G6B5,
    NV2A_SRC_A1R5G5B5,
    NV2A_SRC_X1R5G5B5,
    NV2A_SRC_A4R4G4B4,
    NV2A_SRC_Y8,
    NV2A_SRC_A8,
    NV2A_SRC_A8Y8,
    NV2A_SRC_G8B8,          /* low/high bytes expand to low,high,low,high RGBA */
    NV2A_SRC_R8B8,          /* low/high bytes expand to high,low,low,high RGBA */
    NV2A_SRC_YUY2,          /* Y0 Cb Y1 Cr, two pixels per four bytes */
    NV2A_SRC_UYVY,          /* Cb Y0 Cr Y1 */
    NV2A_SRC_DXT1,
    NV2A_SRC_DXT3,
    NV2A_SRC_DXT5
} Nv2aSourceFormat;

/*
 * Decode a texture colour code.
 *
 * Everything uncompressed is converted to A8R8G8B8 on upload and the texture
 * is created as LIN_A8R8G8B8.  That is not laziness about formats -- it is
 * what keeps the layering honest.  The D3D8 layer unswizzles in
 * tex_UnlockRect() based on the format the texture was *created* with, so
 * handing it already-linear bytes under a swizzled format makes it unswizzle
 * them a second time.  With a 256x240 texture that is also a crash, since
 * xbox_unswizzle_rect() assumes power-of-two dimensions.
 *
 * Compressed formats pass straight through: DXT is never swizzled, the layer
 * knows not to touch it, and re-encoding would only lose quality.
 */
static Nv2aSourceFormat nv2a_texture_source_format(uint32_t color)
{
    switch (color) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
        return NV2A_SRC_A8R8G8B8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        return NV2A_SRC_X8R8G8B8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
        return NV2A_SRC_R5G6B5;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
        return NV2A_SRC_A1R5G5B5;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
        return NV2A_SRC_X1R5G5B5;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4:
        return NV2A_SRC_A4R4G4B4;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
        return NV2A_SRC_Y8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
        return NV2A_SRC_A8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
        return NV2A_SRC_A8Y8;
    /* Two-channel formats.  Leaving these out is what made the scene render
     * as flat white polygons: the title's main 128x128 scene texture is
     * SZ_G8B8, the upload refused it, and 14344 draws a run fell through to
     * the untextured path, which selects diffuse -- and diffuse is white. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
        return NV2A_SRC_G8B8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
        return NV2A_SRC_R8B8;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
        return NV2A_SRC_YUY2;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8:
        return NV2A_SRC_UYVY;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return NV2A_SRC_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return NV2A_SRC_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return NV2A_SRC_DXT5;
    default:
        return NV2A_SRC_UNSUPPORTED;
    }
}

/* Bytes one source pixel occupies.  Zero for compressed and for the 4:2:2
 * formats, which are counted in whole two-pixel groups instead. */
static uint32_t nv2a_source_bpp(Nv2aSourceFormat sf)
{
    switch (sf) {
    case NV2A_SRC_A8R8G8B8:
    case NV2A_SRC_X8R8G8B8: return 4u;
    case NV2A_SRC_R5G6B5:
    case NV2A_SRC_A1R5G5B5:
    case NV2A_SRC_X1R5G5B5:
    case NV2A_SRC_A4R4G4B4: return 2u;
    case NV2A_SRC_A8Y8:
    case NV2A_SRC_G8B8:
    case NV2A_SRC_R8B8:     return 2u;
    case NV2A_SRC_Y8:
    case NV2A_SRC_A8:       return 1u;
    default:                return 0u;
    }
}

static int nv2a_source_is_compressed(Nv2aSourceFormat sf)
{
    return sf == NV2A_SRC_DXT1 || sf == NV2A_SRC_DXT3 || sf == NV2A_SRC_DXT5;
}

static int nv2a_source_is_yuv(Nv2aSourceFormat sf)
{
    return sf == NV2A_SRC_YUY2 || sf == NV2A_SRC_UYVY;
}

/* One row of source pixels to BGRA.  BT.601 full range for the YUV pair,
 * which is what the console's video path uses. */
static void nv2a_decode_row(uint32_t *dst, const uint8_t *src, uint32_t width,
                            Nv2aSourceFormat sf)
{
    uint32_t x;

    if (nv2a_source_is_yuv(sf)) {
        uint32_t groups = (width + 1u) / 2u;
        uint32_t g;
        for (g = 0; g < groups; ++g) {
            const uint8_t *p = src + (size_t)g * 4u;
            int y0, y1, cb, cr, i;

            if (sf == NV2A_SRC_YUY2) {
                y0 = p[0]; cb = p[1]; y1 = p[2]; cr = p[3];
            } else {
                cb = p[0]; y0 = p[1]; cr = p[2]; y1 = p[3];
            }
            cb -= 128; cr -= 128;
            for (i = 0; i < 2; ++i) {
                int y = i ? y1 : y0;
                int r = y + ((91881 * cr) >> 16);
                int gg = y - ((22554 * cb + 46802 * cr) >> 16);
                int b = y + ((116130 * cb) >> 16);
                uint32_t px = g * 2u + (uint32_t)i;

                if (px >= width)
                    break;
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;
                dst[px] = 0xFF000000u | ((uint32_t)r << 16) |
                          ((uint32_t)gg << 8) | (uint32_t)b;
            }
        }
        return;
    }

    for (x = 0; x < width; ++x) {
        uint32_t out;
        switch (sf) {
        case NV2A_SRC_A8R8G8B8:
            out = ((const uint32_t *)src)[x];
            break;
        case NV2A_SRC_X8R8G8B8:
            out = ((const uint32_t *)src)[x] | 0xFF000000u;
            break;
        case NV2A_SRC_R5G6B5: {
            uint32_t v = ((const uint16_t *)src)[x];
            uint32_t r = (v >> 11) & 0x1Fu, g = (v >> 5) & 0x3Fu, b = v & 0x1Fu;
            out = 0xFF000000u |
                  (((r << 3) | (r >> 2)) << 16) |
                  (((g << 2) | (g >> 4)) << 8) |
                  ((b << 3) | (b >> 2));
            break;
        }
        case NV2A_SRC_A1R5G5B5:
        case NV2A_SRC_X1R5G5B5: {
            uint32_t v = ((const uint16_t *)src)[x];
            uint32_t a = (sf == NV2A_SRC_X1R5G5B5 || (v & 0x8000u)) ? 0xFFu : 0u;
            uint32_t r = (v >> 10) & 0x1Fu, g = (v >> 5) & 0x1Fu, b = v & 0x1Fu;
            out = (a << 24) |
                  (((r << 3) | (r >> 2)) << 16) |
                  (((g << 3) | (g >> 2)) << 8) |
                  ((b << 3) | (b >> 2));
            break;
        }
        case NV2A_SRC_A4R4G4B4: {
            uint32_t v = ((const uint16_t *)src)[x];
            uint32_t a = (v >> 12) & 0xFu, r = (v >> 8) & 0xFu;
            uint32_t g = (v >> 4) & 0xFu, b = v & 0xFu;
            out = ((a | (a << 4)) << 24) | ((r | (r << 4)) << 16) |
                  ((g | (g << 4)) << 8) | (b | (b << 4));
            break;
        }
        case NV2A_SRC_Y8: {
            uint32_t v = src[x];
            out = 0xFF000000u | (v << 16) | (v << 8) | v;
            break;
        }
        case NV2A_SRC_A8:
            out = ((uint32_t)src[x] << 24) | 0x00FFFFFFu;
            break;
        case NV2A_SRC_A8Y8: {
            uint32_t y = src[(size_t)x * 2u];
            uint32_t a = src[(size_t)x * 2u + 1u];
            out = (a << 24) | (y << 16) | (y << 8) | y;
            break;
        }
        case NV2A_SRC_G8B8: {
            /* NV2A duplicates the two components into RGBA. They are not
             * zero-filled missing channels with opaque alpha. See xemu's
             * pgraph/gl/constants.h kelvin_color_format_gl_map. */
            uint32_t b = src[(size_t)x * 2u];
            uint32_t g = src[(size_t)x * 2u + 1u];
            out = (g << 24) | (b << 16) | (g << 8) | b;
            break;
        }
        case NV2A_SRC_R8B8: {
            uint32_t b = src[(size_t)x * 2u];
            uint32_t r = src[(size_t)x * 2u + 1u];
            out = (r << 24) | (r << 16) | (b << 8) | b;
            break;
        }
        default:
            out = 0xFF000000u;
            break;
        }
        dst[x] = out;
    }
}

/* Read a texture out of guest memory and hand it to the D3D8 layer. */
/* TEMPORARY: texture-source content tracking, see pgraph_texsrc_note. */
#define TEXSRC_MAX 8u
struct texsrc_rec {
    uint32_t offset, color, width, height;
    uint64_t bytes;
    unsigned nonzero_at_upload, sampled;
    uint32_t hash_at_upload;
};
static struct texsrc_rec g_texsrc[TEXSRC_MAX];
static unsigned g_texsrc_n;

static void texsrc_scan(const uint8_t *p, uint64_t bytes, unsigned *nonzero,
                        unsigned *sampled, uint32_t *hash)
{
    uint64_t i;
    uint32_t h = 2166136261u;
    unsigned nz = 0, ns = 0;
    /* Sample every 64th byte: enough to tell black from picture, cheap
       enough to run on the upload path. */
    for (i = 0; i < bytes; i += 64u) {
        uint8_t v = p[i];
        ++ns;
        if (v) ++nz;
        h = (h ^ v) * 16777619u;
    }
    *nonzero = nz; *sampled = ns; *hash = h;
}

void pgraph_texsrc_note(uint32_t offset, uint32_t color, uint32_t width,
                        uint32_t height, const uint8_t *guest, uint64_t bytes)
{
    struct texsrc_rec *r;
    if (g_texsrc_n >= TEXSRC_MAX) return;
    r = &g_texsrc[g_texsrc_n++];
    r->offset = offset; r->color = color;
    r->width = width; r->height = height; r->bytes = bytes;
    texsrc_scan(guest, bytes, &r->nonzero_at_upload, &r->sampled,
                &r->hash_at_upload);
}

/* Re-read each uploaded texture's guest source now and compare. */
void pgraph_d3d11_dump_texsrc(void)
{
    unsigned i;
    if (g_texsrc_n == 0u) return;
    fprintf(stderr, "[TEXSRC] cache re-uploads after a source change: %llu\n",
            g_texture_reuploads);
    fprintf(stderr, "[TEXSRC] guest source content at upload time vs now "
            "(texture cache has no invalidation)" "\n");
    for (i = 0; i < g_texsrc_n; ++i) {
        struct texsrc_rec *r = &g_texsrc[i];
        const uint8_t *guest = (const uint8_t *)(uintptr_t)
            ((uintptr_t)r->offset + (uintptr_t)g_xbox_mem_offset);
        unsigned nz, ns; uint32_t h;
        texsrc_scan(guest, r->bytes, &nz, &ns, &h);
        fprintf(stderr, "[TEXSRC] %08X %ux%u colour=0x%02X | at upload "
                "%u/%u non-zero hash %08X | now %u/%u non-zero hash %08X | %s" "\n",
                r->offset, r->width, r->height, r->color,
                r->nonzero_at_upload, r->sampled, r->hash_at_upload,
                nz, ns, h,
                (h == r->hash_at_upload) ? "unchanged"
                                         : "SOURCE CHANGED SINCE UPLOAD");
    }
    fflush(stderr);
}

/* Linear images have one level. Swizzled / BC chains store each successively
 * smaller level immediately after the previous one, with BC block rounding. */
static UINT pgraph_texture_levels(uint32_t format, UINT width, UINT height)
{
    uint32_t color = (format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    Nv2aSourceFormat sf = nv2a_texture_source_format(color);
    if (!nv2a_source_is_compressed(sf) && !d3d8_format_is_swizzled(color))
        return 1u;
    UINT levels = (format >> 16) & 15u, maximum = 1u;
    for (UINT extent = width > height ? width : height; extent > 1u; extent >>= 1)
        ++maximum;
    if (!levels) levels = 1u;
    return levels < maximum ? levels : maximum;
}

static uint64_t pgraph_level_bytes(Nv2aSourceFormat sf, UINT width, UINT height)
{
    if (sf == NV2A_SRC_UNSUPPORTED) return 0u;
    if (nv2a_source_is_compressed(sf))
        return (uint64_t)((width + 3u) / 4u) * ((height + 3u) / 4u) *
               (sf == NV2A_SRC_DXT1 ? 8u : 16u);
    if (nv2a_source_is_yuv(sf))
        return (uint64_t)(((width + 1u) / 2u) * 4u) * height;
    return (uint64_t)width * nv2a_source_bpp(sf) * height;
}

/* Hash every supplied mip and face. NV2A stores a complete mip chain per
 * cube face, padding each chain to 128 bytes, in +X,-X,+Y,-Y,+Z,-Z order. */
static uint64_t pgraph_source_bytes(uint32_t format, UINT width, UINT height)
{
    Nv2aSourceFormat sf = nv2a_texture_source_format((format >> 8) & 255u);
    UINT levels = pgraph_texture_levels(format, width, height);
    uint64_t bytes = 0;
    for (UINT level = 0; level < levels; ++level) {
        bytes += pgraph_level_bytes(sf, width, height);
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
    }
    if (format & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE)
        bytes = ((bytes + NV2A_CUBEMAP_FACE_ALIGNMENT - 1u) &
                 ~(uint64_t)(NV2A_CUBEMAP_FACE_ALIGNMENT - 1u)) * 6u;
    return bytes;
}

static IDirect3DTexture8 *pgraph_texture_upload(IDirect3DDevice8 *dev,
                                                uint32_t offset,
                                                uint32_t format,
                                                UINT width, UINT height)
{
    uint32_t color = (format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    Nv2aSourceFormat sf = nv2a_texture_source_format(color);
    D3DFORMAT d3d_format;
    IDirect3DTexture8 *texture = NULL;
    D3DLOCKED_RECT locked;
    const uint8_t *guest;
    uint32_t source_pitch;
    uint64_t source_bytes;
    UINT y;
    UINT levels = pgraph_texture_levels(format, width, height);
    UINT faces = (format & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE) ? 6u : 1u;
    UINT base_width = width, base_height = height;

    if (sf == NV2A_SRC_UNSUPPORTED) {
        static uint32_t reported[16];
        static unsigned reported_count;
        unsigned i;
        for (i = 0; i < reported_count; ++i)
            if (reported[i] == color)
                return NULL;
        if (reported_count < 16u) {
            reported[reported_count++] = color;
            fprintf(stderr, "[WARN PGRAPH-TEXTURE] unsupported colour format "
                    "0x%02X (%ux%u at %08X)\n", color, (unsigned)width,
                    (unsigned)height, offset);
        }
        return NULL;
    }

    if (width == 0u || height == 0u || width > 4096u || height > 4096u)
        return NULL;
    if (faces == 6u && (width != height ||
        (!nv2a_source_is_compressed(sf) && !d3d8_format_is_swizzled(color))))
        return NULL;

    if (nv2a_source_is_compressed(sf)) {
        d3d_format = (sf == NV2A_SRC_DXT1) ? D3DFMT_DXT1
                   : (sf == NV2A_SRC_DXT3) ? D3DFMT_DXT3 : D3DFMT_DXT5;
    } else {
        /* Linear A8R8G8B8 for everything else: the bytes written below are
         * already linear, and the D3D8 layer decides whether to unswizzle
         * from the format the texture was created with. */
        d3d_format = D3DFMT_LIN_A8R8G8B8;
    }

    source_bytes = pgraph_source_bytes(format, width, height);
    if (offset < 0x00001000u || (uint64_t)offset + source_bytes > 0x04000000u)
        return NULL;
    guest = (const uint8_t *)(uintptr_t)((uintptr_t)offset +
                                         (uintptr_t)g_xbox_mem_offset);
    const uint8_t *face_base = guest;
    uint64_t face_stride = source_bytes / faces;

    {   /* TEMPORARY: how much signal was in the source at the moment this
         * texture was uploaded?  The cache below is keyed on
         * (offset, format, width, height) and has no invalidation, so a
         * surface the guest rewrites in place -- an XMV video frame -- is
         * uploaded exactly once and served from cache for the rest of the
         * run.  Recording the source content here, and re-reading the same
         * guest memory later, separates "the decode never produced pixels"
         * from "it did, but only the first upload was ever seen". */
        extern void pgraph_texsrc_note(uint32_t, uint32_t, uint32_t, uint32_t,
                                       const uint8_t *, uint64_t);
        pgraph_texsrc_note(offset, color, width, height, guest, source_bytes);
    }

    g_rp_phase = 4; /* texture upload */
    g_rp_tex = offset;
    HRESULT created = faces == 6u
        ? d3d8_CreateCubeTexture(width, levels, d3d_format, &texture)
        : dev->lpVtbl->CreateTexture(dev, width, height, levels, 0, d3d_format, 0, &texture);
    if (created != 0 || texture == NULL)
        return NULL;

    for (UINT face = 0; face < faces; ++face) {
        guest = face_base + (size_t)(face * face_stride);
        width = base_width;
        height = base_height;
        for (UINT level = 0; level < levels; ++level) {
            source_bytes = pgraph_level_bytes(sf, width, height);
            source_pitch = nv2a_source_is_compressed(sf)
                ? ((width + 3u) / 4u) * (sf == NV2A_SRC_DXT1 ? 8u : 16u)
                : nv2a_source_is_yuv(sf) ? ((width + 1u) / 2u) * 4u
                : width * nv2a_source_bpp(sf);
            memset(&locked, 0, sizeof(locked));
            if (d3d8_LockTextureFace(texture, face, level, &locked) != 0 ||
                !locked.pBits) {
                texture->lpVtbl->Release(texture);
                return NULL;
            }

            if (nv2a_source_is_compressed(sf)) {
                for (UINT row = 0; row < (height + 3u) / 4u; ++row)
                    memcpy((uint8_t *)locked.pBits + (size_t)row * locked.Pitch,
                           guest + (size_t)row * source_pitch, source_pitch);
            } else {
                const uint8_t *rows = guest;
                uint8_t *linear = NULL;

                /* Swizzled sources are untangled once, here, into a scratch buffer.
                 * The layer will not do it again: the texture is created linear. */
                if (d3d8_format_is_swizzled(color) && !nv2a_source_is_yuv(sf)) {
                    linear = (uint8_t *)malloc((size_t)source_pitch * height);
                    if (!linear) {
                        d3d8_UnlockTextureFace(texture, face, level);
                        texture->lpVtbl->Release(texture);
                        return NULL;
                    }
                    {
                        xbox_unswizzle_rect(linear, guest, width, height,
                                            nv2a_source_bpp(sf));
                        rows = linear;
                    }
                }
                for (y = 0; y < height; ++y)
                    nv2a_decode_row(
                        (uint32_t *)((uint8_t *)locked.pBits + (size_t)y * locked.Pitch),
                        rows + (size_t)y * source_pitch, width, sf);

                /* TEMPORARY: what the CONVERTED texture holds, counted CPU-side in
                 * the buffer we just wrote -- no GPU readback, so this cannot stall
                 * the pipeline the way an after-draw probe did.
                 *
                 * The earlier measurement counted non-zero bytes of the YUY2 SOURCE.
                 * That does not distinguish a real picture from luma-zero data with
                 * chroma sitting at 0x80: both count as "non-zero", and only the
                 * second decodes to black.  This counts output pixels whose RGB is
                 * actually non-zero, and the mean luma, which separates them. */
                if (nv2a_source_is_yuv(sf)) {
                    unsigned nz = 0, tot = 0;
                    unsigned long long lum = 0;
                    for (y = 0; y < height; y += 4u) {
                        const uint32_t *row = (const uint32_t *)
                            ((const uint8_t *)locked.pBits + (size_t)y * locked.Pitch);
                        UINT x;
                        for (x = 0; x < width; x += 4u) {
                            uint32_t px = row[x];
                            ++tot;
                            if (px & 0x00FFFFFFu) ++nz;
                            lum += ((px >> 16) & 0xFFu) + ((px >> 8) & 0xFFu)
                                   + (px & 0xFFu);
                        }
                    }
                    { static unsigned logged;
                      if (logged++ < 12u)
                          fprintf(stderr, "[YUV] %08X %ux%u converted: %u/%u pixels non-black, mean RGB sum %llu (max 765)" "\n",
                                  offset, (unsigned)width, (unsigned)height, nz, tot,
                                  tot ? lum / tot : 0ull); }
                }
                free(linear);
            }
            if (d3d8_UnlockTextureFace(texture, face, level) != 0) {
                texture->lpVtbl->Release(texture);
                return NULL;
            }
            guest += (size_t)source_bytes;
            width = width > 1u ? width / 2u : 1u;
            height = height > 1u ? height / 2u : 1u;
        }
    }
    return texture;
}

/*
 * The texture bound to a stage, with the geometry a caller needs to address
 * it.
 *
 * `out_texel_coords` reports NV2A's split convention: a linear (LU_IMAGE_*)
 * texture is addressed in texels, a swizzled or block-compressed one in the
 * usual 0..1.  D3D11 samplers only do 0..1, so the caller has to divide.
 * Missing this is silent -- the coordinates stay in range as far as the
 * sampler is concerned, CLAMP pins them to an edge texel, and the draw comes
 * out the colour of that edge.
 */
static IDirect3DTexture8 *pgraph_texture_for_stage(IDirect3DDevice8 *dev,
                                                   unsigned stage,
                                                   UINT *out_width,
                                                   UINT *out_height,
                                                   int *out_texel_coords)
{
    uint32_t offset = g_pg.tex[stage].offset & 0x03FFFFFFu;
    uint32_t format = g_pg.tex[stage].format;
    uint32_t color = (format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    UINT width, height;
    int texel_coords_for_color = 0;
    FrontendRuntimeTarget *rendered;
    unsigned i;

    /* A stage the title has switched off must not sample anything, however
     * recently it was bound.  Without this the last texture set stays live
     * across untextured draws -- and once render targets exist, the surface
     * being drawn into looks like a legitimate sample of itself, so every
     * draw modulates by a copy of the surface and nothing can ever become
     * non-black. */
    if (!g_pg.tex[stage].enabled)
        return NULL;
    if (out_width) *out_width = 0u;
    if (out_height) *out_height = 0u;
    {
        Nv2aSourceFormat sf = nv2a_texture_source_format(color);
        texel_coords_for_color = !nv2a_source_is_compressed(sf) &&
                                 !d3d8_format_is_swizzled(color);
    }
    if (out_texel_coords)
        *out_texel_coords = texel_coords_for_color;
    if (offset == 0u || format == 0u)
        return NULL;

    /* Swizzled textures are powers of two and give their size as log2 in the
     * format register; linear ones are arbitrary and give it in the image
     * rect.  Reading the wrong one yields 1x1, silently. */
    if (texel_coords_for_color) {
        uint32_t rect = g_pg.tex[stage].image_rect;
        width  = (UINT)((rect & NV097_SET_TEXTURE_IMAGE_RECT_WIDTH) >> 16);
        height = (UINT)(rect & NV097_SET_TEXTURE_IMAGE_RECT_HEIGHT);
    } else {
        width  = (UINT)(1u << ((format >> 20) & 0xFu));
        height = (UINT)(1u << ((format >> 24) & 0xFu));
    }
    if (width == 0u || height == 0u) {
        width  = (UINT)(1u << ((format >> 20) & 0xFu));
        height = (UINT)(1u << ((format >> 24) & 0xFu));
    }

    IDirect3DTexture8 *depth_color = d3d8_ResolveDepthColorTexture(offset, color,
                                                               width, height, out_width, out_height);
    if (depth_color != NULL) {
        if (out_texel_coords) *out_texel_coords = 1;
        return depth_color;
    }

    rendered = frontend_runtime_target_find(offset);
    if (rendered != NULL && rendered->texture != NULL) {
        if (out_width) *out_width = rendered->width;
        if (out_height) *out_height = rendered->height;
        if (rendered == g_scene_bound_target) {
            /* Sampling the surface being rendered into.  Hand back a copy:
             * binding the target itself would unbind it as the render target
             * and the draw would produce nothing. */
            /* Resolve once before binding the draw's inputs. Repeating the
             * copy for each blur tap unbinds inputs set by earlier stages. */
            if (!g_draw_feedback_texture)
                g_draw_feedback_texture = d3d8_ResolveReadWriteHazard(dev, rendered->texture);
            return g_draw_feedback_texture;
        }
        return rendered->texture;
    }

    if (pgraph_offset_is_surface(offset)) {
        static uint32_t reported[8];
        static unsigned reported_count;
        unsigned k;
        for (k = 0; k < reported_count; ++k)
            if (reported[k] == offset)
                return NULL;
        if (reported_count < 8u) {
            reported[reported_count++] = offset;
            fprintf(stderr, "[WARN PGRAPH-TEXTURE] %08X is a render surface, "
                    "not guest pixels; sampling it needs render-to-texture\n",
                    offset);
        }
        return NULL;
    }

    for (i = 0; i < g_texture_cache_count; ++i) {
        PgraphTextureCacheEntry *entry = &g_texture_cache[i];
        if (entry->offset == offset && entry->format == format &&
            entry->width == width && entry->height == height) {
            entry->last_used = ++g_texture_cache_clock;
            unsigned frame = g_pg.stats.frames + 1u;
            if (entry->texture != NULL && entry->checked_frame != frame) {
                uint64_t bytes = pgraph_source_bytes(format, width, height);
                entry->checked_frame = frame;
                if (bytes != 0u) {
                    uint32_t h = pgraph_source_hash(offset, bytes);
                    if (h != entry->source_hash) {
                        IDirect3DTexture8 *fresh =
                            pgraph_texture_upload(dev, offset, format,
                                                  width, height);
                        if (fresh != NULL) {
                            entry->texture->lpVtbl->Release(entry->texture);
                            entry->texture = fresh;
                            entry->source_hash = h;
                            ++g_texture_reuploads;
                        }
                    }
                }
            }
            if (out_width) *out_width = entry->width;
            if (out_height) *out_height = entry->height;
            return entry->texture;
        }
    }
    {
        unsigned slot = g_texture_cache_count;
        if (slot >= PGRAPH_TEXTURE_CACHE_SIZE) {
            uint64_t oldest = UINT64_MAX;
            slot = PGRAPH_TEXTURE_CACHE_SIZE;
            for (i = 0; i < g_texture_cache_count; ++i) {
                PgraphTextureCacheEntry *entry = &g_texture_cache[i];
                int active = 0;
                for (unsigned s = 0; s < 4; ++s)
                    active |= g_pg.tex[s].enabled && entry->offset ==
                              (g_pg.tex[s].offset & 0x03FFFFFFu);
                if (!active && entry->last_used < oldest) {
                    oldest = entry->last_used; slot = i;
                }
            }
            if (slot == PGRAPH_TEXTURE_CACHE_SIZE) return NULL;
        }
        IDirect3DTexture8 *texture =
            pgraph_texture_upload(dev, offset, format, width, height);
        PgraphTextureCacheEntry *entry = &g_texture_cache[slot];
        static unsigned logged;

        if (slot == g_texture_cache_count) ++g_texture_cache_count;
        else if (entry->texture) entry->texture->lpVtbl->Release(entry->texture);

        /* Cache the miss too: an unsupported format must not be retried on
         * every draw. */
        entry->offset = offset;
        entry->format = format;
        entry->width = width;
        entry->height = height;
        entry->texture = texture;
        entry->last_used = ++g_texture_cache_clock;
        entry->checked_frame = g_pg.stats.frames + 1u;
        entry->source_hash = pgraph_source_hash(
            offset, pgraph_source_bytes(format, width, height));
        if (out_width) *out_width = width;
        if (out_height) *out_height = height;
        if (logged++ < 24u)
            fprintf(stderr, "[INFO PGRAPH-TEXTURE] %s %ux%u colour=0x%02X "
                    "at %08X (%u cached)\n",
                    texture ? "uploaded" : "FAILED", (unsigned)width,
                    (unsigned)height, color, offset, g_texture_cache_count);
        return texture;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Scene surfaces as render targets
 *
 * The title renders into its own colour surfaces -- 0x01C80000 and 0x01DAC000
 * on the menu, alternating per frame -- and composites by binding one of them
 * as a texture.  Drawing straight to the swap chain instead means those
 * samples read guest memory that nothing writes, so a composite pass sampled
 * black and the frame went with it.
 *
 * Each surface therefore gets its own D3D11 render target, draws go to the
 * target for whichever surface is currently bound, and the flip copies the
 * last full-size one to the swap chain.
 *
 * The dimensions come from the surface clip registers rather than a
 * registration table: SET_SURFACE_CLIP_HORIZONTAL carries x in the low half
 * and width in the high half, VERTICAL the same for y and height.  That is
 * how a scene surface can be sized without anything having declared it --
 * frontend_runtime_target_dimensions() only knew addresses the Frontend paths
 * had registered, and returned 0 for these.
 * ══════════════════════════════════════════════════════════════════════ */

/* The surface currently being rendered into, as a D3D texture, plus whichever
 * full-size one was drawn to most recently -- that is what the flip shows. */
/* Where geometry goes, and where it is lost.  Reported periodically so a
 * screen that renders too little can be attributed rather than guessed at. */
static struct {
    unsigned inline_empty;      /* SET_BEGIN_END with no vertices */
    unsigned inline_short;      /* fewer than three vertices */
    unsigned inline_noprims;    /* primitive count came out zero */
    unsigned inline_drawn;
    unsigned array_skipped;     /* array draw rejected before submission */
    unsigned array_drawn;
} g_geom;

static IDirect3DTexture8 *g_scene_present_texture;
static uint32_t g_scene_present_offset;

/* Direct writes to the displayed surface remain visible without a page flip.
 * The loading UI uses this path: it copies its progress image to the front
 * surface repeatedly, while the normal scene/video renderer swaps buffers. */
static uint32_t g_display_surface_offset, g_display_copy_offset;
static uint32_t g_display_dirty_offset;
static int g_display_copy_pending;
static ULONGLONG g_display_refresh_tick;
unsigned long long g_display_front_updates;

static uint32_t pgraph_displayed_surface(void)
{
    uint32_t scanout = nv2a_display_scanout_start() & 0x03FFFFFFu;
    return scanout ? scanout : g_display_surface_offset;
}

static void pgraph_display_surface_written(uint32_t offset)
{
    if (offset && offset == pgraph_displayed_surface())
        g_display_dirty_offset = offset;
}

static void pgraph_display_presented(void)
{
    g_display_surface_offset = g_display_copy_offset;
    g_display_dirty_offset = 0;
    g_display_copy_pending = 0;
    g_display_refresh_tick = GetTickCount64();
}

/* The full-size surface currently bound, promoted to the presented one only
 * once something is drawn into it. */
static IDirect3DTexture8 *g_scene_bound_texture;
static uint32_t g_scene_bound_offset;

/* The destination of the last full-size composite -- the finished frame. */
static IDirect3DTexture8 *g_scene_composite_texture;
static uint32_t g_scene_composite_offset;


/*
 * Size of the current render surface.
 *
 * Width comes from the colour pitch and guest pixel size: 0x0A00 bytes at
 * 4 bytes per pixel is 640, while RGB565 pitch 0x100 describes 128 pixels.
 * The clip
 * registers are a scissor rectangle and move around inside the surface --
 * sizing by them produced a 638x480 target for 0x01DAC000 one moment and
 * 128x256 the next, and since acquire() keys on dimensions each change threw
 * the target away and took the rendered frame with it.
 *
 * Height has no register of its own, so the clip height is the best available
 * estimate and is only ever used to create a target, never to match one.
 */
static int pgraph_surface_dimensions(UINT *out_width, UINT *out_height)
{
    UINT bytes_per_pixel = 4u;
    switch (g_pg.surface_format & 0xFu) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        bytes_per_pixel = 2u;
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        bytes_per_pixel = 1u;
        break;
    }
    UINT width = (UINT)((g_pg.surface_pitch & 0xFFFFu) / bytes_per_pixel);
    UINT height = (UINT)((g_pg.surface_clip_v >> 16) & 0xFFFFu);

    if (width == 0u)
        width = (UINT)((g_pg.surface_clip_h >> 16) & 0xFFFFu);
    if (height == 0u)
        height = d3d8_GetBackbufferHeight();
    if (width == 0u || height == 0u || width > 4096u || height > 4096u)
        return 0;
    *out_width = width;
    *out_height = height;
    return 1;
}

/*
 * Bind the render target for the surface the title is drawing into.
 *
 * Returns 1 when a target was bound, so the caller knows the swap chain is no
 * longer the destination.  Called from every draw path: splitting a frame
 * between a runtime target and the default RTV would put half the scene
 * somewhere the flip never looks.
 */
/*
 * One frame's clears and draws, in arrival order.
 *
 * The title asks for a full black clear of each scene surface about once a
 * frame and none of those clears reaches the surface, because dev_Clear()
 * sends them to the back buffer.  Routing them to the bound target instead
 * blanks the screen, so the frame is evidently already drawn into the surface
 * by the time its clear arrives.  Printing both against a single counter is
 * the way to see that phase directly rather than infer it.
 */
static unsigned g_seq;
static double dt_now(void);

static int pgraph_seq_active(void)
{
    /* The window was a hardcoded 300000-300003, which a normal session
     * never reaches -- so this per-draw log could not be used to look at
     * the boot video at all.  CONKER_SEQ_FRAME picks the first frame of a
     * window; CONKER_SEQ_COUNT can select 1-256 batches instead of four.
     * Unset keeps the original range. */
    static int resolved;
    static unsigned first = 300000u;
    static unsigned count = 4u;
    static double after = -1.0;
    static int triggered;
    if (!resolved) {
        const char *s = getenv("CONKER_SEQ_FRAME");
        resolved = 1;
        if (s != NULL && *s != 0) first = (unsigned)strtoul(s, NULL, 0);
        s = getenv("CONKER_SEQ_COUNT");
        if (s != NULL && *s != 0) {
            unsigned requested = (unsigned)strtoul(s, NULL, 0);
            if (requested > 0u && requested <= 256u) count = requested;
        }
        s = getenv("CONKER_SEQ_AFTER");
        if (s != NULL && *s != 0) after = atof(s);
    }
    if (after >= 0.0 && !triggered) {
        if (dt_now() < after || g_pg.transform_program_count <= 20u) return 0;
        first = g_pg.stats.frames;
        triggered = 1;
        fprintf(stderr, "[SEQ] scene trace triggered at %.3fs frame=%u program=%u\n",
                dt_now(), first, g_pg.transform_program_count);
    }
    return g_pg.stats.frames >= first && g_pg.stats.frames - first < count;
}

static int pgraph_bind_depth_surface(void)
{
    float width, height;
    uint32_t zeta_format = (g_pg.surface_format >> 4) & 0xFu;
    D3DFORMAT format;
    HRESULT hr;
    if (zeta_format == 1u) format = D3DFMT_D16;
    else if (zeta_format == 2u) format = D3DFMT_D24S8;
    else return 0;
    d3d8_GetRenderTargetSize(&width, &height);
    UINT pitch = g_pg.surface_pitch >> 16;
    if (!pitch) pitch = (UINT)width * (format == D3DFMT_D16 ? 2u : 4u);
    hr = d3d8_BindRuntimeDepthStencilLayout(g_pg.surface_zeta_offset & 0x03FFFFFFu,
        (UINT)width, (UINT)height, format, pitch,
        ((g_pg.surface_format >> 8) & 3u) == 2u,
        g_pg.depth_test && !g_pg.depth_mask && !g_pg.stencil_test);
    if (FAILED(hr)) {
        static unsigned warnings;
        if (warnings++ < 8u)
            fprintf(stderr, "[WARN PGRAPH-ZETA] bind %08X failed: %08lX\n",
                    g_pg.surface_zeta_offset, (unsigned long)hr);
    }
    return SUCCEEDED(hr);
}

static DWORD pgraph_texture_address(uint32_t address)
{
    switch (address & 7u) {
    case 1: return D3DTADDRESS_WRAP;
    case 2: return D3DTADDRESS_MIRROR;
    case 4: return D3DTADDRESS_BORDER;
    case 3: case 5: return D3DTADDRESS_CLAMP; /* legacy GL_CLAMP approximated */
    default: return D3DTADDRESS_WRAP;
    }
}

static void pgraph_apply_sampler_state(IDirect3DDevice8 *dev, unsigned stage)
{
    uint32_t filter = g_pg.tex[stage].filter;
    unsigned min = (filter >> 16) & 0x3Fu;
    unsigned mag = (filter >> 24) & 15u;
    DWORD min_filter = D3DTEXF_POINT, mip_filter = D3DTEXF_NONE;
    switch (min) {
    case 2: case 7: min_filter = D3DTEXF_LINEAR; break;
    case 3: mip_filter = D3DTEXF_POINT; break;
    case 4: min_filter = D3DTEXF_LINEAR; mip_filter = D3DTEXF_POINT; break;
    case 5: mip_filter = D3DTEXF_LINEAR; break;
    case 6: min_filter = D3DTEXF_LINEAR; mip_filter = D3DTEXF_LINEAR; break;
    }
    uint32_t control = g_pg.tex[stage].control0;
    DWORD anisotropy = 1u << ((control >> 4) & 3u);
    if (anisotropy > 1u && min_filter == D3DTEXF_LINEAR)
        min_filter = D3DTEXF_ANISOTROPIC;
    int bias_bits = (int)(filter & 0x1FFFu);
    if (bias_bits & 0x1000) bias_bits -= 0x2000;
    float bias = (float)bias_bits / 256.0f;
    DWORD bias_word;
    memcpy(&bias_word, &bias, sizeof(bias_word));
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MIPMAPLODBIAS, bias_word);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MAXANISOTROPY, anisotropy);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MAXMIPLEVEL,
                                    (control >> 18) & 0xFFFu);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_NV2A_MAXMIPLEVEL,
                                    ((control >> 6) & 0xFFFu) + 1u);
    /* Convolution modes use linear filtering until their kernels are supported. */
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MINFILTER, min_filter);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MAGFILTER,
                                    mag == 2u || mag == 4u ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_MIPFILTER, mip_filter);
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_ADDRESSU,
                                    pgraph_texture_address(g_pg.tex[stage].address));
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_ADDRESSV,
                                    pgraph_texture_address(g_pg.tex[stage].address >> 8));
    dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_BORDERCOLOR,
                                    g_pg.tex[stage].border_color);
}

static int pgraph_apply_cull_state(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE primitive)
{
    DWORD mode = D3DCULL_NONE;
    int draw = 1;
    if (g_pg.cull_enable && primitive >= D3DPT_TRIANGLELIST) {
        if (g_pg.cull_face == NV097_SET_CULL_FACE_V_FRONT_AND_BACK) {
            draw = 0; /* D3D11 has no rasterizer mode that culls both faces. */
        } else if (g_pg.cull_face == NV097_SET_CULL_FACE_V_FRONT ||
                   g_pg.cull_face == NV097_SET_CULL_FACE_V_BACK) {
            /* These vertices already use the guest's window coordinates.
             * D3D's viewport reverses the VS screen-to-clip Y conversion;
             * do not add the inversion needed by an OpenGL viewport. */
            int front_cw = g_pg.front_face == NV097_SET_FRONT_FACE_V_CW;
            int cull_cw = g_pg.cull_face == NV097_SET_CULL_FACE_V_FRONT
                ? front_cw : !front_cw;
            mode = cull_cw ? D3DCULL_CW : D3DCULL_CCW;
        }
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, mode);
    return draw;
}

static DWORD pgraph_color_write_mask(void)
{
    return ((g_pg.color_mask & 0x00010000u) ? 0x1u : 0u) |
           ((g_pg.color_mask & 0x00000100u) ? 0x2u : 0u) |
           ((g_pg.color_mask & 0x00000001u) ? 0x4u : 0u) |
           ((g_pg.color_mask & 0x01000000u) ? 0x8u : 0u);
}

static void pgraph_apply_alpha_state(IDirect3DDevice8 *dev)
{
    DWORD func = g_pg.alpha_func >= 0x0200u && g_pg.alpha_func <= 0x0207u
        ? g_pg.alpha_func - 0x0200u + D3DCMP_NEVER : D3DCMP_ALWAYS;
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE,
                                g_pg.alpha_test ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAFUNC, func);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAREF, g_pg.alpha_ref);
}

static DWORD pgraph_stencil_op(uint32_t op)
{
    switch (op) {
    case NV097_SET_STENCIL_OP_V_ZERO: return 2;
    case NV097_SET_STENCIL_OP_V_REPLACE: return 3;
    case NV097_SET_STENCIL_OP_V_INCRSAT: return 4;
    case NV097_SET_STENCIL_OP_V_DECRSAT: return 5;
    case NV097_SET_STENCIL_OP_V_INVERT: return 6;
    case NV097_SET_STENCIL_OP_V_INCR: return 7;
    case NV097_SET_STENCIL_OP_V_DECR: return 8;
    default: return 1; /* KEEP */
    }
}

static void pgraph_apply_depth_state(IDirect3DDevice8 *dev)
{
    int bound = (g_pg.depth_test || g_pg.stencil_test) && pgraph_bind_depth_surface();
    int enabled = g_pg.depth_test && bound;
    DWORD func = g_pg.depth_func >= 0x0200u && g_pg.depth_func <= 0x0207u
        ? g_pg.depth_func - 0x0200u + D3DCMP_NEVER : D3DCMP_LESSEQUAL;
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, enabled ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE,
                                g_pg.depth_mask ? TRUE : FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, func);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE, g_pg.stencil_test && bound);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFUNC,
        g_pg.stencil_func >= 0x200u && g_pg.stencil_func <= 0x207u
        ? g_pg.stencil_func - 0x200u + D3DCMP_NEVER : D3DCMP_ALWAYS);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILREF, g_pg.stencil_ref);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILMASK, g_pg.stencil_read_mask);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILWRITEMASK, g_pg.stencil_write_mask);
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFAIL, pgraph_stencil_op(g_pg.stencil_fail));
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILZFAIL, pgraph_stencil_op(g_pg.stencil_zfail));
    dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILPASS, pgraph_stencil_op(g_pg.stencil_pass));
    if (pgraph_seq_active())
        fprintf(stderr, "[DEPTH] test=%d bound=%d func=%04X write=%u zeta=%08X "
                "clear=%08X zminmax=%08X control0=%08X\n", g_pg.depth_test, enabled,
                g_pg.depth_func, g_pg.depth_mask, g_pg.surface_zeta_offset,
                g_pg.clear_zstencil, g_pg.zmin_max_control, g_pg.control0);
}

static void pgraph_clear_depth_value(float *depth, DWORD *stencil)
{
    if (((g_pg.surface_format >> 4) & 0xFu) == 1u) {
        *depth = (g_pg.clear_zstencil & 0xFFFFu) / 65535.0f;
        *stencil = 0;
    } else {
        *depth = (g_pg.clear_zstencil >> 8) / 16777215.0f;
        *stencil = g_pg.clear_zstencil & 0xFFu;
    }
}

static D3DCOLOR pgraph_clear_color_value(void)
{
    uint32_t raw = g_pg.clear_color;
    uint32_t r, g, b;
    switch (g_pg.surface_format & 0xFu) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        r = (((raw >> 11) & 31u) * 255u + 15u) / 31u;
        g = (((raw >> 5) & 63u) * 255u + 31u) / 63u;
        b = ((raw & 31u) * 255u + 15u) / 31u;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        r = (((raw >> 10) & 31u) * 255u + 15u) / 31u;
        g = (((raw >> 5) & 31u) * 255u + 15u) / 31u;
        b = ((raw & 31u) * 255u + 15u) / 31u;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
        return raw | 0xFF000000u;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
        return (raw & 0x00FFFFFFu) | (((((raw >> 24) & 127u) * 255u + 63u) / 127u) << 24);
    default:
        return raw;
    }
}

static int pgraph_bind_scene_surface(IDirect3DDevice8 *dev)
{
    uint32_t offset = g_pg.surface_color_offset & 0x03FFFFFFu;
    FrontendRuntimeTarget *target;
    UINT width, height;
    uint32_t submission;

    if (dev == NULL || offset < 0x00001000u || offset >= 0x04000000u)
        return 0;

    /* An existing target keeps the size it was created with.  Re-acquiring at
     * whatever the scissor happens to say would evict it and discard the
     * frame drawn into it so far. */
    target = frontend_runtime_target_find(offset);
    if (target == NULL) {
        if (!pgraph_surface_dimensions(&width, &height))
            return 0;
        target = frontend_runtime_target_acquire(dev, offset, width, height);
    }
    if (target == NULL || target->texture == NULL)
        return 0;
    target->last_used_submission = g_pg.stats.frames + 1u;
    width = target->width;
    height = target->height;

    /* A target cannot be a pixel-shader input and a render target at once. */
    dev->lpVtbl->SetTexture(dev, 0, NULL);

    /* Bind without clearing.  The title clears when it means to, through
     * NV097_CLEAR_SURFACE, and that path binds this same target first.
     * Clearing on bind looked reasonable and was not: g_pg.stats.frames counts
     * push-buffer submissions rather than frames, so "once per frame" fired
     * before nearly every draw and left only the last one standing. */
    if (pgraph_seq_active())
        d3d8_DebugIdentities(target->texture, "before-bind", offset);
    g_rp_phase = 5; /* render-target bind */
    g_rp_rt = offset;
    if (FAILED(d3d8_BindRuntimeRenderTexture(target->texture, FALSE, 0)))
        return 0;
    if (pgraph_seq_active())
        d3d8_DebugIdentities(target->texture, "after-bind", offset);
    (void)submission;
    g_scene_bound_texture = target->texture;
    g_scene_bound_target = target;
    g_draw_feedback_texture = NULL;
    { static uint32_t seen[16]; static unsigned seen_count; unsigned k;
      uint32_t key = offset ^ ((uint32_t)target->width << 4) ^
                     ((uint32_t)target->height << 18);
      for (k = 0; k < seen_count; ++k) if (seen[k] == key) break;
      if (k == seen_count && seen_count < 16u) {
          seen[seen_count++] = key;
          fprintf(stderr, "[INFO PGRAPH-SURFACE] draws bound to %08X as %ux%u "
                  "(pitch=%08X clip=%08X/%08X)\n", offset,
                  (unsigned)target->width, (unsigned)target->height,
                  g_pg.surface_pitch, g_pg.surface_clip_h,
                  g_pg.surface_clip_v); } }

    /* Remember which full-size surface this is, but do not make it the one to
     * present yet.  Binding a surface says the title is about to draw into it,
     * not that it has finished with it -- and by the time the flip arrives the
     * title has already bound the next frame's surface.  Choosing on bind
     * therefore presented the finished frame and the one being started on
     * alternate flips, which is two different pictures at 60 Hz.
     *
     * pgraph_scene_surface_drawn() below promotes it once geometry actually
     * lands in it. */
    if (width == d3d8_GetBackbufferWidth() &&
        height == d3d8_GetBackbufferHeight()) {
        g_scene_bound_texture = target->texture;
        g_scene_bound_offset = offset;
    } else {
        g_scene_bound_texture = NULL;
        g_scene_bound_offset = 0u;
    }
    return 1;
}

/*
 * A draw landed on the bound full-size surface.
 *
 * Which of the title's two full-size surfaces is the finished frame cannot be
 * read anywhere: the flip methods carry fixed indices, NV_PCRTC_START is never
 * written through MMIO, and the title alternates which surface it binds every
 * frame.  Presenting by bind order or by draw order therefore showed the
 * finished frame and the half-built one alternately -- two pictures at 60 Hz.
 *
 * The surface last drawn into before the flip is the frame.  An ordered log of
 * one frame's clears and draws shows why: the title clears one scene surface,
 * draws the whole frame into it, runs its bloom chain, composites the result
 * back into that same surface, and flips.  The next frame does the same into
 * the other one.  So it alternates, and following it is correct.
 *
 * Choosing by the destination of a full-size composite was tried instead, on
 * the theory that alternating was itself the fault.  It latches: the final
 * composite samples a 128x256 blur target, not a full-size surface, so the
 * rule never fired for it and kept pointing at an earlier frame's surface --
 * the one the title was about to clear and reuse.
 */
/* PRESENT TRACE (temporary).
 *
 * The XMV frame is measured to decode, to be sampled by thousands of draws,
 * and to be drawn as a full 640x480 quad -- yet the presented back buffer is
 * opaque black apart from ~16384 pixels, exactly 128x128.  These record which
 * surface each draw actually lands in, so the per-Present line below can say
 * whether the frame reaches the presented surface at all. */
static uint32_t g_pt_xmv_rt_offset;
static unsigned long long g_pt_xmv_draws;
static uint32_t g_pt_small_rt_offset;
static unsigned g_pt_small_w, g_pt_small_h;
static unsigned long long g_pt_small_draws;
static unsigned long long g_pt_presents;

uint64_t pgraph_d3d11_present_count(void)
{
    return g_pt_presents;
}

/* Is this guest offset a YUY2 texture in the cache?  0x24 is D3DFMT_YUY2,
 * which in this title only the video decoder produces. */
static int pt_offset_is_yuv(uint32_t offset)
{
    unsigned i;
    for (i = 0; i < g_texture_cache_count; ++i) {
        PgraphTextureCacheEntry *e = &g_texture_cache[i];
        if (e->offset != offset) continue;
        return nv2a_source_is_yuv(nv2a_texture_source_format(
            (e->format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8));
    }
    return 0;
}

static void pgraph_scene_surface_drawn(uint32_t sampled_offset)
{
    FrontendRuntimeTarget *source;

    if (g_scene_bound_texture == NULL)
        return;

    pgraph_display_surface_written(g_scene_bound_offset);

    (void)sampled_offset;
    (void)source;

    /* Record where the video actually lands, and where the small offscreen
     * passes land, without changing either. */
    if (pt_offset_is_yuv(sampled_offset)) {
        g_pt_xmv_rt_offset = g_scene_bound_offset;
        ++g_pt_xmv_draws;
    } else if (g_scene_bound_texture != NULL) {
        FrontendRuntimeTarget *b =
            frontend_runtime_target_find(g_scene_bound_offset);
        if (b != NULL && b->width <= 128u && b->height <= 128u) {
            g_pt_small_rt_offset = g_scene_bound_offset;
            g_pt_small_w = (unsigned)b->width;
            g_pt_small_h = (unsigned)b->height;
            ++g_pt_small_draws;
        }
    }

    g_scene_present_texture = g_scene_bound_texture;
    g_scene_present_offset = g_scene_bound_offset;
}

/*
 * Copy the finished scene surface onto the swap chain.
 *
 * A textured full-screen quad rather than CopyResource: the surface target and
 * the back buffer are different resources with different descriptions, and the
 * quad also lets the swap chain stay whatever size the host window wants.
 */
/* Opt-in readback audit: distinguish a bad selected surface from a copy
 * overwritten between FLIP_INCREMENT_WRITE and FLIP_STALL. */
static void pgraph_audit_present(int at_stall)
{
    static int initialized;
    static const char *prefix;
    static const char *trigger;
    static FILE *log;
    static unsigned count, captures, copy_hash, copy_nonblack;
    static unsigned capture_frames;
    static int capture_effects;
    if (!initialized) {
        initialized = 1;
        prefix = getenv("CONKER_PRESENT_AUDIT");
        trigger = getenv("CONKER_PRESENT_TRIGGER");
        const char *frames = getenv("CONKER_PRESENT_CAPTURE_COUNT");
        capture_frames = frames ? (unsigned)atoi(frames) : 0u;
        capture_effects = getenv("CONKER_PRESENT_EFFECTS") != NULL;
        if (capture_frames > 256u) capture_frames = 256u;
        if (prefix) {
            char name[512];
            snprintf(name, sizeof(name), "%s.csv", prefix);
            log = fopen(name, "w");
            if (log) fprintf(log, "present,stage,ms,selected,scanout,bound,nonblack,hash,copy_nonblack,copy_hash\n");
        }
    }
    if (!log || count >= 2400u) return;
    if (trigger && GetFileAttributesA(trigger) == INVALID_FILE_ATTRIBUTES) return;
    unsigned nb = 0, sampled = 0;
    uint32_t hash = 0;
    d3d8_DebugProbeBackbuffer(&nb, &sampled, &hash);
    if (!at_stall) { copy_hash = hash; copy_nonblack = nb; }
    fprintf(log, "%u,%d,%llu,%08X,%08X,%08X,%u,%08X,%u,%08X\n",
            count, at_stall, (unsigned long long)GetTickCount64(),
            g_scene_present_offset, nv2a_display_scanout_start() & 0x03FFFFFFu,
            g_pg.surface_color_offset, nb, hash, copy_nonblack, copy_hash);
    if (at_stall) {
        int suspicious = (copy_nonblack > 1600 && nb < 1600) || (nb > 0 && nb < 1100);
        if ((capture_frames && captures < capture_frames) ||
            (!capture_frames && count > 40 && captures < 8 && suspicious)) {
            extern void d3d8_DebugDumpBackbuffer(const char *path);
            char name[512];
            snprintf(name, sizeof(name), "%s.%u.backbuffer.bmp", prefix, count);
            d3d8_DebugDumpBackbuffer(name);
            for (FrontendRuntimeTarget *t = g_pg.frontend_runtime_target;
                 (!capture_frames || capture_effects) && t != NULL; t = t->next) {
                if (!t->texture || (capture_effects
                    ? t->width < 128 || t->width > 256
                    : t->width < 600)) continue;
                snprintf(name, sizeof(name), "%s.%u.rt%08X.bmp", prefix, count, t->guest_offset);
                d3d8_DebugDumpRuntimeTexture(t->texture, name);
            }
            ++captures;
        }
        ++count;
        fflush(log);
    }
}

static void pgraph_present_scene_surface(IDirect3DDevice8 *dev)
{
    g_rp_phase = 6; /* Present / scene composite */
    ++g_rp_presents;
    OutputVertex quad[6];
    float w, h;
    unsigned i;

    /* Prefer the surface the display is actually scanning out.
     *
     * The title renders into two full-size surfaces and composites one onto
     * the other, alternating which it binds every frame, and the flip methods
     * carry fixed indices rather than a per-frame choice.  So neither bind
     * order nor draw order distinguishes the finished frame from the work
     * surface, and presenting by either showed one of each in alternation --
     * two different pictures at 60 Hz.  NV_PCRTC_START is the register that
     * says which one is on screen.
     *
     * If the title has not set it, or it names something with no runtime
     * target, fall back to the last surface drawn into. */
    { uint32_t scanout = nv2a_display_scanout_start() & 0x03FFFFFFu;
      if (scanout != 0u) {
          FrontendRuntimeTarget *shown = frontend_runtime_target_find(scanout);
          if (shown != NULL && shown->texture != NULL) {
              g_scene_present_texture = shown->texture;
              g_scene_present_offset = shown->guest_offset;
          }
      } }

    if (dev == NULL || g_scene_present_texture == NULL)
        return;

    w = (float)d3d8_GetBackbufferWidth();
    h = (float)d3d8_GetBackbufferHeight();

    memset(quad, 0, sizeof(quad));
    for (i = 0; i < 6u; ++i) {
        quad[i].z = 0.0f;
        quad[i].rhw = 1.0f;
        quad[i].color = 0xFFFFFFFFu;
        quad[i].fog = 1.0f;
        quad[i].q = 1.0f;
        for (unsigned stage = 0; stage < 3; ++stage) quad[i].extra_uv[stage][3] = 1.0f;
    }
    quad[0].x = 0.0f; quad[0].y = 0.0f; quad[0].u = 0.0f; quad[0].v = 0.0f;
    quad[1].x = w;    quad[1].y = 0.0f; quad[1].u = 1.0f; quad[1].v = 0.0f;
    quad[2].x = w;    quad[2].y = h;    quad[2].u = 1.0f; quad[2].v = 1.0f;
    quad[3].x = 0.0f; quad[3].y = 0.0f; quad[3].u = 0.0f; quad[3].v = 0.0f;
    quad[4].x = w;    quad[4].y = h;    quad[4].u = 1.0f; quad[4].v = 1.0f;
    quad[5].x = 0.0f; quad[5].y = h;    quad[5].u = 0.0f; quad[5].v = 1.0f;

    { static unsigned n;
      if (n++ < 6u)
          d3d8_DebugIdentities(g_scene_present_texture, "at-present",
                               g_scene_present_offset); }
    d3d8_RestoreDefaultRenderTarget();

    /* Diagnostic: paint the back buffer a colour nothing else uses before the
     * scene copy.  Anything still visible on top of it was drawn after this
     * point; anything replaced by it was drawn before.  Opt-in via
     * CONKER_PRESENT_TINT, so normal runs are untouched. */
    if (getenv("CONKER_PRESENT_TINT"))
        dev->lpVtbl->Clear(dev, 0, NULL, 1 /*D3DCLEAR_TARGET*/,
                           0xFFFF00FFu, 1.0f, 0);

    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetVertexShader(dev,
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_NV2A_FOG | D3DFVF_NV2A_TEX4);
    dev->lpVtbl->SetTexture(dev, 0,
        (IDirect3DBaseTexture8 *)g_scene_present_texture);
    dev->lpVtbl->SetTextureStageState(dev, 0, 1, 2 /*SELECTARG1*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 2, 2 /*TEXTURE*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 4, 2 /*SELECTARG1*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 5, 2 /*TEXTURE*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 13, 3 /*CLAMP*/);
    dev->lpVtbl->SetTextureStageState(dev, 0, 14, 3 /*CLAMP*/);

    { /* Presentation copies a finished image. Do not run the last guest
       * material's combiner or auxiliary texture stages over it again. */
      extern DWORD d3d8_combiners_debug_token(void);
      DWORD token = d3d8_combiners_debug_token(), alpha, fog, mask, specular, colorop[3];
      dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &alpha);
      dev->lpVtbl->GetRenderState(dev, D3DRS_FOGENABLE, &fog);
      dev->lpVtbl->GetRenderState(dev, D3DRS_COLORWRITEENABLE, &mask);
      dev->lpVtbl->GetRenderState(dev, D3DRS_SPECULARENABLE, &specular);
      for (i = 1; i < 4; ++i) {
          dev->lpVtbl->GetTextureStageState(dev, i, D3DTSS_COLOROP, &colorop[i-1]);
          dev->lpVtbl->SetTextureStageState(dev, i, D3DTSS_COLOROP, D3DTOP_DISABLE);
      }
      d3d8_combiners_set_pixel_shader(0);
      dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
      dev->lpVtbl->SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
      dev->lpVtbl->SetRenderState(dev, D3DRS_SPECULARENABLE, FALSE);
      dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, 15);
      dev->lpVtbl->BeginScene(dev);
      dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad, sizeof(OutputVertex));
      g_display_copy_offset = g_scene_present_offset;
      d3d8_combiners_set_pixel_shader(token);
      dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, alpha);
      dev->lpVtbl->SetRenderState(dev, D3DRS_FOGENABLE, fog);
      dev->lpVtbl->SetRenderState(dev, D3DRS_SPECULARENABLE, specular);
      dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, mask);
      for (i = 1; i < 4; ++i)
          dev->lpVtbl->SetTextureStageState(dev, i, D3DTSS_COLOROP, colorop[i-1]);
    }

    dev->lpVtbl->SetTexture(dev, 0, NULL);

    {
        static unsigned logged;
        if (logged++ < 4u)
            fprintf(stderr, "[INFO PGRAPH-SCENE] presented surface %08X "
                    "as %.0fx%.0f\n", g_scene_present_offset, w, h);

        { unsigned n = (unsigned)(g_pt_presents++);
          if (n < 40u || (n % 200u) == 0u) {
              unsigned sw = 0, sh = 0, snb = 0, sn = 0;
              unsigned xw = 0, xh = 0, xnb = 0, xn = 0;
              unsigned bnb = 0, bn = 0;
              uint32_t shash = 0, xhash = 0, bhash = 0;
              uint32_t scanout = nv2a_display_scanout_start() & 0x03FFFFFFu;
              FrontendRuntimeTarget *xt = g_pt_xmv_rt_offset
                  ? frontend_runtime_target_find(g_pt_xmv_rt_offset) : NULL;

              d3d8_DebugProbeTexture(g_scene_present_texture, &sw, &sh,
                                     &snb, &sn, &shash);
              if (xt != NULL && xt->texture != NULL)
                  d3d8_DebugProbeTexture(xt->texture, &xw, &xh, &xnb, &xn,
                                         &xhash);
              d3d8_DebugProbeBackbuffer(&bnb, &bn, &bhash);

              fprintf(stderr,
                  "[PT] present #%u frame=%u" "\n"
                  "[PT]   selected src=%08X %ux%u nonblack=%u/%u hash=%08X" "\n"
                  "[PT]   scanout=%08X  lastRT=%08X" "\n"
                  "[PT]   XMV RT=%08X %ux%u nonblack=%u/%u hash=%08X draws=%llu" "\n"
                  "[PT]   small RT=%08X %ux%u draws=%llu" "\n"
                  "[PT]   backbuffer nonblack=%u/%u hash=%08X" "\n",
                  n, g_pg.stats.frames,
                  g_scene_present_offset, sw, sh, snb, sn, shash,
                  scanout, g_scene_bound_offset,
                  g_pt_xmv_rt_offset, xw, xh, xnb, xn, xhash, g_pt_xmv_draws,
                  g_pt_small_rt_offset, g_pt_small_w, g_pt_small_h,
                  g_pt_small_draws,
                  bnb, bn, bhash);
              fflush(stderr);
          } }
        /* Every runtime target once, as an image.  Which surface holds the
         * miniature and the noise decides whether they arrive wrong or are
         * composited wrong. */
        { static int dumped;
          const char *dir = getenv("CONKER_DUMP_SURFACES");
          /* Two sets: one on the loading screen, one well after it.  A
           * single early set is what kept these dumps showing a picture that
           * was no longer on the window. */
          if (dir != NULL && dumped < 2 &&
              g_pg.stats.frames >= (dumped == 0 ? 2000u : 4500u)) {
              unsigned i;
              char stamp[400];
              snprintf(stamp, sizeof(stamp), "%s.f%u", dir,
                       (unsigned)g_pg.stats.frames);
              dir = stamp;
              ++dumped;
              /* Textures uploaded from guest memory first.  A full-screen
               * quad samples one of these 1:1, so a bad upload reaches the
               * frame unchanged and looks exactly like a bad draw. */
              for (i = 0u; i < g_texture_cache_count; ++i) {
                  PgraphTextureCacheEntry *e = &g_texture_cache[i];
                  char path[512];
                  if (e->texture == NULL)
                      continue;
                  snprintf(path, sizeof(path), "%s.tex%08X.%ux%u.bmp",
                           dir, e->offset, (unsigned)e->width,
                           (unsigned)e->height);
                  d3d8_DebugDumpRuntimeTexture(e->texture, path);
              }
              for (FrontendRuntimeTarget *t = g_pg.frontend_runtime_target;
                   t != NULL; t = t->next) {
                  char path[512];
                  if (t->texture == NULL)
                      continue;
                  snprintf(path, sizeof(path), "%s.%08X.%ux%u.bmp",
                           dir, t->guest_offset, (unsigned)t->width,
                           (unsigned)t->height);
                  d3d8_DebugDumpRuntimeTexture(t->texture, path);
              }

          } }

        /* Temporary: what each full-size surface holds at the moment one of
         * them is presented.  The title alternates between two every frame,
         * so if one is consistently fuller than the other, the difference is
         * in what reaches them, not in which one is chosen. */
        { static unsigned sampled;
          if (sampled++ < 6u) {
              for (FrontendRuntimeTarget *t = g_pg.frontend_runtime_target;
                   t != NULL; t = t->next) {
                  if (t->texture != NULL && (float)t->width == w)
                      d3d8_DebugSampleRuntimeTexture(
                          t->texture,
                          t->texture == g_scene_present_texture
                              ? "presented" : "other",
                          t->guest_offset);
              }
          } }
    }
}

/* A completed command submission can update scanout in place without issuing
 * FLIP_INCREMENT_WRITE/FLIP_STALL. Refresh that same surface at display cadence;
 * never expose a back buffer or replace a copy awaiting its guest flip stall.
 * This does not advance the guest flip ring or synthesize a vblank interrupt. */
static void pgraph_refresh_displayed_surface(IDirect3DDevice8 *dev)
{
    uint32_t displayed = pgraph_displayed_surface();
    if (!dev || !g_display_dirty_offset || g_display_copy_pending ||
        g_display_dirty_offset != displayed ||
        GetTickCount64() - g_display_refresh_tick < 16u)
        return;
    FrontendRuntimeTarget *target = frontend_runtime_target_find(displayed);
    if (!target || !target->texture)
        return;

    IDirect3DTexture8 *saved_texture = g_scene_present_texture;
    uint32_t saved_offset = g_scene_present_offset;
    g_scene_present_texture = target->texture;
    g_scene_present_offset = displayed;
    pgraph_present_scene_surface(dev);
    pgraph_audit_present(0);
    pgraph_audit_present(1);
    d3d8_PresentFrame();
    g_scene_present_texture = saved_texture;
    g_scene_present_offset = saved_offset;
    ++g_display_front_updates;
}

/*
 * Bind texture stages 1..3.
 *
 * The D3D8 layer's pixel shader has always implemented four stages with the
 * full set of D3D8 colour operations, but the draw paths bound stage 0 and
 * nothing else.  The title is a multi-texture renderer -- the trace shows it
 * setting stage 0 through stage 3 -- so it was being drawn with one of its
 * textures, and for most draws that one was 023EEC00, a two-channel gradient
 * that is a lookup map rather than surface colour.  Hence flat white where
 * nothing bound and coloured speckle where that one did.
 *
 * Each further enabled stage modulates onto the running result, which is the
 * fixed-function chain this layer expects.  Stages the title has switched off
 * are disabled explicitly so a stage left over from an earlier draw cannot
 * contribute to this one.
 *
 * Numeric stage-state indices match the ones already used here: 1 COLOROP,
 * 2 COLORARG1, 3 COLORARG2, 4 ALPHAOP, 5 ALPHAARG1, 6 ALPHAARG2, 13/14 the
 * address modes.  Argument values: 0 DIFFUSE, 1 CURRENT, 2 TEXTURE.
 */
/*
 * Report what the title actually programs into the register combiners.
 *
 * The state is captured but never consulted: every stage is chained with
 * MODULATE, which is a fixed-function guess rather than what was asked for.
 * Translating it needs the real encoding, and the encoding is worth reading off
 * the title rather than reconstructing from memory -- an input decode that is
 * subtly wrong produces confidently wrong colours that look plausible.
 *
 * Each 32-bit input control word packs four inputs, A..D, one byte each, A in
 * the high byte.  Within a byte the top nibble selects the source register and
 * the low nibble selects the component and mapping.  Print the raw words and
 * that split for the active stages, once per distinct configuration.
 */
/*
 * A register-combiner input byte as a D3D8 texture-stage argument.
 *
 * Layout read off the title's own programming rather than from memory: the low
 * nibble selects the source register, the high nibble the mapping.  The values
 * that appear are 08200000 (texture times one), 04200000 (diffuse times one),
 * C4C80000 (diffuse times texture) and longer chains through the spare
 * registers -- so a fixed MODULATE(texture, diffuse) is right for only some of
 * them, and picks a texture for draws that asked for flat diffuse.
 *
 * Returns a D3DTA_* value, or -1 when the input is not something a fixed
 * function stage can express.  *is_one is set when the input is the constant
 * one -- zero through an inverting mapping -- which turns a product into a
 * plain select.
 */
/*
 * Hand the title's register-combiner programming to the D3D8 layer's
 * translator.
 *
 * d3d8_combiners.c already implements the whole thing -- eight stages of RGB
 * and alpha math over textures, vertex colours and constants, a final
 * combiner, generated as HLSL and cached by configuration.  It was never
 * reached, because the only path that feeds it is SetPixelShader and this
 * title programs the combiners through the push buffer instead.  So the
 * fixed-function approximations here were competing with a complete
 * implementation sitting unused in the same build.
 *
 * The translator reads its inputs out of the device's render-state array at
 * the D3DRS_PS* indices, and those slots hold exactly the register values
 * pgraph already captures -- the input and output control words per stage, the
 * two final-combiner words, the per-stage constants.  So this publishes them
 * and sets a shader token; d3d8_combiners_prepare_draw(), which the D3D8 draw
 * calls already invoke, compiles and binds the result.
 *
 * Republished only when the programming changes.  Fifty SetRenderState calls
 * on every draw is the shape of thing that stopped the window responding when
 * the extra texture stages were bound unconditionally.
 */
static uint32_t pgraph_bswap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static void pgraph_publish_combiners(IDirect3DDevice8 *dev)
{
    static int have_last;
    static uint32_t last[NV2A_COMBINER_SNAPSHOT_WORDS];
    uint32_t now[NV2A_COMBINER_SNAPSHOT_WORDS];
    unsigned stages, i, n = 0u;
    uint32_t token;

    /* The guest combiner is required by normal scene rendering. An explicit
     * CONKER_RC_COMBINERS=0 retains the diagnostic fallback. */
    if (!conker_runtime_options()->register_combiners || dev == NULL)
        return;

    for (i = 0u; i < 8u; ++i) {
        now[n++] = g_pg.combiner_color_icw[i];
        now[n++] = g_pg.combiner_alpha_icw[i];
        now[n++] = g_pg.combiner_color_ocw[i];
        now[n++] = g_pg.combiner_alpha_ocw[i];
        now[n++] = g_pg.combiner_factor0[i];
        now[n++] = g_pg.combiner_factor1[i];
    }
    now[n++] = g_pg.combiner_control;
    now[n++] = g_pg.combiner_final0;
    now[n++] = g_pg.combiner_final1;
    now[n++] = g_pg.shader_stage_program;
    now[n++] = (g_pg.tex[0].enabled ? 1u : 0u) |
               (g_pg.tex[1].enabled ? 2u : 0u) |
               (g_pg.tex[2].enabled ? 4u : 0u) |
               (g_pg.tex[3].enabled ? 8u : 0u);
    now[n++] = g_pg.shader_other_stage_input;
    for (i = 0; i < 4u; ++i) {
        now[n++] = g_pg.tex[i].format;
        for (unsigned j = 0; j < 6u; ++j) now[n++] = g_pg.bump_env[i][j];
    }

    if (have_last && memcmp(now, last, sizeof(now)) == 0)
        return;
    memcpy(last, now, sizeof(now));
    have_last = 1;
    dev->lpVtbl->SetRenderState(dev, D3DRS_PSINPUTTEXTURE, g_pg.shader_other_stage_input);
    for (i = 0; i < 4u; ++i) {
        for (unsigned j = 0; j < 4u; ++j)
            dev->lpVtbl->SetTextureStageState(dev, i, D3DTSS_BUMPENVMAT00 + j, g_pg.bump_env[i][j]);
        dev->lpVtbl->SetTextureStageState(dev, i, D3DTSS_BUMPENVLSCALE, g_pg.bump_env[i][4]);
        dev->lpVtbl->SetTextureStageState(dev, i, D3DTSS_BUMPENVLOFFSET, g_pg.bump_env[i][5]);
    }

    for (i = 0u; i < 8u; ++i) {
        /* Input words reverse for the same reason the final ones do:
         * parse_four_inputs() takes A from the low byte, NV2A puts it in the
         * high byte.  Verified against the running title -- C4C80000 is
         * A=diffuse, B=texture0, a modulate, which is what the loading screen
         * draws.  Read the other way round it is A=0, B=0, and the stage
         * computes nothing, which is the black.
         *
         * The output words are not reversed: those are a bitfield in the low
         * eighteen bits and already line up, 00000C00 giving sum_dst = spare0. */
        dev->lpVtbl->SetRenderState(dev, D3DRS_PSRGBINPUTS0 + i,
                                    pgraph_bswap32(g_pg.combiner_color_icw[i]));
        dev->lpVtbl->SetRenderState(dev, D3DRS_PSALPHAINPUTS0 + i,
                                    pgraph_bswap32(g_pg.combiner_alpha_icw[i]));
        dev->lpVtbl->SetRenderState(dev, D3DRS_PSRGBOUTPUTS0 + i,
                                    g_pg.combiner_color_ocw[i]);
        dev->lpVtbl->SetRenderState(dev, D3DRS_PSALPHAOUTPUTS0 + i,
                                    g_pg.combiner_alpha_ocw[i]);
        dev->lpVtbl->SetRenderState(dev, D3DRS_PSCONSTANT0_0 + i,
                                    g_pg.combiner_factor0[i]);
        dev->lpVtbl->SetRenderState(dev, D3DRS_PSCONSTANT1_0 + i,
                                    g_pg.combiner_factor1[i]);
    }
    /* The final combiner words are byte-reversed relative to what the D3D8
     * render states carry.
     *
     * NV2A packs SET_COMBINER_SPECULAR_FOG_CW0 as A in the high byte down to D
     * in the low byte; D3DRS_PSFINALCOMBINERINPUTSABCD is the other way round,
     * A in [7:0].  Published verbatim, the loading screen's 0000000C reads as
     * A=0x0C with B, C and D zero, and the final combiner -- A*B + (1-A)*C + D
     * -- evaluates to zero.  That is the black screen.
     *
     * Reversed it reads as D=0x0C, spare0, "output what the stages computed",
     * which is right.  Confirmed against hardware: the tavern's 130C0300
     * reverses to A=0x13 fog, B=0x0C spare0, C=0x03 fog colour, D=0 --
     * fog*spare0 + (1-fog)*fogcolour, the ordinary fog blend.
     *
     * CW1 reverses the same way: E, F, G descend from the high byte with flags
     * in the low one, and the reversed word puts G at [23:16] and E at [7:0]
     * where the parser wants them, leaving the flag byte in the [31:24] field
     * it ignores. */
    dev->lpVtbl->SetRenderState(dev, D3DRS_PSFINALCOMBINERINPUTSABCD,
                                pgraph_bswap32(g_pg.combiner_final0));
    dev->lpVtbl->SetRenderState(dev, D3DRS_PSFINALCOMBINERINPUTSEFG,
                                pgraph_bswap32(g_pg.combiner_final1));
    dev->lpVtbl->SetRenderState(dev, D3DRS_PSCOMBINERCOUNT,
                                g_pg.combiner_control);

    /* The stage count is the field itself, not one more than it.  Hardware
     * shows control=...01 with a single non-zero stage and control=...03 with
     * three, so the earlier "+ 1" processed one empty stage too many. */
    stages = g_pg.combiner_control & 0xFu;
    if (stages < 1u) stages = 1u;
    if (stages > 8u) stages = 8u;

    /* Texture mode per stage, four bits each from bit 8.
     *
     * The enum is NV2A_TEXMODE_2D = 0, _3D = 1, _CUBEMAP = 2, _NONE = 3, so an
     * enabled 2D stage leaves its nibble at zero and only a disabled one needs
     * writing.  Setting an enabled stage to 1 -- as this did, on the strength
     * of an assumption rather than the header -- declares the sampler as a
     * Texture3D, and the generated shader then samples it with a float3 while
     * a 2D texture is bound: every fetch comes back black.
     *
     * PROJECT3D (2) on an ordinary 2D resource also projects XY by Q;
     * resource dimensionality, not the shader mode name, selects the sampler.
     * CUBEMAP (3) uses all six faces and the full XYZ reflection vector.
     * Other enabled modes retain the existing 2D fallback until implemented. */
    token = stages;
    for (i = 0u; i < 4u; ++i) {
        if (!g_pg.tex[i].enabled)
            token |= (uint32_t)NV2A_TEXMODE_NONE << (8u + i * 4u);
        else if (i > 0u && ((g_pg.shader_stage_program >> (5u * i)) & 31u) == 6u)
            token |= (uint32_t)NV2A_TEXMODE_BUMPENV << (8u + i * 4u);
        else if (i > 0u && ((g_pg.shader_stage_program >> (5u * i)) & 31u) == 7u)
            token |= (uint32_t)NV2A_TEXMODE_BUMPENV_LUM << (8u + i * 4u);
        else if (((g_pg.shader_stage_program >> (5u * i)) & 31u) == 3u &&
                 (g_pg.tex[i].format & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE))
            token |= (uint32_t)NV2A_TEXMODE_CUBEMAP << (8u + i * 4u);
        else if (((g_pg.shader_stage_program >> (5u * i)) & 31u) == 1u ||
                 (((g_pg.shader_stage_program >> (5u * i)) & 31u) == 2u &&
                  (g_pg.tex[i].format & NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY) == 0x20u &&
                  !(g_pg.tex[i].format & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE)))
            token |= (uint32_t)NV2A_TEXMODE_PROJECT2D << (8u + i * 4u);
    }

    dev->lpVtbl->SetRenderState(dev, D3DRS_PSTEXTUREMODES,
                                (token >> 8) & 0xFFFFu);
    d3d8_combiners_set_pixel_shader(token);

    { static unsigned logged;
      if (logged++ < 8u)
          fprintf(stderr, "[INFO PGRAPH-RC] published token=%08X stages=%u\n",
                  token, stages); }
}

static int pgraph_rc_input_to_d3dta(uint8_t input, int *is_one)
{
    unsigned reg = input & 0x0Fu;
    unsigned map = (input >> 4) & 0x0Fu;

    *is_one = 0;
    switch (reg) {
    case 0x0u:                      /* ZERO, or one when inverted */
        *is_one = (map == 0x1u || map == 0x2u);
        return -1;
    case 0x1u: case 0x2u: return 3; /* the combiner constants -> TFACTOR */
    case 0x4u:            return 0; /* primary   -> DIFFUSE  */
    case 0x5u:            return 4; /* secondary -> SPECULAR */
    case 0x8u: case 0x9u:
    case 0xAu: case 0xBu: return 2; /* texture0..3 -> TEXTURE */
    case 0xCu: case 0xDu: return 1; /* spare0/1    -> CURRENT */
    default:              return -1;
    }
}

/*
 * Configure stage 0 from the first combiner stage.
 *
 * Only the shapes a fixed-function stage can actually represent are taken --
 * a product of two inputs, or a single input.  Anything longer stays on the
 * previous behaviour rather than being approximated into something wrong.
 * Returns non-zero when it configured the stage.
 */
static int pgraph_stage0_from_combiner(IDirect3DDevice8 *dev, int have_texture)
{
    uint32_t icw = g_pg.combiner_color_icw[0];
    int a_one = 0, b_one = 0;
    int a = pgraph_rc_input_to_d3dta((uint8_t)(icw >> 24), &a_one);
    int b = pgraph_rc_input_to_d3dta((uint8_t)(icw >> 16), &b_one);

    if (icw == 0u)
        return 0;
    /* A texture argument with no texture bound would sample nothing. */
    if ((a == 2 || b == 2) && !have_texture)
        return 0;

    if (b_one && a >= 0) {
        dev->lpVtbl->SetTextureStageState(dev, 0, 1, 2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 2, (DWORD)a);
        dev->lpVtbl->SetTextureStageState(dev, 0, 4, 2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 5, (DWORD)a);
        return 1;
    }
    if (a_one && b >= 0) {
        dev->lpVtbl->SetTextureStageState(dev, 0, 1, 2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 2, (DWORD)b);
        dev->lpVtbl->SetTextureStageState(dev, 0, 4, 2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 5, (DWORD)b);
        return 1;
    }
    if (a >= 0 && b >= 0) {
        dev->lpVtbl->SetTextureStageState(dev, 0, 1, 4 /*MODULATE*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 2, (DWORD)a);
        dev->lpVtbl->SetTextureStageState(dev, 0, 3, (DWORD)b);
        dev->lpVtbl->SetTextureStageState(dev, 0, 4, 4 /*MODULATE*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 5, (DWORD)a);
        dev->lpVtbl->SetTextureStageState(dev, 0, 6, (DWORD)b);
        return 1;
    }
    return 0;
}

static void pgraph_report_combiners(void)
{
    static uint32_t seen[16];
    static int done;
    if (done) return;
    static unsigned seen_count;
    unsigned stages = (g_pg.combiner_control & 0xFu) + 1u;
    uint32_t key = g_pg.combiner_control ^ g_pg.combiner_color_icw[0] ^
                   (g_pg.combiner_color_ocw[0] << 1) ^
                   (g_pg.combiner_color_icw[1] << 2);
    unsigned i, k;

    for (k = 0u; k < seen_count; ++k)
        if (seen[k] == key)
            return;
    if (seen_count >= 16u) {
        done = 1;
        return;
    }
    seen[seen_count++] = key;

    if (stages > 8u)
        stages = 8u;
    fprintf(stderr, "[INFO PGRAPH-RC] control=%08X stages=%u final=%08X,%08X\n",
            g_pg.combiner_control, stages, g_pg.combiner_final0,
            g_pg.combiner_final1);
    for (i = 0u; i < stages; ++i) {
        uint32_t c = g_pg.combiner_color_icw[i];
        fprintf(stderr,
                "[INFO PGRAPH-RC]   s%u colour icw=%08X ocw=%08X  "
                "A=r%u/%X B=r%u/%X C=r%u/%X D=r%u/%X  alpha icw=%08X ocw=%08X c0=%08X c1=%08X\n",
                i, c, g_pg.combiner_color_ocw[i],
                (c >> 28) & 0xFu, (c >> 24) & 0xFu,
                (c >> 20) & 0xFu, (c >> 16) & 0xFu,
                (c >> 12) & 0xFu, (c >> 8) & 0xFu,
                (c >> 4) & 0xFu, c & 0xFu,
                g_pg.combiner_alpha_icw[i], g_pg.combiner_alpha_ocw[i],
                g_pg.combiner_factor0[i], g_pg.combiner_factor1[i]);
    }
}

static unsigned pgraph_bind_extra_stages(IDirect3DDevice8 *dev)
{
    /* The programmable path uses all four guest texture stages by default.
     * Texture lookup owns coherence; bindings are restored
     * every draw because native render-target hazards can detach them. */
    if (!conker_runtime_options()->multistage_textures)
        return 0;

    /* What each stage was left holding, so a draw that changes nothing costs
     * nothing.  Setting all nine states on three stages for every draw was
     * roughly a million redundant calls a run, each of which can rebuild the
     * pixel constant buffer underneath -- enough to make the window stop
     * responding even though the draws themselves were fine. */
    static int last_enabled[4];
    unsigned stage, bound = 0u;

    for (stage = 1u; stage < 4u; ++stage) {
        UINT w = 0u, h = 0u;
        int texel_coords = 0;
        IDirect3DTexture8 *tex;
        /* Resolve every draw: enable/size/content can change without changing
         * offset or format. Render-target hazards can also detach a native
         * SRV while the guest texture registers remain unchanged. */
        tex = pgraph_texture_for_stage(dev, stage, &w, &h, &texel_coords);
        dev->lpVtbl->SetTexture(dev, stage, (IDirect3DBaseTexture8 *)tex);
        /* All linear stages use texel coordinates, not only stage zero.
         * The glow pass's second input spans 0..639/479; leaving that as
         * normalized UV clamps almost the entire pass to one edge pixel.
         * A matrix works for both gathered and native array vertices. */
        if (tex != NULL && texel_coords && w && h) {
            D3DMATRIX m = {0};
            m._11 = 1.0f / (float)w; m._22 = 1.0f / (float)h;
            m._33 = m._44 = 1.0f;
            dev->lpVtbl->SetTransform(dev, (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + stage), &m);
            dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        } else {
            dev->lpVtbl->SetTextureStageState(dev, stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        }

        if (tex == NULL) {
            if (last_enabled[stage]) {
                dev->lpVtbl->SetTextureStageState(dev, stage, 1, 1 /*DISABLE*/);
                dev->lpVtbl->SetTextureStageState(dev, stage, 4, 1 /*DISABLE*/);
                last_enabled[stage] = 0;
            }
            continue;
        }

        if (!last_enabled[stage]) {
            dev->lpVtbl->SetTextureStageState(dev, stage, 1, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, stage, 2, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, stage, 3, 1 /*CURRENT*/);
            dev->lpVtbl->SetTextureStageState(dev, stage, 4, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, stage, 5, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, stage, 6, 1 /*CURRENT*/);
            dev->lpVtbl->SetTextureStageState(dev, stage, 13, 3 /*CLAMP*/);
            dev->lpVtbl->SetTextureStageState(dev, stage, 14, 3 /*CLAMP*/);
            last_enabled[stage] = 1;
        }
        ++bound;
    }

    { static unsigned logged; static unsigned seen[4];
      if (bound < 4u) ++seen[bound];
      if ((logged++ % 20000u) == 0u)
          fprintf(stderr, "[INFO PGRAPH-STAGES] extra stages bound: "
                  "0:%u 1:%u 2:%u 3:%u\n", seen[0], seen[1], seen[2], seen[3]); }
    return bound;
}

/* Complete pixel/render state consumed by one draw, printed without any
 * GPU readback.  The XMV quad has correct geometry, correct UVs and white
 * diffuse, samples a texture with content, and still leaves the target
 * black -- so the answer has to be in the state below.  Every draw in the
 * CONKER_SEQ_FRAME window is dumped, which puts the video draw and the
 * visibly-correct UI draws side by side for comparison. */
static void pgraph_dump_pixel_state(unsigned seq, int have_texture,
                                    uint32_t diffuse)
{
    uint32_t icw = g_pg.combiner_color_icw[0];
    uint32_t aicw = g_pg.combiner_alpha_icw[0];
    int a_one = 0, b_one = 0;
    int a = pgraph_rc_input_to_d3dta((uint8_t)(icw >> 24), &a_one);
    int b = pgraph_rc_input_to_d3dta((uint8_t)(icw >> 16), &b_one);
    uint32_t color = (g_pg.tex[0].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    Nv2aSourceFormat sf = nv2a_texture_source_format(color);
    const char *decision;

    if (icw == 0u) decision = "icw==0 -> stage0 NOT programmed";
    else if ((a == 2 || b == 2) && !have_texture)
        decision = "texture arg with no texture -> NOT programmed";
    else if (b_one && a >= 0) decision = "SELECTARG1(a)";
    else if (a_one && b >= 0) decision = "SELECTARG1(b)";
    else if (a >= 0 && b >= 0) decision = "MODULATE(a,b)";
    else decision = "unmapped inputs -> stage0 NOT programmed";

    fprintf(stderr,
        "[PXS] seq %u  rt=%08X tex=%08X have_tex=%d diffuse=%08X" "\n"
        "[PXS]   tex fmt=%08X colour=0x%02X src=%d yuv=%d ctl0=%08X rect=%08X en=%d" "\n"
        "[PXS]   combiner_control=%08X final0=%08X final1=%08X" "\n"
        "[PXS]   colour icw[0]=%08X ocw[0]=%08X  A=%02X B=%02X C=%02X D=%02X" "\n"
        "[PXS]   alpha  icw[0]=%08X ocw[0]=%08X  A=%02X B=%02X C=%02X D=%02X" "\n"
        "[PXS]   colour icw[1]=%08X ocw[1]=%08X  alpha icw[1]=%08X" "\n"
        "[PXS]   factor0[0]=%08X factor1[0]=%08X  stage0 -> %s (a=%d b=%d a1=%d b1=%d)" "\n"
        "[PXS]   blend=%d src=%04X dst=%04X  alpha_test=%d  colour_mask=%08X" "\n"
        "[PXS]   depth_test=%d cull=%d specular=%u shader_prog=%08X" "\n",
        seq, g_pg.surface_color_offset, g_pg.tex[0].offset & 0x03FFFFFFu,
        have_texture, diffuse,
        g_pg.tex[0].format, color, (int)sf, nv2a_source_is_yuv(sf),
        g_pg.tex[0].control0, g_pg.tex[0].image_rect, g_pg.tex[0].enabled,
        g_pg.combiner_control, g_pg.combiner_final0, g_pg.combiner_final1,
        icw, g_pg.combiner_color_ocw[0],
        (icw >> 24) & 0xFFu, (icw >> 16) & 0xFFu,
        (icw >> 8) & 0xFFu, icw & 0xFFu,
        aicw, g_pg.combiner_alpha_ocw[0],
        (aicw >> 24) & 0xFFu, (aicw >> 16) & 0xFFu,
        (aicw >> 8) & 0xFFu, aicw & 0xFFu,
        g_pg.combiner_color_icw[1], g_pg.combiner_color_ocw[1],
        g_pg.combiner_alpha_icw[1],
        g_pg.combiner_factor0[0], g_pg.combiner_factor1[0],
        decision, a, b, a_one, b_one,
        g_pg.blend_enable, (unsigned)g_pg.blend_sfactor,
        (unsigned)g_pg.blend_dfactor, g_pg.alpha_test, g_pg.color_mask,
        g_pg.depth_test, g_pg.cull_enable, g_pg.specular_enable,
        g_pg.shader_stage_program);
    fflush(stderr);
}
/* Inline vertices arrive already in screen space, including Z: the title has
 * applied the NV2A viewport transform, whose Z half maps NDC [0,1] onto the
 * depth buffer's range via vp_scale[2] / vp_offset[2].  For a D24 target that
 * scale is 16777215, so a perfectly ordinary Z of 10.0 means 10 depth units,
 * not 10 in clip space.
 *
 * The pre-transformed vertex path converts X and Y back to NDC but passed Z
 * through untouched and forced w = 1, so the vertex reached the rasterizer at
 * z=10, w=1.  With DepthClipEnable the whole primitive falls outside
 * 0 <= z <= w and is discarded -- which is why the XMV quad drew nothing while
 * its render target, texture, pixel state and geometry were all provably
 * correct.  Draws carrying z=1.0 survived only by landing exactly on the far
 * plane.
 *
 * Undo the viewport Z transform here, where the rest of the vertex is decoded,
 * so the shader receives the NDC depth it expects. */
static float pgraph_ndc_z(float screen_z)
{
    float scale = g_pg.vp_scale[2];
    if (scale == 0.0f)
        return 0.0f;
    return (screen_z - g_pg.vp_offset[2]) / scale;
}

/* ---- temporary: per-draw census of the frontend ----------------------- */
static double dt_now(void)
{
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (f.QuadPart == 0) { QueryPerformanceFrequency(&f);
                           QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

static int dt_open(void)
{
    static int initialized, enabled;
    static double after;
    if (!initialized) {
        const char *e = getenv("CONKER_DRAW_AFTER");
        enabled = e && *e;
        after = enabled ? atof(e) : 0.0;
        initialized = 1;
    }
    return enabled && dt_now() >= after;
}

static unsigned g_dt_batches;          /* batches traced so far */
static unsigned g_dt_seq;              /* draw sequence in this batch */
static unsigned g_dt_draws, g_dt_textured, g_dt_untextured;
static unsigned g_dt_rej_empty, g_dt_rej_short, g_dt_rej_target;
static unsigned g_dt_arr, g_dt_arr_ok, g_dt_arr_fail;
/* Which constant slots the title ever writes, and with which load bases.
 * The program reads c58/c59/c148/c149 and finds them zero, so the question
 * is whether they are never written or written somewhere else. */
static unsigned g_kc_writes[192];
static unsigned g_kc_load_hist[256];
unsigned long long g_kc_total;
static uint32_t g_dt_tex_seen[32];
static unsigned g_dt_tex_n;

static void dt_note_tex(uint32_t off)
{
    unsigned i;
    if (!off) return;
    for (i = 0; i < g_dt_tex_n; ++i) if (g_dt_tex_seen[i] == off) return;
    if (g_dt_tex_n < 32u) g_dt_tex_seen[g_dt_tex_n++] = off;
}

/* One line per draw, plus the first vertices of the first few, so a draw can
 * be judged by what it actually puts on screen rather than by whether it
 * happened. */
static void dt_log_draw(uint32_t num_verts, const uint32_t *src)
{
    unsigned i, n;
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;

    /* The program image as it stands AT THE DRAW, which is what actually
     * runs -- upload-time logging only ever caught individual fragments. */
    {   static unsigned dumped;
        if (dt_open() && dumped < 1u && g_pg.transform_program_count > 20u) {
            unsigned s;
            dumped = 1;
            fprintf(stderr, "[VPIMG] %u slots, start=%u mode=%08X\n",
                    g_pg.transform_program_count,
                    g_pg.transform_program_start,
                    g_pg.transform_execution_mode);
            for (s = 0; s < g_pg.transform_program_count && s < 136u; ++s)
                fprintf(stderr, "[VPIMG] %3u %08X %08X %08X %08X\n", s,
                        g_pg.transform_program[s][0],
                        g_pg.transform_program[s][1],
                        g_pg.transform_program[s][2],
                        g_pg.transform_program[s][3]);
            fflush(stderr);
        }
    }
    ++g_dt_draws;
    if (g_pg.tex[0].offset) ++g_dt_textured; else ++g_dt_untextured;
    dt_note_tex(g_pg.tex[0].offset & 0x03FFFFFFu);
    if (!dt_open() || g_dt_batches >= 600u) return;

    for (i = 0; i < num_verts && i < 64u; ++i) {
        const float *v = (const float *)(src + i * g_pg.vert_stride);
        if (v[0] < minx) minx = v[0];
        if (v[0] > maxx) maxx = v[0];
        if (v[1] < miny) miny = v[1];
        if (v[1] > maxy) maxy = v[1];
    }
    fprintf(stderr,
            "[DRAW] b%u #%u mode=%u verts=%u stride=%u  rt=%08X zeta=%08X  clip=%08X/%08X\n"
            "[DRAW]     vp off=(%.1f,%.1f,%.1f) scale=(%.1f,%.1f,%.1f)  prog=%u/%u start=%u\n"
            "[DRAW]     tex0=%08X tex1=%08X tex2=%08X tex3=%08X\n"
            "[DRAW]     blend=%d(%04X/%04X) depth=%d alpha=%d cull=%d  mask=%08X\n"
            "[DRAW]     screen bbox x[%.1f..%.1f] y[%.1f..%.1f]\n",
            g_dt_batches, ++g_dt_seq, g_pg.draw_mode, num_verts,
            g_pg.vert_stride, g_pg.surface_color_offset,
            g_pg.surface_zeta_offset, g_pg.surface_clip_h, g_pg.surface_clip_v,
            g_pg.vp_offset[0], g_pg.vp_offset[1], g_pg.vp_offset[2],
            g_pg.vp_scale[0], g_pg.vp_scale[1], g_pg.vp_scale[2],
            g_pg.transform_program_count, g_pg.transform_execution_mode,
            g_pg.transform_program_start,
            g_pg.tex[0].offset, g_pg.tex[1].offset, g_pg.tex[2].offset,
            g_pg.tex[3].offset,
            g_pg.blend_enable, (unsigned)g_pg.blend_sfactor,
            (unsigned)g_pg.blend_dfactor, g_pg.depth_test, g_pg.alpha_test,
            g_pg.cull_enable, g_pg.color_mask,
            minx, maxx, miny, maxy);
    if (g_dt_seq <= 4u) {
        n = (num_verts < 16u) ? num_verts : 16u;
        for (i = 0; i < n; ++i) {
            const uint32_t *v = src + i * g_pg.vert_stride;
            const float *f = (const float *)v;
            fprintf(stderr, "[DRAW]       v%-2u x=%9.2f y=%9.2f z=%9.3f  raw=%08X %08X %08X %08X\n",
                    i, f[0], f[1], g_pg.vert_stride > 2u ? f[2] : 0.0f,
                    v[0], v[1],
                    g_pg.vert_stride > 2u ? v[2] : 0u,
                    g_pg.vert_stride > 3u ? v[3] : 0u);
        }
    }
    fflush(stderr);
}

/* The array path is where the frontend actually draws: it binds vertex
 * streams and issues DRAW_ARRAYS rather than pushing inline vertices. */
static void dt_log_array(uint32_t start_vertex, uint32_t vertex_count,
                         uint32_t prim_count, uint32_t hr)
{
    uint32_t addr = g_pg.array_offset[0] & 0x03FFFFFFu;
    uint32_t fmt  = g_pg.array_format[0];
    uint32_t stride = (fmt >> 8) & 0xFFu;
    unsigned i;

    ++g_dt_arr;
    if (hr == 0u) ++g_dt_arr_ok; else ++g_dt_arr_fail;
    dt_note_tex(g_pg.tex[0].offset & 0x03FFFFFFu);
    if (!dt_open() || g_dt_batches >= 600u) return;

    fprintf(stderr,
            "[ADRAW] b%u #%u mode=%u start=%u verts=%u prims=%u hr=%08X\n"
            "[ADRAW]     stream0 base=%08X format=%08X stride=%u  rt=%08X clip=%08X/%08X\n"
            "[ADRAW]     vp off=(%.1f,%.1f) scale=(%.1f,%.1f)  prog=%u start=%u mode=%08X\n"
            "[ADRAW]     tex0=%08X tex1=%08X tex2=%08X tex3=%08X  blend=%d depth=%d alpha=%d cull=%d mask=%08X\n",
            g_dt_batches, ++g_dt_seq, g_pg.draw_mode, start_vertex,
            vertex_count, prim_count, hr,
            addr, fmt, stride, g_pg.surface_color_offset,
            g_pg.surface_clip_h, g_pg.surface_clip_v,
            g_pg.vp_offset[0], g_pg.vp_offset[1],
            g_pg.vp_scale[0], g_pg.vp_scale[1],
            g_pg.transform_program_count, g_pg.transform_program_start,
            g_pg.transform_execution_mode,
            g_pg.tex[0].offset, g_pg.tex[1].offset, g_pg.tex[2].offset,
            g_pg.tex[3].offset, g_pg.blend_enable, g_pg.depth_test,
            g_pg.alpha_test, g_pg.cull_enable, g_pg.color_mask);
    if (g_dt_seq <= 6u && addr >= 0x1000u && addr < 0x04000000u &&
        stride >= 4u) {
        const uint8_t *base = (const uint8_t *)(uintptr_t)
            ((uintptr_t)addr + (uintptr_t)g_xbox_mem_offset);
        for (i = 0; i < vertex_count && i < 16u; ++i) {
            const uint32_t *v = (const uint32_t *)
                (base + (size_t)(start_vertex + i) * stride);
            const float *f = (const float *)v;
            fprintf(stderr, "[ADRAW]       v%-2u %9.2f %9.2f %9.3f  raw=%08X %08X %08X %08X\n",
                    i, f[0], f[1], f[2], v[0], v[1], v[2], v[3]);
        }
    }
    fflush(stderr);
}

/* What the frontend batches contain, if not draws. */
static unsigned g_dt_mhist[0x2000/4];
void nv2a_draw_census_method(uint32_t method)
{
    if (!dt_open() || g_dt_batches >= 600u) return;
    if (method < 0x2000u) ++g_dt_mhist[method >> 2];
}

static void dt_dump_hist(void)
{
    unsigned i, printed = 0;
    unsigned best;
    static unsigned done;
    if (done) return;
    done = 1;
    fprintf(stderr, "[MHIST] most common methods in the frontend window:\n");
    for (printed = 0; printed < 24u; ++printed) {
        unsigned bi = 0; best = 0;
        for (i = 0; i < 0x2000u/4u; ++i)
            if (g_dt_mhist[i] > best) { best = g_dt_mhist[i]; bi = i; }
        if (!best) break;
        fprintf(stderr, "[MHIST]   %04X  x%u\n", bi << 2, best);
        g_dt_mhist[bi] = 0;
    }
    fflush(stderr);
}

void nv2a_kc_report(void)
{
    unsigned i, printed = 0;
    fprintf(stderr, "[KC] %llu constant writes; CONSTANT_LOAD bases used:\n", g_kc_total);
    for (i = 0; i < 256u; ++i)
        if (g_kc_load_hist[i] && printed++ < 20u)
            fprintf(stderr, "[KC]   load=%3u x%u\n", i,
                    g_kc_load_hist[i]);
    fprintf(stderr, "[KC] slots written (index xcount):\n");
    printed = 0;
    for (i = 0; i < 192u; ++i)
        if (g_kc_writes[i] && printed++ < 40u)
            fprintf(stderr, "[KC]   c%-3u x%u\n", i, g_kc_writes[i]);
    { extern void recomp_c146_report(void); recomp_c146_report(); }
    { extern void recomp_c146e_report(void); recomp_c146e_report(); }
    { extern void recomp_c146h_report(void); recomp_c146h_report(); }
    {   extern unsigned long long g_cwin_packets, g_cwin_garbage,
               g_cwin_next_is_header, g_cwin_next_not_header;
        fprintf(stderr, "[KC] constant-window packets=%llu  containing non-float garbage=%llu\n"
                "[KC] word after declared payload: header %llu / NOT header %llu\n",
                g_cwin_packets, g_cwin_garbage,
                g_cwin_next_is_header, g_cwin_next_not_header); }
    fprintf(stderr, "[KC] the four the program needs:  c58 x%u  c59 x%u  c148 x%u  c149 x%u\n",
            g_kc_writes[58], g_kc_writes[59], g_kc_writes[148],
            g_kc_writes[149]);
    fflush(stderr);
}

void nv2a_draw_census_frame(void)
{
    unsigned i;
    if (!dt_open() || g_dt_batches >= 600u) { g_dt_seq = 0; return; }
    if ((g_dt_batches % 100u) == 0u)
    fprintf(stderr, "[DRAWSUM] batch %u: draws=%u  array draws=%u (ok %u, failed %u)  rejected empty=%u short=%u no-target=%u  unique tex=%u\n",
            g_dt_batches, g_dt_seq, g_dt_arr, g_dt_arr_ok, g_dt_arr_fail,
            g_dt_rej_empty, g_dt_rej_short, g_dt_rej_target, g_dt_tex_n);
    for (i = 0; i < g_dt_tex_n && i < 12u; ++i)
        fprintf(stderr, "[DRAWSUM]   tex %08X\n", g_dt_tex_seen[i]);
    fflush(stderr);
    if (g_dt_batches == 599u) { dt_dump_hist(); nv2a_kc_report(); }
    ++g_dt_batches;
    if ((g_dt_batches % 100u) == 0u) g_dt_seq = 0;
    if ((g_dt_batches % 100u) == 0u) {
        g_dt_rej_empty = g_dt_rej_short = g_dt_rej_target = 0;
        g_dt_arr = g_dt_arr_ok = g_dt_arr_fail = 0;
    }
}
/* Is the title running a vertex program for this draw?
 *
 * SET_TRANSFORM_EXECUTION_MODE bits 1..0 select the mode; 2 is PROGRAM.
 * The observed value is 6, i.e. program mode with the range field set. */
static int vsh_program_active(void)
{
    if (!conker_runtime_options()->vertex_shaders) return 0;
    return (g_pg.transform_execution_mode & 3u) == 2u &&
           g_pg.transform_program_count > 0u;
}

static float vsh_attr_component(const uint32_t *vtx, unsigned attr,
                                unsigned comp)
{
    int off = g_pg.inline_attr_off[attr];
    uint32_t type, size;

    if (off < 0) return comp == 3u ? 1.0f : 0.0f;
    type = g_pg.inline_attr_type[attr];
    size = g_pg.inline_attr_size[attr];

    if (type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP) {
        /* Count describes one packed word, not one float component. CMP
         * expands to signed normalized 11:11:10 XYZ and the default W=1.
         * Decoding it after the component-count check drops Y/Z and treats
         * the packed X bits as a float, including NaNs and huge magnitudes. */
        if (comp >= 3u) return 1.0f;
        unsigned bits = comp == 2u ? 10u : 11u;
        unsigned shift = comp == 2u ? 22u : comp * 11u;
        unsigned sign = 1u << (bits - 1u);
        uint32_t field = (vtx[off] >> shift) & ((1u << bits) - 1u);
        int32_t value = (int32_t)(field ^ sign) - (int32_t)sign;
        float normalized = (float)value / (float)(sign - 1u);
        return normalized < -1.0f ? -1.0f : normalized;
    }
    if (type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D ||
        type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL) {
        uint32_t packed = vtx[off];
        unsigned b;
        /* D3D order is BGRA in memory, OGL is RGBA. */
        if (type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D)
            b = (comp == 0u) ? 2u : (comp == 2u) ? 0u : comp;
        else
            b = comp;
        return (float)((packed >> (b * 8u)) & 0xFFu) / 255.0f;
    }
    if (comp >= size) return comp == 3u ? 1.0f : 0.0f;
    if (type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1 ||
        type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K) {
        int16_t value;
        float decoded;
        memcpy(&value, (const uint8_t *)(vtx + off) + comp * 2u, sizeof(value));
        decoded = (float)value;
        if (type == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1) {
            decoded /= 32767.0f;
            if (decoded < -1.0f) decoded = -1.0f;
        }
        return decoded;
    }
    { union { uint32_t u; float f; } c; c.u = vtx[off + comp]; return c.f; }
}

/* Run the program for one source vertex and produce the screen-space vertex
 * the existing fixed-function buffer expects.
 *
 * The program emits clip space in o0; the hardware then divides by w and
 * applies the viewport scale and bias, which is what the XYZRHW vertex needs
 * to carry.  o3 is the diffuse colour and o9 texcoord 0. */
static nv2a_vsh_program g_decoded_vsh;
static nv2a_vertex_cache g_vertex_cache;
unsigned long long g_vsh_cache_hits, g_vsh_cache_misses;
static nv2a_vsh_state g_vertex_state;
static unsigned g_vertex_attributes[16], g_vertex_attribute_count;

static int vsh_gpu_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *setting = getenv("CONKER_GPU_VSH");
        enabled = !setting || setting[0] != '0';
        /* CPU-output probes must observe the reference conversion path. */
        const char *probes[] = {"CONKER_PIXEL_CAPTURE", "CONKER_VSH_CAPTURE",
            "CONKER_DRAW_AFTER", "CONKER_SEQ_AFTER", "CONKER_SEQ_FRAME",
            "CONKER_SEQ_COUNT", "CONKER_VTX_TRACE"};
        for (unsigned i=0;i<sizeof(probes)/sizeof(probes[0]);i++)
            if(getenv(probes[i]))enabled=0;
    }
    return enabled && !g_gpu_draw_failed && !pgraph_seq_active();
}

static void vsh_prepare_vertices(void)
{
    /* Constants and absent attributes are uniform for the whole draw. The
     * interpreter reads these files; only temporary/output registers change. */
    memcpy(g_vertex_state.c, g_pg.transform_constant, sizeof(g_vertex_state.c));
    nv2a_vsh_bind_constants(&g_decoded_vsh, (const float (*)[4])g_vertex_state.c);
    memset(g_vertex_state.v, 0, sizeof(g_vertex_state.v));
    g_vertex_attribute_count = 0;
    for (unsigned a = 0; a < 16u; ++a) {
        g_vertex_state.v[a][3] = 1.0f;
        if (g_pg.inline_attr_off[a] >= 0)
            g_vertex_attributes[g_vertex_attribute_count++] = a;
    }
}

static void vsh_transform_vertex(const uint32_t *vtx, OutputVertex *out)
{
    nv2a_vsh_state *state = &g_vertex_state;
    unsigned a, k;
    float w, inv;

    memset(state->r, 0, sizeof(state->r));
    memset(state->o, 0, sizeof(state->o));
    /* A partially written texture output retains the default homogeneous Q. */
    for (a = 9; a < 13; ++a) state->o[a][3] = 1.0f;
    for (unsigned active = 0; active < g_vertex_attribute_count; ++active) {
        a = g_vertex_attributes[active];
        for (k = 0; k < 4u; ++k)
            state->v[a][k] = vsh_attr_component(vtx, a, k);
    }

    nv2a_vsh_run_decoded(&g_decoded_vsh, state);

    /* Bounded, opt-in replay records from the failing short3 scene draw.
     * Words are stored verbatim: metadata, program, inputs, constants, outputs. */
    {
        static unsigned captured;
        static int capture_initialized;
        static const char *path;
        static uint32_t selected_texture;
        if (!capture_initialized) {
            path = getenv("CONKER_VSH_CAPTURE");
            const char *texture = getenv("CONKER_VSH_CAPTURE_TEXTURE");
            if (texture && *texture)
                selected_texture = (uint32_t)strtoul(texture, NULL, 0) & 0x03FFFFFFu;
            capture_initialized = 1;
        }
        if (path && *path && captured < 6u &&
            (selected_texture ?
             ((g_pg.tex[0].offset & 0x03FFFFFFu) == selected_texture &&
              g_pg.transform_program_count > 20u && g_pg.inline_count == 28u) :
             (dt_open() && g_pg.array_format[0] == 0x00001235u &&
              (g_pg.tex[0].offset & 0x03FFFFFFu) == 0x03601180u))) {
            FILE *capture = fopen(path, captured ? "ab" : "wb");
            if (capture) {
                uint32_t header[8] = {0x56534831u, g_pg.stats.frames,
                    g_pg.transform_program_count, g_pg.transform_program_start,
                    g_pg.surface_color_offset, g_pg.tex[0].offset,
                    g_pg.transform_execution_mode, captured};
                fwrite(header, sizeof(header), 1, capture);
                fwrite(g_pg.transform_program, 16u, g_pg.transform_program_count, capture);
                fwrite(state->v, sizeof(state->v), 1, capture);
                fwrite(state->c, sizeof(state->c), 1, capture);
                fwrite(state->o, sizeof(state->o), 1, capture);
                fclose(capture);
                ++captured;
                fprintf(stderr, "[VSH-REPLAY] record=%u frame=%u start=%u slots=%u\n",
                        captured, g_pg.stats.frames, g_pg.transform_program_start,
                        g_pg.transform_program_count);
            }
        }
    }

    /* In program mode the viewport transform is part of the program, not a
     * later hardware stage: slot 12 multiplies by RCC (the reciprocal of w)
     * and slot 13 applies scale and bias, both writing o0.xyz.  So o0 is
     * already screen space and applying vp_scale/vp_offset again halves and
     * shifts every draw into one quadrant, which is exactly what the first
     * attempt did.  w is left as the clip w from the DP4, so it still
     * provides rhw for perspective-correct interpolation. */
    w = state->o[0][3];
    inv = (w != 0.0f) ? 1.0f / w : 1.0f;
    out->x = state->o[0][0];
    out->y = state->o[0][1];
    out->z = state->o[0][2];
    /* Preserve out-of-range depth for homogeneous clipping by D3D11.
     * Clamping here made behind-camera and far-plane geometry visible. */
    if (g_pg.vp_scale[2] > 1.0f)
        out->z /= g_pg.vp_scale[2];
    out->rhw = inv;

    {   static unsigned shown;
        int glyph = ((g_pg.tex[0].offset & 0x03FFFFFFu) == 0x0360F000u);
        if (shown < 8u && dt_open() && glyph) {
            ++shown;
            { unsigned q, nz = 0;
              for (q = 0; q < 192u; ++q)
                  if (g_pg.transform_constant[q][0] |
                      g_pg.transform_constant[q][1] |
                      g_pg.transform_constant[q][2] |
                      g_pg.transform_constant[q][3]) ++nz;
              { unsigned first = 999, last = 0;
                for (q = 0; q < 192u; ++q)
                    if (g_pg.transform_constant[q][0] |
                        g_pg.transform_constant[q][1] |
                        g_pg.transform_constant[q][2] |
                        g_pg.transform_constant[q][3]) {
                        if (first == 999u) first = q; last = q; }
                fprintf(stderr, "[VSHC] non-zero range c%u..c%u  load=%u\n",
                        first, last, g_pg.transform_constant_load); }
              fprintf(stderr, "[VSHC] non-zero consts=%u  c58=(%.3f,%.3f,%.3f,%.3f) c59=(%.3f,%.3f,%.3f,%.3f)\n"
                      "[VSHC] c148=(%.3f,%.3f,%.3f,%.3f) c149=(%.3f,%.3f,%.3f,%.3f)\n",
                      nz, state->c[58][0], state->c[58][1], state->c[58][2], state->c[58][3],
                      state->c[59][0], state->c[59][1], state->c[59][2], state->c[59][3],
                      state->c[148][0], state->c[148][1], state->c[148][2], state->c[148][3],
                      state->c[149][0], state->c[149][1], state->c[149][2], state->c[149][3]); }
            fprintf(stderr, "[VSHV] v0=(%.3f,%.3f,%.3f,%.3f) -> o0=(%.2f,%.2f,%.4f,%.4f)  screen=(%.1f,%.1f) z=%.4f rhw=%.5f  o9=(%.3f,%.3f)\n",
                    state->v[0][0], state->v[0][1], state->v[0][2], state->v[0][3],
                    state->o[0][0], state->o[0][1], state->o[0][2], state->o[0][3],
                    out->x, out->y, out->z, out->rhw,
                    state->o[9][0], state->o[9][1]);
            fflush(stderr);
        } }

    {   float r = state->o[3][0], g = state->o[3][1], b = state->o[3][2],
              al = state->o[3][3];
        unsigned R, G, B, A;
        /* A shader can derive alpha from constants without a v3 input.
         * Preserve its output, including zero at the end of a fade. */
        R = (unsigned)(r < 0 ? 0 : r > 1 ? 255 : r * 255.0f + 0.5f);
        G = (unsigned)(g < 0 ? 0 : g > 1 ? 255 : g * 255.0f + 0.5f);
        B = (unsigned)(b < 0 ? 0 : b > 1 ? 255 : b * 255.0f + 0.5f);
        A = (unsigned)(al < 0 ? 0 : al > 1 ? 255 : al * 255.0f + 0.5f);
        out->color = (A << 24) | (R << 16) | (G << 8) | B; }
    out->specular = 0;
    for (a = 0; a < 4; ++a) {
        static const unsigned shift[4] = {16, 8, 0, 24};
        float value = state->o[4][a];
        unsigned channel = value < 0 ? 0 : value > 1 ? 255 : (unsigned)(value * 255.0f + 0.5f);
        out->specular |= channel << shift[a];
    }
    out->u = state->o[9][0];
    out->v = state->o[9][1];
    out->r = state->o[9][2];
    out->q = state->o[9][3];
    for (a = 0; a < 3u; ++a) {
        memcpy(out->extra_uv[a], state->o[10u + a], sizeof(out->extra_uv[a]));
    }
    out->fog = nv2a_fog_factor(g_pg.fog_enable, g_pg.fog_mode,
                                g_pg.fog_params, state->o[5][0]);
}
/* Gather each declared array attribute into the inline layout. Keeping the
 * accumulated stream until END preserves primitive continuity across multiple
 * DRAW_ARRAYS packets and uses the same shader/state path as INLINE_ARRAY. */
static int gather_array_vertices(uint32_t first_vertex, uint32_t count)
{
    uint32_t a, v;
    uint32_t capacity = (uint32_t)(sizeof(g_pg.inline_data) / sizeof(uint32_t));
    if (count == 0u) return 1;
    if (!g_pg.inline_layout_valid || !g_pg.vert_stride ||
        count > (capacity - g_pg.inline_count) / g_pg.vert_stride) return 0;
    /* Declaration writes and layout changes invalidate this metadata. Index
     * packets therefore need no repeated 192-byte comparison. Vertex contents
     * are always read afresh so guest edits remain visible on the next packet. */
    if (!g_pg.gather_prepared) {
        g_pg.gather_active_count = 0;
        for (a = 0; a < 16u; ++a) {
            uint32_t bytes = inline_attr_bytes(g_pg.array_format[a]);
            if (bytes) {
                unsigned index = g_pg.gather_active_count++;
                g_pg.gather_active[index].address = g_pg.array_offset[a] & 0x03FFFFFFu;
                g_pg.gather_active[index].bytes = bytes;
                g_pg.gather_active[index].stride = (g_pg.array_format[a] >> 8) & 0xFFu;
                g_pg.gather_active[index].offset = (uint32_t)g_pg.inline_attr_off[a];
            }
        }
        g_pg.gather_prepared = 1;
    }
    /* Validate every source range before changing the destination. */
    for (a = 0; a < g_pg.gather_active_count; ++a) {
        uint64_t end = (uint64_t)g_pg.gather_active[a].address +
            ((uint64_t)first_vertex + count - 1u) * g_pg.gather_active[a].stride + g_pg.gather_active[a].bytes;
        if (g_pg.gather_active[a].address < 0x10000u || end > 0x04000000u) return 0;
    }
    memset(g_pg.inline_data + g_pg.inline_count, 0,
           count * g_pg.vert_stride * sizeof(uint32_t));
    for (v = 0; v < count; ++v) for (a = 0; a < g_pg.gather_active_count; ++a) {
        void *destination = g_pg.inline_data + g_pg.inline_count + v * g_pg.vert_stride + g_pg.gather_active[a].offset;
        const void *source = (const void *)((uintptr_t)g_xbox_mem_offset + g_pg.gather_active[a].address +
            ((uint64_t)first_vertex + v) * g_pg.gather_active[a].stride);
        /* Constant-size copies let the compiler use unaligned loads/stores. */
        switch (g_pg.gather_active[a].bytes) {
        case 4: memcpy(destination,source,4); break;
        case 8: memcpy(destination,source,8); break;
        case 12: memcpy(destination,source,12); break;
        case 16: memcpy(destination,source,16); break;
        default: memcpy(destination,source,g_pg.gather_active[a].bytes); break;
        }
    }
    g_pg.inline_count += count * g_pg.vert_stride;
    return 1;
}

static int gather_array_elements(uint32_t packed, int wide)
{
    uint32_t saved = g_pg.inline_count;
    /* ARRAY_ELEMENT16 orders the low index before the high index. Keep
     * duplicates: they join strips with degenerate triangles. */
    if (!gather_array_vertices(wide ? packed : packed & 0xFFFFu, 1u) ||
        (!wide && !gather_array_vertices(packed >> 16, 1u))) {
        g_pg.inline_count = saved;
        return 0;
    }
    return 1;
}

static void gather_immediate_vertex(void)
{
    if (!g_pg.in_draw || g_pg.indexed_draw_invalid) return;
    if (!g_pg.immediate_draw) {
        /* Immediate attributes have their own format, independent of the
         * array declaration left by an earlier indexed/inline-array draw. */
        if (g_pg.inline_count) { g_pg.indexed_draw_invalid = 1; return; }
        g_pg.immediate_draw = 1;
        g_pg.gather_prepared = 0;
        g_pg.vert_stride = 65; /* 16 float4s plus fixed-function packed color. */
        g_pg.inline_layout_valid = 1;
        for (unsigned a = 0; a < 16; ++a) {
            g_pg.inline_attr_off[a] = a * 4;
            g_pg.inline_attr_size[a] = 4;
            g_pg.inline_attr_type[a] = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
        }
        g_pg.inline_pos_offset = 0; g_pg.inline_pos_size = 4;
        g_pg.inline_diffuse_offset = 64;
        g_pg.inline_texcoord_offset = 36; g_pg.inline_texcoord_size = 4;
    }
    unsigned capacity = sizeof(g_pg.inline_data) / sizeof(g_pg.inline_data[0]);
    if (capacity - g_pg.inline_count < g_pg.vert_stride) {
        g_pg.indexed_draw_invalid = 1; return;
    }
    uint32_t *destination = g_pg.inline_data + g_pg.inline_count;
    memcpy(destination, g_pg.immediate_value, sizeof(g_pg.immediate_value));
    uint32_t color = 0;
    static const unsigned shift[4] = {16, 8, 0, 24};
    for (unsigned ch = 0; ch < 4; ++ch) {
        float value = g_pg.immediate_value[3][ch];
        unsigned byte = value <= 0 ? 0 : value >= 1 ? 255 : (unsigned)(value * 255 + .5f);
        color |= byte << shift[ch];
    }
    destination[64] = color;
    g_pg.inline_count += g_pg.vert_stride;
}

static void submit_draw(void)
{
    if (g_pg.indexed_draw_invalid) return;
    /* TEMPORARY: the Frontend compositor draws its passes as inline quads
     * between SET_SURFACE_COLOR_OFFSET bindings of 01C09FC0 / 01BA9FC0.  None
     * of them have ever produced a draw, so report which precondition fails. */
    {
        static unsigned s_submit_draw_logs;
        /* Log every call that has inline data, or that lands on a Frontend
         * compositor surface -- those are the ones we care about.  A blanket
         * cap only ever captured the cache passes. */
        int interesting = (g_pg.inline_count != 0) ||
            g_pg.surface_color_offset == 0x01C09FC0u ||
            g_pg.surface_color_offset == 0x01BA9FC0u ||
            g_pg.surface_color_offset == 0x01BE9FC0u;
        if (interesting && s_submit_draw_logs++ < 40u)
            fprintf(stderr,
                    "[DEBUG PGRAPH-SUBMIT] inline_count=%u vert_stride=%u "
                    "draw_mode=%u target=%08X num_verts=%u\n",
                    g_pg.inline_count, g_pg.vert_stride, g_pg.draw_mode,
                    g_pg.surface_color_offset,
                    g_pg.vert_stride ? g_pg.inline_count / g_pg.vert_stride : 0u);
    }
    g_rp_phase = 2; /* submit_draw */
    if (g_pg.inline_count == 0 || g_pg.vert_stride == 0) {
        ++g_geom.inline_empty; ++g_dt_rej_empty;
        return;
    }

    uint32_t num_verts = g_pg.inline_count / g_pg.vert_stride;
    if (num_verts < 3) {
        ++g_geom.inline_short; ++g_dt_rej_short;
        return;
    }

    dt_log_draw(num_verts, g_pg.inline_data);

    /* Everything in a frame has to land on the same surface, so bind it
     * here rather than only on the array and clear paths.
     *
     * A draw whose target cannot be bound must not be drawn at all.  Letting
     * it through does not send it nowhere -- it sends it to whichever target
     * was bound last, at its own geometry's size.  That is how the scene's
     * 256x256 bloom downsample ended up drawn into the 640x480 scene surface
     * at 1:1: a miniature of the frame in the upper left, alongside the frame
     * itself.  The swap-chain escape check never saw it because the surface it
     * landed on was another runtime target, not the swap chain. */
    if (!pgraph_bind_scene_surface(xbox_GetD3DDevice())) {
        static unsigned skipped, logged;
        ++skipped; ++g_dt_rej_target;
        if (logged++ < 8u)
            fprintf(stderr, "[WARN PGRAPH-TARGET] no target for %08X "
                    "(clip %08X/%08X); draw skipped (#%u)\n",
                    g_pg.surface_color_offset, g_pg.surface_clip_h,
                    g_pg.surface_clip_v, skipped);
        return;
    }

    const uint32_t *src = g_pg.inline_data;
    int actual_prim_type = g_pg.d3d_prim_type;
    uint32_t out_vert_count = num_verts;

    /* Handle QUADS (mode 8): convert to triangle list (6 verts per quad) */
    int is_quads = (g_pg.draw_mode == 8 || g_pg.draw_mode == 9);
    uint32_t num_quads = is_quads ? (num_verts / 4) : 0;
    if (is_quads) {
        out_vert_count = num_quads * 6;  /* 2 triangles per quad */
        actual_prim_type = D3DPT_TRIANGLELIST;
    }

    /* Calculate primitive count */
    uint32_t prim_count = 0;
    switch (actual_prim_type) {
        case D3DPT_TRIANGLELIST:  prim_count = out_vert_count / 3; break;
        case D3DPT_TRIANGLESTRIP: prim_count = out_vert_count - 2; break;
        case D3DPT_TRIANGLEFAN:   prim_count = out_vert_count - 2; break;
        case D3DPT_LINELIST:      prim_count = out_vert_count / 2; break;
        case D3DPT_LINESTRIP:     prim_count = out_vert_count - 1; break;
        default: prim_count = out_vert_count / 3; break;
    }
    if (prim_count == 0) {
        ++g_geom.inline_noprims;
        return;
    }

    void *gpu_program = NULL;
    Nv2aGpuConstants gpu_constants = {0};
    uint32_t immediate_formats[16];
    const uint32_t *draw_formats = g_pg.array_format;
    if (g_pg.immediate_draw) {
        for (unsigned a = 0; a < 16; ++a) immediate_formats[a] = 0x42u;
        draw_formats = immediate_formats;
    }
    if (vsh_program_active() && vsh_gpu_enabled() && g_pg.chyron_scroll_offset == 0) {
        size_t words=is_quads ? (size_t)out_vert_count*g_pg.vert_stride : 0;
        if (words > g_gpu_input_capacity) {
            void *storage = realloc(g_gpu_inputs, words * 4u);
            if (storage) {g_gpu_inputs=storage;g_gpu_input_capacity=words;}
        }
        if (words <= g_gpu_input_capacity)
            gpu_program = d3d8_nv2a_gpu_program((const uint32_t (*)[4])g_pg.transform_program,
                g_pg.transform_program_count,draw_formats,g_pg.inline_attr_off);
        for(unsigned attr=0;attr<16;attr++)gpu_constants.formats[attr]=
            g_pg.inline_attr_off[attr]<0?0:draw_formats[attr];
        for(unsigned stage=0;stage<4;stage++)
            for(unsigned lane=0;lane<4;lane++)gpu_constants.tex_matrix[stage][lane*5]=1;
    }

    /* Convert inline vertices to the four-coordinate-set FVF layout. */
    OutputVertex *out = gpu_program ? NULL : (OutputVertex *)_alloca(out_vert_count * sizeof(OutputVertex));
    if (out) memset(out, 0, out_vert_count * sizeof(OutputVertex));
    for (uint32_t i = 0; out && i < out_vert_count; ++i) {
        out[i].fog = 1.0f;
        out[i].q = 1.0f;
        for (unsigned stage = 0; stage < 3; ++stage) out[i].extra_uv[stage][3] = 1.0f;
    }

    /* Decode the current program once for this draw. Constants and register
     * contents still come from each invocation; no outputs persist across
     * draws, program uploads or animation frames. */
    int has_vertex_program = vsh_program_active();
    if (has_vertex_program && !gpu_program) {
        nv2a_vsh_decode(&g_decoded_vsh, (const uint32_t (*)[4])g_pg.transform_program,
                         g_pg.transform_program_count);
        nv2a_vertex_cache_reset(&g_vertex_cache);
        vsh_prepare_vertices();
    }

    /* Helper to convert one inline vertex */
    /* The mode-based guess, kept only for a title that draws inline without
     * ever declaring its vertex attributes. */
    #define CONVERT_VERT_GUESSED(dst_idx, src_idx) do { \
        uint32_t _b = (src_idx) * g_pg.vert_stride; \
        out[dst_idx].x     = u2f(src[_b + 0]); \
        out[dst_idx].y     = u2f(src[_b + 1]); \
        out[dst_idx].z     = 0.0f; \
        out[dst_idx].rhw   = 1.0f; \
        out[dst_idx].u     = (g_pg.vert_stride == 4 && g_pg.draw_mode != 8) \
                                ? 0.0f : u2f(src[_b + 2]); \
        out[dst_idx].v     = (g_pg.vert_stride == 4 && g_pg.draw_mode != 8) \
                                ? 0.0f : u2f(src[_b + 3]); \
        out[dst_idx].color = (g_pg.vert_stride == 4) \
                                ? ((g_pg.draw_mode == 8) ? 0xFFFFFFFFu \
                                                        : src[_b + 3]) \
                                : src[_b + 4]; \
    } while(0)

    /* The declared layout: attribute offsets read out of the format
     * registers by inline_layout_from_formats(). */
    #define CONVERT_VERT_DECLARED(dst_idx, src_idx) do { \
        uint32_t _b = (src_idx) * g_pg.vert_stride; \
        const uint32_t *_p = src + _b + (uint32_t)g_pg.inline_pos_offset; \
        out[dst_idx].x   = u2f(_p[0]); \
        out[dst_idx].y   = g_pg.inline_pos_size >= 2u ? u2f(_p[1]) : 0.0f; \
        out[dst_idx].z   = g_pg.inline_pos_size >= 3u \
                             ? pgraph_ndc_z(u2f(_p[2])) : 0.0f; \
        out[dst_idx].rhw = g_pg.inline_pos_size >= 4u ? u2f(_p[3]) : 1.0f; \
        out[dst_idx].color = g_pg.inline_diffuse_offset >= 0 \
            ? src[_b + (uint32_t)g_pg.inline_diffuse_offset] : 0xFFFFFFFFu; \
        out[dst_idx].u = g_pg.inline_texcoord_offset >= 0 \
            ? u2f(src[_b + (uint32_t)g_pg.inline_texcoord_offset]) : 0.0f; \
        out[dst_idx].v = (g_pg.inline_texcoord_offset >= 0 && \
                          g_pg.inline_texcoord_size >= 2u) \
            ? u2f(src[_b + (uint32_t)g_pg.inline_texcoord_offset + 1u]) \
            : 0.0f; \
    } while(0)

    #define CONVERT_VERT(dst_idx, src_idx) do { \
        if (gpu_program) { \
            memcpy(g_gpu_inputs+(dst_idx)*g_pg.vert_stride,src+(src_idx)*g_pg.vert_stride, \
                   g_pg.vert_stride*4u); \
        } else if (has_vertex_program) { \
            uint32_t _cached_output; \
            if (nv2a_vertex_cache_lookup(&g_vertex_cache, src, g_pg.vert_stride, \
                    (src_idx), (dst_idx), &_cached_output)) { \
                out[dst_idx] = out[_cached_output]; \
                ++g_vsh_cache_hits; \
            } else { \
                vsh_transform_vertex(src + (src_idx) * g_pg.vert_stride, \
                                     &out[dst_idx]); \
                ++g_vsh_cache_misses; \
            } \
        } \
        else if (g_pg.inline_layout_valid && g_pg.inline_pos_offset >= 0) \
            CONVERT_VERT_DECLARED(dst_idx, src_idx); \
        else \
            CONVERT_VERT_GUESSED(dst_idx, src_idx); \
    } while(0)

    if (is_quads) {
        /* Convert quads (v0,v1,v2,v3) → two triangles (v0,v1,v2), (v0,v2,v3) */
        uint32_t out_idx = 0;
        for (uint32_t q = 0; q < num_quads; q++) {
            uint32_t qi = q * 4;
            CONVERT_VERT(out_idx + 0, qi + 0);  /* tri 1: v0 */
            CONVERT_VERT(out_idx + 1, qi + 1);  /* tri 1: v1 */
            CONVERT_VERT(out_idx + 2, qi + 2);  /* tri 1: v2 */
            if (g_pg.draw_mode == 9) {
                /* The native compositor publishes its four corners in
                 * triangle-strip order: TL, TR, BL, BR. */
                CONVERT_VERT(out_idx + 3, qi + 2);  /* tri 2: v2 */
                CONVERT_VERT(out_idx + 4, qi + 1);  /* tri 2: v1 */
                CONVERT_VERT(out_idx + 5, qi + 3);  /* tri 2: v3 */
            } else {
                CONVERT_VERT(out_idx + 3, qi + 0);  /* tri 2: v0 */
                CONVERT_VERT(out_idx + 4, qi + 2);  /* tri 2: v2 */
                CONVERT_VERT(out_idx + 5, qi + 3);  /* tri 2: v3 */
            }
            out_idx += 6;
        }
    } else if (!gpu_program) {
        for (uint32_t i = 0; i < num_verts; i++) {
            CONVERT_VERT(i, i);
        }
    }
    #undef CONVERT_VERT
    #undef CONVERT_VERT_DECLARED
    #undef CONVERT_VERT_GUESSED

    /* Chyron scroll: shift X for vertices in the chyron Y band (366-382).
     * Simple continuous scroll — no per-vertex wrapping to avoid artifacts
     * from split triangle-strip quads spanning the screen. */
    if (g_pg.chyron_scroll_offset != 0.0f && out_vert_count >= 6) {
        /* Check if this draw is in the chyron band */
        int is_chyron = 1;
        for (uint32_t i = 0; i < (out_vert_count < 8 ? out_vert_count : 8); i++) {
            if (out[i].y < 360.0f || out[i].y > 390.0f) {
                is_chyron = 0;
                break;
            }
        }
        if (is_chyron) {
            /* Find the total text width */
            float min_x = 9999.0f, max_x = -9999.0f;
            for (uint32_t i = 0; i < out_vert_count; i++) {
                if (out[i].x < min_x) min_x = out[i].x;
                if (out[i].x > max_x) max_x = out[i].x;
            }
            float text_width = max_x - min_x;

            /* Scroll loops: text slides left, then resets to start position.
             * Total cycle = text scrolls fully off-left + re-enters from right. */
            float cycle = text_width + 640.0f;
            float scroll = fmodf(g_pg.chyron_scroll_offset, cycle);

            /* Apply uniform shift to ALL vertices (no per-vertex wrap) */
            for (uint32_t i = 0; i < out_vert_count; i++) {
                out[i].x -= scroll;
            }
        }
    }

    /* Log first few draws' vertex positions (once) */
    if (g_pg.stats.draw_calls < 3 && num_verts >= 3) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw verts (mode=%u, %u in → %u out):\n",
                g_pg.draw_mode, num_verts, out_vert_count);
        uint32_t show = num_verts < 8 ? num_verts : 8;
        for (uint32_t i = 0; i < show; i++) {
            uint32_t b = i * g_pg.vert_stride;
            fprintf(stderr, "  [%u] pos=(%.1f, %.1f) uv=(%.3f, %.3f) color=0x%08X\n",
                    i, u2f(src[b+0]), u2f(src[b+1]), u2f(src[b+2]), u2f(src[b+3]), src[b+4]);
        }
    }

    /* Get D3D8 device */
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    int scene_begun = 0;
    int tex_bound = 0;
    UINT tex_bw = 0u, tex_bh = 0u;
    if (!dev) return;

    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    /* RGB-only scene draws must preserve alpha for subsequent effects.
     * Applying this only on the DrawArrays fallback left inline/indexed
     * geometry using the previous native write mask (usually all RGBA). */
    dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE,
                                pgraph_color_write_mask());
    /* Blending is whatever the title asked for, not a fixed guess.  This
     * used to force SRCALPHA/INVSRCALPHA on every draw, which is right for
     * the menu quads and wrong for the one draw that matters most: the
     * compositor copies a finished surface onto another with ONE/ZERO, a
     * straight replace.  Forced through alpha blending against a source
     * whose alpha is zero almost everywhere, it wrote nothing, and the
     * surface being presented stayed empty while the surface holding the
     * frame sat next to it with the content in it. */
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                g_pg.blend_enable ? TRUE : FALSE);
    if (g_pg.blend_enable) {
        dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND,
                                    nv2a_blend_to_d3d(g_pg.blend_sfactor));
        dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND,
                                    nv2a_blend_to_d3d(g_pg.blend_dfactor));
    }

    /* Set FVF for pre-transformed 2D with texture */
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_NV2A_FOG | D3DFVF_NV2A_TEX4);

    /* Quad strips follow the guest texture and blend state. */
    {
    /* Generic path: the texture the title bound, uploaded from guest memory.
     * See pgraph_texture_for_stage(). */
    {
        UINT tex_w = 0u, tex_h = 0u;
        int texel_coords = 0;
        IDirect3DTexture8 *bound =
            pgraph_texture_for_stage(dev, 0, &tex_w, &tex_h, &texel_coords);
        tex_bound = bound != NULL;
        tex_bw = tex_w; tex_bh = tex_h;
        if (bound) {
            /* NV2A addresses a linear texture in texels; D3D11 samplers only
             * do 0..1.  Without this the coordinates still look valid to the
             * sampler, CLAMP pins them to an edge texel, and the draw comes
             * out the colour of that edge -- which for the compositor's
             * full-screen copy meant black, with a correct source sitting in
             * the surface it was reading. */
            /* The inline path rewrites its own coordinates below, so any
             * matrix the array path left enabled must be cleared. */
            dev->lpVtbl->SetTextureStageState(
                dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            if (gpu_program && texel_coords && tex_w && tex_h) {
                gpu_constants.tex_matrix[0][0] = 1.0f / (float)tex_w;
                gpu_constants.tex_matrix[0][5] = 1.0f / (float)tex_h;
            } else if (texel_coords && tex_w != 0u && tex_h != 0u) {
                uint32_t vi;
                float umin = out[0].u, umax = out[0].u;
                float vmin = out[0].v, vmax = out[0].v;
                for (vi = 1u; vi < out_vert_count; ++vi) {
                    if (out[vi].u < umin) umin = out[vi].u;
                    if (out[vi].u > umax) umax = out[vi].u;
                    if (out[vi].v < vmin) vmin = out[vi].v;
                    if (out[vi].v > vmax) vmax = out[vi].v;
                }
                { static unsigned logged;
                  if (logged < 48u && g_pg.stats.frames >= 20000u) {
                      ++logged;
                      fprintf(stderr, "[INFO PGRAPH-TEXCOORD] rt=%08X "
                              "tex=%08X %ux%u raw u=%.1f..%.1f "
                              "v=%.1f..%.1f -> %.3f..%.3f/%.3f..%.3f\n",
                              g_pg.surface_color_offset,
                              g_pg.tex[0].offset & 0x03FFFFFFu,
                              (unsigned)tex_w, (unsigned)tex_h,
                              umin, umax, vmin, vmax,
                              umin / (float)tex_w, umax / (float)tex_w,
                              vmin / (float)tex_h, vmax / (float)tex_h);
                  } }
                for (vi = 0; vi < out_vert_count; ++vi) {
                    out[vi].u /= (float)tex_w;
                    out[vi].v /= (float)tex_h;
                }
            }
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)bound);
            if (!pgraph_stage0_from_combiner(dev, 1)) {
            dev->lpVtbl->SetTextureStageState(dev, 0, 1, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 3, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 6, 0 /*DIFFUSE*/);
            }
            dev->lpVtbl->SetTextureStageState(dev, 0, 13, 3 /*CLAMP*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 14, 3 /*CLAMP*/);

            /* DIAGNOSTIC ONLY -- CONKER_XMV_RAW=1.
             *
             * For the video quad alone, replace the pixel path with the raw
             * sampled texture: select TEXTURE for colour and alpha, and take
             * blending, alpha test and the write mask out of the picture.
             * Geometry, UVs, the bound texture and the render target are
             * untouched.  The converted texture is measured bright
             * (19200/19200 non-black, mean RGB 384/765) and the target still
             * ends up black, so this says whether the state layer between
             * them is what loses it.  Not a fix: it discards the combiner the
             * title asked for.
             */
            { static int raw = -1;
              if (raw < 0) raw = (getenv("CONKER_XMV_RAW") != NULL);
              if (raw && pt_offset_is_yuv(g_pg.tex[0].offset & 0x03FFFFFFu)) {
                  dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                              FALSE);
                  dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE,
                                              FALSE);
                  dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE,
                                              0xFu);
                  dev->lpVtbl->SetTextureStageState(dev, 0, 1, 2);
                  dev->lpVtbl->SetTextureStageState(dev, 0, 2, 2);
                  dev->lpVtbl->SetTextureStageState(dev, 0, 4, 2);
                  dev->lpVtbl->SetTextureStageState(dev, 0, 5, 2);
              } }
        } else {
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            if (!pgraph_stage0_from_combiner(dev, 0)) {
            dev->lpVtbl->SetTextureStageState(dev, 0, 1, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5, 0 /*DIFFUSE*/);
            }
        }

        pgraph_publish_combiners(dev);
        /* And whatever the title has on the remaining stages. */
        pgraph_bind_extra_stages(dev);
        pgraph_report_combiners();
    }
    }

    /* Keep the window alive between flips.  A scene frame is tens of thousands
     * of draws and the flip that would have pumped is a long way off. */
    d3d8_PumpWindowMessages(0);

    /* Begin scene if needed */
    if (!scene_begun)
        dev->lpVtbl->BeginScene(dev);

    { static unsigned escaped, reported;
      /* Every draw, not every 256th.  The check is a flag read now, so there
       * is no reason to sample -- and sampling is what made this report zero
       * while a couple of draws per frame were landing on the swap chain. */
      if (!gpu_program && d3d8_IsDefaultRenderTargetBound()) {
          ++escaped;
          if (reported++ < 16u)
              fprintf(stderr, "[WARN PGRAPH-TARGET] inline draw #%u on the "
                      "swap chain: rt=%08X bbox=(%.0f,%.0f)-(%.0f,%.0f)\n",
                      escaped, g_pg.surface_color_offset,
                      out[0].x, out[0].y, out[2].x, out[2].y);
      } }

    /* Which draw makes the big corner shape.
     *
     * A gradient triangle pinned to the top-left, changing size between
     * frames, is geometry read with the wrong layout rather than a compositor
     * artefact -- so report any draw whose output covers a large part of the
     * surface and starts at its origin, together with the layout the vertex
     * format registers produced.  A correct full-screen quad also lands here,
     * so the stride and the texture matter more than the box itself. */
    float minx_dbg = 0, maxx_dbg = 0, miny_dbg = 0, maxy_dbg = 0;
    if (!gpu_program) { static unsigned reported;
      float minx = out[0].x, maxx = out[0].x;
      float miny = out[0].y, maxy = out[0].y;
      uint32_t vi;
      for (vi = 1u; vi < out_vert_count; ++vi) {
          if (out[vi].x < minx) minx = out[vi].x;
          if (out[vi].x > maxx) maxx = out[vi].x;
          if (out[vi].y < miny) miny = out[vi].y;
          if (out[vi].y > maxy) maxy = out[vi].y;
      }
      float vpw = 0.0f, vph = 0.0f;
      d3d8_DebugGetViewportSize(&vpw, &vph);
      minx_dbg = minx; maxx_dbg = maxx; miny_dbg = miny; maxy_dbg = maxy;
      /* One whole frame, well past start-up, rather than the first N draws --
       * the first N are all the full-screen composite and say nothing about
       * what else lands in the picture. */
      if (reported < 400u &&
          g_pg.stats.frames >= 300000u && g_pg.stats.frames <= 300003u) {
          ++reported;
          fprintf(stderr, "[INFO PGRAPH-CORNER] %.0fx%.0f at (%.0f,%.0f) "
                  "stride=%u pos=%d/%u diff=%d tc=%d/%u prim=%u verts=%u "
                  "rt=%08X tex=%08X c0=%08X bound=%d tex=%ux%u vp=%.0fx%.0f\n",
                  maxx - minx, maxy - miny, minx, miny,
                  g_pg.vert_stride, g_pg.inline_pos_offset,
                  g_pg.inline_pos_size, g_pg.inline_diffuse_offset,
                  g_pg.inline_texcoord_offset, g_pg.inline_texcoord_size,
                  (unsigned)g_pg.d3d_prim_type, out_vert_count,
                  g_pg.surface_color_offset, g_pg.tex[0].offset,
                  out[0].color,
                  tex_bound, (unsigned)tex_bw, (unsigned)tex_bh, vpw, vph);
      } }

    /* Bisecting the corner miniature: skip the passes that composite a
     * smaller runtime target back onto a full-size surface.  Opt-in, so
     * ordinary runs are untouched. */
    if (getenv("CONKER_SKIP_BLOOM") != NULL && tex_bound &&
        tex_bw != 0u && tex_bw < d3d8_GetBackbufferWidth() &&
        g_scene_bound_texture != NULL)
        return;

    if (pgraph_seq_active()) {
        /* The UV range as it reaches the sampler.  Every draw into a full-size
         * surface is a 640x480 quad over a 640x480 viewport, yet the surface
         * ends up with content only in a small corner -- so what is sampled,
         * not what is covered, is where the picture is being lost. */
        float umin = out[0].u, umax = out[0].u;
        float vmin = out[0].v, vmax = out[0].v;
        uint32_t vi;
        for (vi = 1u; vi < out_vert_count; ++vi) {
            if (out[vi].u < umin) umin = out[vi].u;
            if (out[vi].u > umax) umax = out[vi].u;
            if (out[vi].v < vmin) vmin = out[vi].v;
            if (out[vi].v > vmax) vmax = out[vi].v;
        }
        fprintf(stderr, "[SEQ] %4u f%u draw   rt=%08X tex=%08X %.0fx%.0f "
                "uv=%.3f..%.3f/%.3f..%.3f c0=%08X\n",
                ++g_seq, (unsigned)g_pg.stats.frames,
                g_pg.surface_color_offset,
                tex_bound ? (g_pg.tex[0].offset & 0x03FFFFFFu) : 0u,
                maxx_dbg - minx_dbg, maxy_dbg - miny_dbg,
                umin, umax, vmin, vmax, out[0].color);
        pgraph_dump_pixel_state(g_seq, tex_bound, out[0].color);
        fprintf(stderr, "[PXS]   fog enable=%u mode=%04X params=(%g,%g,%g) vertex=%g colour=%08X\n",
                g_pg.fog_enable, g_pg.fog_mode, g_pg.fog_params[0],
                g_pg.fog_params[1], g_pg.fog_params[2], out[0].fog, g_pg.fog_color);
        { extern DWORD d3d8_combiners_debug_token(void);
          DWORD tok = d3d8_combiners_debug_token();
          fprintf(stderr, "[PXS]   combiner PS token=%08X -> %s" "\n",
                  (unsigned)tok, tok ? "COMBINER SHADER decides the pixel; the texture-stage states above are dead" : "fixed-function"); }
    }

    /* Vertex path, for the one draw under study.  Everything upstream is
     * proven correct, so this is the last stretch: the guest dwords, the
     * layout used to decode them, and the vertices that actually go to
     * D3D11 -- the [VTX] D3D11 lines come from dev_DrawPrimitiveUP. */
    if (pgraph_seq_active()) {
        uint32_t vi, vn = out_vert_count < 6u ? out_vert_count : 6u;
        fprintf(stderr,
            "[VTX] seq %u rt=%08X tex=%08X  layout_valid=%d stride=%u prim=%u verts=%u" "\n"
            "[VTX]   pos_off=%d pos_size=%u  diffuse_off=%d  tc_off=%d tc_size=%u" "\n",
            g_seq + 1u, g_pg.surface_color_offset,
            tex_bound ? (g_pg.tex[0].offset & 0x03FFFFFFu) : 0u,
            g_pg.inline_layout_valid, g_pg.vert_stride,
            (unsigned)g_pg.d3d_prim_type, out_vert_count,
            g_pg.inline_pos_offset, g_pg.inline_pos_size,
            g_pg.inline_diffuse_offset,
            g_pg.inline_texcoord_offset, g_pg.inline_texcoord_size);
        fprintf(stderr, "[VTX]   vp_scale=%.4f,%.4f,%.4f,%.4f  vp_offset=%.4f,%.4f,%.4f,%.4f  zeta=%08X surf_fmt=%08X depth_test=%d" "\n",
                g_pg.vp_scale[0], g_pg.vp_scale[1], g_pg.vp_scale[2],
                g_pg.vp_scale[3], g_pg.vp_offset[0], g_pg.vp_offset[1],
                g_pg.vp_offset[2], g_pg.vp_offset[3],
                g_pg.surface_zeta_offset, g_pg.surface_format,
                g_pg.depth_test);
        for (vi = 0; vi < vn; ++vi) {
            uint32_t b = vi * g_pg.vert_stride, k;
            if (vi < num_verts) {
                fprintf(stderr, "[VTX]   guest v%u raw:", vi);
                for (k = 0; k < g_pg.vert_stride && k < 12u; ++k)
                    fprintf(stderr, " %08X", src[b + k]);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "[VTX]   conv  v%u  xyz=%.2f,%.2f,%.2f rhw=%.4f  uv=%.4f,%.4f  c=%08X" "\n",
                    vi, out[vi].x, out[vi].y, out[vi].z, out[vi].rhw,
                    out[vi].u, out[vi].v, out[vi].color);
        }
        fflush(stderr);
        g_vtx_trace = 1;
    }

    /* Texture feedback can rebind the color target without its DSV. Apply
     * depth after those resolves, immediately before the draw. */
    pgraph_apply_depth_state(dev);
    if (pgraph_seq_active())
        d3d8_DebugIdentities(g_scene_bound_target != NULL
                             ? g_scene_bound_target->texture : NULL,
                             "at-draw", g_pg.surface_color_offset & 0x03FFFFFFu);
    pgraph_apply_alpha_state(dev);
    for (unsigned stage = 0; stage < 4; ++stage)
        pgraph_apply_sampler_state(dev, stage);
    g_rp_phase = 3; /* DrawPrimitiveUP */
    g_rp_draw_seq = g_seq;
    const char *pixel_capture = NULL;
    char pixel_capture_name[512];
    { static unsigned captured, capture_serial;
      static uint32_t seen[256];
      static ULONGLONG epoch;
      static FILETIME last_trigger;
      static unsigned simple_effects;
      static float selected_x, selected_y;
      static uint32_t selected_texture;
      static int capture_initialized, all_targets, effects_only, rearm;
      static const char *path, *after, *count, *trigger, *target, *sequential;
      static unsigned effects_limit;
      if (!capture_initialized) {
          /* Startup options are stable; only the trigger file changes live. */
          path = getenv("CONKER_PIXEL_CAPTURE");
          after = getenv("CONKER_PIXEL_AFTER");
          count = getenv("CONKER_PIXEL_COUNT");
          trigger = getenv("CONKER_PIXEL_TRIGGER");
          target = getenv("CONKER_PIXEL_TARGET");
          sequential = getenv("CONKER_PIXEL_SEQUENTIAL");
          const char *max_vertices = getenv("CONKER_PIXEL_MAX_VERTS");
          effects_limit = max_vertices ? (unsigned)atoi(max_vertices) : 6u;
          all_targets = getenv("CONKER_PIXEL_ALL_TARGETS") != NULL;
          effects_only = getenv("CONKER_PIXEL_EFFECTS_ONLY") != NULL;
          rearm = getenv("CONKER_PIXEL_REARM") != NULL;
          capture_initialized = 1;
      }
      /* A changed trigger timestamp starts another bounded capture batch.
       * Serial names preserve earlier evidence in the same process. */
      if (path && *path && trigger && rearm) {
          WIN32_FILE_ATTRIBUTE_DATA data;
          if (GetFileAttributesExA(trigger, GetFileExInfoStandard, &data) &&
              CompareFileTime(&data.ftLastWriteTime, &last_trigger) != 0) {
              last_trigger = data.ftLastWriteTime;
              captured = 0;
              /* A particle batch can contain many quads. The trigger can
               * select simple combiners without another game restart. */
              FILE *settings = fopen(trigger, "r");
              char selection[32] = {0};
              int fields = 0;
              if (settings) {
                  fields = fscanf(settings, "%31s %f %f", selection, &selected_x, &selected_y);
                  if (strcmp(selection, "texture") == 0) {
                      rewind(settings);
                      fields = fscanf(settings, "%31s %x", selection, &selected_texture);
                  }
                  fclose(settings);
              }
              simple_effects = strcmp(selection, "particles") == 0 ? 1u :
                               strcmp(selection, "cubemaps") == 0 ? 2u :
                               strcmp(selection, "pixel") == 0 && fields == 3 ? 3u :
                               strcmp(selection, "texture") == 0 && fields == 2 ? 4u : 0u;
          }
      }
      unsigned limit = count ? (unsigned)atoi(count) : 1u;
      if (limit > 256u) limit = 256u;
      if (!epoch) epoch = GetTickCount64();
      uint32_t signature = g_pg.transform_program_start ^ g_pg.combiner_final0;
      int capture_due = path && *path && captured < limit &&
          (!trigger || GetFileAttributesA(trigger) != INVALID_FILE_ATTRIBUTES);
      if (capture_due) {
          for (unsigned pc = g_pg.transform_program_start; pc < g_pg.transform_program_count; ++pc) {
              for (unsigned word = 0; word < 4; ++word)
                  signature = (signature ^ g_pg.transform_program[pc][word]) * 16777619u;
              if (g_pg.transform_program[pc][3] & 1u) break;
          }
          for (unsigned stage = 0; stage < 8; ++stage)
              signature = (signature ^ g_pg.combiner_color_icw[stage]) * 16777619u;
          for (unsigned stage = 0; stage < 4; ++stage)
              signature = (signature ^ g_pg.tex[stage].offset) * 16777619u;
          signature = (signature ^ g_pg.alpha_func ^ (g_pg.alpha_ref << 16)
                       ^ (g_pg.alpha_test << 8)) * 16777619u;
      }
      unsigned known = 0;
      for (unsigned i = 0; i < captured; ++i) known |= seen[i] == signature;
      int cube_material = 0;
      int selected_material = 0;
      if (capture_due && simple_effects == 4u)
          for (unsigned i = 0; i < 4u; ++i)
              selected_material |= g_pg.tex[i].enabled &&
                  (g_pg.tex[i].offset & 0x03FFFFFFu) == (selected_texture & 0x03FFFFFFu);
      if (capture_due && simple_effects == 2u)
          for (unsigned i = 0; i < 4u; ++i)
              cube_material |= g_pg.tex[i].enabled &&
                  (g_pg.tex[i].format & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE) &&
                  ((g_pg.shader_stage_program >> (5u * i)) & 31u) == 3u;
      if (capture_due && (!known || sequential) &&
          (!target || (g_pg.surface_color_offset & 0x03FFFFFFu) == strtoul(target, NULL, 16)) &&
          (!after || GetTickCount64() - epoch >= atof(after) * 1000.0) &&
          (!effects_only || (simple_effects == 3u
             ? ((g_pg.surface_clip_h >> 16) == 640u && (g_pg.surface_clip_v >> 16) == 480u &&
                minx_dbg <= selected_x && maxx_dbg >= selected_x &&
                miny_dbg <= selected_y && maxy_dbg >= selected_y)
             : simple_effects == 4u ? selected_material
             : simple_effects == 2u ? cube_material : simple_effects
             ? (g_pg.blend_enable && (g_pg.combiner_control & 15u) <= 2u)
             : out_vert_count <= effects_limit)) &&
          (all_targets || ((g_pg.surface_clip_h >> 16) == 640u &&
          (g_pg.surface_clip_v >> 16) == 480u &&
          maxx_dbg > 0 && minx_dbg < 640 && maxy_dbg > 0 && miny_dbg < 480 &&
          maxx_dbg - minx_dbg > 64 && maxy_dbg - miny_dbg > 64))) {
          seen[captured] = signature;
          ++captured;
          snprintf(pixel_capture_name, sizeof(pixel_capture_name), "%s.%u", path, capture_serial++);
          pixel_capture = pixel_capture_name;
          char name[512];
          snprintf(name, sizeof(name), "%s.before.bmp", pixel_capture);
          d3d8_DebugDumpRuntimeTexture(g_scene_bound_target ? g_scene_bound_target->texture : NULL, name);
          fprintf(stderr, "[PIXEL-CAPTURE] capture=%s rt=%08X verts=%u primitive=%u final=%08X/%08X control=%08X\n",
                  pixel_capture,
                  g_pg.surface_color_offset, out_vert_count, g_pg.d3d_prim_type,
                  g_pg.combiner_final0, g_pg.combiner_final1, g_pg.combiner_control);
          fprintf(stderr, "[PIXEL-CAPTURE] frame=%u draw=%u bounds=(%g,%g,%g,%g) depth=%u/%X/%u viewport=(%g,%g,%g)/(%g,%g,%g)\n",
                  g_pg.stats.frames, g_seq, minx_dbg, miny_dbg, maxx_dbg, maxy_dbg,
                  g_pg.depth_test, g_pg.depth_func, g_pg.depth_mask,
                  g_pg.vp_offset[0], g_pg.vp_offset[1], g_pg.vp_offset[2],
                  g_pg.vp_scale[0], g_pg.vp_scale[1], g_pg.vp_scale[2]);
          fprintf(stderr, "[PIXEL-CAPTURE] stencil=%u func=%X ref=%X read=%X write=%X ops=%X/%X/%X\n",
                  g_pg.stencil_test, g_pg.stencil_func, g_pg.stencil_ref,
                  g_pg.stencil_read_mask, g_pg.stencil_write_mask,
                  g_pg.stencil_fail, g_pg.stencil_zfail, g_pg.stencil_pass);
          snprintf(name, sizeof(name), "%s.vertices.bin", pixel_capture);
          FILE *vertices = fopen(name, "wb");
          if (vertices) {
              uint32_t header[4] = {0x56545831u, out_vert_count, sizeof(OutputVertex), actual_prim_type};
              fwrite(header, sizeof(header), 1, vertices);
              fwrite(out, sizeof(OutputVertex), out_vert_count, vertices);
              fclose(vertices);
          }
          /* Compact complete source replay, independent of the first-three
           * expanded shader records below. No game-derived data is embedded
           * in source or runtime defaults. */
          snprintf(name, sizeof(name), "%s.inputs.bin", pixel_capture);
          FILE *inputs = fopen(name, "wb");
          if (inputs) {
              uint32_t header[8] = {0x56534931u, num_verts, g_pg.vert_stride,
                  g_pg.transform_program_count, g_pg.transform_program_start,
                  g_pg.transform_execution_mode, g_pg.stats.frames, 0u};
              fwrite(header, sizeof(header), 1, inputs);
              fwrite(draw_formats, sizeof(g_pg.array_format), 1, inputs);
              fwrite(g_pg.inline_attr_off, sizeof(g_pg.inline_attr_off), 1, inputs);
              fwrite(g_pg.transform_program, 16u, g_pg.transform_program_count, inputs);
              fwrite(g_pg.transform_constant, sizeof(g_pg.transform_constant), 1, inputs);
              fwrite(src, sizeof(uint32_t) * g_pg.vert_stride, num_verts, inputs);
              fclose(inputs);
          }
          fprintf(stderr, "[PIXEL-CAPTURE] program=%u mode=%X texture_modes=%08X fog=%u/%X color=%08X blend=%u(%X/%X) alpha=%u(%X/%X) cull=%u(%X/%X) mask=%08X\n",
                  g_pg.transform_program_start, g_pg.transform_execution_mode,
                  g_pg.shader_stage_program,
                  g_pg.fog_enable, g_pg.fog_mode, g_pg.fog_color,
                  g_pg.blend_enable, g_pg.blend_sfactor, g_pg.blend_dfactor,
                  g_pg.alpha_test, g_pg.alpha_func, g_pg.alpha_ref,
                  g_pg.cull_enable, g_pg.cull_face, g_pg.front_face, g_pg.color_mask);
          for (unsigned a = 0; a < 16; ++a)
              if (g_pg.inline_attr_off[a] >= 0)
                  fprintf(stderr, "[PIXEL-CAPTURE] attr%u format=%08X array=%08X inline=%d\n",
                          a, g_pg.array_format[a], g_pg.array_offset[a], g_pg.inline_attr_off[a]);
          snprintf(name, sizeof(name), "%s.vsh.bin", pixel_capture);
          FILE *replay = fopen(name, "wb");
          if (replay && vsh_program_active()) {
              for (unsigned v = 0; v < num_verts && v < 3; ++v) {
                  nv2a_vsh_state st = {0};
                  for (unsigned t = 9; t <= 12; ++t) st.o[t][3] = 1.0f;
                  uint32_t header[8] = {0x56534831u, g_pg.stats.frames,
                      g_pg.transform_program_count, g_pg.transform_program_start,
                      g_pg.surface_color_offset, g_pg.tex[0].offset,
                      g_pg.transform_execution_mode, v};
                  for (unsigned a = 0; a < 16; ++a)
                      for (unsigned k = 0; k < 4; ++k)
                          st.v[a][k] = vsh_attr_component(g_pg.inline_data + v * g_pg.vert_stride, a, k);
                  memcpy(st.c, g_pg.transform_constant, sizeof(st.c));
                  nv2a_vsh_run((const uint32_t (*)[4])g_pg.transform_program,
                               g_pg.transform_program_count, &st);
                  fwrite(header, sizeof(header), 1, replay);
                  fwrite(g_pg.transform_program, 16u, g_pg.transform_program_count, replay);
                  fwrite(st.v, sizeof(st.v), 1, replay);
                  fwrite(st.c, sizeof(st.c), 1, replay);
                  fwrite(st.o, sizeof(st.o), 1, replay);
              }
          }
          if (replay) fclose(replay);
          for (unsigned stage = 0; stage < 4; ++stage) {
              extern void d3d8_DebugDumpBoundTexture(unsigned, const char *);
              fprintf(stderr, "[PIXEL-CAPTURE] texture%u offset=%08X format=%08X rect=%08X enable=%u address=%08X filter=%08X border=%08X\n",
                      stage, g_pg.tex[stage].offset, g_pg.tex[stage].format,
                      g_pg.tex[stage].image_rect, g_pg.tex[stage].enabled,
                      g_pg.tex[stage].address, g_pg.tex[stage].filter, g_pg.tex[stage].border_color);
              snprintf(name, sizeof(name), "%s.texture%u.bmp", pixel_capture, stage);
              d3d8_DebugDumpBoundTexture(stage, name);

          }
          for (unsigned stage = 0; stage < (g_pg.combiner_control & 15u) && stage < 8u; ++stage)
              fprintf(stderr, "[PIXEL-CAPTURE] combiner%u rgb=%08X/%08X alpha=%08X/%08X c0=%08X c1=%08X\n",
                      stage, g_pg.combiner_color_icw[stage], g_pg.combiner_color_ocw[stage],
                      g_pg.combiner_alpha_icw[stage], g_pg.combiner_alpha_ocw[stage],
                      g_pg.combiner_factor0[stage], g_pg.combiner_factor1[stage]);
          for (unsigned v = 0; v < out_vert_count && v < 6; ++v)
              fprintf(stderr, "[PIXEL-CAPTURE] v%u xyzw=(%g,%g,%g,%g) color=%08X fog=%g uv0=(%g,%g) uv1=(%g,%g) uv2=(%g,%g) uv3=(%g,%g)\n",
                      v, out[v].x, out[v].y, out[v].z, out[v].rhw, out[v].color, out[v].fog,
                      out[v].u, out[v].v, out[v].extra_uv[0][0], out[v].extra_uv[0][1],
                      out[v].extra_uv[1][0], out[v].extra_uv[1][1], out[v].extra_uv[2][0], out[v].extra_uv[2][1]);
      }
    }
    /* Draw */
    if (pgraph_apply_cull_state(dev, (D3DPRIMITIVETYPE)actual_prim_type)) {
        if(gpu_program) {
            memcpy(gpu_constants.constants,g_pg.transform_constant,sizeof(gpu_constants.constants));
            d3d8_DebugGetViewportSize(&gpu_constants.viewport[0],&gpu_constants.viewport[1]);
            gpu_constants.viewport[2]=g_pg.vp_scale[2]>1?g_pg.vp_scale[2]:1;
            gpu_constants.fog[0]=g_pg.fog_params[0];gpu_constants.fog[1]=g_pg.fog_params[1];
            gpu_constants.fog[2]=(float)g_pg.fog_enable;gpu_constants.fog[3]=(float)g_pg.fog_mode;
            HRESULT result=d3d8_nv2a_gpu_draw(gpu_program,&gpu_constants,
                (D3DPRIMITIVETYPE)actual_prim_type,prim_count,is_quads?g_gpu_inputs:src,g_pg.vert_stride*4u);
            if(FAILED(result)) {
                fprintf(stderr,"[GPU-VSH] draw failed %08lX; reverting to CPU vertex processing\n",(unsigned long)result);
                g_gpu_draw_failed = 1;
                submit_draw();
                return;
            }
        } else dev->lpVtbl->DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)actual_prim_type,
                                    prim_count, out, sizeof(OutputVertex));
    }
    if (pixel_capture) {
        char name[512]; snprintf(name, sizeof(name), "%s.after.bmp", pixel_capture);
        d3d8_DebugDumpRuntimeTexture(g_scene_bound_target ? g_scene_bound_target->texture : NULL, name);
    }
    g_rp_phase = 1; /* back to pushbuffer decode */
    g_vtx_trace = 0;
    pgraph_scene_surface_drawn(tex_bound ? (g_pg.tex[0].offset & 0x03FFFFFFu)
                                        : 0u);

    ++g_geom.inline_drawn;
    /* Which textures actually reach a draw.  A texture that uploads but never
     * appears here is loaded and then never used, which is a different problem
     * from one that fails to load. */
    { static uint32_t off[24]; static unsigned cnt[24]; static unsigned used;
      uint32_t o0 = g_pg.tex[0].enabled ? (g_pg.tex[0].offset & 0x03FFFFFFu) : 0u;
      unsigned k;
      for (k = 0; k < used; ++k) if (off[k] == o0) break;
      if (k == used && used < 24u) { off[used] = o0; cnt[used] = 0; used++; }
      if (k < 24u) cnt[k]++;
      { static unsigned n;
        if (++n % 20000u == 0u) {
            fprintf(stderr, "[INFO PGRAPH-TEXUSE] draws by texture:\n");
            for (k = 0; k < used; ++k)
                fprintf(stderr, "[INFO PGRAPH-TEXUSE]   %08X x%u\n",
                        off[k], cnt[k]);
        } } }
    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += num_verts;

    if (g_pg.stats.draw_calls <= 5 || (g_pg.stats.draw_calls % 1000) == 0) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw #%u: %u verts, prim=%d, prims=%u\n",
                g_pg.stats.draw_calls, num_verts, g_pg.d3d_prim_type, prim_count);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Method Handler
 * ══════════════════════════════════════════════════════════════════════ */

/* Uncapped accessor for the sampler.  Every progress figure in this
 * investigation has at some point come from a capped log or a periodic warning
 * stream and been misread as a measurement; this is the raw counter. */
/* RP_* values are decoded by rp_phase_name() in main.c. */
unsigned nv2a_frame_count(void)
{
    return (unsigned)g_pg.stats.frames;
}

/* ---- temporary: retire-token census ----------------------------------
 * S, the counter sub_0053C190 spins on, is advanced only by
 * NV097_BACK_END_WRITE_SEMAPHORE_RELEASE.  The question is whether the
 * releases stop being emitted, stop being parsed, or stop being written,
 * so all three are counted separately and the token value is recorded --
 * the guest supplies it and it increases by 2 per allocation, so the
 * token IS the allocation id. */
static unsigned long long g_sem_parsed, g_sem_written, g_sem_dropped;
static unsigned long long g_sem_ctx_sets, g_sem_ctx_unresolved;
unsigned long long g_dma_ctx_unresolved;   /* any slot, any subchannel */
unsigned long long g_pg_offset_rejected;   /* offset outside the DMA limit */
unsigned long long g_pg_unbound_dispatch;  /* method on an unbound subchannel */
static uint32_t g_sem_first_param, g_sem_last_written, g_sem_first_dropped;
static uint32_t g_sem_last_addr, g_sem_last_base;
static uint32_t g_sem_first_drop_handle, g_sem_last_good_handle;
static int g_sem_first_drop_sub = -1, g_sem_last_good_sub = -1;
static uint32_t g_sem_first_drop_R, g_sem_first_drop_S;

static uint32_t sem_dev_field(uint32_t off)
{
    uint32_t dev = *(volatile uint32_t *)((uintptr_t)0x005499E8u
                                          + (uintptr_t)g_xbox_mem_offset);
    if (!dev) return 0u;
    return *(volatile uint32_t *)((uintptr_t)(dev + off)
                                  + (uintptr_t)g_xbox_mem_offset);
}

static uint32_t sem_read_S(void)
{
    uint32_t p = sem_dev_field(0x30u);
    if (!p) return 0u;
    return *(volatile uint32_t *)((uintptr_t)p + (uintptr_t)g_xbox_mem_offset);
}

void nv2a_sem_report(void)
{
    unsigned long long expected = 0;
    if (g_sem_written && g_sem_last_written >= g_sem_first_param)
        expected = (g_sem_last_written - g_sem_first_param) / 2u + 1u;
    fprintf(stderr,
            "[SEM] SET_CONTEXT_DMA_SEMAPHORE: %llu sets, %llu unresolved\n"
            "[SEM] releases parsed=%llu written=%llu dropped=%llu\n"
            "[SEM] token first=%u last written=%u  first dropped=%u\n"
            "[SEM] tokens implied by written range=%llu\n"
            "[SEM] last good: sub=%d handle=0x%08X base=0x%08X addr=0x%08X\n"
            "[SEM] first drop: sub=%d handle=0x%08X R=0x%08X S=0x%08X\n"
            "[SEM] now: R=[dev+0x2C]=0x%08X S=0x%08X\n",
            g_sem_ctx_sets, g_sem_ctx_unresolved,
            g_sem_parsed, g_sem_written, g_sem_dropped,
            g_sem_first_param, g_sem_last_written, g_sem_first_dropped,
            expected,
            g_sem_last_good_sub, g_sem_last_good_handle, g_sem_last_base,
            g_sem_last_addr,
            g_sem_first_drop_sub, g_sem_first_drop_handle,
            g_sem_first_drop_R, g_sem_first_drop_S,
            sem_dev_field(0x2Cu), sem_read_S());
    {   extern unsigned long long g_pb_skip_events, g_pb_skip_bytes,
               g_pb_skip_retires, g_pb_desyncs, g_pb_resync_fires;
        fprintf(stderr,
                "[SEM] never walked: %llu regions, %llu bytes, %llu retire releases inside\n"
                "[SEM] walker: %llu swallowed real headers, %llu resync fires\n",
                g_pb_skip_events, g_pb_skip_bytes, g_pb_skip_retires,
                g_pb_desyncs, g_pb_resync_fires); }
    fprintf(stderr, "[SEM] invariants: %llu offsets rejected, %llu methods on unbound subchannels\n",
            g_pg_offset_rejected, g_pg_unbound_dispatch);
    {   extern unsigned long long g_pb_jmp_old_ok, g_pb_jmp_old_bad,
               g_pb_jmp_long_ok, g_pb_jmp_long_bad, g_pb_jmp_budget_hit;
        fprintf(stderr, "[SEM] jumps: old %llu ok / %llu rejected, long %llu ok / %llu rejected, %llu over budget\n",
                g_pb_jmp_old_ok, g_pb_jmp_old_bad,
                g_pb_jmp_long_ok, g_pb_jmp_long_bad, g_pb_jmp_budget_hit); }
    fflush(stderr);
}
/* For the pushbuffer walker diagnostics: whether SET_OBJECT has ever bound
 * anything in this subchannel. */
int nv2a_pg_subchannel_bound(int sub)
{
    if (sub < 0 || sub >= 8) return 0;
    return g_pg.subchannel_object[sub] != 0u;
}

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    { extern void nv2a_draw_census_method(uint32_t);
      nv2a_draw_census_method(method); }
    /* Invariant: class-specific methods require an object bound in this
     * subchannel.  SET_OBJECT (0x0000) is what binds it, so it is the only
     * method allowed through beforehand.  A payload word that happens to
     * satisfy the header bitmask therefore cannot become a command on a
     * subchannel the title never set up. */
    if (method != 0x0000u && subchannel >= 0 && subchannel < 8 &&
        g_pg.subchannel_object[subchannel] == 0u) {
        static unsigned logs;
        ++g_pg_unbound_dispatch;
        if (logs++ < 12u)
            fprintf(stderr, "[NV2A-INV] method %04X param %08X on unbound subchannel %d ignored\n", method, param, subchannel);
        return 1;
    }
    /* TEMPORARY: how often the notify family goes past.
     *
     * The title arms every PGRAPH interrupt and its service routine queues the
     * DPC that drives the presentation counter -- about 15,000 times a second
     * on hardware.  PFIFO's enables are all error conditions, so the
     * per-operation source has to be PGRAPH, and NOTIFY is its only
     * per-operation bit.  Whichever of these goes past at that rate is the
     * event to raise the interrupt on. */
    { static unsigned n_total, n_noop, n_notify, n_wait, n_dma;
      static ULONGLONG t0;
      if (t0 == 0ull) t0 = GetTickCount64();
      ++n_total;
      if (method == 0x0100u) n_noop++;
      else if (method == 0x0104u) n_notify++;
      else if (method == 0x0110u) n_wait++;
      else if (method == 0x0180u) n_dma++;
      if ((n_total % 300000u) == 0u) {
          ULONGLONG ms = GetTickCount64() - t0;
          if (ms == 0ull) ms = 1ull;
          fprintf(stderr, "[NOTIFY] %llums, %u methods: NO_OP=%u (%.0f/s)  "
                  "NOTIFY=%u (%.0f/s)  WAIT_IDLE=%u (%.0f/s)  SET_DMA_NOTIFIES=%u\n",
                  (unsigned long long)ms, n_total,
                  n_noop,   n_noop   * 1000.0 / (double)ms,
                  n_notify, n_notify * 1000.0 / (double)ms,
                  n_wait,   n_wait   * 1000.0 / (double)ms,
                  n_dma);
          fflush(stderr);
      } }

    if (!g_pg.initialized)
        return 0;

    g_pg.stats.methods_handled++;

    if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET &&
        method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 16u * 4u) {
        g_pg.array_offset[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4u] =
            param;
        g_pg.gather_prepared = 0;
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT &&
        method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 16u * 4u) {
        g_pg.array_format[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4u] =
            param;
        g_pg.gather_prepared = 0;
        return 1;
    }

    /* NV097_SET_TRANSFORM_CONSTANT_LOAD selects the persistent write cursor.
     * Window offsets select xyzw; completing w advances the cursor even
     * when the next packet restarts at 0x0B80. */
    if (method == NV097_SET_TRANSFORM_CONSTANT_LOAD) {
        if (param < 256u) ++g_kc_load_hist[param];
        g_pg.transform_constant_load = param;
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT &&
        method < NV097_SET_TRANSFORM_CONSTANT + 0x80u) {
        uint32_t word = (method - NV097_SET_TRANSFORM_CONSTANT) / 4u;
        uint32_t constant = g_pg.transform_constant_load;
        uint32_t lane = word & 3u;

        if (constant < 192u) {
            g_pg.transform_constant[constant][lane] = param;
            ++g_kc_writes[constant]; ++g_kc_total;
            if ((constant == 148u || constant == 149u)) {
                static unsigned logs;
                if (logs++ < 12u) {
                    union { uint32_t u; float f; } cv; cv.u = param;
                    fprintf(stderr, "[KCW] c%u.%u = %08X (%.4f)  load=%u\n",
                            constant, lane, param, cv.f,
                            g_pg.transform_constant_load);
                }
            }
            g_pg.transform_constant_seen[constant / 32u] |=
                1u << (constant & 31u);
        }
        /* The method selects the component, not the constant offset. Each
         * completed vec4 advances the persistent load cursor, including
         * across packets that restart at method 0x0B80. See xemu's
         * SET_TRANSFORM_CONSTANT handler in hw/xbox/nv2a/pgraph/pgraph.c. */
        if (lane == 3u)
            g_pg.transform_constant_load = constant + 1u;
        return 1;
    }

    /* Each light owns an 0x80-byte NV097 method block at 0x1000. */
    if (method >= NV097_SET_LIGHT_AMBIENT_COLOR && method < 0x1400u) {
        uint32_t offset = method - NV097_SET_LIGHT_AMBIENT_COLOR;
        uint32_t light = offset / 0x80u;
        uint32_t word = (offset % 0x80u) / 4u;
        g_pg.light_state[light][word] = param;
        g_pg.lighting_methods_seen |= 0x00000100u << light;
        return 1;
    }

    /* Back-light colors occupy eight 0x40-byte blocks at 0x0C00. */
    if (method >= NV097_SET_BACK_LIGHT_AMBIENT_COLOR && method < 0x0E00u) {
        uint32_t offset = method - NV097_SET_BACK_LIGHT_AMBIENT_COLOR;
        uint32_t light = offset / 0x40u;
        uint32_t word = (offset % 0x40u) / 4u;
        if (light < 8u)
            g_pg.back_light_state[light][word] = param;
        g_pg.lighting_methods_seen |= 0x00010000u << light;
        return 1;
    }

    if (method >= NV097_SET_MATERIAL_EMISSION &&
        method < NV097_SET_MATERIAL_EMISSION + 12u) {
        g_pg.material_emission[
            (method - NV097_SET_MATERIAL_EMISSION) / 4u] = param;
        g_pg.lighting_methods_seen |= 0x00000008u;
        return 1;
    }
    if (method >= NV097_SET_TEXTURE_SET_BUMP_ENV_MAT && method <= 0x1BFCu) {
        unsigned stage = (method - 0x1B00u) / 0x40u;
        unsigned local = (method - 0x1B00u) % 0x40u;
        if (stage < 4u && local >= 0x28u && !(local & 3u)) {
            g_pg.bump_env[stage][(local - 0x28u) / 4u] = param;
            return 1;
        }
    }
    if (method >= NV097_SET_SPECULAR_PARAMS &&
        method < NV097_SET_SPECULAR_PARAMS + 28u) {
        g_pg.specular_params[
            (method - NV097_SET_SPECULAR_PARAMS) / 4u] = param;
        g_pg.lighting_methods_seen |= 0x00000040u;
        return 1;
    }
    if (method >= NV097_SET_SCENE_AMBIENT_COLOR &&
        method < NV097_SET_SCENE_AMBIENT_COLOR + 12u) {
        g_pg.scene_ambient[
            (method - NV097_SET_SCENE_AMBIENT_COLOR) / 4u] = param;
        g_pg.lighting_methods_seen |= 0x00000080u;
        return 1;
    }

    /* ── Object binding (method 0x0000) ──
     *
     * Binds a graphics object to this subchannel.  The parameter is a RAMHT
     * handle, not an address, so it has to be resolved through the channel's
     * hash table to learn which object it names.  Recording it is what lets a
     * subchannel's methods be interpreted against the class actually bound
     * there rather than assumed to be Kelvin.
     *
     * Note this is 0x0000; the older comment further down calling 0x0180
     * "SET_OBJECT" was wrong -- 0x0180 is SET_CONTEXT_DMA_NOTIFIES. */
    if (method == 0x0000u) {
        uint32_t context = 0;
        uint32_t instance = nv2a_ramht_lookup(param, &context);

        if (subchannel >= 0 && subchannel < 8) {
            g_pg.subchannel_object[subchannel] = param;
            g_pg.subchannel_instance[subchannel] = instance;
        }
        {
            static unsigned logs;
            if (logs++ < 16u)
                fprintf(stderr, "[NV2A-PB] SET_OBJECT sub=%d handle=0x%08X "
                        "-> instance=0x%05X context=0x%08X%s\n",
                        subchannel, param, instance, context,
                        instance ? "" : "  (not in RAMHT)");
        }
        return 1;
    }

    /* ── Context DMA binding (0x0180 .. 0x01A4) ──
     *
     * NOTIFIES, A, B, STATE, COLOR, ZETA, VERTEX_A, VERTEX_B, SEMAPHORE and
     * REPORT.  Each names a DMA object by handle; the object carries the base
     * address and limit of the guest memory that engine reads or writes.
     * Surface and vertex offsets that arrive later are relative to these, so
     * resolving them is what makes those offsets mean anything. */
    if (method >= 0x0180u && method <= 0x01A4u && (method & 3u) == 0u) {
        uint32_t slot = (method - 0x0180u) / 4u;
        uint32_t base = 0, limit = 0;
        int resolved = nv2a_dma_from_handle(param, &base, &limit);

        /* An unresolvable handle leaves the existing binding alone.
         *
         * Substituting 0 turned a handle this runtime could not look up
         * into a poisoned context that silently discarded every write
         * through it, and the release path still reported success, so the
         * loss never appeared in the handled/unhandled census.  Whatever
         * the reason a handle does not resolve, destroying known-good
         * PGRAPH state is not a defensible response to it. */
        if (subchannel >= 0 && subchannel < 8 &&
            slot < sizeof(g_pg.dma_context[0]) / sizeof(g_pg.dma_context[0][0])) {
            if (resolved) {
                g_pg.dma_context_handle[subchannel][slot] = param;
                g_pg.dma_context[subchannel][slot] = base;
                g_pg.dma_context_limit[subchannel][slot] = limit;
            } else {
                static unsigned unresolved_logs;
                ++g_dma_ctx_unresolved;
                if (unresolved_logs++ < 16u)
                    fprintf(stderr, "[NV2A-DMA] sub=%d slot=%u handle=0x%08X does not resolve; keeping base=0x%08X\n",
                            subchannel, slot, param,
                            g_pg.dma_context[subchannel][slot]);
            }
        }
        if (slot == (NV097_SET_CONTEXT_DMA_SEMAPHORE - 0x0180u) / 4u) {
                                       /* the retire target */
            ++g_sem_ctx_sets;
            if (!resolved) ++g_sem_ctx_unresolved;
            fprintf(stderr, "[SEM] bind #%llu sub=%d handle=0x%08X -> base=0x%08X %s  R=0x%08X S=0x%08X\n",
                    g_sem_ctx_sets, subchannel, param, base,
                    resolved ? "resolved" : "*** UNRESOLVED, binding kept ***",
                    sem_dev_field(0x2Cu), sem_read_S());
            fflush(stderr);
        }
        {
            static const char *const names[10] = {
                "NOTIFIES", "A", "B", "STATE", "COLOR",
                "ZETA", "VERTEX_A", "VERTEX_B", "SEMAPHORE", "REPORT"
            };
            static unsigned logs;
            if (logs++ < 24u)
                fprintf(stderr, "[NV2A-PB] SET_CONTEXT_DMA_%s sub=%d "
                        "handle=0x%08X -> base=0x%08X limit=0x%08X%s\n",
                        slot < 10u ? names[slot] : "?", subchannel, param,
                        base, limit, resolved ? "" : "  (unresolved)");
        }
        return 1;
    }

    /* ── Semaphore release ──
     *
     * This is how the title learns the GPU has caught up: it keeps a submitted
     * counter of its own and waits for the engine to write a matching completed
     * counter into guest memory.  sub_0053C190 spins on exactly that, and both
     * methods were being swallowed by the 0x1D60..0x1EA0 "safely ignored"
     * range, so the counter never moved.
     *
     * Writing it as soon as the method is parsed is correct here rather than
     * optimistic: push buffers are translated synchronously, so by the time
     * this method is reached everything queued ahead of it really has been
     * carried out. */
    if (method == NV097_SET_SEMAPHORE_OFFSET) {
        if (subchannel >= 0 && subchannel < 8) {
            /* The DMA object carries base and limit precisely so the
             * engine can bound what is written through it.  This runtime
             * was discarding the limit, so a malformed header could move
             * the retire target anywhere and every later release landed
             * at a valid-looking wrong address, counted as written, and
             * advanced nothing.  Reject the offset instead and keep the
             * one that was working. */
            enum { SEM_SLOT = (NV097_SET_CONTEXT_DMA_SEMAPHORE - 0x0180u) / 4u };
            uint32_t lim = g_pg.dma_context_limit[subchannel][SEM_SLOT];
            if (lim != 0u && (param > lim || param + 4u > lim + 1u)) {
                static unsigned logs;
                ++g_pg_offset_rejected;
                if (logs++ < 12u)
                    fprintf(stderr, "[NV2A-INV] sub=%d SET_SEMAPHORE_OFFSET 0x%08X outside DMA limit 0x%08X; keeping 0x%08X\n",
                            subchannel, param, lim,
                            g_pg.semaphore_offset[subchannel]);
                return 1;
            }
            if (g_pg.semaphore_offset[subchannel] != param) {
                static unsigned logs;
                if (logs++ < 16u)
                    fprintf(stderr, "[NV2A-DMA] sub=%d SET_SEMAPHORE_OFFSET 0x%08X -> 0x%08X\n",
                            subchannel, g_pg.semaphore_offset[subchannel],
                            param);
            }
            g_pg.semaphore_offset[subchannel] = param;
        }
        return 1;
    }
    if (method == NV097_BACK_END_WRITE_SEMAPHORE_RELEASE) {
        ++g_sem_parsed;
        enum { SEMAPHORE_SLOT =
                   (NV097_SET_CONTEXT_DMA_SEMAPHORE - 0x0180u) / 4u };
        int sc = (subchannel >= 0 && subchannel < 8) ? subchannel : 0;
        uint32_t base = g_pg.dma_context[sc][SEMAPHORE_SLOT];
        uint32_t addr = base + g_pg.semaphore_offset[sc];

        /* Only write somewhere the guest could legitimately have asked for. */
        if (addr >= 0x00010000u && addr + 4u <= 0x04000000u) {
            *(volatile uint32_t *)((uintptr_t)addr +
                                   (uintptr_t)g_xbox_mem_offset) = param;
            ++g_sem_written;
            if (g_sem_written == 1ull) g_sem_first_param = param;
            g_sem_last_written = param;
            g_sem_last_addr = addr;
            g_sem_last_base = base;
            g_sem_last_good_sub = subchannel;
            g_sem_last_good_handle = g_pg.dma_context_handle[sc][SEMAPHORE_SLOT];
            {
                static unsigned logs;
                if (logs++ < 8u)
                    fprintf(stderr, "[NV2A-SEM] release 0x%08X <- %u "
                            "(base=0x%08X offset=0x%08X)\n",
                            addr, param, base, g_pg.semaphore_offset[sc]);
            }
        } else {
            static unsigned logs;
            ++g_sem_dropped;
            if (g_sem_dropped == 1ull) {
                g_sem_first_dropped = param;
                g_sem_first_drop_sub = subchannel;
                g_sem_first_drop_handle =
                    g_pg.dma_context_handle[sc][SEMAPHORE_SLOT];
                g_sem_first_drop_R = sem_dev_field(0x2Cu);
                g_sem_first_drop_S = sem_read_S();
            }
            if (logs++ < 12u)
                fprintf(stderr, "[SEM] DROP #%llu token=%u sub=%d handle=0x%08X base=0x%08X offset=0x%08X addr=0x%08X  R=0x%08X S=0x%08X\n",
                        g_sem_dropped, param, subchannel,
                        g_pg.dma_context_handle[sc][SEMAPHORE_SLOT],
                        base, g_pg.semaphore_offset[sc], addr,
                        sem_dev_field(0x2Cu), sem_read_S());
        }
        return 1;
    }

    /* ── Vertex program upload ──
     *
     * SET_TRANSFORM_PROGRAM_LOAD points at an instruction slot; the 32-dword
     * window at 0x0B00 then appends microcode there, four dwords per 128-bit
     * instruction, advancing as it goes.  Without this the title's vertex
     * programs were dropped entirely and the geometry that follows had no
     * transform to run through.
     *
     * These three registers were previously swallowed by the 0x1D60..0x1EA0
     * "safely ignored" range, and the comment naming 0x0394/0x0398/0x039C as
     * the transform-program registers is wrong -- the real ones are
     * 0x1E94/0x1E98/0x1E9C, as nv2a_regs.h defines them. */
    if (method == NV097_SET_TRANSFORM_PROGRAM_LOAD) {
        g_pg.transform_program_load = param;
        g_pg.transform_program_sub = 0;
        return 1;
    }
    if (method == NV097_SET_TRANSFORM_PROGRAM_START) {
        g_pg.transform_program_start = param;
        /* START marks the end of an upload: report what was captured so the
         * microcode can be eyeballed before it is handed to the translator. */
        if (g_pg.transform_program_dirty) {
            static unsigned logs;
            if (logs++ < 2u || (g_pg.transform_program_count >= 20u && logs < 60u)) {
                uint32_t i;
                fprintf(stderr, "[NV2A-VSH] program uploaded: %u instructions, "
                        "start=%u mode=0x%08X\n", g_pg.transform_program_count,
                        param, g_pg.transform_execution_mode);
                for (i = 0; i < g_pg.transform_program_count && i < 160u; i++)
                    fprintf(stderr, "[NV2A-VSH]   insn[%u] %08X %08X %08X %08X\n",
                            i, g_pg.transform_program[i][0],
                            g_pg.transform_program[i][1],
                            g_pg.transform_program[i][2],
                            g_pg.transform_program[i][3]);
            }
        }
        return 1;
    }
    if (method == NV097_SET_TRANSFORM_EXECUTION_MODE) {
        g_pg.transform_execution_mode = param;
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_PROGRAM &&
        method < NV097_SET_TRANSFORM_PROGRAM + 0x80u) {
        uint32_t slot = g_pg.transform_program_load;

        if (slot < 136u) {
            g_pg.transform_program[slot][g_pg.transform_program_sub] = param;
            if (slot + 1u > g_pg.transform_program_count)
                g_pg.transform_program_count = slot + 1u;
            g_pg.transform_program_dirty = 1;
        }
        /* Advance a dword at a time; a full instruction moves the slot on.
         * The pointer is kept here rather than re-derived from the method
         * offset because a program can span several packets, each of which
         * restarts that offset at zero. */
        if (++g_pg.transform_program_sub == 4u) {
            g_pg.transform_program_sub = 0;
            g_pg.transform_program_load = slot + 1u;
            /* Report the first few completed instructions so the captured
             * microcode can be checked before it is handed to the translator.
             * Done here rather than at SET_TRANSFORM_PROGRAM_START, which the
             * title issues before the upload rather than after. */
            if (slot < 6u) {
                static unsigned logs;
                if (logs++ < 6u)
                    fprintf(stderr, "[NV2A-VSH] insn[%u] %08X %08X %08X %08X\n",
                            slot, g_pg.transform_program[slot][0],
                            g_pg.transform_program[slot][1],
                            g_pg.transform_program[slot][2],
                            g_pg.transform_program[slot][3]);
            }
        }
        return 1;
    }

    int immediate = nv2a_immediate_write(g_pg.immediate_value, method, param);
    if (immediate) {
        if (immediate == 2) gather_immediate_vertex();
        return 1;
    }

    switch (method) {

    /* ── Draw Begin/End ── */
    case NV097_SET_BEGIN_END:
        if (param == 0) {
            /* END: submit accumulated vertices */
            if (g_pg.in_draw) {
                submit_draw();
                g_pg.in_draw = 0;
            }
        } else {
            /* BEGIN: start new draw */
            g_pg.in_draw = 1;
            g_pg.draw_mode = param;
            g_pg.d3d_prim_type = nv2a_draw_mode_to_d3d(param);
            /* The layout comes from the vertex-attribute format registers,
             * which the title sets before the draw.  Guessing it from the
             * primitive mode -- which is what the fallback below does -- was
             * wrong for the title's own UI and cost every menu quad its
             * coordinates. */
            g_pg.vert_stride = inline_layout_from_formats();
            g_pg.inline_layout_valid = (g_pg.vert_stride != 0u);
            if (!g_pg.inline_layout_valid) {
                g_pg.vert_stride =
                    (param == 2 || param == 3 || param == 8 || param == 9)
                    ? 4u : INLINE_VERT_DWORDS;
                if (param == 9)
                    g_pg.vert_stride = 5u;
            }
            g_pg.inline_count = 0;
            g_pg.indexed_draw_invalid = 0;
            g_pg.immediate_draw = 0;
        }
        return 1;

    /* ── Inline Vertex Data ── */
    case NV097_INLINE_ARRAY:
        if (g_pg.in_draw && g_pg.inline_count < MAX_INLINE_VERTS * INLINE_VERT_DWORDS) {
            g_pg.inline_data[g_pg.inline_count++] = param;
        }
        return 1;

    case NV097_ARRAY_ELEMENT16:
    case NV097_ARRAY_ELEMENT32:
        if (g_pg.in_draw && !g_pg.indexed_draw_invalid) {
            int ok = gather_array_elements(param, method == NV097_ARRAY_ELEMENT32);
            if (!ok) {
                static unsigned logs;
                g_pg.indexed_draw_invalid = 1;
                ++g_geom.array_skipped;
                if (logs++ < 16u)
                    fprintf(stderr, "[INDEX-REJECT] method=%04X value=%08X "
                            "stride=%u count=%u format0=%08X offset0=%08X\n",
                            method, param, g_pg.vert_stride, g_pg.inline_count,
                            g_pg.array_format[0], g_pg.array_offset[0]);
            }
        }
        return 1;

    /* ── Draw from the currently bound vertex stream ──
     * Bits 0..23 contain the first vertex and bits 24..31 contain
     * (vertex_count - 1).  Unlike INLINE_ARRAY this method submits
     * immediately; SET_BEGIN_END only supplies the primitive topology. */
    case NV097_DRAW_ARRAYS:
        if (g_pg.in_draw && vsh_program_active()) {
            uint32_t first = param & 0x00FFFFFFu;
            uint32_t count = (param >> 24) + 1u;
            int gathered = gather_array_vertices(first, count);
            if (pgraph_seq_active() || !gathered)
                fprintf(stderr, "[ARRAY-VSH] first=%u count=%u gathered=%d stride=%u format0=%08X\n",
                        first, count, gathered, g_pg.vert_stride, g_pg.array_format[0]);
            if (!gathered) ++g_geom.array_skipped;
            return 1;
        }
        if (g_pg.in_draw) {
            static unsigned draw_arrays_trace_count;
            static unsigned frontend_array_trace_count;
            IDirect3DDevice8 *dev = xbox_GetD3DDevice();
            uint32_t start_vertex = param & 0x00FFFFFFu;
            uint32_t vertex_count = (param >> 24) + 1u;
            uint32_t primitive_count = 0;
            HRESULT hr = E_FAIL;

            if (frontend_array_trace_count++ < 8u) {
                uint32_t address =
                    g_pg.array_offset[0] & 0x03FFFFFFu;
                uint32_t format = g_pg.array_format[0];
                uint32_t stride = format >> 8;
                uint32_t size = (format >> 4) & 0xFu;
                uint32_t type = format & 0xFu;
                uint64_t first = (uint64_t)address +
                                 (uint64_t)start_vertex * stride;
                uint32_t word0 = 0, word1 = 0, word2 = 0, word3 = 0;

                if (address >= 0x00010000u && first + 16u <= 0x04000000u) {
                    const uint32_t *live = (const uint32_t *)(uintptr_t)(
                        (uintptr_t)first + (uintptr_t)g_xbox_mem_offset);
                    word0 = live[0];
                    word1 = live[1];
                    word2 = live[2];
                    word3 = live[3];
                }
                fprintf(stderr,
                        "[DEBUG PGRAPH-ARRAY] start=%u count=%u "
                        "a0=%08X/%08X type=%u size=%u stride=%u "
                        "a1=%08X/%08X data=%08X,%08X,%08X,%08X "
                        "pos=%g,%g,%g\n",
                        start_vertex, vertex_count,
                        g_pg.array_offset[0], g_pg.array_format[0],
                        type, size, stride,
                        g_pg.array_offset[1], g_pg.array_format[1],
                        word0, word1, word2, word3,
                        u2f(word0), u2f(word1), u2f(word2));
            }

            switch (g_pg.d3d_prim_type) {
            case D3DPT_POINTLIST:     primitive_count = vertex_count; break;
            case D3DPT_LINELIST:      primitive_count = vertex_count / 2u; break;
            case D3DPT_LINESTRIP:     primitive_count = vertex_count > 1u ? vertex_count - 1u : 0u; break;
            case D3DPT_TRIANGLELIST:  primitive_count = vertex_count / 3u; break;
            case D3DPT_TRIANGLESTRIP:
            case D3DPT_TRIANGLEFAN:   primitive_count = vertex_count > 2u ? vertex_count - 2u : 0u; break;
            default:                  primitive_count = 0; break;
            }

            if (dev != NULL && primitive_count != 0) {
                static unsigned frontend_offscreen_array_logged;
                FrontendRuntimeTarget *runtime_target = NULL;
                int runtime_target_requested = 0;
                int runtime_target_bound = 0;
                UINT runtime_width = d3d8_GetBackbufferWidth();
                UINT runtime_height = d3d8_GetBackbufferHeight();
                uint32_t address =
                    g_pg.array_offset[0] & 0x03FFFFFFu;
                uint32_t stride = g_pg.array_format[0] >> 8;
                uint64_t first = (uint64_t)address +
                                 (uint64_t)start_vertex * stride;
                uint64_t end = first + (uint64_t)vertex_count * stride;

                if (g_pg.surface_color_offset >= 0x00010000u &&
                    g_pg.surface_color_offset < 0x04000000u &&
                    frontend_runtime_target_dimensions(
                        g_pg.surface_color_offset,
                        &runtime_width, &runtime_height)) {
                    uint32_t submission = g_pg.stats.frames + 1u;
                    runtime_target_requested = 1;
                    runtime_target = frontend_runtime_target_acquire(
                        dev, g_pg.surface_color_offset,
                        runtime_width, runtime_height);
                    /* An RTV cannot remain bound as a pixel-shader input.
                     * Drop the prior compositor sampling view before the
                     * same live texture becomes the next render target. */
                    dev->lpVtbl->SetTexture(dev, 0, NULL);
                    if (runtime_target != NULL &&
                        SUCCEEDED(d3d8_BindRuntimeRenderTexture(
                            runtime_target->texture,
                            runtime_target->last_clear_submission !=
                                submission,
                            0x00000000u))) {
                        runtime_target_bound = 1;
                        runtime_target->last_clear_submission = submission;
                    }
                    if (frontend_offscreen_array_logged++ < 8u) {
                        fprintf(stderr,
                                "[INFO PGRAPH-OFFSCREEN] bound live Frontend "
                                "DrawArrays target=%08X texture=%p bound=%d "
                                "format=%08X pitch=%08X start=%u vertices=%u\n",
                                g_pg.surface_color_offset,
                                runtime_target != NULL
                                    ? (void *)runtime_target->texture : NULL,
                                runtime_target_bound,
                                g_pg.surface_format,
                                g_pg.surface_pitch,
                                start_vertex, vertex_count);
                    }
                }
                /* Anything the Frontend paths do not claim still has to land
                 * on the title's own surface.  Otherwise it draws onto
                 * whatever is bound -- which after the flip copy is the swap
                 * chain itself, so the pass appears on screen directly and
                 * survives the next present untouched.  That is what put a
                 * 256x256 white square in the corner: it was never in the
                 * back buffer we sample at the flip, because it was painted
                 * after it. */
                if (!runtime_target_bound) {
                    runtime_target_bound = pgraph_bind_scene_surface(dev);
                    /* That call binds a target without telling this scope which
                     * one, and the block below dereferences runtime_target on the
                     * strength of the flag alone.  It stayed NULL, so the first
                     * array draw to reach here read through a null pointer -- which
                     * is what happened the moment the title got past its boot video
                     * and started drawing a real scene. */
                    if (runtime_target_bound && runtime_target == NULL)
                        runtime_target = g_scene_bound_target;
                }
                if (!((!runtime_target_requested || runtime_target_bound) &&
                      address >= 0x00010000u && stride != 0u &&
                      end <= 0x04000000u))
                    ++g_geom.array_skipped;
                if ((!runtime_target_requested || runtime_target_bound) &&
                    address >= 0x00010000u && stride != 0u &&
                    end <= 0x04000000u) {
                    static int frontend_array_pipeline_logged;
                    static unsigned frontend_array_probe_done;
                    static unsigned frontend_compositor_array_probe_done;
                    static const D3DMATRIX identity = {
                        ._11 = 1.0f, ._22 = 1.0f,
                        ._33 = 1.0f, ._44 = 1.0f
                    };
                    DWORD color_write = pgraph_color_write_mask();
                    D3DVIEWPORT8 old_viewport;
                    D3DVIEWPORT8 live_viewport;

                    memset(&old_viewport, 0, sizeof(old_viewport));
                    dev->lpVtbl->GetViewport(dev, &old_viewport);
                    live_viewport.X = 0;
                    live_viewport.Y = 0;
                    live_viewport.Width = runtime_target_bound
                        ? runtime_width : d3d8_GetBackbufferWidth();
                    live_viewport.Height = runtime_target_bound
                        ? runtime_height : d3d8_GetBackbufferHeight();
                    live_viewport.MinZ = 0.0f;
                    live_viewport.MaxZ = 1.0f;
                    dev->lpVtbl->SetViewport(dev, &live_viewport);

                    /* DrawArrays references a guest vertex stream rather
                     * than an IDirect3DVertexBuffer8.  Upload that live
                     * offline-owned range through the existing UP path.
                     *
                     * The Frontend stream has two float3 attributes at a
                     * 24-byte stride.  The immediately preceding inline
                     * draw leaves XYZRHW|DIFFUSE|TEX1 active; retaining that
                     * FVF decodes these clip-space vertices as 28-byte
                     * screen-space records and collapses the grid. */
                    dev->lpVtbl->SetTransform(
                        dev, D3DTS_WORLD, &identity);
                    dev->lpVtbl->SetTransform(
                        dev, D3DTS_VIEW, &identity);
                    dev->lpVtbl->SetTransform(
                        dev, D3DTS_PROJECTION, &identity);
                    dev->lpVtbl->SetVertexShader(
                        dev, D3DFVF_XYZ | D3DFVF_NORMAL);
                    dev->lpVtbl->SetPixelShader(dev, 0);

                    /* This path is fed directly by NV097 methods, not by the
                     * D3D8 COM state setters.  Apply the live PGRAPH state
                     * explicitly so an earlier inline pass cannot leave a
                     * texture-selected pixel stage or rejecting depth/blend
                     * state active for otherwise valid array geometry. */
                    dev->lpVtbl->SetRenderState(
                        dev, D3DRS_LIGHTING, FALSE);
                    pgraph_apply_alpha_state(dev);
                    dev->lpVtbl->SetRenderState(
                        dev, D3DRS_ALPHABLENDENABLE,
                        g_pg.blend_enable ? TRUE : FALSE);
                    if (g_pg.blend_enable) {
                        dev->lpVtbl->SetRenderState(
                            dev, D3DRS_SRCBLEND,
                            nv2a_blend_to_d3d(g_pg.blend_sfactor));
                        dev->lpVtbl->SetRenderState(
                            dev, D3DRS_DESTBLEND,
                            nv2a_blend_to_d3d(g_pg.blend_dfactor));
                    }
                    dev->lpVtbl->SetRenderState(
                        dev, D3DRS_COLORWRITEENABLE, color_write);

                    /* Bind the title's texture, as the inline path does.
                     * This path used to force DIFFUSE unconditionally, so
                     * every array draw rendered as flat vertex colour.
                     *
                     * Only where the coordinates are already normalised. A
                     * linear texture is addressed in texels, and unlike the
                     * inline path these vertices come straight out of guest
                     * memory -- rescaling them would mean a texture transform,
                     * which this D3D8 layer does not implement yet. Those stay
                     * untextured rather than sampling one clamped edge texel.
                     */
                    {
                        UINT atex_w = 0u, atex_h = 0u;
                        int atexel = 0;
                        IDirect3DTexture8 *abound =
                            pgraph_texture_for_stage(dev, 0, &atex_w, &atex_h,
                                                     &atexel);
                        /* A linear texture is addressed in texels.  These
                         * vertices come straight out of guest memory and
                         * cannot be rewritten, so scale the coordinates with a
                         * texture matrix instead. */
                        if (abound != NULL && atexel &&
                            atex_w != 0u && atex_h != 0u) {
                            D3DMATRIX m;
                            memset(&m, 0, sizeof(m));
                            m._11 = 1.0f / (float)atex_w;
                            m._22 = 1.0f / (float)atex_h;
                            m._33 = 1.0f;
                            m._44 = 1.0f;
                            dev->lpVtbl->SetTransform(dev, D3DTS_TEXTURE0, &m);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                D3DTTFF_COUNT2);
                        } else {
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                D3DTTFF_DISABLE);
                        }
                        if (abound != NULL) {
                            dev->lpVtbl->SetTexture(
                                dev, 0, (IDirect3DBaseTexture8 *)abound);
                            if (!pgraph_stage0_from_combiner(dev, 1)) {
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                            }
                        } else {
                            dev->lpVtbl->SetTexture(dev, 0, NULL);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                            dev->lpVtbl->SetTextureStageState(
                                dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
                        }
                    }
                    /* This used to disable stages 1..3 outright, which is
                     * why the scene drew with a single texture -- and for most
                     * of its geometry that one was the lookup map, not the
                     * base colour.  Bind what the title actually has on them;
                     * the helper still disables the ones it has switched off. */
                    pgraph_publish_combiners(dev);
                    pgraph_bind_extra_stages(dev);
                    d3d8_PumpWindowMessages(0);

                    /* The 128/256 pixel compositor surfaces are populated by
                     * the live position+normal stream long after the first
                     * Frontend array draw.  Record that exact current-run
                     * stream and its active transform constants once.  This
                     * is deliberately live instrumentation only: it neither
                     * reads nor persists a captured/static memory image. */
                    if (runtime_target_bound &&
                        (runtime_width != d3d8_GetBackbufferWidth() ||
                         runtime_height != d3d8_GetBackbufferHeight()) &&
                        !frontend_compositor_array_probe_done++) {
                        unsigned attribute;
                        unsigned constant;

                        fprintf(stderr,
                                "[INFO PGRAPH-COMPOSITOR-ARRAY] target=%08X "
                                "size=%ux%u start=%u vertices=%u primitive=%d "
                                "color=%08X depth=%d blend=%d\n",
                                g_pg.surface_color_offset,
                                (unsigned)runtime_width,
                                (unsigned)runtime_height,
                                start_vertex, vertex_count,
                                g_pg.d3d_prim_type, color_write,
                                g_pg.depth_test, g_pg.blend_enable);
                        for (attribute = 0; attribute < 2u; ++attribute) {
                            uint32_t attribute_address =
                                g_pg.array_offset[attribute] & 0x03FFFFFFu;
                            uint32_t attribute_stride =
                                g_pg.array_format[attribute] >> 8;
                            uint64_t attribute_first =
                                (uint64_t)attribute_address +
                                (uint64_t)start_vertex * attribute_stride;

                            if (attribute_first + 24u <= 0x04000000u &&
                                attribute_stride >= 12u) {
                                const uint32_t *words =
                                    (const uint32_t *)(uintptr_t)(
                                        (uintptr_t)attribute_first +
                                        (uintptr_t)g_xbox_mem_offset);
                                fprintf(stderr,
                                        "[INFO PGRAPH-COMPOSITOR-VERTEX] "
                                        "attribute=%u offset=%08X "
                                        "format=%08X stride=%u "
                                        "v0=%08X,%08X,%08X "
                                        "v1=%08X,%08X,%08X\n",
                                        attribute,
                                        g_pg.array_offset[attribute],
                                        g_pg.array_format[attribute],
                                        attribute_stride,
                                        words[0], words[1], words[2],
                                        words[attribute_stride / 4u + 0u],
                                        words[attribute_stride / 4u + 1u],
                                        words[attribute_stride / 4u + 2u]);
                            }
                        }
                        for (constant = 0; constant < 192u; ++constant) {
                            const uint32_t *value =
                                g_pg.transform_constant[constant];
                            if ((g_pg.transform_constant_seen[
                                     constant / 32u] &
                                 (1u << (constant & 31u))) != 0u &&
                                (value[0] != 0u || value[1] != 0u ||
                                 value[2] != 0u || value[3] != 0u)) {
                                fprintf(stderr,
                                        "[INFO PGRAPH-COMPOSITOR-XF] "
                                        "c%u=%08X,%08X,%08X,%08X\n",
                                        constant, value[0], value[1],
                                        value[2], value[3]);
                            }
                        }
                    }

                    if (!frontend_array_pipeline_logged++) {
                        uint32_t live_device = 0;
                        uint32_t live_descriptor = 0;
                        uint32_t live_device_flags = 0;
                        uint32_t live_device_mode = 0;
                        uint32_t live_light_mask = 0;
                        uint32_t live_color_material = 0;
                        uint32_t live_material[3] = {0, 0, 0};
                        uint32_t live_global_color = 0;
                        uint32_t live_global_scale = 0;
                        uint32_t live_light_ptr[8] = {0};
                        const uint32_t *global_device =
                            (const uint32_t *)(uintptr_t)(
                                (uintptr_t)0x005499E8u +
                                (uintptr_t)g_xbox_mem_offset);

                        live_device = *global_device;
                        if (live_device >= 0x00010000u &&
                            live_device + 0x192Cu <= 0x04000000u) {
                            const uint8_t *device =
                                (const uint8_t *)(uintptr_t)(
                                    (uintptr_t)live_device +
                                    (uintptr_t)g_xbox_mem_offset);
                            live_device_flags =
                                *(const uint32_t *)(device + 0x0008u);
                            live_descriptor =
                                *(const uint32_t *)(device + 0x0794u);
                            live_light_mask =
                                *(const uint32_t *)(device + 0x07CCu);
                            live_color_material =
                                *(const uint32_t *)(device + 0x07D0u);
                            live_device_mode =
                                *(const uint32_t *)(device + 0x1928u);
                            live_material[0] =
                                *(const uint32_t *)(device + 0x0F30u);
                            live_material[1] =
                                *(const uint32_t *)(device + 0x0F34u);
                            live_material[2] =
                                *(const uint32_t *)(device + 0x0F38u);
                            for (unsigned light = 0; light < 8u; ++light) {
                                live_light_ptr[light] =
                                    *(const uint32_t *)(
                                        device + 0x07ACu + light * 4u);
                            }
                        }
                        live_global_color =
                            *(const uint32_t *)(uintptr_t)(
                                (uintptr_t)0x00549BC4u +
                                (uintptr_t)g_xbox_mem_offset);
                        live_global_scale =
                            *(const uint32_t *)(uintptr_t)(
                                (uintptr_t)0x00549C2Cu +
                                (uintptr_t)g_xbox_mem_offset);

                        fprintf(stderr,
                                "[INFO PGRAPH-ARRAY] isolated live Frontend "
                                "array pipeline depth=%d blend=%d alpha=%d "
                                "color-write=%X fvf=%08X viewport="
                                "%u,%u %ux%u -> %ux%u\n",
                                g_pg.depth_test, g_pg.blend_enable,
                                g_pg.alpha_test, (unsigned)color_write,
                                D3DFVF_XYZ | D3DFVF_NORMAL,
                                (unsigned)old_viewport.X,
                                (unsigned)old_viewport.Y,
                                (unsigned)old_viewport.Width,
                                (unsigned)old_viewport.Height,
                                (unsigned)live_viewport.Width,
                                (unsigned)live_viewport.Height);
                        fprintf(stderr,
                                "[INFO PGRAPH-XF] c103=%08X,%08X,%08X,%08X "
                                "c116=%08X,%08X,%08X,%08X "
                                "c190=%08X,%08X,%08X,%08X "
                                "c191=%08X,%08X,%08X,%08X\n",
                                g_pg.transform_constant[103][0],
                                g_pg.transform_constant[103][1],
                                g_pg.transform_constant[103][2],
                                g_pg.transform_constant[103][3],
                                g_pg.transform_constant[116][0],
                                g_pg.transform_constant[116][1],
                                g_pg.transform_constant[116][2],
                                g_pg.transform_constant[116][3],
                                g_pg.transform_constant[190][0],
                                g_pg.transform_constant[190][1],
                                g_pg.transform_constant[190][2],
                                g_pg.transform_constant[190][3],
                                g_pg.transform_constant[191][0],
                                g_pg.transform_constant[191][1],
                                g_pg.transform_constant[191][2],
                                g_pg.transform_constant[191][3]);
                        fprintf(stderr,
                                "[INFO PGRAPH-FIXED] device=%08X "
                                "flags=%08X descriptor=%08X mode=%08X "
                                "mask=%08X color-material=%08X "
                                "global-color=%08X scale=%08X "
                                "material=%08X,%08X,%08X\n",
                                live_device, live_device_flags,
                                live_descriptor, live_device_mode,
                                live_light_mask, live_color_material,
                                live_global_color, live_global_scale,
                                live_material[0], live_material[1],
                                live_material[2]);
                        fprintf(stderr,
                                "[INFO PGRAPH-FIXED-LIGHTS] "
                                "%08X,%08X,%08X,%08X,"
                                "%08X,%08X,%08X,%08X\n",
                                live_light_ptr[0], live_light_ptr[1],
                                live_light_ptr[2], live_light_ptr[3],
                                live_light_ptr[4], live_light_ptr[5],
                                live_light_ptr[6], live_light_ptr[7]);
                        fprintf(stderr,
                                "[INFO PGRAPH-VIEWPORT] old=%u,%u,%u,%u "
                                "live=%u,%u,%u,%u\n",
                                (unsigned)old_viewport.X,
                                (unsigned)old_viewport.Y,
                                (unsigned)old_viewport.Width,
                                (unsigned)old_viewport.Height,
                                (unsigned)live_viewport.X,
                                (unsigned)live_viewport.Y,
                                (unsigned)live_viewport.Width,
                                (unsigned)live_viewport.Height);
                        fprintf(stderr,
                                "[INFO PGRAPH-MATERIAL] tex0=%08X,%08X,%08X/%d "
                                "tex1=%08X,%08X,%08X/%d shader=%08X dot=%08X "
                                "other=%08X control=%08X final=%08X,%08X\n",
                                g_pg.tex[0].offset, g_pg.tex[0].format,
                                g_pg.tex[0].control0, g_pg.tex[0].enabled,
                                g_pg.tex[1].offset, g_pg.tex[1].format,
                                g_pg.tex[1].control0, g_pg.tex[1].enabled,
                                g_pg.shader_stage_program,
                                g_pg.dot_rgb_mapping,
                                g_pg.shader_other_stage_input,
                                g_pg.combiner_control,
                                g_pg.combiner_final0,
                                g_pg.combiner_final1);
                        fprintf(stderr,
                                "[INFO PGRAPH-COMBINER] stage0="
                                "%08X,%08X,%08X,%08X factors=%08X,%08X "
                                "stage1=%08X,%08X,%08X,%08X\n",
                                g_pg.combiner_color_icw[0],
                                g_pg.combiner_alpha_icw[0],
                                g_pg.combiner_color_ocw[0],
                                g_pg.combiner_alpha_ocw[0],
                                g_pg.combiner_factor0[0],
                                g_pg.combiner_factor1[0],
                                g_pg.combiner_color_icw[1],
                                g_pg.combiner_alpha_icw[1],
                                g_pg.combiner_color_ocw[1],
                                g_pg.combiner_alpha_ocw[1]);
                        fprintf(stderr,
                                "[INFO PGRAPH-LIGHTING] seen=%08X "
                                "enable=%08X mask=%08X control=%08X "
                                "color-material=%08X specular=%08X "
                                "emission=%08X,%08X,%08X alpha=%08X "
                                "ambient=%08X,%08X,%08X\n",
                                g_pg.lighting_methods_seen,
                                g_pg.lighting_enable,
                                g_pg.light_enable_mask,
                                g_pg.light_control,
                                g_pg.color_material,
                                g_pg.specular_enable,
                                g_pg.material_emission[0],
                                g_pg.material_emission[1],
                                g_pg.material_emission[2],
                                g_pg.material_alpha,
                                g_pg.scene_ambient[0],
                                g_pg.scene_ambient[1],
                                g_pg.scene_ambient[2]);
                        for (unsigned light = 0; light < 8u; ++light) {
                            uint32_t kind =
                                (g_pg.light_enable_mask >> (light * 2u)) & 3u;
                            const uint32_t *state = g_pg.light_state[light];
                            if (kind != 0u ||
                                (g_pg.lighting_methods_seen &
                                 (0x00000100u << light)) != 0u) {
                                fprintf(stderr,
                                        "[INFO PGRAPH-LIGHT] index=%u kind=%u "
                                        "ambient=%08X,%08X,%08X "
                                        "diffuse=%08X,%08X,%08X "
                                        "specular=%08X,%08X,%08X "
                                        "range=%08X direction=%08X,%08X,%08X "
                                        "position=%08X,%08X,%08X "
                                        "attenuation=%08X,%08X,%08X\n",
                                        light, kind,
                                        state[0], state[1], state[2],
                                        state[3], state[4], state[5],
                                        state[6], state[7], state[8],
                                        state[9],
                                        state[13], state[14], state[15],
                                        state[23], state[24], state[25],
                                        state[26], state[27], state[28]);
                            }
                        }
                    }
                    { static unsigned n, reported;
                      if ((n++ & 0xFFu) == 0u && reported < 16u &&
                          d3d8_IsDefaultRenderTargetBound() && ++reported)
                          fprintf(stderr, "[WARN PGRAPH-TARGET] array draw on "
                                  "the swap chain: rt=%08X verts=%u\n",
                                  g_pg.surface_color_offset, vertex_count); }
                    ++g_geom.array_drawn;
                    if (pgraph_seq_active()) {
                        fprintf(stderr, "[SEQ] %4u f%u ARRAY rt=%08X start=%u count=%u stride=%u program=%u mode=%u\n",
                                ++g_seq, g_pg.stats.frames, g_pg.surface_color_offset,
                                start_vertex, vertex_count, stride,
                                g_pg.transform_program_count, g_pg.transform_execution_mode);
                    }
                    pgraph_apply_depth_state(dev);
                    for (unsigned stage = 0; stage < 4; ++stage)
                        pgraph_apply_sampler_state(dev, stage);
                    hr = S_OK;
                    if (pgraph_apply_cull_state(dev, (D3DPRIMITIVETYPE)g_pg.d3d_prim_type))
                        hr = dev->lpVtbl->DrawPrimitiveUP(
                        dev, (D3DPRIMITIVETYPE)g_pg.d3d_prim_type,
                        primitive_count, (const void *)(uintptr_t)first,
                        stride);
                    dev->lpVtbl->SetViewport(dev, &old_viewport);

                    /* Sample one live array draw so the corrected diffuse
                     * selection is visible in the log without persisting a
                     * framebuffer/static-memory dump. */
                    if (!runtime_target_bound && !frontend_array_probe_done++) {
                        d3d8_DebugSampleBackbuffer("after-live-array");
                    }
                } else if (!runtime_target_requested) {
                    hr = dev->lpVtbl->DrawPrimitive(
                        dev, (D3DPRIMITIVETYPE)g_pg.d3d_prim_type,
                        start_vertex, primitive_count);
                }
                if (runtime_target_bound) {
                    d3d8_RestoreDefaultRenderTarget();
                    if (SUCCEEDED(hr) && runtime_target != NULL) {
                        g_pg.frontend_live_texture = runtime_target->texture;
                        g_pg.frontend_live_offset =
                            runtime_target->guest_offset;
                    }
                }
                if (SUCCEEDED(hr)) {
                    if (runtime_target_bound && runtime_target)
                        pgraph_display_surface_written(runtime_target->guest_offset);
                    g_pg.stats.draw_calls++;
                    g_pg.stats.vertices_submitted += vertex_count;
                    g_pg.array_drawn_this_frame = 1;
                    g_pg.array_drawn_since_present = 1;
                    ++g_pg.array_draws_since_present;
                    g_pg.last_array_submission = g_pg.stats.frames + 1u;
                }
            }

            dt_log_array(start_vertex, vertex_count, primitive_count, (uint32_t)hr);
            if (draw_arrays_trace_count++ < 400) {
                fprintf(stderr,
                        "[PGRAPH-D3D11] DrawArrays start=%u vertices=%u "
                        "prim=%d primitives=%u hr=%08X target=%08X "
                        "clip=%08X/%08X frame=%u\n",
                        start_vertex, vertex_count, g_pg.d3d_prim_type,
                        primitive_count, (uint32_t)hr,
                        g_pg.surface_color_offset,
                        g_pg.surface_clip_h, g_pg.surface_clip_v,
                        g_pg.stats.frames + 1u);
            }
        }
        return 1;

    /* ── Clear ── */
    case NV097_SET_COLOR_CLEAR_VALUE:
        g_pg.clear_color = param;
        return 1;

    case NV097_SET_CLEAR_RECT_HORIZONTAL:
        g_pg.clear_rect_h = param;
        return 1;

    case NV097_SET_CLEAR_RECT_VERTICAL:
        g_pg.clear_rect_v = param;
        return 1;

    case NV097_CLEAR_SURFACE:
    {
        static unsigned clear_after_array_log_count;
        IDirect3DDevice8 *dev = xbox_GetD3DDevice();

        if (pgraph_seq_active())
            fprintf(stderr, "[SEQ] %4u f%u CLEAR  rt=%08X mask=%08X "
                    "color=%08X\n",
                    ++g_seq, (unsigned)g_pg.stats.frames,
                    g_pg.surface_color_offset, param, g_pg.clear_color);


        /* Does the title clear both full-size surfaces, or only one?
         *
         * Binding does not clear -- NV097_CLEAR_SURFACE is the only thing that
         * does -- so a surface the title never clears keeps whatever it last
         * held.  Counting clears per surface says whether the white block is
         * redrawn every frame or simply never wiped. */
        { static uint32_t seen[8]; static uint32_t count[8];
          static unsigned seen_count; static unsigned reported;
          uint32_t off = g_pg.surface_color_offset & 0x03FFFFFFu;
          unsigned k;
          for (k = 0; k < seen_count; ++k) if (seen[k] == off) break;
          if (k == seen_count && seen_count < 8u) { seen[k] = off; ++seen_count; }
          if (k < 8u) ++count[k];
          if (g_pg.stats.frames >= 20000u && reported < 10u) {
              ++reported;
              fprintf(stderr, "[INFO PGRAPH-CLEARS] rt=%08X mask=%08X "
                      "color=%08X rect=%08X/%08X clip=%08X/%08X\n",
                      off, param, g_pg.clear_color, g_pg.clear_rect_h,
                      g_pg.clear_rect_v, g_pg.surface_clip_h,
                      g_pg.surface_clip_v);
          }
          if (g_pg.stats.frames >= 20000u && reported == 1u) {
              fprintf(stderr, "[INFO PGRAPH-CLEARS] by surface at frame %u:\n",
                      (unsigned)g_pg.stats.frames);
              for (k = 0; k < seen_count; ++k)
                  fprintf(stderr, "[INFO PGRAPH-CLEARS]   %08X x%u\n",
                          seen[k], count[k]);
          } }
        FrontendRuntimeTarget *runtime_target = NULL;
        int runtime_target_bound = 0;
        if (g_pg.array_drawn_this_frame &&
            clear_after_array_log_count++ < 16u) {
            fprintf(stderr,
                    "[WARN PGRAPH-CLEAR-AFTER-ARRAY] frame=%u "
                    "mask=%08X color=%08X rect=%08X/%08X\n",
                    g_pg.stats.frames + 1u, param, g_pg.clear_color,
                    g_pg.clear_rect_h, g_pg.clear_rect_v);
        }
        if (g_pg.array_drawn_since_present) {
            g_pg.clear_after_array_since_present = 1;
            g_pg.last_clear_submission = g_pg.stats.frames + 1u;
        }
        if (dev) {
            uint32_t flags = 0;
            if (g_pg.surface_color_offset >= 0x00010000u &&
                g_pg.surface_color_offset < 0x04000000u) {
                UINT width;
                UINT height;

                if (frontend_runtime_target_dimensions(
                        g_pg.surface_color_offset, &width, &height)) {
                    runtime_target = frontend_runtime_target_acquire(
                        dev, g_pg.surface_color_offset, width, height);
                    dev->lpVtbl->SetTexture(dev, 0, NULL);
                    if (runtime_target != NULL &&
                        SUCCEEDED(d3d8_BindRuntimeRenderTexture(
                            runtime_target->texture, 0, 0)))
                        runtime_target_bound = 1;
                }
            }
            /* Clear whichever surface the title is rendering into, not the
             * swap chain. */
            if (!runtime_target_bound)
                pgraph_bind_scene_surface(dev);
            if (param & 0xF0) flags |= 1;  /* D3DCLEAR_TARGET */
            if (param & 0x01) flags |= 2;  /* D3DCLEAR_ZBUFFER */
            if (param & 0x02) flags |= 4;  /* D3DCLEAR_STENCIL */
            {
                float depth;
                DWORD stencil;
                D3DCOLOR color = pgraph_clear_color_value();
                if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
                    if (!pgraph_bind_depth_surface())
                        flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
                    if (((g_pg.surface_format >> 4) & 0xFu) == 1u)
                        flags &= ~D3DCLEAR_STENCIL;
                }
                pgraph_clear_depth_value(&depth, &stencil);
                if (pgraph_seq_active() && (flags & D3DCLEAR_ZBUFFER))
                    fprintf(stderr, "[DEPTH-CLEAR] rt=%08X zeta=%08X format=%08X "
                            "raw=%08X z=%.9g stencil=%u control0=%08X\n",
                            g_pg.surface_color_offset, g_pg.surface_zeta_offset,
                            g_pg.surface_format, g_pg.clear_zstencil, depth,
                            (unsigned)stencil, g_pg.control0);
                if (pgraph_seq_active() && (flags & D3DCLEAR_TARGET))
                    fprintf(stderr, "[COLOR-CLEAR] rt=%08X format=%08X raw=%08X argb=%08X\n",
                            g_pg.surface_color_offset, g_pg.surface_format,
                            g_pg.clear_color, (unsigned)color);
                dev->lpVtbl->Clear(dev, 0, NULL, flags, color,
                                   depth, stencil);
                if (flags & D3DCLEAR_TARGET)
                    pgraph_display_surface_written(g_pg.surface_color_offset);
            }
            if (runtime_target_bound) {
                runtime_target->last_clear_submission =
                    g_pg.stats.frames + 1u;
                g_pg.frontend_live_texture = runtime_target->texture;
                g_pg.frontend_live_offset = runtime_target->guest_offset;
                d3d8_RestoreDefaultRenderTarget();
            }
        }
        g_pg.stats.clears++;
        return 1;
    }

    /* ── Render State ── */
    case NV097_SET_DEPTH_TEST_ENABLE:
        g_pg.depth_test = param ? 1 : 0;
        return 1;

    case NV097_SET_CONTROL0:
        g_pg.control0 = param;
        return 1;

    case NV097_SET_DEPTH_FUNC:
        g_pg.depth_func = param;
        return 1;

    case NV097_SET_DEPTH_MASK:
        g_pg.depth_mask = param ? 1u : 0u;
        return 1;

    case NV097_SET_ZSTENCIL_CLEAR_VALUE:
        g_pg.clear_zstencil = param;
        return 1;

    case NV097_SET_ZMIN_MAX_CONTROL:
        g_pg.zmin_max_control = param;
        return 1;

    case NV097_SET_BLEND_ENABLE:
        g_pg.blend_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_BLEND_FUNC_SFACTOR:
        g_pg.blend_sfactor = param;
        return 1;

    case NV097_SET_BLEND_FUNC_DFACTOR:
        g_pg.blend_dfactor = param;
        return 1;

    case NV097_SET_CULL_FACE_ENABLE:
        g_pg.cull_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_STENCIL_TEST_ENABLE: g_pg.stencil_test = param != 0; return 1;
    case NV097_SET_STENCIL_MASK: g_pg.stencil_write_mask = param & 0xFFu; return 1;
    case NV097_SET_STENCIL_FUNC: g_pg.stencil_func = param; return 1;
    case NV097_SET_STENCIL_FUNC_REF: g_pg.stencil_ref = param & 0xFFu; return 1;
    case NV097_SET_STENCIL_FUNC_MASK: g_pg.stencil_read_mask = param & 0xFFu; return 1;
    case NV097_SET_STENCIL_OP_FAIL: g_pg.stencil_fail = param; return 1;
    case NV097_SET_STENCIL_OP_ZFAIL: g_pg.stencil_zfail = param; return 1;
    case NV097_SET_STENCIL_OP_ZPASS: g_pg.stencil_pass = param; return 1;
    case NV097_SET_CULL_FACE:
        g_pg.cull_face = param;
        return 1;
    case NV097_SET_FRONT_FACE:
        g_pg.front_face = param;
        return 1;

    case NV097_SET_FOG_ENABLE: g_pg.fog_enable = param; return 1;
    case NV097_SET_FOG_MODE: g_pg.fog_mode = param; return 1;
    case NV097_SET_FOG_COLOR:
        g_pg.fog_color = param;
        { IDirect3DDevice8 *dev = xbox_GetD3DDevice();
          uint32_t argb = (param & 0xFF00FF00u) | ((param & 0xFFu) << 16) | ((param >> 16) & 0xFFu);
          if (dev) dev->lpVtbl->SetRenderState(dev, D3DRS_FOGCOLOR, argb); }
        return 1;
    case NV097_SET_FOG_PARAMS:
    case NV097_SET_FOG_PARAMS + 4:
    case NV097_SET_FOG_PARAMS + 8:
        memcpy(&g_pg.fog_params[(method - NV097_SET_FOG_PARAMS) / 4u], &param, 4); return 1;
    case NV097_SET_ALPHA_TEST_ENABLE:
        g_pg.alpha_test = param ? 1 : 0;
        return 1;
    case NV097_SET_ALPHA_FUNC:
        g_pg.alpha_func = param;
        return 1;
    case NV097_SET_ALPHA_REF:
        g_pg.alpha_ref = param & 0xFFu;
        return 1;

    case NV097_SET_COLOR_MASK:
        g_pg.color_mask = param;
        return 1;

    case NV097_SET_LIGHT_CONTROL:
        g_pg.light_control = param;
        g_pg.lighting_methods_seen |= 0x00000001u;
        return 1;

    case NV097_SET_COLOR_MATERIAL:
        g_pg.color_material = param;
        g_pg.lighting_methods_seen |= 0x00000002u;
        return 1;

    case NV097_SET_LIGHTING_ENABLE:
        g_pg.lighting_enable = param;
        g_pg.lighting_methods_seen |= 0x00000004u;
        return 1;

    case NV097_SET_MATERIAL_ALPHA:
        g_pg.material_alpha = param;
        g_pg.lighting_methods_seen |= 0x00000010u;
        return 1;

    case NV097_SET_SPECULAR_ENABLE:
        g_pg.specular_enable = param;
        g_pg.lighting_methods_seen |= 0x00000020u;
        return 1;

    case NV097_SET_LIGHT_ENABLE_MASK:
        g_pg.light_enable_mask = param;
        g_pg.lighting_methods_seen |= 0x00000004u;
        return 1;

    case NV097_SET_SHADE_MODE:
        /* 1=flat, 2=gouraud — we always use gouraud */
        return 1;

    /* ── Viewport ── */
    case NV097_SET_VIEWPORT_OFFSET:
    case NV097_SET_VIEWPORT_OFFSET + 4:
    case NV097_SET_VIEWPORT_OFFSET + 8:
    case NV097_SET_VIEWPORT_OFFSET + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
        g_pg.vp_offset[idx] = u2f(param);
        /* The viewport registers are also the vertex program constants the
         * transform reads.  A program in PROGRAM mode does its own viewport
         * transform out of c58/c59 -- this title MULs by c58 and MADs c59 in
         * its last two instructions -- and nothing else ever writes them:
         * the constant census counted 551268 writes to c148/c149 and ZERO to
         * c58/c59, so position came out (0,0,0,0). */
        g_pg.transform_constant[NV2A_VP_CONST_VPOFF][idx] = param;
        return 1;
    }

    case NV097_SET_VIEWPORT_SCALE:
    case NV097_SET_VIEWPORT_SCALE + 4:
    case NV097_SET_VIEWPORT_SCALE + 8:
    case NV097_SET_VIEWPORT_SCALE + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_SCALE) / 4;
        g_pg.vp_scale[idx] = u2f(param);
        g_pg.transform_constant[NV2A_VP_CONST_VPSCL][idx] = param;
        return 1;
    }

    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        g_pg.surface_clip_h = param;
        return 1;

    case NV097_SET_SURFACE_CLIP_VERTICAL:
        g_pg.surface_clip_v = param;
        return 1;

    case NV097_SET_SURFACE_FORMAT:
        g_pg.surface_format = param;
        return 1;

    case NV097_SET_SURFACE_PITCH:
        g_pg.surface_pitch = param;
        return 1;

    case NV097_SET_SURFACE_COLOR_OFFSET:
        {
            static unsigned frontend_surface_offset_logged;
            if (frontend_surface_offset_logged++ < 24u)
                fprintf(stderr,
                        "[DEBUG PGRAPH-SURFACE] frame=%u color-offset="
                        "%08X old=%08X\n",
                        g_pg.stats.frames + 1u, param,
                        g_pg.surface_color_offset);
        }
        g_pg.surface_color_offset = param;
        pgraph_note_surface_offset(param & 0x03FFFFFFu);
        return 1;

    case NV097_SET_SURFACE_ZETA_OFFSET:
        g_pg.surface_zeta_offset = param;
        pgraph_note_surface_offset(param & 0x03FFFFFFu);
        return 1;

    /* ── Texture state tracking (4 stages, 0x40 stride) ── */
    case NV097_SET_TEXTURE_OFFSET:
    case NV097_SET_TEXTURE_OFFSET + 0x40:
    case NV097_SET_TEXTURE_OFFSET + 0x80:
    case NV097_SET_TEXTURE_OFFSET + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_OFFSET) / 0x40;
        static unsigned frontend_texture_offset_logged;
        if (frontend_texture_offset_logged++ < 32u)
            fprintf(stderr,
                    "[DEBUG PGRAPH-TEXTURE] frame=%u stage=%d offset="
                    "%08X old=%08X\n",
                    g_pg.stats.frames + 1u, stage, param,
                    g_pg.tex[stage].offset);
        g_pg.tex[stage].offset = param;
        return 1;
    }
    case NV097_SET_TEXTURE_FORMAT:
    case NV097_SET_TEXTURE_FORMAT + 0x40:
    case NV097_SET_TEXTURE_FORMAT + 0x80:
    case NV097_SET_TEXTURE_FORMAT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_FORMAT) / 0x40;
        g_pg.tex[stage].format = param;
        return 1;
    }
    case NV097_SET_TEXTURE_IMAGE_RECT:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0x40:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0x80:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0xC0:
    {
        /* A linear texture carries its size here, not in the format's log2
         * fields -- those are for swizzled textures, which must be powers of
         * two.  Nothing read this register, so every linear texture came out
         * 1x1: the loading screen's art sampled a single texel and rendered
         * as flat colour. */
        int stage = (method - NV097_SET_TEXTURE_IMAGE_RECT) / 0x40;
        g_pg.tex[stage].image_rect = param;
        return 1;
    }
    case NV097_SET_TEXTURE_ADDRESS:
    case NV097_SET_TEXTURE_ADDRESS + 0x40:
    case NV097_SET_TEXTURE_ADDRESS + 0x80:
    case NV097_SET_TEXTURE_ADDRESS + 0xC0:
        g_pg.tex[(method - NV097_SET_TEXTURE_ADDRESS) / 0x40].address = param;
        return 1;
    case NV097_SET_TEXTURE_FILTER:
    case NV097_SET_TEXTURE_FILTER + 0x40:
    case NV097_SET_TEXTURE_FILTER + 0x80:
    case NV097_SET_TEXTURE_FILTER + 0xC0:
        g_pg.tex[(method - NV097_SET_TEXTURE_FILTER) / 0x40].filter = param;
        return 1;
    case NV097_SET_TEXTURE_BORDER_COLOR:
    case NV097_SET_TEXTURE_BORDER_COLOR + 0x40:
    case NV097_SET_TEXTURE_BORDER_COLOR + 0x80:
    case NV097_SET_TEXTURE_BORDER_COLOR + 0xC0:
        g_pg.tex[(method - NV097_SET_TEXTURE_BORDER_COLOR) / 0x40].border_color = param;
        return 1;
    case NV097_SET_TEXTURE_CONTROL0:
    case NV097_SET_TEXTURE_CONTROL0 + 0x40:
    case NV097_SET_TEXTURE_CONTROL0 + 0x80:
    case NV097_SET_TEXTURE_CONTROL0 + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_CONTROL0) / 0x40;
        g_pg.tex[stage].control0 = param;
        g_pg.tex[stage].enabled = (param >> 30) & 1;
        return 1;
    }

    /* ── Register-combiner and texture-shader state ── */
    case 0x0260: case 0x0264: case 0x0268: case 0x026C:
    case 0x0270: case 0x0274: case 0x0278: case 0x027C:
        g_pg.combiner_alpha_icw[(method - 0x0260u) / 4u] = param;
        return 1;
    case 0x0AA0: case 0x0AA4: case 0x0AA8: case 0x0AAC:
    case 0x0AB0: case 0x0AB4: case 0x0AB8: case 0x0ABC:
        g_pg.combiner_alpha_ocw[(method - 0x0AA0u) / 4u] = param;
        return 1;
    case 0x0AC0: case 0x0AC4: case 0x0AC8: case 0x0ACC:
    case 0x0AD0: case 0x0AD4: case 0x0AD8: case 0x0ADC:
        g_pg.combiner_color_icw[(method - 0x0AC0u) / 4u] = param;
        return 1;
    case 0x1E40: case 0x1E44: case 0x1E48: case 0x1E4C:
    case 0x1E50: case 0x1E54: case 0x1E58: case 0x1E5C:
        g_pg.combiner_color_ocw[(method - 0x1E40u) / 4u] = param;
        return 1;
    case 0x0A60: case 0x0A64: case 0x0A68: case 0x0A6C:
    case 0x0A70: case 0x0A74: case 0x0A78: case 0x0A7C:
        g_pg.combiner_factor0[(method - 0x0A60u) / 4u] = param;
        return 1;
    case 0x0A80: case 0x0A84: case 0x0A88: case 0x0A8C:
    case 0x0A90: case 0x0A94: case 0x0A98: case 0x0A9C:
        g_pg.combiner_factor1[(method - 0x0A80u) / 4u] = param;
        return 1;
    case 0x0288:
        g_pg.combiner_final0 = param;
        return 1;
    case 0x028C:
        g_pg.combiner_final1 = param;
        return 1;
    case 0x1E60:
        g_pg.combiner_control = param;
        return 1;
    case 0x1E70:
        g_pg.shader_stage_program = param;
        return 1;
    case 0x1E74:
        g_pg.dot_rgb_mapping = param;
        return 1;
    case 0x1E78:
        g_pg.shader_other_stage_input = param;
        return 1;

    /* ---- Flip chain -------------------------------------------------
     *
     * The title drives its swap through the push buffer, not through the
     * D3D8 Present entry point: dev_Present never runs, and
     * d3d8_PresentFrame() had no callers at all.  This is the only frame
     * boundary the runtime gets.
     *
     * The ignore list below used to name 0x0108..0x0118 "FLIP_*", which is
     * wrong twice over -- the flip methods are 0x0120..0x0130 and 0x0110 is
     * WAIT_FOR_IDLE -- so FLIP_STALL fell through as "truly unhandled" and
     * was discarded.  202,692 flips in ninety seconds, 3.5M draws, and
     * nothing ever reached the screen.
     */
    case NV097_SET_FLIP_READ:
    case NV097_SET_FLIP_WRITE:
    case NV097_SET_FLIP_MODULO:
        /* Which buffer of the flip chain the GPU reads and the title writes.
         * Temporary: report the indices together with the surface bound when
         * they arrive, to see whether the chain can say which surface is the
         * finished one.  Picking by bind order and by draw order both present
         * the wrong surface on some frames. */
        { static unsigned logged;
          if (logged++ < 24u)
              fprintf(stderr, "[INFO PGRAPH-FLIPCHAIN] %s=%u surface=%08X\n",
                      method == NV097_SET_FLIP_READ ? "read" :
                      method == NV097_SET_FLIP_WRITE ? "write" : "modulo",
                      param, g_pg.surface_color_offset); }
        return nv2a_pgraph_flip_method(method, param);

    case NV097_FLIP_INCREMENT_WRITE:
        /* The title has finished a buffer and is handing it to the display.
         * This -- not FLIP_STALL -- is where the front buffer is designated.
         *
         * On hardware FLIP_STALL only stalls the pusher until an already
         * queued flip retires, so by the time it executes the title has moved
         * on and cleared the next buffer.  Copying to the swap chain there
         * showed a surface that had just been wiped: the trace reads
         * "CLEAR rt=01C80000 mask=F3" immediately before "FLIP
         * present=01C80000", and the finished frame was sitting in the other
         * surface the whole time. */
        { static unsigned logged;
          if (logged++ < 24u)
              fprintf(stderr, "[INFO PGRAPH-FLIPCHAIN] increment: bound=%08X "
                      "lastdrawn=%08X\n",
                      g_pg.surface_color_offset, g_scene_present_offset); }
        if (pgraph_seq_active())
            fprintf(stderr, "[SEQ] %4u f%u FLIP   present=%08X\n",
                    ++g_seq, (unsigned)g_pg.stats.frames,
                    g_scene_present_offset);
        pgraph_present_scene_surface(xbox_GetD3DDevice());
        g_display_copy_pending = 1;
        pgraph_audit_present(0);
        return nv2a_pgraph_flip_method(method, param);

    case NV097_FLIP_STALL:
        /* Present the surface copied at FLIP_INCREMENT_WRITE, then stop
         * consuming commands if the flip ring is full. The core wait pumps
         * messages and guest interrupts until the driver's MMIO retirement
         * makes a buffer available. */
        { static unsigned n;
          /* Across the run, not only its first seconds.  The title moves off
           * the loading screen after a few seconds and what is on the window
           * then is not what the early frames showed. */
          if (n < 120000u && ++n % 600u == 0u)
              d3d8_DebugSampleBackbuffer("flip"); }
        pgraph_audit_present(1);
        d3d8_PresentFrame();
        return nv2a_pgraph_flip_method(method, param);

    default:
        /* Check if it's in a known range we can safely ignore */
        { static int effect_trace = -1;
          static unsigned char reported[2048];
          if (effect_trace < 0) effect_trace = getenv("CONKER_EFFECT_METHODS") != NULL;
          if (effect_trace && method < 0x2000u && !reported[method >> 2]) {
              reported[method >> 2] = 1;
              fprintf(stderr, "[EFFECT-METHOD] method=%04X value=%08X in_draw=%u primitive=%u\n",
                      method, param, g_pg.in_draw, g_pg.draw_mode);
          } }
        if ((method >= 0x0E00 && method < 0x1000) ||  /* Transform constants */
            (method >= 0x1680 && method < 0x1780) ||  /* Vertex array format/offset */
            (method >= 0x1B00 && method < 0x1C00) ||  /* Texture registers */
            (method >= 0x1D60 && method < 0x1EA0) ||  /* Combiners */
            method == 0x0100 ||                        /* NOP */
            method == 0x0180 ||                        /* SET_OBJECT */
            method == 0x0394 ||                        /* TRANSFORM_EXECUTION_MODE */
            method == 0x0398 ||                        /* TRANSFORM_PROGRAM_CXT_WRITE_EN */
            method == 0x039C ||                        /* TRANSFORM_PROGRAM_LOAD */
            method == 0x01E0 ||                        /* SHADER_STAGE_PROGRAM */
            method == NV097_WAIT_FOR_IDLE ||           /* 0x0110 */
            /* Unidentified, and deliberately not guessed at again: these
             * four were the ones labelled "FLIP_*" above.  Ignored to keep
             * the log quiet, named honestly so the next reader does not
             * inherit the same mistake. */
            method == 0x0108 || method == 0x010C ||
            method == 0x0114 || method == 0x0118)
        {
            return 1;  /* Silently handled (ignored but acknowledged) */
        }

        /* Complete histogram, not a rate-limited sample: a method that
         * arrives constantly and one that arrives once look identical in a
         * capped log, and the difference is the whole question. */
        { static unsigned hist[2048];
          if ((method >> 2) < 2048u) hist[method >> 2]++;
          { static unsigned n;
            if (++n % 20000u == 0u) {
                unsigned i, shown;
                fprintf(stderr, "[INFO PGRAPH-IGNORED] top unhandled methods:\n");
                for (shown = 0; shown < 12u; ++shown) {
                    unsigned best = 0, bi = 0;
                    for (i = 0; i < 2048u; ++i)
                        if (hist[i] > best) { best = hist[i]; bi = i; }
                    if (best == 0u) break;
                    fprintf(stderr, "[INFO PGRAPH-IGNORED]   %04X x%u\n",
                            bi << 2, best);
                    hist[bi] = 0;
                }
            } } }
        g_pg.stats.methods_ignored++;
        return 0;  /* Truly unhandled */
    }
}

void pgraph_d3d11_flush(void)
{
    IDirect3DDevice8 *dev;

    if (g_pg.in_draw) {
        if (getenv("CONKER_PB_SKIP_AUDIT"))
            fprintf(stderr, "[PBFLUSH-AUDIT] open primitive=%u vertices=%u\n",
                    g_pg.draw_mode, g_pg.vert_stride ? g_pg.inline_count / g_pg.vert_stride : 0u);
        submit_draw();
        g_pg.in_draw = 0;
    }
    /* The host bridge presents outside the original Xbox Swap path.  Close
     * the translated scene here so command-ring submissions form complete
     * host frames even when the guest never reaches a COM EndScene call. */
    dev = xbox_GetD3DDevice();
    if (dev)
        dev->lpVtbl->EndScene(dev);
    pgraph_refresh_displayed_surface(dev);
    g_pg.array_drawn_this_frame = 0;
    g_pg.stats.frames++;
}

void pgraph_d3d11_present_notify(void)
{
    pgraph_display_presented();
    { static unsigned n;
      if (++n % 600u == 0u)
          fprintf(stderr, "[INFO PGRAPH-GEOM] inline drawn=%u empty=%u short=%u "
                  "noprims=%u | array drawn=%u skipped=%u\n",
                  g_geom.inline_drawn, g_geom.inline_empty, g_geom.inline_short,
                  g_geom.inline_noprims, g_geom.array_drawn,
                  g_geom.array_skipped); }
    static uint32_t present_count;
    static unsigned present_log_count;

    ++present_count;
    if (g_pg.array_drawn_since_present) {
        if (present_log_count++ < 16u) {
            fprintf(stderr,
                    "[INFO PGRAPH-PRESENT] present=%u submissions=%u "
                    "array-draws=%u last-array=%u cleared-after=%d "
                    "last-clear=%u\n",
                    present_count, g_pg.stats.frames,
                    g_pg.array_draws_since_present,
                    g_pg.last_array_submission,
                    g_pg.clear_after_array_since_present,
                    g_pg.last_clear_submission);
        }
        d3d8_DebugSampleBackbuffer("before-present");
    }

    g_pg.array_drawn_since_present = 0;
    g_pg.clear_after_array_since_present = 0;
    g_pg.array_draws_since_present = 0;
    g_pg.last_clear_submission = 0;
}

void pgraph_d3d11_set_chyron_scroll(uint32_t pixels)
{
    g_pg.chyron_scroll_offset = (float)pixels;
}

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out)
{
    if (out) *out = g_pg.stats;
}
