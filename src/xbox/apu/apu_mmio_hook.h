/*
 * MCPX APU MMIO Hook - VEH entry point
 *
 * The APU register window has no backing page, so every access from
 * recompiled code arrives as an access violation.  This decodes the faulting
 * x86-64 instruction, routes the read or write through the APU register
 * model, and advances RIP past it -- the same arrangement nv2a_mmio_hook.h
 * provides for GPU registers.
 *
 * Without it the window folds into ordinary RAM and every status register
 * behaves as a latch.  Conker spins on 0xFE820010 waiting for it to read
 * >= 0x20 before setting up its 256 voices, and a latch never gets there.
 */

#ifndef XBOXRECOMP_APU_MMIO_HOOK_H
#define XBOXRECOMP_APU_MMIO_HOOK_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#include "apu.h"

/* APU register window in Xbox VA space. */
#define APU_MMIO_VA_BASE 0xFE800000u
#define APU_MMIO_VA_SIZE 0x00080000u   /* 512KB */

/* Set by mcpx_apu_init_standalone; NULL until then, and the hook declines
 * every access while it is NULL. */
extern MCPXAPUState *g_apu_state;

/* Returns true if the access was decoded and serviced, in which case the
 * caller must EXCEPTION_CONTINUE_EXECUTION. */
bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write);

/* AC'97 controller window, separate from the APU's and modelled separately.
 * Needs no MCPXAPUState: it is a register file whose only behaviour is that
 * the NABM channel-control reset bit is self-clearing. */
#define ACI_MMIO_VA_BASE 0xFEC00000u
#define ACI_MMIO_VA_SIZE 0x00001000u

bool aci_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write);

#endif /* XBOXRECOMP_APU_MMIO_HOOK_H */
