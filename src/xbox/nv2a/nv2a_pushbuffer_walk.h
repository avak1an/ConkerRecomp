#ifndef NV2A_PUSHBUFFER_WALK_H
#define NV2A_PUSHBUFFER_WALK_H
#include <stdint.h>
#include <stddef.h>

typedef int (*Nv2aPbMethod)(void *opaque, unsigned subchannel,
                            uint32_t method, uint32_t value);
typedef struct {
    uint32_t pc, words, methods, unhandled, jumps, external_jumps;
    /* 0 = PUT reached; 1 = address/packet bounds; 2 = opcode/call state;
     * 3 = work budget; 4 = synchronous method could not complete.
     * Never search payload for a replacement header. */
    unsigned error;
} Nv2aPbWalkResult;

/* GET can leave the newly submitted ring interval: compiled guest command
 * buffers jump back to the ring after their draws. The DMA memory limit bounds
 * reads; PUT is the stopping address, not a bound on every jump target.
 * PUT may be below GET after a ring wrap; follow the queued tail and its
 * jump rather than dropping it or assuming a physical ring end. */
static Nv2aPbWalkResult nv2a_pb_walk(const uint32_t *memory, uint32_t memory_bytes,
                                    uint32_t begin, uint32_t put, uint32_t budget,
                                    Nv2aPbMethod emit, void *opaque)
{
    Nv2aPbWalkResult r = {0};
    uint32_t return_pc = 0;
    r.pc = begin;
    if (!memory || !emit || (begin | put | memory_bytes) & 3u ||
        begin < 0x10000u || begin > memory_bytes ||
        put < 0x10000u || put > memory_bytes) {
        r.error = 1; return r;
    }
    while (r.pc != put) {
        uint32_t h, form, count, method, next, target = 0;
        int jump = 0, call = 0;
        if (r.words >= budget) {r.error = 3; break;}
        if (r.pc < 0x10000u || r.pc > memory_bytes - 4u) {r.error = 1; break;}
        h = memory[r.pc / 4u];
        ++r.words;
        next = r.pc + 4u;
        if ((h & 3u) == 1u) {target = h & 0xFFFFFFFCu; jump = 1;}
        else if ((h & 0xE0000003u) == 0x20000000u) {target = h & 0x1FFFFFFCu; jump = 1;}
        else if ((h & 3u) == 2u) {target = h & 0xFFFFFFFCu; jump = call = 1;}
        else if (h == 0x00020000u) {
            if (!return_pc) {r.error = 2; break;}
            target = return_pc; return_pc = 0; jump = 1;
        }
        if (jump) {
            if (target < 0x10000u || target > memory_bytes - 4u) {
                if (target != put) {r.error = 1; break;}
            }
            if (call) {
                if (return_pc) {r.error = 2; break;}
                return_pc = next;
            }
            ++r.jumps;
            if (begin <= put ? (target < begin || target >= put)
                             : (target < begin && target >= put))
                ++r.external_jumps;
            r.pc = target;
            continue;
        }
        form = h & 0xE0030003u;
        if (form != 0u && form != 0x40000000u) {r.error = 2; break;}
        count = (h >> 18) & 0x7FFu;
        method = h & 0x1FFCu;
        /* No packet may run across PUT, including the head of a wrapped
         * publication. Packets above PUT are bounded by DMA memory. */
        uint32_t limit = r.pc < put ? put : memory_bytes;
        if (count > (limit - next) / 4u) {r.error = 1; break;}
        if (count > budget - r.words) {r.error = 3; break;}
        if (form == 0u && count && method + (count - 1u) * 4u > 0x1FFCu) {
            r.error = 2; break;
        }
        for (uint32_t i = 0; i < count; ++i) {
            int handled = emit(opaque, (h >> 13) & 7u,
                               method + (form == 0u ? i * 4u : 0u),
                               memory[next / 4u + i]);
            if (handled < 0) {r.error = 4; return r;}
            if (!handled) ++r.unhandled;
        }
        r.methods += count;
        r.words += count;
        r.pc = next + count * 4u;
    }
    return r;
}
#endif
