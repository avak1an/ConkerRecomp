#include "nv2a_vsh_hlsl.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef struct { char *text; size_t capacity, length; int failed; } Writer;
static void emit(Writer *w, const char *format, ...)
{
    if (w->failed) return;
    va_list args; va_start(args, format);
    int n = vsnprintf(w->text + w->length, w->capacity - w->length, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->capacity - w->length) w->failed = 1;
    else w->length += (size_t)n;
}
static void operand(Writer *w, const nv2a_vsh_instruction *d, unsigned s)
{
    const nv2a_vsh_operand *p = &d->source[s];
    char reg[160];
    switch (p->mux) {
    case 1: snprintf(reg, sizeof(reg), p->reg == 12 ? "o[0]" : "r[%u]", p->reg); break;
    case 2: snprintf(reg, sizeof(reg), "v[%u]", p->reg); break;
    case 3:
        if (d->relative) snprintf(reg, sizeof(reg),
            "((ci >= 0.0 && ci < 192.0) ? c[(uint)(ci >= 0.0 && ci < 192.0 ? ci : 0.0)] : float4(0,0,0,0))");
        else if (d->constant < 192) snprintf(reg, sizeof(reg), "c[%u]", d->constant);
        else strcpy(reg, "float4(0,0,0,0)");
        break;
    default: strcpy(reg, "float4(0,0,0,0)"); break;
    }
    emit(w, "%s(%s).%c%c%c%c", p->negate ? "-" : "", reg,
         "xyzw"[p->swizzle[0]], "xyzw"[p->swizzle[1]],
         "xyzw"[p->swizzle[2]], "xyzw"[p->swizzle[3]]);
}
static void write_result(Writer *w, const char *dst, unsigned mask, const char *value, int fog)
{
    if (fog) {
        for (unsigned lane=0; lane<4; ++lane) if (mask & (8u>>lane)) {
            emit(w, "%s.x=%s.%c;\n", dst, value, "xyzw"[lane]); break;
        }
        return;
    }
    for (unsigned lane=0; lane<4; ++lane) if (mask & (8u>>lane))
        emit(w, "%s.%c=%s.%c;\n", dst, "xyzw"[lane], value, "xyzw"[lane]);
}
size_t nv2a_vsh_emit_hlsl(char *text, size_t capacity, const nv2a_vsh_program *program)
{
    if (!capacity || program->count > 136) return 0;
    Writer w={text,capacity,0,0};
    emit(&w, "void nv2a_exec(float4 v[16], out float4 r[16], out float4 o[16]) {\n"
             "[unroll] for(uint i=0;i<16;i++){r[i]=0;o[i]=0;}\n"
             "o[9].w=1;o[10].w=1;o[11].w=1;o[12].w=1;\n"
             "precise float address=0;\n");
    for (unsigned pc=0; pc<program->count; ++pc) {
        const nv2a_vsh_instruction *d=&program->instruction[pc];
        emit(&w, "{ // slot %u\nprecise float4 a=0,b=0,z=0,m=0,u=0;\n", pc);
        if (d->relative) emit(&w, "precise float ci=%u.0+address;\n", d->constant);
        for (unsigned s=0;s<3;++s) if (d->sources & (1u<<s)) {
            emit(&w, "%c=", "abz"[s]); operand(&w,d,s); emit(&w, ";\n");
        }
        switch(d->mac) {
        case 1: emit(&w,"m=a;\n"); break;
        case 2: emit(&w,"m=a*b;\n"); break;
        case 3: emit(&w,"m=a+z;\n"); break;
        case 4: emit(&w,"m=a*b+z;\n"); break;
        case 5: emit(&w,"m=((0.0+a.x*b.x)+a.y*b.y)+a.z*b.z;\n"); break;
        case 6: emit(&w,"m=((a.x*b.x+a.y*b.y)+a.z*b.z)+b.w;\n"); break;
        case 7: emit(&w,"m=(((0.0+a.x*b.x)+a.y*b.y)+a.z*b.z)+a.w*b.w;\n"); break;
        case 8: emit(&w,"m=float4(1,a.y*b.y,a.z,b.w);\n"); break;
        case 9: emit(&w,"m=(a<b)?a:b;\n"); break;
        case 10: emit(&w,"m=(a>b)?a:b;\n"); break;
        case 11: emit(&w,"m=(a<b)?1.0:0.0;\n"); break;
        case 12: emit(&w,"m=(a>=b)?1.0:0.0;\n"); break;
        case 13: emit(&w,"address=floor(a.x);\n"); break;
        }
        switch(d->ilu) {
        case 1: emit(&w,"u=z;\n"); break;
        case 2: emit(&w,"u=(z.x!=0.0)?1.0/z.x:0.0;\n"); break;
        case 3: emit(&w,"precise float rc=(z.x!=0.0)?1.0/z.x:0.0;\n"
             "if(rc>0)rc=clamp(rc,5.42101e-20,1.884467e19);"
             "else if(rc<0)rc=clamp(rc,-1.884467e19,-5.42101e-20);u=rc;\n"); break;
        case 4: emit(&w,"u=(z.x>0.0)?1.0/sqrt(z.x):0.0;\n"); break;
        case 5: emit(&w,"u=float4(exp2(floor(z.x)),z.x-floor(z.x),exp2(z.x),1);\n"); break;
        case 6: emit(&w,"u=(z.x>0.0)?log(z.x)/log(2.0):-1e30;\n"); break;
        case 7: emit(&w,"precise float power=clamp(z.w,-127.99609375,127.99609375);\n"
             "precise float h=z.y>0?z.y:0;\n"
             "precise float lit=h==0?(power==0?1:power<0?asfloat(0x7f800000):0):pow(h,power);\n"
             "u=float4(1,z.x>0?z.x:0,z.x>0?lit:0,1);\n"); break;
        }
        char dst[24];
        if(d->mac>=1 && d->mac<=12) {
            snprintf(dst,sizeof(dst),d->temp==12?"o[0]":"r[%u]",d->temp);
            write_result(&w,dst,d->ilu && d->temp==1?0:d->mac_mask,"m",0);
            if(!d->output_mux && d->output<16) {
                snprintf(dst,sizeof(dst),"o[%u]",d->output);
                write_result(&w,dst,d->output_mask,"m",d->output==5);
            }
        }
        if(d->ilu) {
            unsigned temp=d->mac?1:d->temp;
            snprintf(dst,sizeof(dst),temp==12?"o[0]":"r[%u]",temp);
            write_result(&w,dst,d->ilu_mask,"u",0);
            if(d->output_mux && d->output<16) {
                snprintf(dst,sizeof(dst),"o[%u]",d->output);
                write_result(&w,dst,d->output_mask,"u",d->output==5);
            }
        }
        emit(&w,"}\n");
    }
    emit(&w,"}\n");
    return w.failed ? 0 : w.length;
}
