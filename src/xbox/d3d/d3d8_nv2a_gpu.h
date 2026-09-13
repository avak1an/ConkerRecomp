#ifndef D3D8_NV2A_GPU_H
#define D3D8_NV2A_GPU_H
#include "d3d8_xbox.h"
#include "../nv2a/nv2a_vsh.h"
typedef struct {
    float constants[192][4];
    float viewport[4]; /* width, height, depth divisor, padding */
    float fog[4]; /* coefficient 0, coefficient 1, enable, mode */
    float tex_matrix[4][16];
    uint32_t formats[16];
} Nv2aGpuConstants;
void *d3d8_nv2a_gpu_program(const uint32_t words[][4], unsigned count,
    const uint32_t formats[16], const int offsets[16]);
HRESULT d3d8_nv2a_gpu_draw(void *program, Nv2aGpuConstants *constants,
    D3DPRIMITIVETYPE primitive, unsigned count, const void *vertices, unsigned stride);
/* Called by DrawPrimitiveUP after preparing the ordinary pixel path. */
int d3d8_nv2a_gpu_bind(void);
void d3d8_nv2a_gpu_shutdown(void);
#endif
