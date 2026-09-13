/* Native implementation of the 64-bit integer lanes on x64 hosts.
 *
 * Keep the scalar functions in recomp_mmx.h as the portable reference. These
 * expression macros evaluate each operand once and emit intrinsics even in an
 * /Od guest translation unit. No host MMX registers or x87 state are touched.
 * SSE2 is part of the x64 baseline; no optional CPU feature is required.
 */
#ifndef RECOMP_MMX_SSE2_H
#define RECOMP_MMX_SSE2_H
#include <emmintrin.h>

#define RECOMP_MMX_PUT(v) _mm_cvtsi64_si128((int64_t)(uint64_t)(v))
#define RECOMP_MMX_GET(v) ((uint64_t)_mm_cvtsi128_si64(v))
#define RECOMP_MMX_BINARY(op,a,b) RECOMP_MMX_GET(op(RECOMP_MMX_PUT(a),RECOMP_MMX_PUT(b)))
/* Packs read both MMX operands from the low/high halves of one XMM input. */
#define RECOMP_MMX_PACK(op,a,b) RECOMP_MMX_GET(op( \
    _mm_unpacklo_epi64(RECOMP_MMX_PUT(a),RECOMP_MMX_PUT(b)),_mm_setzero_si128()))
/* The high half of an MMX value starts at bit 32, not bit 64. */
#define RECOMP_MMX_UNPACK_HIGH(op,a,b) RECOMP_MMX_BINARY(op,((uint64_t)(a)>>32),((uint64_t)(b)>>32))

#define mmx_paddb(a,b) RECOMP_MMX_BINARY(_mm_add_epi8,a,b)
#define mmx_paddw(a,b) RECOMP_MMX_BINARY(_mm_add_epi16,a,b)
#define mmx_paddd(a,b) RECOMP_MMX_BINARY(_mm_add_epi32,a,b)
#define mmx_psubb(a,b) RECOMP_MMX_BINARY(_mm_sub_epi8,a,b)
#define mmx_psubw(a,b) RECOMP_MMX_BINARY(_mm_sub_epi16,a,b)
#define mmx_psubd(a,b) RECOMP_MMX_BINARY(_mm_sub_epi32,a,b)
#define mmx_pmullw(a,b) RECOMP_MMX_BINARY(_mm_mullo_epi16,a,b)
#define mmx_pmaddwd(a,b) RECOMP_MMX_BINARY(_mm_madd_epi16,a,b)
#define mmx_punpcklbw(a,b) RECOMP_MMX_BINARY(_mm_unpacklo_epi8,a,b)
#define mmx_punpcklwd(a,b) RECOMP_MMX_BINARY(_mm_unpacklo_epi16,a,b)
#define mmx_punpckldq(a,b) RECOMP_MMX_BINARY(_mm_unpacklo_epi32,a,b)
#define mmx_punpckhbw(a,b) RECOMP_MMX_UNPACK_HIGH(_mm_unpacklo_epi8,a,b)
#define mmx_punpckhwd(a,b) RECOMP_MMX_UNPACK_HIGH(_mm_unpacklo_epi16,a,b)
#define mmx_punpckhdq(a,b) RECOMP_MMX_UNPACK_HIGH(_mm_unpacklo_epi32,a,b)
#define mmx_packuswb(a,b) RECOMP_MMX_PACK(_mm_packus_epi16,a,b)
#define mmx_packsswb(a,b) RECOMP_MMX_PACK(_mm_packs_epi16,a,b)
#define mmx_packssdw(a,b) RECOMP_MMX_PACK(_mm_packs_epi32,a,b)
/* Variable-count intrinsics use the entire low 64 bits, including oversized
 * counts. Logical shifts zero and arithmetic shifts extend the sign. */
#define mmx_psllw(a,c) RECOMP_MMX_BINARY(_mm_sll_epi16,a,c)
#define mmx_pslld(a,c) RECOMP_MMX_BINARY(_mm_sll_epi32,a,c)
#define mmx_psllq(a,c) RECOMP_MMX_BINARY(_mm_sll_epi64,a,c)
#define mmx_psrlw(a,c) RECOMP_MMX_BINARY(_mm_srl_epi16,a,c)
#define mmx_psrld(a,c) RECOMP_MMX_BINARY(_mm_srl_epi32,a,c)
#define mmx_psrlq(a,c) RECOMP_MMX_BINARY(_mm_srl_epi64,a,c)
#define mmx_psraw(a,c) RECOMP_MMX_BINARY(_mm_sra_epi16,a,c)
#define mmx_psrad(a,c) RECOMP_MMX_BINARY(_mm_sra_epi32,a,c)
#define mmx_pcmpeqb(a,b) RECOMP_MMX_BINARY(_mm_cmpeq_epi8,a,b)
#define mmx_pcmpeqw(a,b) RECOMP_MMX_BINARY(_mm_cmpeq_epi16,a,b)
#define mmx_pcmpeqd(a,b) RECOMP_MMX_BINARY(_mm_cmpeq_epi32,a,b)
#define mmx_pcmpgtb(a,b) RECOMP_MMX_BINARY(_mm_cmpgt_epi8,a,b)
#define mmx_pcmpgtw(a,b) RECOMP_MMX_BINARY(_mm_cmpgt_epi16,a,b)
#define mmx_pcmpgtd(a,b) RECOMP_MMX_BINARY(_mm_cmpgt_epi32,a,b)
#define mmx_pavgb(a,b) RECOMP_MMX_BINARY(_mm_avg_epu8,a,b)
#define mmx_pminub(a,b) RECOMP_MMX_BINARY(_mm_min_epu8,a,b)

#endif
