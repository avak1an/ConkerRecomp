#ifndef NV2A_FOG_H
#define NV2A_FOG_H
#include <math.h>
#include <float.h>
#include <stdint.h>
/* NV097 fog parameters contain the hardware transform coefficients, not
 * D3D fog start/end. Keep the result unclamped until pixel interpolation.
 * See xemu hw/xbox/nv2a/pgraph/glsl/vsh.c for the register equations. */
static float nv2a_fog_factor(uint32_t enabled, uint32_t mode,
                             const float params[3], float distance)
{
    float f, exceptional;
    int absolute = mode == 0x802u || mode == 0x803u || mode == 0x804u;
    if (!enabled) return 1.0f;
    exceptional = mode == 0x2601u || mode == 0x804u || mode == 0x800u ? 1.0f : 0.0f;
    if (isinf(distance)) return exceptional;
    switch (mode) {
    case 0x2601u: case 0x804u:
        f = params[0] + distance * params[1] - 1.0f; break;
    case 0x800u: case 0x802u:
        f = params[0] + exp2f(distance * params[1] * 16.0f) - 1.5f; break;
    case 0x801u: case 0x803u:
        f = params[0] + exp2f(-distance * distance * params[1] * params[1] * 32.0f) - 1.5f; break;
    default: return 1.0f;
    }
    if (absolute) f = fabsf(f);
    if (isnan(f)) return exceptional;
    return f < -FLT_MAX ? -FLT_MAX : f > FLT_MAX ? FLT_MAX : f;
}
#endif
