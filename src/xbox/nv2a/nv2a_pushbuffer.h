#ifndef XBOXRECOMP_NV2A_PUSHBUFFER_H
#define XBOXRECOMP_NV2A_PUSHBUFFER_H

#include <stdint.h>

/*
 * Feed a guest-memory NV2A command chain to the generic
 * PGRAPH-to-D3D11 translator.  Both endpoints are Xbox virtual addresses;
 * the function performs all address/range validation itself.
 *
 * This is deliberately independent of the optional NV2A MMIO emulator.  The
 * lifted D3D8LTCG library writes command words directly into a guest command
 * ring, so the frame-submit bridge can decode that ring without emulating a
 * physical FIFO first.
 */
/*
 * Host safety bound for a newly published interval. The scene command ring
 * spans about 768 KiB and legitimate individual publications exceed 64 KiB.
 * The old 64 KiB bound silently dropped those draws and incorrectly changed
 * the observed ring base, also losing the following wrap fence. Allow an
 * entire ring-sized publication. The bootstrap-to-main relocation is still
 * much larger (about 11 MiB); NV_USER retains its existing relocation rule.
 * Guest-memory bounds and the linked walker's work budget are independent.
 */
#define NV2A_PB_MAX_BATCH_BYTES 0x100000u

/*
 * Decide what a DMA_PUT write submits.
 *
 * `dma_put` is the last value written and `ring_base` the lowest PUT seen in
 * the buffer currently in use; both are updated in place.  Returns 1 and fills
 * the initial GET and stopping PUT when there is work to parse, 0 otherwise.
 * On a ring wrap out_begin > out_end; follow the guest's jump to the head.
 *
 * Separated from the MMIO handler because the arbitration is the whole of the
 * difficulty and none of the plumbing -- see test_user_dma_put.c.
 */
int nv2a_user_dma_put_step(uint32_t *dma_put, uint32_t *ring_base,
                           uint32_t put,
                           uint32_t *out_begin, uint32_t *out_end);

void nv2a_pushbuffer_init(void);
void nv2a_pushbuffer_submit_guest(uint32_t begin, uint32_t end);

#endif
