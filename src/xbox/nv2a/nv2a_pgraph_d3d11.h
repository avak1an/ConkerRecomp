/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Intercepts NV2A push buffer method calls and translates them into
 * D3D8→D3D11 rendering commands. This is the core of the GPU translation
 * layer for Xbox static recompilation.
 *
 * The push buffer contains NV2A Kelvin (NV097) methods:
 *   - Surface/viewport setup → D3D11 render target + viewport
 *   - Render state (blend, depth, cull) → D3D11 state objects
 *   - Begin/End draw + Inline vertex data → D3D11 DrawPrimitiveUP
 *   - Texture binding → D3D11 shader resource views
 *   - Clear commands → D3D11 ClearRenderTargetView
 *
 * Vertex formats observed in menus:
 *   5 dwords per vertex: float X, float Y, float U, float V, D3DCOLOR
 *   Drawn as TRIANGLE_STRIP (mode 6)
 *
 * This module is designed to be reusable across Xbox recompilation projects.
 * See: https://github.com/sp00nznet/xboxrecomp
 */

#ifndef NV2A_PGRAPH_D3D11_H
#define NV2A_PGRAPH_D3D11_H

#include <stdint.h>

/* Initialize the PGRAPH→D3D11 translator. Call after D3D11 device is created. */
void pgraph_d3d11_init(void);

/* Shut down and release resources. */
void pgraph_d3d11_shutdown(void);

/* Process an NV2A PGRAPH method call. Called from push buffer parser.
 * Returns 1 if handled, 0 if unhandled (caller should log/ignore). */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param);

/* Flush any pending draw commands (call at end of frame). */
void pgraph_d3d11_flush(void);

/* Called immediately before the host swap chain is presented.  This keeps
 * diagnostics aligned with a real display frame rather than an individual
 * guest push-buffer submission. */
void pgraph_d3d11_present_notify(void);

/* Queue the retail Frontend's live surface-copy operation.  The guest builds
 * this request before its push buffer is submitted, so the D3D11 translator
 * resolves it lazily when the destination is sampled by the compositor. */
void pgraph_d3d11_queue_frontend_surface_copy(uint32_t destination,
                                              uint32_t source);

/* Publish dimensions for a render-surface backing allocated by the live
 * Frontend.  The guest surface compiler exposes only the backing address in
 * its NV097 packets, so the producer supplies the matching dimensions before
 * those packets are decoded. */
void pgraph_d3d11_register_frontend_surface(uint32_t guest_offset,
                                            uint32_t width,
                                            uint32_t height);
void pgraph_d3d11_register_frontend_compositor_surface(
    uint32_t guest_offset, uint32_t width, uint32_t height);

/* Set chyron scroll: pass frame counter to animate, 0 to disable.
 * Applies horizontal scroll offset to vertices in the chyron Y band. */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame);

/* Statistics */
typedef struct {
    uint32_t frames;
    uint32_t draw_calls;
    uint32_t vertices_submitted;
    uint32_t methods_handled;
    uint32_t methods_ignored;
    uint32_t clears;
} PgraphD3D11Stats;

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out);
/* Finished images copied for presentation, independent of command batches. */
uint64_t pgraph_d3d11_present_count(void);

#endif /* NV2A_PGRAPH_D3D11_H */
