#include "runtime_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int env_option(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    if (strcmp(value, "0") == 0) return 0;
    if (strcmp(value, "1") == 0) return 1;
    fprintf(stderr, "[startup] %s expects 0 or 1; using %d\n", name, fallback);
    return fallback;
}

const ConkerRuntimeOptions *conker_runtime_options(void)
{
    /* Resolve on the main thread before starting any guest/diagnostic threads. */
    static ConkerRuntimeOptions options;
    static int initialized;
    if (!initialized) {
        options.vertex_shaders = env_option("CONKER_VSH", 1);
        options.register_combiners = env_option("CONKER_RC_COMBINERS", 1);
        options.multistage_textures = env_option("CONKER_MULTISTAGE", 1);
        options.watchdogs = env_option("CONKER_WATCHDOG", 0) &&
                           !env_option("CONKER_NO_WATCHDOG", 0);
        initialized = 1;
    }
    return &options;
}

static int join_path(wchar_t *out, const wchar_t *dir, const wchar_t *leaf)
{
    size_t n = wcslen(dir), m = wcslen(leaf);
    int separator = n && dir[n - 1] != L'\\' && dir[n - 1] != L'/';
    if (n + separator + m >= MAX_PATH) return 0;
    memcpy(out, dir, n * sizeof(wchar_t));
    if (separator) out[n++] = L'\\';
    memcpy(out + n, leaf, (m + 1) * sizeof(wchar_t));
    return 1;
}

static int resolve_directory(const wchar_t *dir, ConkerRuntimePaths *paths)
{
    wchar_t absolute[MAX_PATH], xbe[MAX_PATH], save[MAX_PATH];
    DWORD n = GetFullPathNameW(dir, MAX_PATH, absolute, NULL);
    if (!n || n >= MAX_PATH || !join_path(xbe, absolute, L"default.xbe") ||
        !join_path(save, absolute, L"save")) return 0;
    DWORD attributes = GetFileAttributesW(xbe);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, absolute, -1, paths->game_dir,
                             sizeof(paths->game_dir), NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, save, -1, paths->save_dir,
                             sizeof(paths->save_dir), NULL, NULL)) return 0;
    wcscpy_s(paths->xbe, MAX_PATH, xbe);
    return 1;
}

int conker_runtime_paths(const wchar_t *requested_dir, ConkerRuntimePaths *paths)
{
    wchar_t directory[MAX_PATH], candidate[MAX_PATH];
    const wchar_t *selected = requested_dir ? requested_dir : _wgetenv(L"CONKER_GAME_DIR");
    if (selected && *selected) {
        if (resolve_directory(selected, paths)) return 1;
        fprintf(stderr, "[startup] Cannot find default.xbe in selected data directory: %ls\n",
                selected);
        return 0;
    }

    DWORD n = GetModuleFileNameW(NULL, directory, MAX_PATH);
    if (n && n < MAX_PATH) {
        /* Installed layout: EXE/game_files. Development: build[/Debug]/EXE.
         * Search only these two parents, never arbitrary ancestor trees. */
        for (unsigned level = 0; level < 3; ++level) {
            wchar_t *slash = wcsrchr(directory, L'\\');
            if (!slash) break;
            /* Preserve the separator in a drive root (C:\\). */
            if (slash == directory + 2 && directory[1] == L':') slash[1] = 0;
            else *slash = 0;
            if (join_path(candidate, directory, L"game_files") &&
                resolve_directory(candidate, paths)) return 1;
            if (slash == directory + 2 && directory[1] == L':') break;
        }
    }
    /* Compatibility for existing development commands with a separate EXE. */
    if (resolve_directory(L"game_files", paths)) return 1;
    fprintf(stderr, "[startup] No prepared game data found. Expected game_files/default.xbe "
                    "beside the executable or in the development project.\n"
                    "Use --data-dir <extracted-game-directory> or CONKER_GAME_DIR to select "
                    "your locally extracted files. Use the launcher to verify and prepare your own ISO.\n");
    return 0;
}
