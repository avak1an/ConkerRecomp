#ifndef CONKER_D3D8_COMPILE_H
#define CONKER_D3D8_COMPILE_H
#include <windows.h>
#include <d3dcompiler.h>
/* Initialize once before rendering. Derived bytecode remains in local data. */
void d3d8_shader_cache_init(const char *game_dir_utf8);
HRESULT d3d8_compile_shader(const void *source, SIZE_T size,
    const char *name, const char *entry, const char *target, UINT flags,
    ID3DBlob **code, ID3DBlob **errors);
#endif
