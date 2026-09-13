/* Exact vertex reuse within one immutable draw. Reset before each draw. */
#ifndef NV2A_VERTEX_CACHE_H
#define NV2A_VERTEX_CACHE_H

#include <stdint.h>
#include <string.h>

#define NV2A_VERTEX_CACHE_SLOTS 2048u
typedef struct {
    uint32_t input_plus_one;
    uint32_t output;
} nv2a_vertex_cache_entry;
typedef struct {
    nv2a_vertex_cache_entry entries[NV2A_VERTEX_CACHE_SLOTS];
} nv2a_vertex_cache;

static inline void nv2a_vertex_cache_reset(nv2a_vertex_cache *cache)
{
    memset(cache, 0, sizeof(*cache));
}

/* A collision replaces the entry only after comparing the complete input.
 * Inputs and outputs belong to this draw; no guest pointers survive reset. */
static inline int nv2a_vertex_cache_lookup(nv2a_vertex_cache *cache,
    const uint32_t *vertices, uint32_t stride, uint32_t input,
    uint32_t output, uint32_t *previous_output)
{
    const uint32_t *key = vertices + (size_t)input * stride;
    uint32_t hash = 2166136261u;
    for (uint32_t word = 0; word < stride; ++word)
        hash = (hash ^ key[word]) * 16777619u;
    hash ^= hash >> 16;
    nv2a_vertex_cache_entry *entry = &cache->entries[hash & (NV2A_VERTEX_CACHE_SLOTS - 1u)];
    if (entry->input_plus_one &&
        memcmp(key, vertices + (size_t)(entry->input_plus_one - 1u) * stride,
               (size_t)stride * sizeof(*vertices)) == 0) {
        *previous_output = entry->output;
        return 1;
    }
    entry->input_plus_one = input + 1u;
    entry->output = output;
    return 0;
}

#endif
