/*
 * nv2a_vsh.c - NV2A vertex program execution
 *
 * The title uploads a vertex program and sets SET_TRANSFORM_EXECUTION_MODE to
 * program mode, and until now the runtime captured the program and threw it
 * away, binding a fixed-function pre-transformed FVF instead.  Every draw was
 * therefore rendered as though its vertices were already in screen space.  For
 * geometry that really is screen-space that happens to look right; for the
 * scene it collapses, because the vertices are model-space and the matrix that
 * would move them lives in the program.
 *
 * This executes the program per vertex rather than translating it to HLSL.
 * The draw path already builds a transformed-vertex buffer for the fixed
 * function pipeline, so running the program on the CPU drops into the existing
 * pipeline at the one place the data is needed, instead of requiring a second
 * programmable path through the D3D8 shim.  It is also directly checkable: the
 * outputs are numbers this file can be asked to print.
 *
 * Instruction encoding.  Each slot is 128 bits held as four dwords, d[0] the
 * high word.  The field positions below were confirmed against this title's
 * own program rather than assumed: slots 0..5 decode as
 *
 *     MOV o0, v0    MOV o3, v1    MOV o9, v2
 *     MOV o10, v3   MOV o11, v4   MOV o12, v5
 *
 * which is exactly position / diffuse / texcoord0..3 fed from input registers
 * 0..5, and slots 6..7 as DP4 against c148/c149.  A wrong MAC, input-register
 * or output-address field could not produce that.
 */

#include "nv2a_vsh.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---- field extraction ---------------------------------------------------
 *
 * bit(d, w, p, n) reads n bits at position p of dword w. */
static uint32_t vf(const uint32_t *d, int w, int p, int n)
{
    return (d[w] >> p) & ((1u << n) - 1u);
}

#define F_ILU(d)        vf(d, 1, 25, 3)
#define F_MAC(d)        vf(d, 1, 21, 4)
#define F_CONST(d)      vf(d, 1, 13, 8)
#define F_INPUT(d)      vf(d, 1,  9, 4)

#define F_A_NEG(d)      vf(d, 1,  8, 1)
#define F_A_SWZ(d)      vf(d, 1,  0, 8)
#define F_A_REG(d)      vf(d, 2, 28, 4)
#define F_A_MUX(d)      vf(d, 2, 26, 2)

#define F_B_NEG(d)      vf(d, 2, 25, 1)
#define F_B_SWZ(d)      vf(d, 2, 17, 8)
#define F_B_REG(d)      vf(d, 2, 13, 4)
#define F_B_MUX(d)      vf(d, 2, 11, 2)

#define F_C_NEG(d)      vf(d, 2, 10, 1)
#define F_C_SWZ(d)      vf(d, 2,  2, 8)
#define F_C_REG(d)      ((vf(d, 2, 0, 2) << 2) | vf(d, 3, 30, 2))
#define F_C_MUX(d)      vf(d, 3, 28, 2)

#define F_OUT_MAC_MASK(d) vf(d, 3, 24, 4)
#define F_OUT_R(d)        vf(d, 3, 20, 4)
#define F_OUT_ILU_MASK(d) vf(d, 3, 16, 4)
#define F_OUT_O_MASK(d)   vf(d, 3, 12, 4)
#define F_OUT_ORB(d)      vf(d, 3, 11, 1)
#define F_OUT_ADDR(d)     vf(d, 3,  3, 8)
#define F_OUT_MUX(d)      vf(d, 3,  2, 1)
#define F_A0X(d)          vf(d, 3,  1, 1)
#define F_FINAL(d)        vf(d, 3,  0, 1)

/* Source mux: which file the operand comes from. */
enum { MUX_TEMP = 1, MUX_INPUT = 2, MUX_CONST = 3 };

/* Write masks are xyzw with x in the high bit: 0x8=x 0x4=y 0x2=z 0x1=w.
 * Confirmed by the texcoord moves, which write two components. */
static void write_masked(float *dst, const float *src, uint32_t mask)
{
    if (mask & 8u) dst[0] = src[0];
    if (mask & 4u) dst[1] = src[1];
    if (mask & 2u) dst[2] = src[2];
    if (mask & 1u) dst[3] = src[3];
}

static void write_output(nv2a_vsh_state *st, unsigned reg,
                          const float *value, uint32_t mask)
{
    if (reg == 5u) {
        /* oFog is scalar: the first enabled xyzw lane supplies its x. */
        for (unsigned lane = 0; lane < 4; ++lane)
            if (mask & (8u >> lane)) {st->o[5][0] = value[lane]; break;}
    } else {
        write_masked(st->o[reg], value, mask);
    }
}

static void swizzle(float *out, const float *in, uint32_t swz, int neg)
{
    int i;
    for (i = 0; i < 4; ++i) {
        uint32_t sel = (swz >> (6 - i * 2)) & 3u;
        out[i] = in[sel];
        if (neg) out[i] = -out[i];
    }
}

static const float *src_file(const nv2a_vsh_state *st, uint32_t mux,
                             uint32_t reg, uint32_t cidx)
{
    static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    switch (mux) {
    case MUX_TEMP:  return reg == 12u ? st->o[0] : st->r[reg & 15u];
    case MUX_INPUT: return st->v[reg & 15u];
    case MUX_CONST: return cidx < 192u ? st->c[cidx] : zero;
    default:        return zero;
    }
}

static void fetch(const nv2a_vsh_state *st, const uint32_t *d, int which,
                  float address, float *out)
{
    float raw[4];
    uint32_t mux, reg, swz, neg;
    uint32_t cidx = F_CONST(d);
    if (F_A0X(d)) {
        /* Keep the address integer-valued without a float-to-int overflow
         * on invalid inputs. Out-of-bank reads use the existing zero value. */
        float index = (float)cidx + address;
        cidx = (index >= 0.0f && index < 192.0f) ? (uint32_t)index : 192u;
    }

    switch (which) {
    case 0: mux = F_A_MUX(d); reg = F_A_REG(d); swz = F_A_SWZ(d);
            neg = F_A_NEG(d); break;
    case 1: mux = F_B_MUX(d); reg = F_B_REG(d); swz = F_B_SWZ(d);
            neg = F_B_NEG(d); break;
    default: mux = F_C_MUX(d); reg = F_C_REG(d); swz = F_C_SWZ(d);
            neg = F_C_NEG(d); break;
    }
    /* The input-register number lives in its own field, not in the operand
     * register field. */
    if (mux == MUX_INPUT) reg = F_INPUT(d);

    memcpy(raw, src_file(st, mux, reg, cidx), sizeof(raw));
    swizzle(out, raw, swz, (int)neg);
}

static float dot(const float *a, const float *b, int n)
{
    float s = 0.0f;
    int i;
    for (i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

static void splat(float *o, float v)
{
    o[0] = o[1] = o[2] = o[3] = v;
}

/* MAC unit */
static int run_mac(uint32_t op, const float *a, const float *b, const float *c,
                   float *o)
{
    int i;
    switch (op) {
    case 0: return 0;                                   /* NOP */
    case 1: memcpy(o, a, 16); return 1;                 /* MOV */
    case 2: for (i = 0; i < 4; ++i) o[i] = a[i] * b[i]; return 1;   /* MUL */
    case 3: for (i = 0; i < 4; ++i) o[i] = a[i] + c[i]; return 1;   /* ADD */
    case 4: for (i = 0; i < 4; ++i) o[i] = a[i] * b[i] + c[i]; return 1; /*MAD*/
    case 5: splat(o, dot(a, b, 3)); return 1;                       /* DP3 */
    case 6: splat(o, a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + b[3]); return 1;/*DPH*/
    case 7: splat(o, dot(a, b, 4)); return 1;                       /* DP4 */
    case 8:                                                          /* DST */
        o[0] = 1.0f; o[1] = a[1] * b[1]; o[2] = a[2]; o[3] = b[3];
        return 1;
    case 9: for (i = 0; i < 4; ++i) o[i] = a[i] < b[i] ? a[i] : b[i];
            return 1;                                                /* MIN */
    case 10: for (i = 0; i < 4; ++i) o[i] = a[i] > b[i] ? a[i] : b[i];
            return 1;                                                /* MAX */
    case 11: for (i = 0; i < 4; ++i) o[i] = a[i] < b[i] ? 1.0f : 0.0f;
            return 1;                                                /* SLT */
    case 12: for (i = 0; i < 4; ++i) o[i] = a[i] >= b[i] ? 1.0f : 0.0f;
            return 1;                                                /* SGE */
    case 13: return 0;                                  /* ARL handled by caller */
    default: return 0;
    }
}

/* ILU unit: reciprocal/exponential operations use X; MOV and LIT use vectors. */
static int run_ilu(uint32_t op, const float *c, float *o)
{
    float x = c[0];
    switch (op) {
    case 0: return 0;                                       /* NOP */
    case 1: memcpy(o, c, 16); return 1;                     /* MOV */
    case 2: splat(o, x != 0.0f ? 1.0f / x : 0.0f); return 1;        /* RCP */
    case 3: {                                                       /* RCC */
        float r = x != 0.0f ? 1.0f / x : 0.0f;
        if (r > 0.0f) { if (r < 5.42101e-20f) r = 5.42101e-20f;
                        if (r > 1.884467e+19f) r = 1.884467e+19f; }
        else if (r < 0.0f) { if (r > -5.42101e-20f) r = -5.42101e-20f;
                             if (r < -1.884467e+19f) r = -1.884467e+19f; }
        splat(o, r); return 1; }
    case 4: splat(o, x > 0.0f ? 1.0f / sqrtf(x) : 0.0f); return 1;  /* RSQ */
    case 5:                                                       /* EXP */
        o[0] = exp2f(floorf(x));
        o[1] = x - floorf(x);
        o[2] = exp2f(x);
        o[3] = 1.0f;
        return 1;
    case 6: splat(o, x > 0.0f ? logf(x) / logf(2.0f) : -1e30f); return 1;/*LOG*/
    case 7: {                                                        /* LIT */
        /* NV_vertex_program light coefficients: X is N.L, Y is N.H, W is
         * shininess. The exponent has at least eight fractional bits and
         * is clamped inside (-128,128). The Z result feeds specular lighting
         * maps; returning zero here erased their highlights entirely. */
        const float limit = 127.99609375f;
        float power = c[3] < -limit ? -limit : c[3] > limit ? limit : c[3];
        float half_dot = c[1] > 0.0f ? c[1] : 0.0f;
        o[0] = 1.0f;
        o[1] = x > 0.0f ? x : 0.0f;
        o[2] = x > 0.0f ? powf(half_dot, power) : 0.0f;
        o[3] = 1.0f;
        return 1; }
    default: return 0;
    }
}

void nv2a_vsh_run(const uint32_t program[][4], unsigned count,
                  nv2a_vsh_state *st)
{
    unsigned pc;
    float address = 0.0f; /* A0 is local to one vertex invocation. */

    for (pc = 0; pc < count; ++pc) {
        const uint32_t *d = program[pc];
        float a[4], b[4], c[4], mac_out[4], ilu_out[4];
        uint32_t mac = F_MAC(d), ilu = F_ILU(d);
        int mac_valid, ilu_valid;

        if (mac == 0u && ilu == 0u && d[0] == 0u && d[1] == 0u &&
            d[2] == 0u && d[3] == 0u)
            continue;

        fetch(st, d, 0, address, a);
        fetch(st, d, 1, address, b);
        fetch(st, d, 2, address, c);

        mac_valid = run_mac(mac, a, b, c, mac_out);
        ilu_valid = run_ilu(ilu, c, ilu_out);
        if (mac == 13u) address = floorf(a[0]);

        /* MAC result: to a temp, and optionally to an output register. */
        if (mac_valid) {
            uint32_t rmask = F_OUT_MAC_MASK(d);
            uint32_t reg = F_OUT_R(d);
            /* Paired ILU owns R1, even for lanes its mask does not write. */
            if (ilu != 0u && reg == 1u) rmask = 0u;
            if (rmask) write_masked(reg == 12u ? st->o[0] : st->r[reg], mac_out, rmask);
            if (F_OUT_MUX(d) == 0u) {
                uint32_t omask = F_OUT_O_MASK(d);
                uint32_t addr = F_OUT_ADDR(d);
                if (omask && addr < 16u)
                    write_output(st, addr, mac_out, omask);
            }
        }
        /* Standalone ILU uses OUT_R; pairing with MAC forces R1. */
        if (ilu_valid) {
            uint32_t rmask = F_OUT_ILU_MASK(d);
            uint32_t reg = mac != 0u ? 1u : F_OUT_R(d);
            if (rmask) write_masked(reg == 12u ? st->o[0] : st->r[reg], ilu_out, rmask);
            if (F_OUT_MUX(d) == 1u) {
                uint32_t omask = F_OUT_O_MASK(d);
                uint32_t addr = F_OUT_ADDR(d);
                if (omask && addr < 16u)
                    write_output(st, addr, ilu_out, omask);
            }
        }

        if (F_FINAL(d)) break;
    }
}

void nv2a_vsh_decode(nv2a_vsh_program *decoded,
                      const uint32_t program[][4], unsigned count)
{
    decoded->count = 0;
    decoded->constants_bound = 0;
    if (count > 136u) count = 136u;
    for (unsigned pc = 0; pc < count; ++pc) {
        const uint32_t *d = program[pc];
        if (!(d[0] | d[1] | d[2] | d[3])) continue;
        nv2a_vsh_instruction *out = &decoded->instruction[decoded->count++];
        out->mac = (uint8_t)F_MAC(d); out->ilu = (uint8_t)F_ILU(d);
        out->constant = (uint8_t)F_CONST(d); out->relative = (uint8_t)F_A0X(d);
        out->mac_mask = (uint8_t)F_OUT_MAC_MASK(d);
        out->ilu_mask = (uint8_t)F_OUT_ILU_MASK(d);
        out->temp = (uint8_t)F_OUT_R(d);
        out->output_mask = (uint8_t)F_OUT_O_MASK(d);
        out->output = (uint8_t)F_OUT_ADDR(d);
        out->output_mux = (uint8_t)F_OUT_MUX(d);
        /* Fetch only operands the arithmetic units actually consume. */
        switch (out->mac) {
        case 1: case 13: out->sources = 1; break;
        case 3: out->sources = 5; break;
        case 4: out->sources = 7; break;
        case 2: case 5: case 6: case 7: case 8: case 9:
        case 10: case 11: case 12: out->sources = 3; break;
        default: out->sources = 0; break;
        }
        if (out->ilu) out->sources |= 4;
        for (unsigned s = 0; s < 3; ++s) {
            nv2a_vsh_operand *operand = &out->source[s];
            unsigned swz;
            if (s == 0) {
                operand->mux = (uint8_t)F_A_MUX(d); operand->reg = (uint8_t)F_A_REG(d);
                operand->negate = (uint8_t)F_A_NEG(d); swz = F_A_SWZ(d);
            } else if (s == 1) {
                operand->mux = (uint8_t)F_B_MUX(d); operand->reg = (uint8_t)F_B_REG(d);
                operand->negate = (uint8_t)F_B_NEG(d); swz = F_B_SWZ(d);
            } else {
                operand->mux = (uint8_t)F_C_MUX(d); operand->reg = (uint8_t)F_C_REG(d);
                operand->negate = (uint8_t)F_C_NEG(d); swz = F_C_SWZ(d);
            }
            if (operand->mux == MUX_INPUT) operand->reg = (uint8_t)F_INPUT(d);
            for (unsigned lane = 0; lane < 4; ++lane)
                operand->swizzle[lane] = (uint8_t)((swz >> (6 - lane * 2)) & 3);
        }
        if (F_FINAL(d)) break;
    }
}

void nv2a_vsh_bind_constants(nv2a_vsh_program *program, const float constants[192][4])
{
    static const float zero[4] = {0};
    for (unsigned pc = 0; pc < program->count; ++pc) {
        const nv2a_vsh_instruction *d = &program->instruction[pc];
        if (d->relative) continue;
        for (unsigned s = 0; s < 3u; ++s) {
            const nv2a_vsh_operand *operand = &d->source[s];
            if (!(d->sources & (1u << s)) || operand->mux != MUX_CONST) continue;
            const float *raw = d->constant < 192u ? constants[d->constant] : zero;
            for (unsigned lane = 0; lane < 4u; ++lane) {
                float value = raw[operand->swizzle[lane]];
                program->constant_source[pc][s][lane] = operand->negate ? -value : value;
            }
        }
    }
    program->constants_bound = 1;
}

void nv2a_vsh_run_decoded(const nv2a_vsh_program *program, nv2a_vsh_state *st)
{
    float address = 0.0f;
    for (unsigned pc = 0; pc < program->count; ++pc) {
        const nv2a_vsh_instruction *d = &program->instruction[pc];
        float source[3][4], mac_out[4], ilu_out[4];
        unsigned cidx = d->constant;
        if (d->relative) {
            float index = (float)cidx + address;
            cidx = index >= 0.0f && index < 192.0f ? (unsigned)index : 192u;
        }
        /* Both units read the register state before either writes it. */
        for (unsigned s = 0; s < 3; ++s) if (d->sources & (1u << s)) {
            const nv2a_vsh_operand *operand = &d->source[s];
            if (program->constants_bound && !d->relative && operand->mux == MUX_CONST) {
                memcpy(source[s], program->constant_source[pc][s], sizeof(source[s]));
                continue;
            }
            const float *raw = src_file(st, operand->mux, operand->reg, cidx);
            for (unsigned lane = 0; lane < 4; ++lane) {
                float value = raw[operand->swizzle[lane]];
                source[s][lane] = operand->negate ? -value : value;
            }
        }
        int mac_valid = run_mac(d->mac, source[0], source[1], source[2], mac_out);
        int ilu_valid = d->ilu ? run_ilu(d->ilu, source[2], ilu_out) : 0;
        if (d->mac == 13u) address = floorf(source[0][0]);
        if (mac_valid) {
            unsigned mask = d->ilu && d->temp == 1 ? 0 : d->mac_mask;
            if (mask) write_masked(d->temp == 12 ? st->o[0] : st->r[d->temp], mac_out, mask);
            if (!d->output_mux && d->output_mask && d->output < 16)
                write_output(st, d->output, mac_out, d->output_mask);
        }
        if (ilu_valid) {
            unsigned reg = d->mac ? 1 : d->temp;
            if (d->ilu_mask) write_masked(reg == 12 ? st->o[0] : st->r[reg], ilu_out, d->ilu_mask);
            if (d->output_mux && d->output_mask && d->output < 16)
                write_output(st, d->output, ilu_out, d->output_mask);
        }
    }
}

void nv2a_vsh_dump(const uint32_t program[][4], unsigned count)
{
    static const char *macn[] = { "NOP","MOV","MUL","ADD","MAD","DP3","DPH",
                                  "DP4","DST","MIN","MAX","SLT","SGE","ARL",
                                  "?","?" };
    static const char *ilun[] = { "NOP","MOV","RCP","RCC","RSQ","EXP","LOG",
                                  "LIT" };
    unsigned i;
    for (i = 0; i < count; ++i) {
        const uint32_t *d = program[i];
        fprintf(stderr, "[VSH] %3u %-3s %-3s  v%-2u c%-3u  Amux=%u Areg=%2u "
                "swz=%02X neg=%u | Bmux=%u Breg=%2u | Cmux=%u Creg=%2u "
                "| macmask=%X R%u ilumask=%X omask=%X oaddr=%3u mux=%u fin=%u\n",
                i, macn[F_MAC(d)], ilun[F_ILU(d)], F_INPUT(d), F_CONST(d),
                F_A_MUX(d), F_A_REG(d), F_A_SWZ(d), F_A_NEG(d),
                F_B_MUX(d), F_B_REG(d), F_C_MUX(d), F_C_REG(d),
                F_OUT_MAC_MASK(d), F_OUT_R(d), F_OUT_ILU_MASK(d),
                F_OUT_O_MASK(d), F_OUT_ADDR(d), F_OUT_MUX(d), F_FINAL(d));
    }
    fflush(stderr);
}
