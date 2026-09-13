/* Private implementation included after RuntimeDepthTarget in d3d8_device.c. */
static HRESULT runtime_depth_import_init(void)
{
    ID3D11Device *dev = g_device_state.d3d11_device;
    HRESULT hr;
    if (!g_depth_import_vs) {
        const char *vs = "float4 main(uint i:SV_VertexID):SV_POSITION{"
            "float2 p=float2((i<<1)&2,i&2);return float4(p*2-1,0,1);}";
        ID3DBlob *code = NULL;
        hr = D3DCompile(vs, strlen(vs), NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &code, NULL);
        if (FAILED(hr)) return hr;
        hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, &g_depth_import_vs);
        ID3D10Blob_Release(code);
        if (FAILED(hr)) return hr;
    }
    if (!g_depth_import_ps) {
        const char *ps =
            "Texture2D<float> z:register(t0);"
            "cbuffer C:register(b0){uint4 src;uint4 dst;}"
            "uint encode(uint2 p,uint2 size){uint a=0,b=0;"
            "[unroll]for(uint i=0;i<12;i++){uint k=1u<<i;"
            "if(k<size.x){a|=((p.x>>i)&1u)<<b;b++;}"
            "if(k<size.y){a|=((p.y>>i)&1u)<<b;b++;}}return a;}"
            "uint2 decode(uint a,uint2 size){uint2 p=0;uint b=0;"
            "[unroll]for(uint i=0;i<12;i++){uint k=1u<<i;"
            "if(k<size.x){p.x|=((a>>b)&1u)<<i;b++;}"
            "if(k<size.y){p.y|=((a>>b)&1u)<<i;b++;}}return p;}"
            "float main(float4 pos:SV_POSITION):SV_Depth{uint2 p=uint2(pos.xy);"
            "uint a=dst.w!=0?encode(p,dst.xy):p.y*dst.z+p.x;"
            "uint2 q=src.w!=0?decode(a,src.xy):uint2(a%src.z,a/src.z);"
            "return z.Load(int3(q,0));}";
        ID3DBlob *code = NULL;
        hr = D3DCompile(ps, strlen(ps), NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &code, NULL);
        if (FAILED(hr)) return hr;
        hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, &g_depth_import_ps);
        ID3D10Blob_Release(code);
        if (FAILED(hr)) return hr;
    }
    if (!g_depth_import_constants) {
        D3D11_BUFFER_DESC d = {0}; d.ByteWidth = 32; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = ID3D11Device_CreateBuffer(dev, &d, NULL, &g_depth_import_constants);
        if (FAILED(hr)) return hr;
    }
    if (!g_depth_import_state) {
        D3D11_DEPTH_STENCIL_DESC d = {0};
        d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = ID3D11Device_CreateDepthStencilState(dev, &d, &g_depth_import_state);
        if (FAILED(hr)) return hr;
    }
    if (!g_depth_import_raster) {
        D3D11_RASTERIZER_DESC d = {0};
        d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_NONE;
        hr = ID3D11Device_CreateRasterizerState(dev, &d, &g_depth_import_raster);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

static uint64_t runtime_depth_extent(const RuntimeDepthTarget *t)
{
    UINT bpp = t->format == D3DFMT_D16 ? 2u : 4u;
    return t->swizzled ? (uint64_t)t->width * t->height * bpp
        : (uint64_t)(t->height - 1u) * t->pitch + t->width * bpp;
}

static HRESULT runtime_depth_import(RuntimeDepthTarget *target)
{
    RuntimeDepthTarget *source = NULL;
    UINT bpp = target->format == D3DFMT_D16 ? 2u : 4u;
    uint64_t current = target->write_serial > target->imported_serial
        ? target->write_serial : target->imported_serial;
    for (RuntimeDepthTarget *t = g_runtime_depth_targets; t; t = t->next) {
        if (t == target || t->guest_offset != target->guest_offset ||
            t->format != target->format || t->write_serial <= current ||
            runtime_depth_extent(t) < runtime_depth_extent(target)) continue;
        /* A padded source covers the destination only when their row
         * layouts agree. Contiguous sources also cover swizzled subviews. */
        if (!t->swizzled && t->pitch != t->width * bpp &&
            (target->swizzled || t->pitch != target->pitch ||
             t->width < target->width)) continue;
        source = t; current = t->write_serial;
    }
    if (!source) return S_OK;
    HRESULT hr = runtime_depth_import_init();
    if (FAILED(hr)) return hr;
    ID3D11Device *dev = g_device_state.d3d11_device;
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    if (!source->depth_srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {0};
        d.Format = source->format == D3DFMT_D16 ? DXGI_FORMAT_R16_UNORM
            : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1;
        hr = ID3D11Device_CreateShaderResourceView(dev,
            (ID3D11Resource *)source->texture, &d, &source->depth_srv);
        if (FAILED(hr)) return hr;
    }
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11GeometryShader *gs = NULL;
    ID3D11Buffer *cb = NULL;
    ID3D11ShaderResourceView *srv = NULL, *null_srv = NULL;
    ID3D11InputLayout *layout = NULL;
    ID3D11RasterizerState *rs = NULL;
    ID3D11DepthStencilState *ds = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, ref;
    ID3D11DeviceContext_VSGetShader(ctx, &vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(ctx, &ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(ctx, &gs, NULL, NULL);
    ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_IAGetInputLayout(ctx, &layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(ctx, &topology);
    ID3D11DeviceContext_RSGetState(ctx, &rs);
    ID3D11DeviceContext_RSGetViewports(ctx, &viewport_count, viewports);
    ID3D11DeviceContext_OMGetDepthStencilState(ctx, &ds, &ref);
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &rtv, &dsv);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, target->view);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_depth_import_state, 0);
    ID3D11DeviceContext_RSSetState(ctx, g_depth_import_raster);
    D3D11_VIEWPORT vp = {0, 0, (float)target->width, (float)target->height, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_depth_import_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_depth_import_ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    UINT values[8] = {source->width, source->height, source->pitch / bpp, source->swizzled,
        target->width, target->height, target->pitch / bpp, target->swizzled};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_depth_import_constants,
        0, NULL, values, 0, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_depth_import_constants);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &source->depth_srv);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds, ref);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    ID3D11DeviceContext_RSSetViewports(ctx, viewport_count, viewports);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topology);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    if (vs) ID3D11VertexShader_Release(vs);
    if (ps) ID3D11PixelShader_Release(ps);
    if (gs) ID3D11GeometryShader_Release(gs);
    if (cb) ID3D11Buffer_Release(cb);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (layout) ID3D11InputLayout_Release(layout);
    if (rs) ID3D11RasterizerState_Release(rs);
    if (ds) ID3D11DepthStencilState_Release(ds);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
    target->imported_serial = source->write_serial;
    return S_OK;
}
