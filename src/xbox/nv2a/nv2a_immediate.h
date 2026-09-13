#ifndef NV2A_IMMEDIATE_H
#define NV2A_IMMEDIATE_H
#include <stdint.h>
#include <string.h>
#include "nv2a_regs.h"

/* Current attributes persist between vertices and BEGIN/END blocks. A write
 * completing position (attribute 0) emits a snapshot of all attributes.
 * Return 0 for another method, 1 for an attribute write, 2 for a vertex. */
static inline int nv2a_immediate_write(float value[16][4], uint32_t method,
                                     uint32_t parameter)
{
    unsigned slot, part;
    if (method & 3u) return 0;
    if (method >= NV097_SET_VERTEX_DATA2F_M && method < NV097_SET_VERTEX_DATA2F_M + 128u) {
        slot = (method - NV097_SET_VERTEX_DATA2F_M) / 8u;
        part = ((method - NV097_SET_VERTEX_DATA2F_M) / 4u) & 1u;
        memcpy(&value[slot][part], &parameter, 4);
        value[slot][2] = 0; value[slot][3] = 1;
        return slot == 0 && part == 1 ? 2 : 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M && method < NV097_SET_VERTEX_DATA4F_M + 256u) {
        slot = (method - NV097_SET_VERTEX_DATA4F_M) / 16u;
        part = ((method - NV097_SET_VERTEX_DATA4F_M) / 4u) & 3u;
        memcpy(&value[slot][part], &parameter, 4);
        return slot == 0 && part == 3 ? 2 : 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2S && method < NV097_SET_VERTEX_DATA2S + 64u) {
        slot = (method - NV097_SET_VERTEX_DATA2S) / 4u;
        value[slot][0] = (float)(int16_t)parameter;
        value[slot][1] = (float)(int16_t)(parameter >> 16);
        value[slot][2] = 0; value[slot][3] = 1;
        return slot == 0 ? 2 : 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB && method < NV097_SET_VERTEX_DATA4UB + 64u) {
        slot = (method - NV097_SET_VERTEX_DATA4UB) / 4u;
        for (part = 0; part < 4; ++part)
            value[slot][part] = ((parameter >> (part * 8u)) & 255u) / 255.0f;
        return slot == 0 ? 2 : 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4S_M && method < NV097_SET_VERTEX_DATA4S_M + 128u) {
        slot = (method - NV097_SET_VERTEX_DATA4S_M) / 8u;
        part = ((method - NV097_SET_VERTEX_DATA4S_M) / 4u) & 1u;
        value[slot][part * 2u] = (float)(int16_t)parameter;
        value[slot][part * 2u + 1u] = (float)(int16_t)(parameter >> 16);
        return slot == 0 && part == 1 ? 2 : 1;
    }
    return 0;
}
#endif
