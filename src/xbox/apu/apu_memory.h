/* Standalone APU DMA addressing for the recomp's guest memory mapping. */
#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint8_t *g_apu_ram_ptr;

/* MmGetPhysicalAddress in this runtime preserves ordinary guest VAs, including
 * committed allocations above 64 MB. Only the 0x80000000/0xA0000000 physical
 * alias windows mirror the low 64 MB. Masking every address made streaming
 * buffers in the game's 0x04000000 heap read unrelated low RAM as PCM.
 * This is the host adapter's addressing contract, not extra console RAM. */
static inline uint8_t *apu_dma_ptr(uint64_t address, size_t bytes)
{
    if (address >= UINT64_C(0x80000000) && address < UINT64_C(0xc0000000))
        address &= UINT64_C(0x03ffffff);
    assert(address < UINT64_C(0x80000000));
    assert(bytes <= UINT64_C(0x80000000) - address);
    return g_apu_ram_ptr + (size_t)address;
}

static inline uint32_t apu_dma_read32(uint64_t address)
{
    uint32_t value;
    memcpy(&value, apu_dma_ptr(address, sizeof(value)), sizeof(value));
    return value;
}

static inline uint16_t apu_dma_read16(uint64_t address)
{
    uint16_t value;
    memcpy(&value, apu_dma_ptr(address, sizeof(value)), sizeof(value));
    return value;
}

static inline void apu_dma_write32(uint64_t address, uint32_t value)
{
    memcpy(apu_dma_ptr(address, sizeof(value)), &value, sizeof(value));
}

static inline void apu_dma_write16(uint64_t address, uint16_t value)
{
    memcpy(apu_dma_ptr(address, sizeof(value)), &value, sizeof(value));
}
