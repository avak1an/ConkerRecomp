#ifndef CONKER_RUNTIME_CONFIG_H
#define CONKER_RUNTIME_CONFIG_H

#include <windows.h>

typedef struct ConkerRuntimeOptions {
    int vertex_shaders;
    int register_combiners;
    int multistage_textures;
    int watchdogs;
} ConkerRuntimeOptions;

typedef struct ConkerRuntimePaths {
    wchar_t xbe[MAX_PATH];
    /* xbox_path_init accepts UTF-8 and converts to Win32 UTF-16. */
    char game_dir[MAX_PATH * 4];
    char save_dir[MAX_PATH * 4];
} ConkerRuntimePaths;

const ConkerRuntimeOptions *conker_runtime_options(void);
/* An explicit directory is authoritative: a typo must not load another copy.
 * Returns 1 on success; reports a startup error and returns 0 otherwise. */
int conker_runtime_paths(const wchar_t *requested_dir, ConkerRuntimePaths *paths);

#endif
