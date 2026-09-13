/*
 * nv2a_vsh.h - NV2A vertex program execution
 */
#ifndef NV2A_VSH_H
#define NV2A_VSH_H

#include <stdint.h>

typedef struct {
    float v[16][4];    /* input registers, from the vertex attributes */
    float c[192][4];   /* constant registers, c0..c191 */
    float r[16][4];    /* temporaries */
    float o[16][4];    /* output registers: 0 position, 3/4 colours,
                        * 9..12 texcoords */
} nv2a_vsh_state;

/* Decoded once per draw; contains no guest pointers or per-vertex state. */
typedef struct {
    uint8_t mux, reg, swizzle[4], negate;
} nv2a_vsh_operand;
typedef struct {
    nv2a_vsh_operand source[3];
    uint8_t mac, ilu, constant, relative, sources;
    uint8_t mac_mask, ilu_mask, temp, output_mask, output, output_mux;
} nv2a_vsh_instruction;
typedef struct {
    unsigned count;
    unsigned constants_bound;
    nv2a_vsh_instruction instruction[136];
    float constant_source[136][3][4];
} nv2a_vsh_program;

void nv2a_vsh_decode(nv2a_vsh_program *decoded,
                      const uint32_t program[][4], unsigned count);
void nv2a_vsh_run_decoded(const nv2a_vsh_program *program, nv2a_vsh_state *st);
/* Optional per-draw preparation. Rebind whenever the constant file changes. */
void nv2a_vsh_bind_constants(nv2a_vsh_program *program, const float constants[192][4]);

void nv2a_vsh_run(const uint32_t program[][4], unsigned count,
                  nv2a_vsh_state *st);
void nv2a_vsh_dump(const uint32_t program[][4], unsigned count);

#endif
