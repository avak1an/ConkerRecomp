/* Runtime-generated shaders use only the user's live uploaded microcode. */
#include "d3d8_internal.h"
#include "d3d8_nv2a_gpu.h"
#include "../nv2a/nv2a_vsh_hlsl.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_compile.h"

#define GPU_VSH_CACHE 256
typedef struct {
    uint32_t formats[16];int offsets[16];
    ID3D11InputLayout *layout;unsigned long long age;
} GpuLayout;
typedef struct {
    uint32_t words[136][4]; unsigned count, used;
    unsigned long long age;
    ID3D11VertexShader *shader; ID3D11InputLayout *layout;
    ID3DBlob *code;
    GpuLayout layouts[8];
} GpuProgram;
static GpuProgram cache[GPU_VSH_CACHE], *pending;
static unsigned long long serial;
static ID3D11Buffer *constant_buffer;
unsigned long long g_gpu_vsh_draws, g_gpu_vsh_compiles, g_gpu_vsh_failures;

static const char prefix[] =
    "cbuffer Nv2aConstants:register(b2){float4 c[192];float4 vp;float4 fog;"
    "row_major float4x4 tm[4];uint4 formats[4];};\n";
static const char suffix[] =
    "struct In {"
    "uint4 a0:ATTR0;uint4 a1:ATTR1;uint4 a2:ATTR2;uint4 a3:ATTR3;"
    "uint4 a4:ATTR4;uint4 a5:ATTR5;uint4 a6:ATTR6;uint4 a7:ATTR7;"
    "uint4 a8:ATTR8;uint4 a9:ATTR9;uint4 a10:ATTR10;uint4 a11:ATTR11;"
    "uint4 a12:ATTR12;uint4 a13:ATTR13;uint4 a14:ATTR14;uint4 a15:ATTR15;};\n"
    "float4 unpack_attribute(uint4 raw,uint format){"
    "if(format==0)return float4(0,0,0,1);uint type=format&15;uint size=(format>>4)&15;"
    "if(type==6){int3 n=int3((int)(raw.x<<21)>>21,(int)(raw.x<<10)>>21,(int)raw.x>>22);"
    "return float4(max(float3(-1,-1,-1),float3(n)/float3(1023,1023,511)),1);}"
    "if(type==0||type==4){float4 b=float4(raw.x&255,(raw.x>>8)&255,(raw.x>>16)&255,raw.x>>24)/255;"
    "if(type==0)return b.bgra;return float4(size>0?b.x:0,size>1?b.y:0,size>2?b.z:0,size>3?b.w:1);}"
    "float4 v;if(type==1||type==5){int4 n=int4((int)(raw.x<<16)>>16,(int)raw.x>>16,"
    "(int)(raw.y<<16)>>16,(int)raw.y>>16);v=float4(n);if(type==1)v=max(-1.0,v/32767.0);}"
    "else v=asfloat(raw);return float4(size>0?v.x:0,size>1?v.y:0,size>2?v.z:0,size>3?v.w:1);}\n"
    "struct Out {float4 pos:SV_POSITION;float4 diffuse:COLOR0;float4 specular:COLOR1;"
    "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;float f:TEXCOORD4;};\n"
    "float fog_factor(float d){uint mode=(uint)fog.w;if(fog.z==0)return 1;"
    "float exceptional=(mode==0x2601||mode==0x804||mode==0x800)?1:0;"
    "if(isinf(d))return exceptional;precise float f;"
    "if(mode==0x2601||mode==0x804)f=fog.x+d*fog.y-1;"
    "else if(mode==0x800||mode==0x802)f=fog.x+exp2(d*fog.y*16)-1.5;"
    "else if(mode==0x801||mode==0x803)f=fog.x+exp2(-d*d*fog.y*fog.y*32)-1.5;"
    "else return 1;if(mode==0x802||mode==0x803||mode==0x804)f=abs(f);"
    "return isnan(f)?exceptional:clamp(f,-3.402823466e38,3.402823466e38);}\n"
    "Out main(In i){float4 v[16]={"
    "unpack_attribute(i.a0,formats[0].x),unpack_attribute(i.a1,formats[0].y),"
    "unpack_attribute(i.a2,formats[0].z),unpack_attribute(i.a3,formats[0].w),"
    "unpack_attribute(i.a4,formats[1].x),unpack_attribute(i.a5,formats[1].y),"
    "unpack_attribute(i.a6,formats[1].z),unpack_attribute(i.a7,formats[1].w),"
    "unpack_attribute(i.a8,formats[2].x),unpack_attribute(i.a9,formats[2].y),"
    "unpack_attribute(i.a10,formats[2].z),unpack_attribute(i.a11,formats[2].w),"
    "unpack_attribute(i.a12,formats[3].x),unpack_attribute(i.a13,formats[3].y),"
    "unpack_attribute(i.a14,formats[3].z),unpack_attribute(i.a15,formats[3].w)};"
    "float4 r[16],o[16];nv2a_exec(v,r,o);Out result;"
    "precise float rhw=o[0].w!=0?1.0/o[0].w:1.0;"
    "precise float w=rhw!=0?1.0/rhw:1.0;"
    "precise float x=(o[0].x/vp.x)*2-1;precise float y=1-(o[0].y/vp.y)*2;"
    "result.pos=float4(x*w,y*w,(o[0].z/vp.z)*w,w);"
    "result.diffuse=floor(saturate(o[3])*255+0.5)/255;"
    "result.specular=floor(saturate(o[4])*255+0.5)/255;"
    "result.t0=mul(o[9],tm[0]);result.t1=mul(o[10],tm[1]);"
    "result.t2=mul(o[11],tm[2]);result.t3=mul(o[12],tm[3]);"
    "result.f=fog_factor(o[5].x);return result;}\n";

static int prepare_layout(GpuProgram *program,const uint32_t formats[16],const int offsets[16])
{
    GpuLayout *entry=&program->layouts[0];
    for(unsigned i=0;i<8;i++) {
        GpuLayout *candidate=&program->layouts[i];
        if(candidate->layout && !memcmp(candidate->formats,formats,64) && !memcmp(candidate->offsets,offsets,64)) {
            candidate->age=++serial;program->layout=candidate->layout;return 1;
        }
        if(!candidate->layout || (entry->layout && candidate->age<entry->age))entry=candidate;
    }
    D3D11_INPUT_ELEMENT_DESC elements[16]={0};
    const DXGI_FORMAT types[]={DXGI_FORMAT_R32_UINT,DXGI_FORMAT_R32G32_UINT,
        DXGI_FORMAT_R32G32B32_UINT,DXGI_FORMAT_R32G32B32A32_UINT};
    for(unsigned i=0;i<16;i++) {
        unsigned type=formats[i]&15,size=(formats[i]>>4)&15;
        unsigned words=offsets[i]<0?1:(type==0||type==4||type==6)?1:
            (type==1||type==5)?(size+1)/2:size;
        if(words<1||words>4)return 0;
        elements[i].SemanticName="ATTR";elements[i].SemanticIndex=i;elements[i].Format=types[words-1];
        elements[i].AlignedByteOffset=offsets[i]<0?0:(unsigned)offsets[i]*4;
        elements[i].InputSlotClass=D3D11_INPUT_PER_VERTEX_DATA;
    }
    ID3D11InputLayout *layout=NULL;
    HRESULT hr=ID3D11Device_CreateInputLayout(d3d8_GetD3D11Device(),elements,16,
        ID3D10Blob_GetBufferPointer(program->code),ID3D10Blob_GetBufferSize(program->code),&layout);
    if(FAILED(hr))return 0;
    if(entry->layout)ID3D11InputLayout_Release(entry->layout);
    memcpy(entry->formats,formats,64);memcpy(entry->offsets,offsets,64);
    entry->layout=layout;entry->age=++serial;program->layout=layout;return 1;
}
void *d3d8_nv2a_gpu_program(const uint32_t words[][4], unsigned count,
    const uint32_t formats[16], const int offsets[16])
{
    if(count>136)count=136;
    /* Upload RAM retains older slots beyond FINAL and empty slots are no-ops.
     * Key only the instructions the reference interpreter actually executes. */
    uint32_t key[136][4];unsigned used=0;
    for(unsigned pc=0;pc<count;pc++) {
        if(words[pc][0]|words[pc][1]|words[pc][2]|words[pc][3])
            memcpy(key[used++],words[pc],16);
        if(words[pc][3]&1u)break;
    }
    count=used;words=(const uint32_t (*)[4])key;
    GpuProgram *entry=&cache[0];
    for(unsigned i=0;i<GPU_VSH_CACHE;i++) {
        if(cache[i].used && cache[i].count==count &&
           memcmp(cache[i].words,words,count*16)==0) {
            cache[i].age=++serial;
            return cache[i].shader && prepare_layout(&cache[i],formats,offsets)?&cache[i]:NULL;
        }
        if(!cache[i].used || (entry->used && cache[i].age<entry->age))entry=&cache[i];
    }
    if(entry->shader)ID3D11VertexShader_Release(entry->shader);
    for(unsigned i=0;i<8;i++)if(entry->layouts[i].layout)ID3D11InputLayout_Release(entry->layouts[i].layout);
    if(entry->code)ID3D10Blob_Release(entry->code);
    memset(entry,0,sizeof(*entry));entry->used=1;entry->count=count;entry->age=++serial;
    memcpy(entry->words,words,count*16);
    char *source=malloc(131072);if(!source)return NULL;
    nv2a_vsh_program decoded;nv2a_vsh_decode(&decoded,words,count);
    memcpy(source,prefix,sizeof(prefix)-1);
    size_t n=nv2a_vsh_emit_hlsl(source+sizeof(prefix)-1,120000,&decoded);
    if(!n){free(source);return NULL;}
    n+=sizeof(prefix)-1;memcpy(source+n,suffix,sizeof(suffix));n+=sizeof(suffix)-1;
    ID3DBlob *blob=NULL,*errors=NULL;
    HRESULT hr=d3d8_compile_shader(source,n,"live-nv2a","main","vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,&blob,&errors);
    free(source);
    if(FAILED(hr)) {
        ++g_gpu_vsh_failures;
        fprintf(stderr,"[GPU-VSH] compile failed: %.*s\n",errors?(int)ID3D10Blob_GetBufferSize(errors):0,
            errors?(const char*)ID3D10Blob_GetBufferPointer(errors):"");
    } else {
        ID3D11Device *dev=d3d8_GetD3D11Device();
        hr=ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(blob),ID3D10Blob_GetBufferSize(blob),NULL,&entry->shader);
        if(SUCCEEDED(hr)){entry->code=blob;blob=NULL;}
        if(FAILED(hr)) {
            if(entry->shader)ID3D11VertexShader_Release(entry->shader);entry->shader=NULL;
            ++g_gpu_vsh_failures;
        } else ++g_gpu_vsh_compiles;
    }
    if(blob)ID3D10Blob_Release(blob);if(errors)ID3D10Blob_Release(errors);
    return entry->shader && prepare_layout(entry,formats,offsets)?entry:NULL;
}

int d3d8_nv2a_gpu_bind(void)
{
    if(!pending)return 0;
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    ID3D11DeviceContext_VSSetShader(ctx,pending->shader,NULL,0);
    ID3D11DeviceContext_IASetInputLayout(ctx,pending->layout);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx,2,1,&constant_buffer);
    return 1;
}
HRESULT d3d8_nv2a_gpu_draw(void *program,Nv2aGpuConstants *constants,
    D3DPRIMITIVETYPE primitive,unsigned count,const void *vertices,unsigned stride)
{
    ID3D11Device *dev=d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx=d3d8_GetD3D11Context();
    if(!constant_buffer) {
        D3D11_BUFFER_DESC desc={0};desc.ByteWidth=sizeof(*constants);
        desc.Usage=D3D11_USAGE_DYNAMIC;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        HRESULT hr=ID3D11Device_CreateBuffer(dev,&desc,NULL,&constant_buffer);
        if(FAILED(hr))return hr;
    }
    for(unsigned stage=1;stage<4;stage++)
        if(d3d8_GetTextureStageStateValue(stage,D3DTSS_TEXTURETRANSFORMFLAGS)!=D3DTTFF_DISABLE)
            memcpy(constants->tex_matrix[stage],d3d8_GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0+stage)),64);
    /* Match the fixed-function XYZRHW bridge's target-size normalization. */
    d3d8_GetRenderTargetSize(&constants->viewport[0],&constants->viewport[1]);
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)constant_buffer,0,D3D11_MAP_WRITE_DISCARD,0,&map);
    if(FAILED(hr))return hr;
    memcpy(map.pData,constants,sizeof(*constants));
    ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)constant_buffer,0);
    pending=program;
    IDirect3DDevice8 *bridge=d3d8_GetDevice();
    hr=bridge->lpVtbl->DrawPrimitiveUP(bridge,primitive,count,vertices,stride);
    pending=NULL;if(SUCCEEDED(hr))++g_gpu_vsh_draws;
    return hr;
}
void d3d8_nv2a_gpu_shutdown(void)
{
    for(unsigned i=0;i<GPU_VSH_CACHE;i++) {
        if(cache[i].shader)ID3D11VertexShader_Release(cache[i].shader);
        for(unsigned j=0;j<8;j++)if(cache[i].layouts[j].layout)ID3D11InputLayout_Release(cache[i].layouts[j].layout);
        if(cache[i].code)ID3D10Blob_Release(cache[i].code);
    }
    memset(cache,0,sizeof(cache));pending=NULL;
    if(constant_buffer)ID3D11Buffer_Release(constant_buffer);constant_buffer=NULL;
}
