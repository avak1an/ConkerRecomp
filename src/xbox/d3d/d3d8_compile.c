#define COBJMACROS
#include "d3d8_compile.h"
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

extern void recomp_safe_point(void);
extern volatile int g_recomp_irq_pending;

/* DXBC is independent of the host GPU. Hash all compiler inputs so changes
 * to generated HLSL automatically invalidate locally cached bytecode. */
static wchar_t shader_cache_dir[MAX_PATH];
static LONG shader_cache_serial;
static unsigned shader_cache_hits, shader_cache_misses;
#define SHADER_CACHE_LIMIT (16u * 1024u * 1024u)
typedef struct ShaderCacheHeader {
    char magic[8];
    unsigned char key[32], digest[32];
    uint32_t size;
} ShaderCacheHeader;

static int shader_hash_part(BCRYPT_HASH_HANDLE hash, const void *data, SIZE_T size)
{
    uint64_t length = size;
    return size <= SHADER_CACHE_LIMIT &&
        BCryptHashData(hash, (PUCHAR)&length, sizeof(length), 0) >= 0 &&
        (!size || BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) >= 0);
}

static int shader_hash(const void *source, SIZE_T size, const char *name,
    const char *entry, const char *target, UINT flags, unsigned char digest[32])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    int ok = 0;
    const char schema[] = "Conker D3DCompile cache v1";
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        goto done;
    if (BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    ok = shader_hash_part(hash, schema, sizeof(schema)) &&
         shader_hash_part(hash, source, size) &&
         shader_hash_part(hash, name ? name : "", name ? strlen(name) : 0) &&
         shader_hash_part(hash, entry ? entry : "", entry ? strlen(entry) : 0) &&
         shader_hash_part(hash, target ? target : "", target ? strlen(target) : 0) &&
         shader_hash_part(hash, &flags, sizeof(flags)) &&
         BCryptFinishHash(hash, digest, 32, 0) >= 0;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

static int shader_cache_path(const unsigned char key[32], wchar_t *path)
{
    static const wchar_t hex[] = L"0123456789abcdef";
    wchar_t leaf[70];
    if (!shader_cache_dir[0]) return 0;
    for (unsigned i = 0; i < 32; ++i) {
        leaf[2*i] = hex[key[i] >> 4]; leaf[2*i+1] = hex[key[i] & 15];
    }
    memcpy(leaf + 64, L".dxbc", 6 * sizeof(wchar_t));
    return swprintf(path, MAX_PATH, L"%ls\\%ls", shader_cache_dir, leaf) > 0;
}

void d3d8_shader_cache_init(const char *game_dir_utf8)
{
    wchar_t game[MAX_PATH], cache[MAX_PATH];
    shader_cache_dir[0] = 0;
    if (!game_dir_utf8 || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            game_dir_utf8, -1, game, MAX_PATH)) return;
    /* Reserve room for a key and the atomic-write temporary suffix. */
    if (wcslen(game) + 120 >= MAX_PATH) return;
    swprintf(cache, MAX_PATH, L"%ls\\shader-cache", game);
    CreateDirectoryW(cache, NULL);
    swprintf(shader_cache_dir, MAX_PATH, L"%ls\\d3d11-v1", cache);
    CreateDirectoryW(shader_cache_dir, NULL);
    DWORD attributes = GetFileAttributesW(shader_cache_dir);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        shader_cache_dir[0] = 0;
        return;
    }
    fprintf(stderr, "[shader-cache] local directory: %ls\n", shader_cache_dir);
}

static int shader_cache_load(const wchar_t *path, const unsigned char key[32], ID3DBlob **code)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    ShaderCacheHeader header;
    LARGE_INTEGER length;
    DWORD read = 0;
    ID3DBlob *blob = NULL;
    unsigned char digest[32];
    int ok = 0;
    if (!GetFileSizeEx(file, &length) ||
        !ReadFile(file, &header, sizeof(header), &read, NULL) || read != sizeof(header) ||
        memcmp(header.magic, "CNKDXBC1", 8) || memcmp(header.key, key, 32) ||
        header.size < 32 || header.size > SHADER_CACHE_LIMIT ||
        length.QuadPart != sizeof(header) + (uint64_t)header.size ||
        FAILED(D3DCreateBlob(header.size, &blob))) goto done;
    void *data = ID3D10Blob_GetBufferPointer(blob);
    if (!ReadFile(file, data, header.size, &read, NULL) || read != header.size ||
        memcmp(data, "DXBC", 4) ||
        !shader_hash(data, header.size, NULL, NULL, NULL, 0, digest) ||
        memcmp(digest, header.digest, 32)) goto done;
    *code = blob; blob = NULL; ok = 1;
done:
    if (blob) ID3D10Blob_Release(blob);
    CloseHandle(file);
    return ok;
}

static void shader_cache_store(const wchar_t *path, const unsigned char key[32], ID3DBlob *code)
{
    SIZE_T size = ID3D10Blob_GetBufferSize(code);
    void *data = ID3D10Blob_GetBufferPointer(code);
    ShaderCacheHeader header = {{0}};
    wchar_t temporary[MAX_PATH];
    if (size < 32 || size > SHADER_CACHE_LIMIT ||
        !shader_hash(data, size, NULL, NULL, NULL, 0, header.digest)) return;
    memcpy(header.magic, "CNKDXBC1", 8); memcpy(header.key, key, 32);
    header.size = (uint32_t)size;
    if (swprintf(temporary, MAX_PATH, L"%ls.%lu.%lu.tmp", path,
        GetCurrentProcessId(), (unsigned long)InterlockedIncrement(&shader_cache_serial)) < 0) return;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    int ok = WriteFile(file, &header, sizeof(header), &written, NULL) && written == sizeof(header) &&
             WriteFile(file, data, (DWORD)size, &written, NULL) && written == size;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(temporary);
}

typedef struct D3D8CompileJob {
    const void *source;
    SIZE_T size;
    const char *name, *entry, *target;
    UINT flags;
    ID3DBlob **code, **errors;
    HRESULT result;
} D3D8CompileJob;

static DWORD WINAPI d3d8_compile_worker(void *opaque)
{
    D3D8CompileJob *job = opaque;
    job->result = D3DCompile(job->source, job->size, job->name, NULL, NULL,
        job->entry, job->target, job->flags, 0, job->code, job->errors);
    return 0;
}

HRESULT d3d8_compile_shader(const void *source, SIZE_T size,
    const char *name, const char *entry, const char *target, UINT flags,
    ID3DBlob **code, ID3DBlob **errors)
{
    wchar_t path[MAX_PATH];
    unsigned char key[32];
    int cacheable = code && shader_cache_dir[0] &&
        shader_hash(source, size, name, entry, target, flags, key) && shader_cache_path(key, path);
    if (code) *code = NULL;
    if (errors) *errors = NULL;
    if (cacheable && shader_cache_load(path, key, code)) {
        if (++shader_cache_hits == 1 || !(shader_cache_hits % 64))
            fprintf(stderr, "[shader-cache] reused=%u compiled=%u\n", shader_cache_hits, shader_cache_misses);
        return S_OK;
    }
    ++shader_cache_misses;
    /* Keep guest interrupt delivery on the calling thread. The worker consumes
     * only host compiler inputs; it never executes guest code or GPU commands. */
    D3D8CompileJob job = {source, size, name, entry, target, flags, code, errors, E_FAIL};
    HANDLE worker = CreateThread(NULL, 0, d3d8_compile_worker, &job, 0, NULL);
    if (!worker) {
        d3d8_compile_worker(&job);
    } else {
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        HANDLE waits[2] = {worker, timer};
        while (WaitForSingleObject(worker, 0) == WAIT_TIMEOUT) {
            if (g_recomp_irq_pending) recomp_safe_point();
            LARGE_INTEGER due;
            due.QuadPart = -20000; /* 2 ms; guest clocks and pacing are unchanged. */
            if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            else
                WaitForSingleObject(worker, 1);
        }
        if (timer) CloseHandle(timer);
        CloseHandle(worker);
    }
    if (cacheable && SUCCEEDED(job.result) && *code) shader_cache_store(path, key, *code);
    return job.result;
}
