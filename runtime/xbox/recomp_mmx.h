/*
 * MMX register model and integer SIMD semantics.
 *
 * The XMV video decoder's pixel loops are MMX.  Before this file they lifted
 * to bare TODO comments, so the decoder's scalar bookkeeping ran perfectly --
 * ring rotation, packet retirement, publication cadence -- while the YUY2
 * output surface stayed zero-filled and the boot video was black.
 *
 * The eight registers are architectural state, exactly like the GPRs and the
 * x87 stack, so they live in TLS rather than as per-function locals: a value
 * produced in one lifted function and read in another must survive the call.
 *
 * Every operation is a pure function of its operand VALUES and returns a new
 * value.  That is what makes "mm0 = mmx_punpcklbw(mm0, mm0)" correct with no
 * explicit snapshot: C evaluates the arguments before the assignment, so
 * source/destination aliasing -- the common case in real MMX code -- can
 * never observe a half-written register.
 *
 * Lane indices are little-endian: lane 0 is the least significant bits,
 * matching how the hardware numbers them.
 */
#ifndef RECOMP_MMX_H
#define RECOMP_MMX_H

#include <stdint.h>
#include <math.h>

/* -- register file -- */

extern RECOMP_TLS uint64_t g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS uint64_t g_mm4, g_mm5, g_mm6, g_mm7;

#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7

/* 64-bit guest memory, for movq/movntq operands. */
#ifndef MEM64
#define MEM64(addr) (*(volatile uint64_t *)XBOX_PTR(addr))
#endif

#ifndef MEM32
#define MEM32(addr) (*(volatile uint32_t *)XBOX_PTR(addr))
#endif

/* The XMM register type, if recomp_types.h has not already defined it.  The
 * standalone MMX test includes this header alone, and cvtpi2ps needs the type
 * to express "preserve the upper two lanes". */
#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

/* -- lane access -- */

static inline uint8_t  mmx_gb(uint64_t v, int i) { return (uint8_t)(v >> (i * 8)); }
static inline uint16_t mmx_gw(uint64_t v, int i) { return (uint16_t)(v >> (i * 16)); }
static inline uint32_t mmx_gd(uint64_t v, int i) { return (uint32_t)(v >> (i * 32)); }

static inline int8_t   mmx_sb(uint64_t v, int i) { return (int8_t)mmx_gb(v, i); }
static inline int16_t  mmx_sw(uint64_t v, int i) { return (int16_t)mmx_gw(v, i); }
static inline int32_t  mmx_sd(uint64_t v, int i) { return (int32_t)mmx_gd(v, i); }

static inline uint64_t mmx_pb(uint64_t r, int i, uint8_t v)
{ return r | ((uint64_t)v << (i * 8)); }
static inline uint64_t mmx_pw(uint64_t r, int i, uint16_t v)
{ return r | ((uint64_t)v << (i * 16)); }
static inline uint64_t mmx_pd(uint64_t r, int i, uint32_t v)
{ return r | ((uint64_t)v << (i * 32)); }

/* -- add / subtract (wrapping, no saturation) -- */

static inline uint64_t mmx_paddb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; ++i)
        r = mmx_pb(r, i, (uint8_t)(mmx_gb(a, i) + mmx_gb(b, i)));
    return r;
}
static inline uint64_t mmx_paddw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i)
        r = mmx_pw(r, i, (uint16_t)(mmx_gw(a, i) + mmx_gw(b, i)));
    return r;
}
static inline uint64_t mmx_paddd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; ++i)
        r = mmx_pd(r, i, mmx_gd(a, i) + mmx_gd(b, i));
    return r;
}
static inline uint64_t mmx_psubb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; ++i)
        r = mmx_pb(r, i, (uint8_t)(mmx_gb(a, i) - mmx_gb(b, i)));
    return r;
}
static inline uint64_t mmx_psubw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i)
        r = mmx_pw(r, i, (uint16_t)(mmx_gw(a, i) - mmx_gw(b, i)));
    return r;
}
static inline uint64_t mmx_psubd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; ++i)
        r = mmx_pd(r, i, mmx_gd(a, i) - mmx_gd(b, i));
    return r;
}

/* -- multiply -- */

/* Signed 16x16, low half kept. */
static inline uint64_t mmx_pmullw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i) {
        int32_t p = (int32_t)mmx_sw(a, i) * (int32_t)mmx_sw(b, i);
        r = mmx_pw(r, i, (uint16_t)(uint32_t)p);
    }
    return r;
}

/* Signed 16x16 products, adjacent pairs summed into 32-bit lanes.  The sum
 * wraps rather than saturating -- the all-0x8000 case is what makes that
 * observable, and it must produce 0x80000000. */
static inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int j;
    for (j = 0; j < 2; ++j) {
        int32_t lo = (int32_t)mmx_sw(a, j * 2)     * (int32_t)mmx_sw(b, j * 2);
        int32_t hi = (int32_t)mmx_sw(a, j * 2 + 1) * (int32_t)mmx_sw(b, j * 2 + 1);
        r = mmx_pd(r, j, (uint32_t)lo + (uint32_t)hi);
    }
    return r;
}

/* -- bitwise -- */

static inline uint64_t mmx_pand (uint64_t a, uint64_t b) { return a & b; }
static inline uint64_t mmx_pandn(uint64_t a, uint64_t b) { return (~a) & b; }
static inline uint64_t mmx_por  (uint64_t a, uint64_t b) { return a | b; }
static inline uint64_t mmx_pxor (uint64_t a, uint64_t b) { return a ^ b; }

/* -- unpack / interleave -- */

static inline uint64_t mmx_punpcklbw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i) {
        r = mmx_pb(r, i * 2,     mmx_gb(a, i));
        r = mmx_pb(r, i * 2 + 1, mmx_gb(b, i));
    }
    return r;
}
static inline uint64_t mmx_punpckhbw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i) {
        r = mmx_pb(r, i * 2,     mmx_gb(a, i + 4));
        r = mmx_pb(r, i * 2 + 1, mmx_gb(b, i + 4));
    }
    return r;
}
static inline uint64_t mmx_punpcklwd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; ++i) {
        r = mmx_pw(r, i * 2,     mmx_gw(a, i));
        r = mmx_pw(r, i * 2 + 1, mmx_gw(b, i));
    }
    return r;
}
static inline uint64_t mmx_punpckhwd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; ++i) {
        r = mmx_pw(r, i * 2,     mmx_gw(a, i + 2));
        r = mmx_pw(r, i * 2 + 1, mmx_gw(b, i + 2));
    }
    return r;
}
static inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b)
{
    return (uint64_t)mmx_gd(a, 0) | ((uint64_t)mmx_gd(b, 0) << 32);
}
static inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b)
{
    return (uint64_t)mmx_gd(a, 1) | ((uint64_t)mmx_gd(b, 1) << 32);
}

/* -- pack with saturation -- */

static inline uint8_t mmx_satub(int32_t v)
{ return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
static inline uint8_t mmx_satsb(int32_t v)
{ return (uint8_t)(int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v)); }
static inline uint16_t mmx_satsw(int32_t v)
{ return (uint16_t)(int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

/* Signed words -> unsigned bytes: destination lanes first, then source. */
static inline uint64_t mmx_packuswb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i) {
        r = mmx_pb(r, i,     mmx_satub((int32_t)mmx_sw(a, i)));
        r = mmx_pb(r, i + 4, mmx_satub((int32_t)mmx_sw(b, i)));
    }
    return r;
}
static inline uint64_t mmx_packsswb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i) {
        r = mmx_pb(r, i,     mmx_satsb((int32_t)mmx_sw(a, i)));
        r = mmx_pb(r, i + 4, mmx_satsb((int32_t)mmx_sw(b, i)));
    }
    return r;
}
static inline uint64_t mmx_packssdw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; ++i) {
        r = mmx_pw(r, i,     mmx_satsw(mmx_sd(a, i)));
        r = mmx_pw(r, i + 2, mmx_satsw(mmx_sd(b, i)));
    }
    return r;
}

/* -- shifts --
 * The count is the whole 64-bit operand.  A count at or beyond the lane width
 * zeroes a logical shift and saturates an arithmetic one to the sign bit; it
 * does not wrap the way a scalar shl/shr does. */

static inline uint64_t mmx_psllw(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    if (c > 15) return 0;
    for (i = 0; i < 4; ++i)
        r = mmx_pw(r, i, (uint16_t)(mmx_gw(a, i) << c));
    return r;
}
static inline uint64_t mmx_pslld(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    if (c > 31) return 0;
    for (i = 0; i < 2; ++i)
        r = mmx_pd(r, i, mmx_gd(a, i) << c);
    return r;
}
static inline uint64_t mmx_psllq(uint64_t a, uint64_t c)
{ return c > 63 ? 0 : (a << c); }

static inline uint64_t mmx_psrlw(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    if (c > 15) return 0;
    for (i = 0; i < 4; ++i)
        r = mmx_pw(r, i, (uint16_t)(mmx_gw(a, i) >> c));
    return r;
}
static inline uint64_t mmx_psrld(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    if (c > 31) return 0;
    for (i = 0; i < 2; ++i)
        r = mmx_pd(r, i, mmx_gd(a, i) >> c);
    return r;
}
static inline uint64_t mmx_psrlq(uint64_t a, uint64_t c)
{ return c > 63 ? 0 : (a >> c); }

static inline uint64_t mmx_psraw(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    unsigned s = (c > 15) ? 15u : (unsigned)c;
    for (i = 0; i < 4; ++i)
        r = mmx_pw(r, i, (uint16_t)(int16_t)(mmx_sw(a, i) >> s));
    return r;
}
static inline uint64_t mmx_psrad(uint64_t a, uint64_t c)
{
    uint64_t r = 0; int i;
    unsigned s = (c > 31) ? 31u : (unsigned)c;
    for (i = 0; i < 2; ++i)
        r = mmx_pd(r, i, (uint32_t)(int32_t)(mmx_sd(a, i) >> s));
    return r;
}

/* -- compares: all-ones or all-zero per lane -- */

static inline uint64_t mmx_pcmpeqb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; ++i)
        r = mmx_pb(r, i, (uint8_t)(mmx_gb(a, i) == mmx_gb(b, i) ? 0xFF : 0));
    return r;
}
static inline uint64_t mmx_pcmpeqw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i)
        r = mmx_pw(r, i, (uint16_t)(mmx_gw(a, i) == mmx_gw(b, i) ? 0xFFFF : 0));
    return r;
}
static inline uint64_t mmx_pcmpeqd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; ++i)
        r = mmx_pd(r, i, mmx_gd(a, i) == mmx_gd(b, i) ? 0xFFFFFFFFu : 0u);
    return r;
}
static inline uint64_t mmx_pcmpgtb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; ++i)
        r = mmx_pb(r, i, (uint8_t)(mmx_sb(a, i) > mmx_sb(b, i) ? 0xFF : 0));
    return r;
}
static inline uint64_t mmx_pcmpgtw(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i)
        r = mmx_pw(r, i, (uint16_t)(mmx_sw(a, i) > mmx_sw(b, i) ? 0xFFFF : 0));
    return r;
}
static inline uint64_t mmx_pcmpgtd(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 2; ++i)
        r = mmx_pd(r, i, mmx_sd(a, i) > mmx_sd(b, i) ? 0xFFFFFFFFu : 0u);
    return r;
}

/* -- SSE-era integer ops that appear in the same loops -- */

/* Result word i comes from source word (imm >> 2i) & 3. */
static inline uint64_t mmx_pshufw(uint64_t a, uint64_t imm)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 4; ++i)
        r = mmx_pw(r, i, mmx_gw(a, (int)((imm >> (i * 2)) & 3u)));
    return r;
}
static inline uint64_t mmx_pavgb(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; ++i)
        r = mmx_pb(r, i, (uint8_t)((mmx_gb(a, i) + mmx_gb(b, i) + 1u) >> 1));
    return r;
}
static inline uint64_t mmx_pminub(uint64_t a, uint64_t b)
{
    uint64_t r = 0; int i;
    for (i = 0; i < 8; ++i) {
        uint8_t x = mmx_gb(a, i), y = mmx_gb(b, i);
        r = mmx_pb(r, i, x < y ? x : y);
    }
    return r;
}
/* Writes a GPR, not an MMX register: the eight byte sign bits. */
static inline uint32_t mmx_pmovmskb(uint64_t a)
{
    uint32_t r = 0; int i;
    for (i = 0; i < 8; ++i)
        if (mmx_gb(a, i) & 0x80u) r |= 1u << i;
    return r;
}

/* -- float to packed integer (cvtps2pi) -- */

/* MXCSR rounding control, bits 14:13.  cvtps2pi rounds by this field; only
 * cvttps2pi truncates unconditionally, and this XBE contains no cvttps2pi.
 * Nothing lifts ldmxcsr yet, so the value stays at the x86 reset default of
 * round-to-nearest-even -- which is the mode the decoder actually runs in.
 * When ldmxcsr is implemented it should write here rather than this being
 * hardcoded, so the dependency stays visible. */
extern RECOMP_TLS uint32_t g_mmx_rc;

#define MMX_RC_NEAREST 0u
#define MMX_RC_DOWN    1u
#define MMX_RC_UP      2u
#define MMX_RC_TRUNC   3u

/* "Integer indefinite": what x86 stores when the source is NaN, infinite, or
 * outside the destination's range.  It is 0x80000000 -- NOT a saturating
 * clamp to INT_MIN/INT_MAX, which is what a naive cast would give. */
#define MMX_INT_INDEFINITE 0x80000000u

static inline uint32_t mmx_f2i(float v)
{
    double d = (double)v;
    double r;

    if (!(d == d))                      /* NaN */
        return MMX_INT_INDEFINITE;

    switch (g_mmx_rc) {
    case MMX_RC_DOWN:  r = floor(d); break;
    case MMX_RC_UP:    r = ceil(d);  break;
    case MMX_RC_TRUNC: r = (d < 0.0) ? ceil(d) : floor(d); break;
    default: {
        /* Round half to even, the default mode.  Naive rounding of x.5 away
         * from zero is wrong here and would bias every conversion. */
        double f = floor(d);
        double diff = d - f;
        if (diff > 0.5)      r = f + 1.0;
        else if (diff < 0.5) r = f;
        else                 r = (fmod(f, 2.0) == 0.0) ? f : f + 1.0;
        break; }
    }

    /* Infinities land here too: floor(inf) is inf and fails the range test. */
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return MMX_INT_INDEFINITE;
    return (uint32_t)(int32_t)r;
}

/* The low two float32 lanes of the source become two signed 32-bit integers
 * in the destination MMX register. */
static inline uint64_t mmx_cvtps2pi(float lo, float hi)
{
    return (uint64_t)mmx_f2i(lo) | ((uint64_t)mmx_f2i(hi) << 32);
}

/* -- 128-bit non-temporal store (movntps) -- */

/* All sixteen bytes, low lane first, as four 32-bit writes so an unaligned
 * guest address is fine.  The "non-temporal" part is a cache hint with no
 * architectural effect, so there is nothing to model beyond the store itself
 * -- but it is a FULL-WIDTH store, and aliasing it to a 64-bit one would
 * silently drop half of every write. */
static inline void mmx_store128(uint32_t addr, RecompXmm v)
{
    MEM32(addr)      = v.u[0];
    MEM32(addr + 4)  = v.u[1];
    MEM32(addr + 8)  = v.u[2];
    MEM32(addr + 12) = v.u[3];
}

/* -- packed integer to float (cvtpi2ps) -- */

/* The two signed 32-bit integers in the MMX source become the low two float
 * lanes of the XMM destination.  Lanes 2 and 3 are left exactly as they were:
 * cvtpi2ps writes only the low 64 bits, and zeroing the rest would corrupt
 * whatever the title had there. */
static inline RecompXmm mmx_cvtpi2ps(RecompXmm dst, uint64_t src)
{
    dst.f[0] = (float)(int32_t)mmx_gd(src, 0);
    dst.f[1] = (float)(int32_t)mmx_gd(src, 1);
    return dst;
}

/* -- movd / emms -- */

/* movd into an MMX register zeroes the upper 32 bits. */
static inline uint64_t mmx_movd_to_mm(uint32_t v) { return (uint64_t)v; }

/* In this runtime the x87 stack is modelled separately from the MMX register
 * file, so the two share no storage and emms has no aliasing to undo.  It is
 * still lifted to a real call rather than dropped, so the instruction is
 * accounted for and the mode boundary stays visible in the generated code. */
static inline void mmx_emms(void) { }

#if (defined(_M_X64) || defined(__x86_64__)) && !defined(RECOMP_MMX_SCALAR)
#include "recomp_mmx_sse2.h"
#endif

#endif /* RECOMP_MMX_H */
