#include "nv2a_pushbuffer.h"

#include "nv2a_pgraph_d3d11.h"
#include "nv2a_pushbuffer_walk.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern ptrdiff_t g_xbox_mem_offset;

#define PB_INC_MASK       0xE0030003u
#define PB_INC_MATCH      0x00000000u
#define PB_NONINC_MASK    0xE0030003u
#define PB_NONINC_MATCH   0x40000000u
#define PB_MAX_DWORDS     (NV2A_PB_MAX_BATCH_BYTES / sizeof(uint32_t))

static int s_initialized;
static uint32_t s_frame;

static const uint32_t *guest_words(uint32_t address)
{
    if (address >= 0x04000000u)
        return NULL;
    return (const uint32_t *)(uintptr_t)(address + (uintptr_t)g_xbox_mem_offset);
}

void nv2a_pushbuffer_init(void)
{
    if (s_initialized)
        return;

    pgraph_d3d11_init();
    s_initialized = 1;
}

static uint32_t s_pb_surface_target = 0xFFFFFFFFu; /* TEMP diagnostic */

/* ---- temporary: walker desynchronization detector ---------------------
 *
 * The retire release is a uniquely identifiable recurring packet -- inc
 * form, count 1, subchannel 0, method 0x1D70, payload a token that rises
 * by 2 and never repeats.  Scanning the submitted range for it before the
 * walk gives an independent opinion about where at least some real headers
 * are.  Any such position the walker does not treat as a header is a word
 * it swallowed as payload, and the first one is the divergence.
 *
 * This is a probe, not a parsing rule: nothing here changes what the
 * walker does. */
#define PB_TRACK_HDRS 8192u
static uint32_t s_hdr_pos[PB_TRACK_HDRS];
static uint32_t s_hdr_word[PB_TRACK_HDRS];
static uint32_t s_hdr_count[PB_TRACK_HDRS];
static unsigned s_hdr_n;

static uint32_t s_probe_pos[256];
static uint32_t s_probe_token[256];
static unsigned s_probe_n;

static uint32_t s_last_real_token;      /* highest token seen in order */
unsigned long long g_pb_desyncs;        /* swallowed real headers */
unsigned long long g_pb_resync_fires;   /* title-specific recoveries */

static int pb_is_retire_header(const uint32_t *w, uint32_t k, uint32_t n)
{
    uint32_t h, tok;

    if (k + 1u >= n) return 0;
    h = w[k];
    if ((h & PB_INC_MASK) != PB_INC_MATCH) return 0;
    if ((h & 0x1FFCu) != 0x1D70u) return 0;      /* WRITE_SEMAPHORE_RELEASE */
    if (((h >> 13) & 7u) != 0u) return 0;        /* subchannel 0 */
    if (((h >> 18) & 0x7FFu) != 1u) return 0;    /* exactly one parameter */
    tok = w[k + 1u];
    /* No parity test: the tokens this title uses are odd, and an earlier
     * probe that assumed even rejected every real release header. */
    if (s_last_real_token != 0u &&
        (tok <= s_last_real_token || tok > s_last_real_token + 0x1000u))
        return 0;                                /* must continue the run */
    return 1;
}

extern int nv2a_pg_subchannel_bound(int sub);

static int pb_ptr_like(uint32_t w)
{
    if (w >= 0x1000u && w < 0x04000000u) return 1;
    if ((w & 0xF0000000u) == 0x80000000u &&
        (w & 0x0FFFFFFFu) < 0x04000000u) return 1;
    return 0;
}

static int pb_cmd_like(uint32_t w)
{
    uint32_t c;
    if ((w & PB_INC_MASK) != PB_INC_MATCH &&
        (w & PB_NONINC_MASK) != PB_NONINC_MATCH) return 0;
    c = (w >> 18) & 0x7FFu;
    if (c == 0u || c > 128u) return 0;
    return nv2a_pg_subchannel_bound((int)((w >> 13) & 7u));
}

/* What is the memory after the last good packet?  Reported rather than acted
 * on: the point is to say whether every divergence lands in the same shape of
 * data, not to pattern-match one title. */
static void pb_classify(const uint32_t *w, uint32_t from, uint32_t n)
{
    uint32_t k, span = 64u, ptrs = 0, cmds = 0, zeros = 0, smalls = 0;
    uint32_t best_p = 0, best_hits = 0, p;

    if (from + span > n) span = n - from;
    if (span < 8u) { fprintf(stderr, "[DIV]   region: too short to classify\n"); return; }
    for (k = from; k < from + span; ++k) {
        uint32_t v = w[k];
        if (v == 0u) ++zeros;
        else if (v < 0x1000u) ++smalls;
        if (pb_ptr_like(v)) ++ptrs;
        if (pb_cmd_like(v)) ++cmds;
    }
    for (p = 4u; p <= 16u; ++p) {
        uint32_t hits = 0;
        for (k = from; k + p < from + span; ++k)
            if ((w[k] & 0xFFFF0000u) == (w[k + p] & 0xFFFF0000u)) ++hits;
        if (hits > best_hits) { best_hits = hits; best_p = p; }
    }
    fprintf(stderr, "[DIV]   region over %u words: ptr-like=%u cmd-like=%u zero=%u small=%u; strongest period=%u (%u/%u matches)\n",
            span, ptrs, cmds, zeros, smalls, best_p, best_hits,
            span - best_p);
    fprintf(stderr, "[DIV]   classification: %s\n",
            (best_hits * 3u > (span - best_p) * 2u && ptrs * 3u > span)
                ? "repeating descriptor records with pointers"
            : (ptrs * 2u > span) ? "pointers/addresses"
            : (cmds * 2u > span) ? "command-looking"
            : (zeros + smalls) * 2u > span ? "constants/small values"
                                           : "unknown");
}
/* Control-flow accounting.  Old and long forms are counted apart because
 * they behave differently in this stream: the long form chains inside the
 * batch, the old form points at buffers outside it. */
unsigned long long g_pb_jmp_old_ok, g_pb_jmp_old_bad;
unsigned long long g_pb_jmp_long_ok, g_pb_jmp_long_bad;
unsigned long long g_pb_jmp_budget_hit;
unsigned long long g_cwin_packets, g_cwin_garbage;
unsigned long long g_cwin_next_is_header, g_cwin_next_not_header;
static const char *pb_form(uint32_t w);

static void pb_dump_window(const uint32_t *w, uint32_t n, uint32_t centre,
                           uint32_t claim_hdr, uint32_t claim_count)
{
    uint32_t lo = (claim_hdr != 0xFFFFFFFFu && claim_hdr > 80u) ? claim_hdr - 80u
                : ((centre > 96u) ? centre - 96u : 0u);
    uint32_t hi = centre + 8u;
    uint32_t k, h;

    if (hi > n) hi = n;
    /* Last good packet = the last recorded header before the first one whose
     * form is neither increasing nor non-increasing, or whose subchannel was
     * never bound.  Both are properties of the stream, not of this title. */
    {   unsigned h; uint32_t lg = 0xFFFFFFFFu, bogus = 0xFFFFFFFFu;
        for (h = 0; h < s_hdr_n && s_hdr_pos[h] <= claim_hdr; ++h) {
            uint32_t hw = s_hdr_word[h];
            int ok = ((hw & PB_INC_MASK) == PB_INC_MATCH ||
                      (hw & PB_NONINC_MASK) == PB_NONINC_MATCH) &&
                     nv2a_pg_subchannel_bound((int)((hw >> 13) & 7u));
            if (ok) { lg = h; } else { bogus = h; break; }
        }
        if (lg != 0xFFFFFFFFu)
            fprintf(stderr, "[DIV]   last good packet @%u %08X method=%04X sub=%u count=%u ends @%u\n",
                    s_hdr_pos[lg], s_hdr_word[lg], s_hdr_word[lg] & 0x1FFCu,
                    (s_hdr_word[lg] >> 13) & 7u, s_hdr_count[lg],
                    s_hdr_pos[lg] + s_hdr_count[lg] + 1u);
        if (bogus != 0xFFFFFFFFu) {
            uint32_t bp = s_hdr_pos[bogus], k2;
            uint32_t lo2 = (bp > 16u) ? bp - 16u : 0u, hi2 = bp + 48u;
            fprintf(stderr, "[DIV]   first bogus header @%u %08X %s method=%04X sub=%u count=%u\n",
                    bp, s_hdr_word[bogus], pb_form(s_hdr_word[bogus]),
                    s_hdr_word[bogus] & 0x1FFCu,
                    (s_hdr_word[bogus] >> 13) & 7u, s_hdr_count[bogus]);
            if (hi2 > n) hi2 = n;
            for (k2 = lo2; k2 < hi2; ++k2)
                fprintf(stderr, "[DIV]   %s[%5u] %08X %s\n",
                        k2 == bp ? "==> " : "    ", k2, w[k2],
                        pb_form(w[k2]));
            pb_classify(w, bp, n);
        }
    }

    /* the packet chain leading in: where the header stream stops making
     * sense is the divergence, not where the swallow is noticed */
    {   unsigned h, first = 0;
        for (h = 0; h < s_hdr_n; ++h)
            if (s_hdr_pos[h] >= claim_hdr) { first = (h > 40u) ? h - 40u : 0u; break; }
        fprintf(stderr, "[DESYNC] last %u packets before it:\n",
                (unsigned)(s_hdr_n > first ? 41u : 0u));
        for (h = first; h < s_hdr_n && s_hdr_pos[h] <= claim_hdr; ++h)
            fprintf(stderr, "[DESYNC]   pkt @%5u %08X method=%04X sub=%u count=%-4u %s  ends @%u\n",
                    s_hdr_pos[h], s_hdr_word[h], s_hdr_word[h] & 0x1FFCu,
                    (s_hdr_word[h] >> 13) & 7u, s_hdr_count[h],
                    pb_form(s_hdr_word[h]),
                    s_hdr_pos[h] + s_hdr_count[h] + 1u);
    }
    for (k = lo; k < hi; ++k) {
        const char *mark = "";
        if (k == centre)          mark = "  <== REAL header, swallowed";
        else if (k == claim_hdr)  mark = "  <== walker header claiming it";
        else if (claim_hdr != 0xFFFFFFFFu && k > claim_hdr &&
                 k <= claim_hdr + claim_count) mark = "   (claimed payload)";
        for (h = 0; h < s_hdr_n; ++h)
            if (s_hdr_pos[h] == k && k != claim_hdr && k != centre) {
                mark = "  <- walker header";
                break;
            }
        fprintf(stderr, "[DESYNC]   [%5u] %08X%s\n", k, w[k], mark);
    }
}
/* How much of the ring never reaches the walker at all, and how many real
 * retire releases are in it.  Counting only -- nothing is dispatched. */
unsigned long long g_pb_skip_events, g_pb_skip_bytes, g_pb_skip_retires;
unsigned long long g_pb_jumps, g_pb_jumps_in_range;

static int pb_count_skipped(void *opaque, unsigned sub, uint32_t method, uint32_t value)
{
    unsigned *counts = opaque;
    (void)sub;
    if (method == 0x17FCu) ++counts[value ? 0 : 1];
    if (method == 0x1800u || method == 0x1808u || method == 0x1810u || method == 0x1818u) ++counts[2];
    return 1;
}

void nv2a_pb_note_skipped(uint32_t begin, uint32_t end, const char *why)
{
    const uint32_t *w;
    uint32_t n, k, found = 0;
    static unsigned logs;

    if (end <= begin || begin < 0x1000u || end > 0x04000000u) return;
    ++g_pb_skip_events;
    g_pb_skip_bytes += (end - begin);
    w = guest_words(begin);
    if (!w) return;
    n = (end - begin) / 4u;
    if (getenv("CONKER_PB_SKIP_AUDIT") && n > 0x4000u && n < 0x40000u) {
        unsigned counts[3] = {0};
        Nv2aPbWalkResult dry = nv2a_pb_walk(guest_words(0), 0x04000000u,
            begin, end, 0x100000u, pb_count_skipped, counts);
        fprintf(stderr, "[PBSKIP-AUDIT] ms=%llu %08X..%08X pc=%08X error=%u methods=%u begins=%u ends=%u geometry=%u\n",
                (unsigned long long)GetTickCount64(), begin, end, dry.pc, dry.error,
                dry.methods, counts[0], counts[1], counts[2]);
    }
    for (k = 0; k + 1u < n; ++k) {
        uint32_t h = w[k];
        if ((h & PB_INC_MASK) != PB_INC_MATCH) continue;
        if ((h & 0x1FFCu) != 0x1D70u) continue;
        if (((h >> 13) & 7u) != 0u) continue;
        if (((h >> 18) & 0x7FFu) != 1u) continue;
        if ((w[k + 1u] & 1u) != 0u) continue;
        ++found; ++k;
    }
    g_pb_skip_retires += found;
    if (logs++ < 12u)
        fprintf(stderr, "[PBSKIP] %s %08X..%08X (%u dwords) contains %u retire releases\n", why, begin, end, n, found);
}

/* ---- temporary: first-desync locator ---------------------------------
 *
 * A batch that is parsed correctly consumes every word exactly once: the
 * walk ends on the last word, nothing is rejected, and no recovery
 * heuristic has to fire.  Any batch failing that is a desynchronized one,
 * and the first such batch after the movie is the one worth looking at.
 *
 * The previous batch is kept as well, because the interesting question is
 * whether the walk started in the wrong place rather than went wrong in
 * the middle: begin is the previous DMA_PUT, so anything the title put
 * between the two is walked as if it were commands. */
static uint32_t s_prev_begin, s_prev_end;
static uint32_t s_prev_tail[16];
static unsigned s_prev_tail_n;
static int      s_prev_clean;
unsigned long long g_pb_batches, g_pb_batches_dirty;

static double pb_seconds(void)
{
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (f.QuadPart == 0) { QueryPerformanceFrequency(&f);
                           QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

static double pb_trace_after(void)
{
    static double v = -1.0;
    if (v < 0.0) {
        const char *e = getenv("CONKER_PB_TRACE_AFTER");
        v = (e && *e) ? atof(e) : 1e9;
    }
    return v;
}

static const char *pb_form(uint32_t w)
{
    if ((w & 3u) == 1u) return "JUMP";
    if ((w & 3u) == 2u) return "CALL";
    if (w == 0x00020000u) return "RET";
    if ((w & 0xE0000003u) == 0x20000000u) return "JUMP-LONG";
    if ((w & PB_INC_MASK) == PB_INC_MATCH) return "inc";
    if ((w & PB_NONINC_MASK) == PB_NONINC_MATCH) return "non-inc";
    return "OTHER";
}
static int pb_emit_linked(void *opaque, unsigned subchannel,
                          uint32_t method, uint32_t value)
{
    if (opaque) pb_count_skipped(opaque, subchannel, method, value);
    if (method == 0x100u && value) {
        extern int nv2a_pgraph_software_method(unsigned, uint32_t);
        return nv2a_pgraph_software_method(subchannel, value);
    }
    return pgraph_d3d11_method((int)subchannel, method, value);
}

void nv2a_pushbuffer_submit_guest(uint32_t begin, uint32_t end)
{
    /* Keep the old interval-only parser available for diagnostic comparisons.
     * It cannot execute the title's compiled command buffers outside the ring. */
    static int legacy = -1;
    if (legacy < 0) legacy = getenv("CONKER_PB_LEGACY") != NULL;
    if (!legacy) {
        static unsigned linked_logs, error_logs;
        nv2a_pushbuffer_init();
        if (begin < 0x10000u || begin >= 0x04000000u ||
            end < 0x10000u || end > 0x04000000u || end == begin ||
            ((begin | end) & 3u) ||
            (end > begin && end - begin > NV2A_PB_MAX_BATCH_BYTES))
            return;
        unsigned wrap_counts[3] = {0};
        int wrap_audit = begin > end && getenv("CONKER_PB_WRAP_AUDIT") != NULL;
        Nv2aPbWalkResult r = nv2a_pb_walk(
            guest_words(0), 0x04000000u, begin, end, 0x100000u,
            pb_emit_linked, wrap_audit ? wrap_counts : NULL);
        if (wrap_audit)
            fprintf(stderr, "[PBWRAP-AUDIT] ms=%llu GET=%08X PUT=%08X pc=%08X error=%u methods=%u begins=%u ends=%u geometry=%u jumps=%u\n",
                    (unsigned long long)GetTickCount64(), begin, end, r.pc,
                    r.error, r.methods, wrap_counts[0], wrap_counts[1], wrap_counts[2], r.jumps);
        ++g_pb_batches;
        if (r.error) ++g_pb_batches_dirty;
        if ((r.external_jumps && linked_logs++ < 16u) ||
            (r.error && error_logs++ < 16u))
            fprintf(stderr, "[PB-LINKED] range=%08X..%08X pc=%08X words=%u "
                    "methods=%u unhandled=%u jumps=%u external=%u error=%u\n",
                    begin, end, r.pc, r.words, r.methods, r.unhandled,
                    r.jumps, r.external_jumps, r.error);
        {extern void nv2a_draw_census_frame(void); nv2a_draw_census_frame();}
        pgraph_d3d11_flush();
        ++s_frame;
        return;
    }
    static unsigned s_frontend_packet_repair_logs;
    static unsigned s_method_trace_logs;
    static unsigned s_unhandled_method_logs;
    static unsigned s_rejected_header_logs;
    static unsigned s_non_kelvin_method_logs;
    const uint32_t *words;
    uint32_t dwords;
    uint32_t pos = 0;
    unsigned k2;
    uint32_t jumps_taken = 0;
    static unsigned s_jump_logs_old, s_jump_logs_long;
    uint32_t methods = 0;
    uint32_t handled = 0;
    uint32_t unhandled = 0;
    uint32_t rejected = 0;
    unsigned long long resync_at_entry = g_pb_resync_fires;
    uint32_t first_bad_pos = 0xFFFFFFFFu;
    uint32_t first_bad_word = 0;

    nv2a_pushbuffer_init();

    /* The bootstrap command ring is a normal linear 64 KiB guest buffer.
     * Do not follow GPU jump commands until the complete DMA-object model is
     * available: rejecting them preserves the current stable title path. */
    if (begin == 0 || end <= begin || begin >= 0x04000000u ||
        end > 0x04000000u || ((begin | end) & 3u) != 0) {
        return;
    }
    dwords = (end - begin) / sizeof(uint32_t);
    if (dwords > PB_MAX_DWORDS)
        return;
    words = guest_words(begin);
    if (!words)
        return;

    /* TEMPORARY DIAGNOSTIC: the surface-state producers (538F18) write
     * SET_SURFACE_PITCH/COLOR_OFFSET/ZETA_OFFSET into this range, yet no
     * such method has ever reached pgraph.  Report whether the words are
     * physically present and where, independent of how the walker below
     * synchronizes.  Remove once surface state is arriving. */
    {
        static unsigned s_surface_scan_logs;
        if (s_surface_scan_logs < 6u || s_pb_surface_target == 0xFFFFFFFFu) {
            uint32_t k, n20c = 0, n210 = 0, n214 = 0, first = 0xFFFFFFFFu;
            for (k = 0; k < dwords; ++k) {
                if (words[k] == 0x0004020Cu) { ++n20c; if (first == 0xFFFFFFFFu) first = k; }
                else if (words[k] == 0x00040210u) { ++n210; if (first == 0xFFFFFFFFu) first = k; }
                else if (words[k] == 0x00040214u) { ++n214; if (first == 0xFFFFFFFFu) first = k; }
            }
            s_pb_surface_target = first;
            ++s_surface_scan_logs;
            fprintf(stderr,
                    "[DEBUG PB-SURFACE-SCAN] range=%08X..%08X dwords=%u "
                    "pitch(020C)=%u color(0210)=%u zeta(0214)=%u first-at=%d\n",
                    begin, end, dwords, n20c, n210, n214,
                    first == 0xFFFFFFFFu ? -1 : (int)first);

            /* The compositor draws each pass as an inline quad: a
             * SET_BEGIN_END (0x17FC) opening the primitive, INLINE_ARRAY
             * (0x1818) vertex payload, then SET_BEGIN_END 0 to submit.  None
             * of those has produced a draw on a compositor target, so report
             * whether the packets are physically in the submitted range at
             * all -- independent of how the walker below synchronizes. */
            {
                uint32_t nbegin = 0, ninline = 0, ntex = 0;
                uint32_t first_begin = 0xFFFFFFFFu;
                uint32_t first_inline = 0xFFFFFFFFu;
                for (k = 0; k < dwords; ++k) {
                    uint32_t m = words[k] & 0x1FFCu;
                    uint32_t top = words[k] & 0xE0030003u;
                    if (top != 0x00000000u && top != 0x40000000u)
                        continue;   /* not a plausible method header */
                    if (m == 0x17FCu) {
                        ++nbegin;
                        if (first_begin == 0xFFFFFFFFu) first_begin = k;
                    } else if (m == 0x1818u) {
                        ++ninline;
                        if (first_inline == 0xFFFFFFFFu) first_inline = k;
                    } else if (m == 0x1B00u) {
                        /* SET_TEXTURE_OFFSET stage 0 -- the compositor's
                         * source select.  Report the value that follows the
                         * header so a missing pass shows up as a value the
                         * walker never dispatched. */
                        ++ntex;
                        if (ntex <= 8u && k + 1u < dwords)
                            fprintf(stderr,
                                    "[DEBUG PB-TEX-SCAN] pos=%u value=%08X\n",
                                    k, words[k + 1u]);
                    }
                }
                fprintf(stderr,
                        "[DEBUG PB-DRAW-SCAN] begin_end(17FC)=%u first-at=%d "
                        "inline_array(1818)=%u first-at=%d tex_offset(1B00)=%u\n",
                        nbegin,
                        first_begin == 0xFFFFFFFFu ? -1 : (int)first_begin,
                        ninline,
                        first_inline == 0xFFFFFFFFu ? -1 : (int)first_inline,
                        ntex);
            }
        }
    }

    /* independent opinion about where the real retire headers are */
    s_hdr_n = 0;
    s_probe_n = 0;
    {   uint32_t k, tok = s_last_real_token;
        for (k = 0; k + 1u < dwords && s_probe_n < 256u; ++k) {
            uint32_t save = s_last_real_token;
            s_last_real_token = tok;
            if (pb_is_retire_header(words, k, dwords)) {
                s_probe_pos[s_probe_n] = k;
                s_probe_token[s_probe_n] = words[k + 1u];
                ++s_probe_n;
                tok = words[k + 1u];
                ++k;              /* its payload is not itself a header */
            }
            s_last_real_token = save;
        }
    }

    while (pos < dwords) {
        uint32_t header = words[pos];
        uint32_t count;
        uint32_t method;
        uint32_t subchannel;
        uint32_t i;

        if (header == 0) {
            ++pos;
            continue;
        }

        /* What kind of command is this actually?
         *
         * The walker below reads every word as an increasing-method header.
         * NV2A has four other forms -- jump, call, return and non-increasing
         * methods -- and the title's geometry may well be behind them: only
         * about nine inline quads reach pgraph per frame and the array path
         * never runs at all.  Count the forms before changing how any of them
         * is treated. */
        { static unsigned long n_inc, n_noninc, n_jump, n_call, n_ret, n_other;
          static unsigned reported;
          if ((header & 3u) == 1u) ++n_jump;
          else if ((header & 3u) == 2u) ++n_call;
          else if (header == 0x00020000u) ++n_ret;
          else if ((header & 0xE0030003u) == 0u) ++n_inc;
          else if ((header & 0xE0030003u) == 0x20000000u) ++n_noninc;
          else ++n_other;
          if ((n_inc + n_noninc + n_jump + n_call + n_ret + n_other) % 200000ul
              == 0ul && reported++ < 12u)
              fprintf(stderr,
                      "[INFO NV2A-PB-FORM] inc=%lu non-inc=%lu jump=%lu "
                      "call=%lu ret=%lu other=%lu\n",
                      n_inc, n_noninc, n_jump, n_call, n_ret, n_other); }

        count = (header >> 18) & 0x7FFu;
        method = header & 0x1FFCu;
        subchannel = (header >> 13) & 7u;
        if (s_hdr_n < PB_TRACK_HDRS) {
            s_hdr_pos[s_hdr_n] = pos;
            s_hdr_word[s_hdr_n] = header;
            s_hdr_count[s_hdr_n] = count;
            ++s_hdr_n;
        }
        /* ---- control flow ------------------------------------------------
         *
         * Two jump encodings appear in this stream and neither was decoded,
         * so both fell to the reject branch, which advances a single dword
         * and then reads the descriptor it landed in as method headers.
         *
         *   old form   (w & 3) == 1                  target = w & FFFFFFFC
         *   long form  (w & E0000003) == 20000000    target = w & 1FFFFFFC
         *
         * The old-form target is not guessed: the eight-dword record that
         * carries the jump also carries the same address at +7 in cached-
         * alias form (030130ED at +2, 830130EC at +7), which fixes the
         * encoding from the data itself.
         *
         * A jump is followed only if it lands inside the range actually
         * submitted.  That is not caution for its own sake: 21xxxxxx words
         * found deeper inside an already-desynchronized region decode to
         * targets before the batch start, so the encoding alone does not
         * make a word a command.  Out-of-range targets are counted and
         * skipped, exactly as before, and nothing is dispatched for them. */
        {   uint32_t target = 0u;
            int is_old  = ((header & 3u) == 1u);
            int is_long = ((header & 0xE0000003u) == 0x20000000u);

            if (is_old || is_long) {
                uint32_t here = begin + pos * 4u;
                int ok;

                target = is_old ? (header & 0xFFFFFFFCu)
                                : (header & 0x1FFFFFFCu);
                ok = ((target & 3u) == 0u) &&
                     (target >= begin) && (target < end) &&
                     (((target - begin) & 3u) == 0u) &&
                     (target != here);

                /* Termination: a batch cannot need more jumps than it has
                 * words, and a chain that says otherwise is a loop. */
                if (ok && jumps_taken >= dwords) {
                    ++g_pb_jmp_budget_hit;
                    ok = 0;
                }

                if ((is_old ? s_jump_logs_old++ : s_jump_logs_long++) < 8u) {

                    fprintf(stderr, "[JMP] frame=%u src=%08X word=%08X %s target=%08X %s -> %s\n",
                            s_frame + 1u, here, header,
                            is_old ? "old " : "long", target,
                            (target >= begin && target < end) ? "in-range"
                                                              : "OUT-OF-RANGE",
                            ok ? "followed" : "rejected");
                }

                if (ok) {
                    if (is_old) ++g_pb_jmp_old_ok; else ++g_pb_jmp_long_ok;
                    ++jumps_taken;
                    pos = (target - begin) / 4u;
                    continue;          /* not dispatched, nothing consumed */
                }
                if (is_old) ++g_pb_jmp_old_bad; else ++g_pb_jmp_long_bad;
                ++rejected;
                ++pos;
                continue;
            }
        }

        if (count == 0 || count > dwords - pos - 1) {
            if (s_rejected_header_logs++ < 16u)
                fprintf(stderr,
                        "[WARN NV2A-PB] rejected header frame=%u pos=%u "
                        "word=%08X count=%u remaining=%u\n",
                        s_frame + 1u, pos, header, count,
                        dwords - pos - 1u);
            if (first_bad_pos == 0xFFFFFFFFu) {
                first_bad_pos = pos; first_bad_word = header;
            }
            ++rejected;
            ++pos;
            continue;
        }

        /* The recovered Frontend compiler can leave an earlier packet's
         * declared payload spanning the live vertex-array setup.  The setup
         * is a uniquely shaped adjacent pair of 16-value incrementing
         * packets (1720 offsets followed by 1760 formats).  Split the outer
         * packet at that boundary so the normal sequential parser retains
         * the live bindings before the following DrawArrays commands. */
        for (i = 1u; i + 33u < count; ++i) {
            uint32_t offset_header = words[pos + 1u + i];
            uint32_t format_header = words[pos + 1u + i + 17u];
            uint32_t first_offset = words[pos + 1u + i + 1u];
            uint32_t first_format = words[pos + 1u + i + 18u];
            uint32_t second_format = words[pos + 1u + i + 19u];
            if (offset_header == 0x00401720u &&
                format_header == 0x00401760u &&
                first_offset != 0u && first_offset < 0x04000000u &&
                first_format != 0u && second_format != 0u) {
                if (s_frontend_packet_repair_logs++ < 16u) {
                    fprintf(stderr,
                            "[INFO NV2A-PB] resynchronized live Frontend "
                            "array setup outer=%04X declared=%u actual=%u "
                            "offset=%08X formats=%08X,%08X\n",
                            method, count, i, first_offset,
                            first_format, second_format);
                }
                ++g_pb_resync_fires;
                fprintf(stderr, "[RESYNC] frame=%u pos=%u header=%08X declared=%u actual=%u skipped=%u\n",
                        s_frame + 1u, pos, header, count, i,
                        count - i); fflush(stderr);
                count = i;
                break;
            }
        }

        /* Direct Frontend startup can leave the packed-state upload at
         * method 0x0804 six values short.  Its late patch application then
         * makes the following BEGIN plus DrawPrimitiveUP packet look like
         * the final values of the 64-value upload.  Retail supplies those
         * absent stage values before this point.  Resynchronize only when a
         * complete BEGIN -> inline-array sequence is present in that short
         * tail, so ordinary method payloads remain untouched. */
        if ((header & PB_INC_MASK) == PB_INC_MATCH &&
            method == 0x0804u && count == 64u) {
            /* The native Frontend packed-state compiler can also terminate
             * this upload after its two real values and append ordinary
             * one-value packets directly behind it.  The first packet in
             * the observed retail stream is SET_TEXTURE_CONTROL1 (1B0C)
             * with value zero.  Treating its header as another 0804 payload
             * shifts every following command into the 0804..08E8 range and
             * hides the vertex-array state from PGRAPH.  Recognize this
             * narrow, self-validating boundary before using the older tail
             * BEGIN resynchronization below. */
            for (i = 2u; i + 3u < count; ++i) {
                uint32_t candidate = words[pos + 1u + i];
                if (candidate == 0x00041B0Cu &&
                    words[pos + 1u + i + 1u] == 0u &&
                    words[pos + 1u + i + 2u] == 0x00041B0Cu &&
                    words[pos + 1u + i + 3u] == 0u) {
                    if (s_frontend_packet_repair_logs++ < 16u) {
                        fprintf(stderr,
                                "[INFO NV2A-PB] resynchronized packed "
                                "Frontend state method=0804 declared=64 "
                                "actual=%u next=1B0C\n",
                                i);
                    }
                    ++g_pb_resync_fires;
                fprintf(stderr, "[RESYNC] frame=%u pos=%u header=%08X declared=%u actual=%u skipped=%u\n",
                        s_frame + 1u, pos, header, count, i,
                        count - i); fflush(stderr);
                count = i;
                    break;
                }
            }
        }

        if ((header & PB_INC_MASK) == PB_INC_MATCH &&
            method == 0x0804u && count == 64u) {
            uint32_t first_tail = count - 8u;
            for (i = first_tail; i < count; ++i) {
                uint32_t candidate = words[pos + 1u + i];
                uint32_t inline_header;
                uint32_t inline_count;
                if ((candidate & PB_INC_MASK) != PB_INC_MATCH ||
                    (candidate & 0x1FFCu) != 0x17FCu ||
                    ((candidate >> 18) & 0x7FFu) != 1u ||
                    pos + 1u + i + 3u > dwords)
                    continue;
                inline_header = words[pos + 1u + i + 2u];
                inline_count = (inline_header >> 18) & 0x7FFu;
                if ((inline_header & PB_NONINC_MASK) == PB_NONINC_MATCH &&
                    (inline_header & 0x1FFCu) == 0x1818u &&
                    inline_count != 0u &&
                    pos + 1u + i + 3u + inline_count <= dwords) {
                    if (s_frontend_packet_repair_logs++ < 16u) {
                        fprintf(stderr,
                                "[INFO NV2A-PB] resynchronized short Frontend "
                                "state packet method=0804 declared=64 actual=%u "
                                "inline=%u\n",
                                i, inline_count);
                    }
                    ++g_pb_resync_fires;
                fprintf(stderr, "[RESYNC] frame=%u pos=%u header=%08X declared=%u actual=%u skipped=%u\n",
                        s_frame + 1u, pos, header, count, i,
                        count - i); fflush(stderr);
                count = i;
                    break;
                }
            }
        }

        /* Every packet that lands in the transform-constant window, with its
         * declared payload and the word that follows it.  The constant
         * handler is stateless -- it derives index and lane from the method
         * alone -- so a command header arriving as constant data can only
         * mean the packet declared more payload than it has. */
        if (method >= 0x0B80u && method < 0x0C00u) {
            static unsigned clogs;
            /* A constant payload word that is non-zero but denormal as a
             * float is an integer wearing a float costume -- a command header,
             * not shader data.  That is the corruption signature. */
            int suspicious = 0;
            {   uint32_t k;
                for (k = 0; k < count && pos + 1u + k < dwords; ++k) {
                    uint32_t w2 = words[pos + 1u + k];
                    if (w2 != 0u && (w2 & 0x7F800000u) == 0u) suspicious = 1;
                } }
            ++g_cwin_packets;
            if (suspicious) ++g_cwin_garbage;
            /* Does the packet actually overrun?  It does not if the word
             * after its declared payload is itself a valid header. */
            if (pos + 1u + count < dwords) {
                uint32_t nx = words[pos + 1u + count];
                if ((nx & PB_INC_MASK) == PB_INC_MATCH ||
                    (nx & PB_NONINC_MASK) == PB_NONINC_MATCH ||
                    (nx & 3u) == 1u || (nx & 3u) == 2u)
                    ++g_cwin_next_is_header;
                else
                    ++g_cwin_next_not_header;
            }
            if (suspicious && clogs++ < 4u) {
                uint32_t k;
                fprintf(stderr, "[CWIN] frame=%u pos=%u header=%08X %s method=%04X count=%u sub=%u  payload at guest %08X..%08X\n",
                        s_frame + 1u, pos, header, pb_form(header), method,
                        count, subchannel,
                        begin + (pos + 1u) * 4u,
                        begin + (pos + 1u + count) * 4u);
                for (k = 0; k < count && pos + 1u + k < dwords; ++k) {
                    union { uint32_t u; float f; } cv;
                    cv.u = words[pos + 1u + k];
                    fprintf(stderr, "[CWIN]   payload[%2u] %08X  %.5f\n",
                            k, cv.u, cv.f);
                }
                if (pos + 1u + count < dwords)
                    fprintf(stderr, "[CWIN]   next word  %08X  %s\n",
                            words[pos + 1u + count],
                            pb_form(words[pos + 1u + count]));
                fflush(stderr);
            }
        }

        if ((header & PB_INC_MASK) == PB_INC_MATCH) {
            for (i = 0; i < count; ++i) {
                uint32_t item_method = method + i * 4u;
                uint32_t item_param = words[pos + 1 + i];
                if (subchannel != 0u && s_non_kelvin_method_logs++ < 128u)
                    fprintf(stderr,
                            "[DEBUG NV2A-SUBCHANNEL] frame=%u sub=%u "
                            "pos=%u method=%04X param=%08X header=%08X\n",
                            s_frame + 1u, subchannel, pos + 1u + i,
                            item_method, item_param, header);
                if (item_method == 0x020Cu || item_method == 0x0210u ||
                    item_method == 0x0214u) {
                    static unsigned s_surface_dispatch_logs;
                    if (s_surface_dispatch_logs++ < 400u)
                        fprintf(stderr,
                                "[DEBUG PB-SURFACE-DISPATCH] pos=%u method=%04X "
                                "param=%08X sub=%u\n",
                                pos + 1u + i, item_method, item_param,
                                subchannel);
                }
                /* Every SET_CONTEXT_DMA_SEMAPHORE, with the raw stream
                 * around it: a bind that zeroes the retire target decides
                 * whether S can advance at all, so it must be possible to
                 * tell a real command from a mis-synchronized one. */
                if (item_method == 0x01A4u) {
                    static unsigned s_sem_bind_logs;
                    if (s_sem_bind_logs++ < 16u) {
                        uint32_t k, lo = (pos > 6u) ? pos - 6u : 0u;
                        uint32_t hi = pos + 8u;
                        if (hi > dwords) hi = dwords;
                        fprintf(stderr,
                                "[SEMPB] frame=%u pos=%u hdr=%08X count=%u sub=%u param=%08X  addr=%08X\n",
                                s_frame + 1u, pos, header, count, subchannel,
                                item_param, begin + (pos + 1u + i) * 4u);
                        for (k = lo; k < hi; ++k)
                            fprintf(stderr, "[SEMPB]   [%u] %08X%s\n",
                                    k, words[k],
                                    k == pos ? "  <- header"
                                    : (k == pos + 1u + i ? "  <- param" : ""));
                        fflush(stderr);
                    }
                }
                /* The retire offset is 0 for the life of the ring.  A
                 * non-zero one on subchannel 0 is therefore a word that is
                 * not a command, and the walk that produced it is the
                 * divergence being looked for. */
                if (item_method == 0x1D6Cu && subchannel == 0u &&
                    item_param != 0u) {
                    static unsigned dlogs;
                    if (dlogs++ < 2u) {
                        uint32_t k, lo = (pos > 32u) ? pos - 32u : 0u;
                        uint32_t hi = pos + 64u, h;
                        if (hi > dwords) hi = dwords;
                        fprintf(stderr, "[DIVERGE] frame=%u pos=%u header=%08X method=%04X count=%u sub=%u param=%08X  guest=%08X\n",
                                s_frame + 1u, pos, header, item_method,
                                count, subchannel, item_param,
                                begin + (pos + 1u + i) * 4u);
                        for (k = lo; k < hi; ++k) {
                            const char *m = "";
                            if (k == pos) m = "  <== walker header";
                            else if (k == pos + 1u + i) m = "  <== the bad param";
                            else for (h = 0; h < s_hdr_n; ++h)
                                if (s_hdr_pos[h] == k) { m = "  <- walker header"; break; }
                            fprintf(stderr, "[DIVERGE]   [%5u] %08X%s\n",
                                    k, words[k], m);
                        }
                        fflush(stderr);
                    }
                }
                int item_handled = pgraph_d3d11_method(
                    (int)subchannel, item_method, item_param);
                if (item_handled) ++handled; else ++unhandled;
                if (!item_handled && s_unhandled_method_logs++ < 400u)
                    fprintf(stderr,
                            "[WARN NV2A-PB] unhandled frame=%u sub=%u "
                            "method=%04X param=%08X\n",
                            s_frame + 1u, subchannel, item_method,
                            item_param);
                if (s_method_trace_logs < 256u) {
                    fprintf(stderr,
                            "[NV2A-METHOD] frame=%u sub=%u method=%04X param=%08X handled=%d\n",
                            s_frame + 1, subchannel, item_method,
                            item_param, item_handled);
                    ++s_method_trace_logs;
                }
            }
            methods += count;
            pos += count + 1;
        } else if ((header & PB_NONINC_MASK) == PB_NONINC_MATCH) {
            for (i = 0; i < count; ++i) {
                uint32_t item_param = words[pos + 1 + i];
                if (subchannel != 0u && s_non_kelvin_method_logs++ < 128u)
                    fprintf(stderr,
                            "[DEBUG NV2A-SUBCHANNEL] frame=%u sub=%u "
                            "pos=%u method=%04X param=%08X header=%08X\n",
                            s_frame + 1u, subchannel, pos + 1u + i,
                            method, item_param, header);
                int item_handled = pgraph_d3d11_method(
                    (int)subchannel, method, item_param);
                if (item_handled) ++handled; else ++unhandled;
                if (!item_handled && s_unhandled_method_logs++ < 400u)
                    fprintf(stderr,
                            "[WARN NV2A-PB] unhandled frame=%u sub=%u "
                            "method=%04X param=%08X\n",
                            s_frame + 1u, subchannel, method,
                            item_param);
                if (s_method_trace_logs < 256u) {
                    fprintf(stderr,
                            "[NV2A-METHOD] frame=%u sub=%u method=%04X param=%08X handled=%d\n",
                            s_frame + 1, subchannel, method,
                            item_param, item_handled);
                    ++s_method_trace_logs;
                }
            }
            methods += count;
            pos += count + 1;
        } else {
            if ((header & 3u) == 1u || (header & 3u) == 2u) {
                uint32_t target = header & ~3u;
                static unsigned jlogs;
                ++g_pb_jumps;
                if (target >= begin && target < end) ++g_pb_jumps_in_range;
                if (jlogs++ < 20u)
                    fprintf(stderr, "[JUMP] frame=%u pos=%u word=%08X target=%08X %s  range=%08X..%08X  (pos addr %08X)\n",
                            s_frame + 1u, pos, header, target,
                            (target >= begin && target < end)
                                ? "INSIDE" : "outside",
                            begin, end, begin + pos * 4u);
            }
            if (s_rejected_header_logs++ < 16u)
                fprintf(stderr,
                        "[WARN NV2A-PB] rejected opcode frame=%u pos=%u "
                        "word=%08X\n",
                        s_frame + 1u, pos, header);
            if (first_bad_pos == 0xFFFFFFFFu) {
                first_bad_pos = pos; first_bad_word = header;
            }
            ++rejected;
            ++pos;
        }
    }

    /* Which real retire headers did the walker never treat as headers? */
    {   unsigned p, h;
        static unsigned reported;
        for (p = 0; p < s_probe_n; ++p) {
            int seen = 0;
            uint32_t claim = 0xFFFFFFFFu, claim_count = 0;
            for (h = 0; h < s_hdr_n; ++h) {
                if (s_hdr_pos[h] == s_probe_pos[p]) { seen = 1; break; }
                if (s_probe_pos[p] > s_hdr_pos[h] &&
                    s_probe_pos[p] <= s_hdr_pos[h] + s_hdr_count[h]) {
                    claim = s_hdr_pos[h];
                    claim_count = s_hdr_count[h];
                }
            }
            if (seen) { s_last_real_token = s_probe_token[p]; continue; }
            ++g_pb_desyncs;
            {   static uint32_t next_frame;
                int take = (reported == 0u) || (s_frame >= next_frame);
                if (take && reported < 6u) { ++reported;
                                             next_frame = s_frame + 400u; }
                else take = 0;
            if (take) {
                fprintf(stderr,
                        "[DESYNC] frame=%u range=%08X..%08X dwords=%u\n"
                        "[DESYNC] real retire header at word %u (guest %08X) token=%u was swallowed\n",
                        s_frame + 1u, begin, end, dwords,
                        s_probe_pos[p], begin + s_probe_pos[p] * 4u,
                        s_probe_token[p]);
                if (claim != 0xFFFFFFFFu) {
                    uint32_t ch = words[claim];
                    fprintf(stderr,
                            "[DESYNC] claimed by header at word %u: %08X  method=%04X sub=%u count=%u form=%s\n",
                            claim, ch, ch & 0x1FFCu, (ch >> 13) & 7u,
                            (ch >> 18) & 0x7FFu,
                            (ch & PB_INC_MASK) == PB_INC_MATCH ? "inc"
                            : ((ch & PB_NONINC_MASK) == PB_NONINC_MATCH
                               ? "non-inc" : "other"));
                } else {
                    fprintf(stderr, "[DESYNC] no walker packet claims it (skipped by a rejected-header advance)\n");
                }
                pb_dump_window(words, dwords, s_probe_pos[p], claim,
                               claim_count);
                fflush(stderr);
            } }
            s_last_real_token = s_probe_token[p];
        }
    }

    {   int clean = (rejected == 0u) && (pos == dwords) &&
                    (g_pb_resync_fires == resync_at_entry);
        static unsigned dumped;
        ++g_pb_batches;
        if (!clean) ++g_pb_batches_dirty;
        static unsigned dumped_late;
        int late = (s_frame > 54000u && dumped_late < 2u);
        if (!clean && (dumped < 2u || late)) {
            if (late) ++dumped_late;
            (void)pb_seconds(); (void)pb_trace_after();
            uint32_t k, h;
            ++dumped;
            fprintf(stderr,
                    "[FIRSTDESYNC] frame=%u batch %08X..%08X (%u dwords)  rejected=%u  ended at %u  resyncs=%llu\n"
                    "[FIRSTDESYNC] previous batch %08X..%08X was %s; gap before this one = %d bytes\n",
                    s_frame + 1u, begin, end, dwords, rejected, pos,
                    g_pb_resync_fires - resync_at_entry,
                    s_prev_begin, s_prev_end,
                    s_prev_clean ? "clean" : "dirty",
                    (int)(begin - s_prev_end));
            fprintf(stderr, "[FIRSTDESYNC] tail of the previous batch:\n");
            for (k = 0; k < s_prev_tail_n; ++k)
                fprintf(stderr, "[FIRSTDESYNC]   prev[-%u] %08X  %s\n",
                        s_prev_tail_n - k, s_prev_tail[k],
                        pb_form(s_prev_tail[k]));
            fprintf(stderr, "[FIRSTDESYNC] first %u words of this batch (first bad word at %u: %08X):\n",
                    dwords < 48u ? dwords : 48u, first_bad_pos,
                    first_bad_word);
            {   uint32_t lo = (first_bad_pos != 0xFFFFFFFFu && first_bad_pos > 16u)
                             ? first_bad_pos - 16u : 0u;
                uint32_t hi = lo + 48u;
                if (hi > dwords) hi = dwords;
            for (k = lo; k < hi; ++k) {
                const char *m = "";
                for (h = 0; h < s_hdr_n; ++h)
                    if (s_hdr_pos[h] == k) { m = "  <- walker header"; break; }
                if (k == first_bad_pos) m = "  <== first rejected";
                fprintf(stderr, "[FIRSTDESYNC]   [%4u] %08X %-9s%s\n",
                        k, words[k], pb_form(words[k]), m);
            } }
            fflush(stderr);
        }
        s_prev_begin = begin; s_prev_end = end; s_prev_clean = clean;
        s_prev_tail_n = (dwords < 16u) ? dwords : 16u;
        for (k2 = 0; k2 < s_prev_tail_n; ++k2)
            s_prev_tail[k2] = words[dwords - s_prev_tail_n + k2];
    }

    { extern void nv2a_draw_census_frame(void);
      nv2a_draw_census_frame(); }
    pgraph_d3d11_flush();
    ++s_frame;
    if (s_frame <= 8u || (s_frame % 300u) == 0) {
        fprintf(stderr,
                "[NV2A-PB] frame=%u range=%08X..%08X methods=%u handled=%u "
                "unhandled=%u rejected=%u\n",
                s_frame, begin, end, methods, handled, unhandled, rejected);
    }
}
