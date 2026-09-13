#ifndef NV2A_VSH_HLSL_H
#define NV2A_VSH_HLSL_H
#include <stddef.h>
#include "nv2a_vsh.h"
/* Emit a shader function using the same decoder as the CPU reference.
 * The caller supplies a float4 c[192] constant buffer. Returns 0 on overflow. */
size_t nv2a_vsh_emit_hlsl(char *text, size_t capacity, const nv2a_vsh_program *program);
#endif
