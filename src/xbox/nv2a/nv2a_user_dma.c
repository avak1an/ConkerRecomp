/*
 * NV_USER DMA_PUT arbitration.
 *
 * Kept apart from nv2a_core.c because the whole of the difficulty is integer
 * reasoning about three shapes of write, and none of it needs the emulator
 * state -- which is what makes test_user_dma_put.c possible.
 */

#include "nv2a_pushbuffer.h"

/* Advancing DMA_PUT is how the title submits work once it drives the
 * hardware directly, rather than through the D3D8 entry points that
 * recomp_hand_stubs.c hooks.  Translate whatever was newly queued so
 * both submission routes end up in the same PGRAPH -> D3D11 path,
 * then report the channel drained (see user_read).
 *
 * Three cases, and only the first is a plain batch.
 *
 * DMA_PUT decreasing within the ring means the producer wrapped. It can
 * append NEW commands after the old PUT and a jump to the base before
 * publishing a lower PUT. GET must start at the old position and follow
 * that jump, then consume the head up to PUT. Starting at the base loses
 * pending tail draws even when the head's end-of-frame fence executes.
 * The linked walker bounds reads and execution without guessing a ring end.
 *
 * The ring base has to be observed.  The FIFO's own DMA object is no
 * help: NV_PFIFO_CACHE1_DMA_INSTANCE resolves to address=0x00000000
 * limit=0x07FFAFFF, the whole of memory, because the Xbox addresses
 * its push buffer absolutely.  The lowest PUT seen is the base, since
 * the title's first write into a ring establishes its position.
 *
 * A forward jump too large to be a batch is the third case: the title
 * moved to a different push buffer.  The bootstrap ring at 0x01102000
 * hands over to the main one at 0x01BBD000 exactly this way, an 11 MB
 * step.  Adopting the new base there is what keeps the wrap arithmetic
 * on the right ring. */
int nv2a_user_dma_put_step(uint32_t *dma_put, uint32_t *ring_base,
                           uint32_t put,
                           uint32_t *out_begin, uint32_t *out_end)
{
    *out_begin = 0;
    *out_end = 0;

    if (*dma_put == 0) {
        /* The first write only establishes the position -- nothing is queued
         * behind it yet -- and it names the ring base. */
        *dma_put = put;
        *ring_base = put;
        return 0;
    }

    if (put == *dma_put)
        return 0;               /* re-armed with no new work; base unchanged */

    if (put > *dma_put) {
        uint32_t begin = *dma_put;

        *dma_put = put;
        if (put - begin > NV2A_PB_MAX_BATCH_BYTES) {
            *ring_base = put;   /* a different buffer, not a batch */
            return 0;
        }
        *out_begin = begin;
        *out_end = put;
        return 1;
    }

    /* A lower PUT is a stopping address, not a replacement GET. */
    uint32_t begin = *dma_put;
    *dma_put = put;
    if (put >= *ring_base) {
        *out_begin = begin;
        *out_end = put;
        return 1;
    }
    /* Below anything seen in this ring: retain the relocation heuristic. */
    *ring_base = put;
    return 0;
}
