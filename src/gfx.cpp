#include "gfx.h"
#include <d3dcompiler.h>
#include <windows.graphics.directx.direct3d11.interop.h>

Gfx g_gfx;

static const char* kShaderSrc = R"HLSL(
cbuffer CB : register(b0) {
    float4 rect;   // x y w h
    float4 shape;  // radius, bezel, refraction, opacity
    float4 look;   // chroma, tintAmt, saturation, highlight
    float4 misc;   // texW, texH, shadowAlpha, shadowRadius
    float4 blur;   // dx, dy, sigma, taps
    float4 tint;   // rgb, brightness
};
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState samp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

// Half-resolution downsample: bilinear tap in the middle of a 2x2 block = box filter.
float4 PSDown(VSOut i) : SV_Target { return tex0.Sample(samp, i.uv); }

// Separable gaussian.
float4 PSBlur(VSOut i) : SV_Target {
    float sigma = max(blur.z, 0.01);
    int taps = (int)blur.w;
    float4 acc = tex0.Sample(samp, i.uv);
    float wsum = 1;
    [loop] for (int k = 1; k <= taps; ++k) {
        float w = exp(-(k * k) / (2 * sigma * sigma));
        acc += (tex0.Sample(samp, i.uv + blur.xy * k) + tex0.Sample(samp, i.uv - blur.xy * k)) * w;
        wsum += 2 * w;
    }
    return acc / wsum;
}

float sdRR(float2 p, float2 c, float2 hs, float r) {
    float2 q = abs(p - c) - hs + r;
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}

// Liquid glass: rounded slab with a convex bezel that refracts the (blurred) backdrop.
float4 PSGlass(VSOut i) : SV_Target {
    float2 p = i.pos.xy;
    float2 hs = rect.zw * 0.5;
    float2 c = rect.xy + hs;
    float r = min(shape.x, min(hs.x, hs.y));
    float d = sdRR(p, c, hs, r);
    float op = shape.w;

    // soft drop shadow (slightly offset downward)
    float ds = sdRR(p - float2(0, misc.w * 0.3), c, hs, r);
    float sh = misc.z * op * exp(-max(ds, 0) / max(misc.w, 1));
    sh *= sh > 0.002 ? 1 : 0;
    if (d > 1.0) return float4(0, 0, 0, sh);

    float2 e = float2(1, 0);
    float2 g = float2(sdRR(p + e.xy, c, hs, r) - sdRR(p - e.xy, c, hs, r),
                      sdRR(p + e.yx, c, hs, r) - sdRR(p - e.yx, c, hs, r));
    float2 n = g / max(length(g), 1e-4);           // outward normal
    float inside = max(-d, 0);
    float bez = max(shape.y, 1);
    float x = saturate(1 - inside / bez);           // 1 at rim -> 0 in the flat middle
    float lens = 1 - sqrt(saturate(1 - x * x));     // circular bezel profile
    float2 off = -n * shape.z * lens;               // convex: rays bend toward the centre

    float2 inv = 1 / misc.xy;
    float ca = look.x;
    float3 col;
    col.r = tex1.Sample(samp, (p + off * (1 + ca)) * inv).r;
    col.g = tex1.Sample(samp, (p + off) * inv).g;
    col.b = tex1.Sample(samp, (p + off * (1 - ca)) * inv).b;

    // vibrancy + frost
    float l = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = lerp(l.xxx, col, look.z) * tint.w;
    col = lerp(col, tint.rgb, look.y);

    // lighting: thin specular rim + broad bezel glints from a top-left light,
    // weaker glint on the opposite side (light passing through the slab)
    float2 L = normalize(float2(-0.55, -0.85));
    float facing = dot(n, L);
    float rim = saturate(1.0 - inside / 1.4);
    float glint = pow(x, 2.5) * (saturate(facing) + 0.55 * saturate(-facing));
    col += look.w * (rim * (0.30 + 0.55 * abs(facing)) + glint * 0.22);
    // faint inner edge darkening gives the slab thickness
    col *= 1 - 0.05 * x * saturate(1 - abs(facing));

    // bright backdrops: a soft darker bezel band keeps the slab readable on white
    float bgL = dot(saturate(col), float3(0.2126, 0.7152, 0.0722));
    col *= 1 - 0.10 * smoothstep(0.55, 0.95, bgL) * pow(x, 3);

    float cov = saturate(0.5 - d) * op;
    return float4(saturate(col) * cov, cov + sh * (1 - cov));
}

// Recorder: desktop capture (t0, full monitor) with the dock frame (t1, premultiplied) composited on top,
// cropped to rect and box-filtered down to the output size (shape.xy). blur = dock window (x, y, w, h).
float4 PSRecord(VSOut i) : SV_Target {
    float2 p = rect.xy + i.uv * rect.zw;
    float2 d = rect.zw / shape.xy * 0.25;
    float3 acc = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        float2 q = p + float2((k & 1) ? d.x : -d.x, (k & 2) ? d.y : -d.y);
        float3 bg = tex0.SampleLevel(samp, q / misc.xy, 0).rgb;
        float2 w = (q - blur.xy) / blur.zw;
        float4 fg = (all(w >= 0) && all(w <= 1)) ? tex1.SampleLevel(samp, w, 0) : 0;
        acc += fg.rgb + bg * (1 - fg.a);
    }
    return float4(acc / 4, 1);
}

// 16x1 luminance probe across the bar (rect), read back by the CPU to pick ink colours.
float4 PSLum(VSOut i) : SV_Target {
    float col = floor(i.pos.x);
    float u0 = rect.x + rect.z * (col / 16), u1 = rect.x + rect.z * ((col + 1) / 16);
    float3 acc = 0;
    [unroll] for (int a = 0; a < 6; ++a)
        [unroll] for (int b = 0; b < 4; ++b) {
            float2 p = float2(lerp(u0, u1, (a + 0.5) / 6), rect.y + rect.w * (b + 0.5) / 4);
            acc += tex1.SampleLevel(samp, p / misc.xy, 0).rgb;
        }
    return float4(acc / 24, 1);
}
)HLSL";

static ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "glass.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        Log(L"shader compile failed: %S %S", entry, err ? (const char*)err->GetBufferPointer() : "");
        return nullptr;
    }
    return code;
}

static ComPtr<IDXGIAdapter1> AdapterForPrimaryMonitor() {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return nullptr;
    HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC d; o->GetDesc(&d);
            if (d.Monitor == primary) return a;
            o.Reset();
        }
        a.Reset();
    }
    return nullptr;
}

bool Gfx::DeviceLost() const {
    return d3d && FAILED(d3d->GetDeviceRemovedReason());
}

// Driver update / GPU reset: drop everything tied to the old device and start over.
bool Gfx::Reset() {
    winrtDevice = nullptr;
    fmtTooltip.Reset(); fmtClock.Reset(); fmtClockSmall.Reset(); fmtGlyph.Reset(); fmtTiny.Reset();
    fmtPanelLabel.Reset(); fmtPanelSub.Reset(); fmtPanelValue.Reset(); fmtPanelSmallR.Reset();
    fontScale = 0;
    fontSets.clear();
    cb.Reset(); blendOver.Reset(); sampLinear.Reset();
    vs.Reset(); psDown.Reset(); psBlur.Reset(); psGlass.Reset(); psLum.Reset(); psRecord.Reset();
    dcomp.Reset(); dc.Reset(); d2dDevice.Reset(); d2dFactory.Reset();
    if (ctx) { ctx->ClearState(); ctx->Flush(); }
    ctx.Reset(); factory.Reset(); dxgi.Reset(); d3d.Reset();
    return Init();
}

bool Gfx::Init() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };   // the shaders are shader model 5
    ComPtr<IDXGIAdapter1> adapter = AdapterForPrimaryMonitor();
    HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d, nullptr, &ctx);
    if (FAILED(hr)) { Log(L"D3D11CreateDevice failed %08x", hr); return false; }
    if (adapter) { DXGI_ADAPTER_DESC1 ad; adapter->GetDesc1(&ad); Log(L"GPU: %s", ad.Description); }

    d3d.As(&dxgi);
    ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
    ad->GetParent(IID_PPV_ARGS(&factory));

    D2D1_FACTORY_OPTIONS fo = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &fo, (void**)d2dFactory.GetAddressOf());
    if (FAILED(hr)) return false;
    if (FAILED(d2dFactory->CreateDevice(dxgi.Get(), &d2dDevice))) return false;
    if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
    dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw.GetAddressOf());
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp)))) { Log(L"DComp failed"); return false; }

    auto vsb = Compile("VSMain", "vs_5_0");
    auto pd = Compile("PSDown", "ps_5_0");
    auto pb = Compile("PSBlur", "ps_5_0");
    auto pg = Compile("PSGlass", "ps_5_0");
    auto pl = Compile("PSLum", "ps_5_0");
    if (!vsb || !pd || !pb || !pg || !pl) return false;
    d3d->CreatePixelShader(pl->GetBufferPointer(), pl->GetBufferSize(), nullptr, &psLum);
    auto pr = Compile("PSRecord", "ps_5_0");
    if (pr) d3d->CreatePixelShader(pr->GetBufferPointer(), pr->GetBufferSize(), nullptr, &psRecord);
    if (FAILED(d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) ||
        FAILED(d3d->CreatePixelShader(pd->GetBufferPointer(), pd->GetBufferSize(), nullptr, &psDown)) ||
        FAILED(d3d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &psBlur)) ||
        FAILED(d3d->CreatePixelShader(pg->GetBufferPointer(), pg->GetBufferSize(), nullptr, &psGlass))) {
        Log(L"shader creation failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    d3d->CreateSamplerState(&sd, &sampLinear);

    D3D11_BLEND_DESC bld = {};
    auto& rt = bld.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    d3d->CreateBlendState(&bld, &blendOver);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(GlassCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d3d->CreateBuffer(&bd, nullptr, &cb);

    winrt::com_ptr<::IInspectable> insp;
    if (SUCCEEDED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put())))
        winrtDevice = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return true;
}

// Text formats per screen scale (monitors can differ). The current set is swapped into the fmt*
// members, so each dock calls this before it lays out or draws.
void Gfx::EnsureFonts(float scale) {
    if (std::fabs(scale - fontScale) < 0.01f && fmtTooltip) return;
    for (auto& s : fontSets) {
        if (std::fabs(s.scale - scale) < 0.01f) {
            fontScale = scale;
            fmtTooltip = s.f[0]; fmtClock = s.f[1]; fmtClockSmall = s.f[2]; fmtGlyph = s.f[3]; fmtTiny = s.f[4];
            fmtPanelLabel = s.f[5]; fmtPanelSub = s.f[6]; fmtPanelValue = s.f[7]; fmtPanelSmallR = s.f[8];
            return;
        }
    }
    fontScale = scale;
    // Windows 11 fonts, with the Windows 10 equivalents as fallback (same icon code points)
    ComPtr<IDWriteFontCollection> sys;
    dw->GetSystemFontCollection(&sys);
    auto have = [&](const wchar_t* fam) { UINT32 i; BOOL e = FALSE; return sys && SUCCEEDED(sys->FindFamilyName(fam, &i, &e)) && e; };
    const wchar_t* text = have(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* display = have(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icons = have(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    auto mk = [&](const wchar_t* fam, DWRITE_FONT_WEIGHT w, float size, ComPtr<IDWriteTextFormat>& out) {
        out.Reset();
        if (!wcscmp(fam, L"Segoe UI Variable Text")) fam = text;
        else if (!wcscmp(fam, L"Segoe UI Variable Display")) fam = display;
        else if (!wcscmp(fam, L"Segoe Fluent Icons")) fam = icons;
        dw->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * scale, L"ko-kr", &out);
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, fmtTooltip);
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, fmtClock);
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 11.0f, fmtClockSmall);
    mk(L"Segoe Fluent Icons", DWRITE_FONT_WEIGHT_NORMAL, 17.0f, fmtGlyph);
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 9.0f, fmtTiny);
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, fmtPanelLabel);
    fmtPanelLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 10.5f, fmtPanelSub);
    fmtPanelSub->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    mk(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 19.0f, fmtPanelValue);
    fmtPanelValue->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 10.5f, fmtPanelSmallR);
    fmtPanelSmallR->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    fontSets.push_back({ scale, { fmtTooltip, fmtClock, fmtClockSmall, fmtGlyph, fmtTiny,
                                  fmtPanelLabel, fmtPanelSub, fmtPanelValue, fmtPanelSmallR } });
}

void Gfx::UpdateCB(const GlassCB& c) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cb.Get(), 0);
    }
}

void Gfx::FullscreenPass(ID3D11RenderTargetView* rtv, int w, int h, ID3D11PixelShader* ps,
                         ID3D11ShaderResourceView* srv0, ID3D11ShaderResourceView* srv1, bool blend) {
    ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nulls);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampLinear.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = { srv0, srv1 };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->OMSetBlendState(blend ? blendOver.Get() : nullptr, nullptr, 0xffffffff);
    ctx->Draw(3, 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->PSSetShaderResources(0, 2, nulls);
    ID3D11RenderTargetView* nullrt = nullptr;
    ctx->OMSetRenderTargets(1, &nullrt, nullptr);
}

bool RTTexture::Create(int w_, int h_, bool renderTarget) {
    w = std::max(1, w_); h = std::max(1, h_);
    tex.Reset(); rtv.Reset(); srv.Reset();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(g_gfx.d3d->CreateTexture2D(&d, nullptr, &tex))) return false;
    g_gfx.d3d->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (renderTarget) g_gfx.d3d->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
    return true;
}
