#include "capture.h"
#include "gfx.h"
#include <thread>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;

void MonitorCapture::RequestBorderlessAccess() {
    // Must not block the STA UI thread; run on a throwaway MTA thread.
    std::thread([] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            auto r = GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
            Log(L"borderless capture access: %d", (int)r);
        } catch (...) { Log(L"borderless capture access request unsupported"); }
    }).detach();
}

bool MonitorCapture::UseGdi() {
    static int v = -1;
    if (v < 0) {
        wchar_t e[8];
        if (GetEnvironmentVariableW(L"LIQUIDDOCK_GDI_CAPTURE", e, 8)) v = 1;
        else {
            try {
                v = winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired") ? 0 : 1;
            } catch (...) { v = 1; }
        }
        Log(L"capture mode: %s", v ? L"GDI (no borderless capture)" : L"Windows.Graphics.Capture");
    }
    return v == 1;
}

bool MonitorCapture::Start(HMONITOR mon) {
    Stop();
    if (UseGdi()) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        gdiMon = mi.rcMonitor;
        gdiActive = true;
        gdiLast = 0;
        startedAt = NowSeconds();
        gotFirstFrame = false;
        return true;
    }
    try {
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForMonitor(mon, guid_of<GraphicsCaptureItem>(), put_abi(item)));
        size = item.Size();
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(g_gfx.winrtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session = pool.CreateCaptureSession(item);
        try { session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { session.IsBorderRequired(false); } catch (...) {}
        session.StartCapture();
        startedAt = NowSeconds();
        gotFirstFrame = false;
        return true;
    } catch (hresult_error const& e) {
        Log(L"capture start failed: %s", e.message().c_str());
        Stop();
        return false;
    }
}

void MonitorCapture::Stop() {
    try {
        if (session) session.Close();
        if (pool) pool.Close();
    } catch (...) {}
    session = nullptr; pool = nullptr; item = nullptr;
    gdiActive = false;
    if (gdiMem) { DeleteDC(gdiMem); gdiMem = nullptr; }
    if (gdiBmp) { DeleteObject(gdiBmp); gdiBmp = nullptr; gdiBits = nullptr; }
    gdiW = gdiH = 0;
}

// Windows 10 fallback: BitBlt only the dock's own region (the dock is excluded from capture,
// so this shows what lies behind it) at up to 30 fps.
bool MonitorCapture::PollGdi(ID3D11Texture2D* dst, int srcX, int srcY, int w, int h) {
    double now = NowSeconds();
    if (now - gdiLast < 1.0 / 30) return false;
    gdiLast = now;
    if (w != gdiW || h != gdiH || !gdiMem) {
        if (gdiMem) DeleteDC(gdiMem);
        if (gdiBmp) DeleteObject(gdiBmp);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        gdiMem = CreateCompatibleDC(nullptr);
        gdiBmp = CreateDIBSection(gdiMem, &bi, DIB_RGB_COLORS, &gdiBits, nullptr, 0);
        SelectObject(gdiMem, gdiBmp);
        gdiW = w; gdiH = h;
    }
    HDC screen = GetDC(nullptr);
    BOOL ok = BitBlt(gdiMem, 0, 0, w, h, screen, gdiMon.left + srcX, gdiMon.top + srcY, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!ok) return false;
    GdiFlush();
    auto* px = (uint32_t*)gdiBits;
    for (int i = 0; i < w * h; ++i) px[i] |= 0xff000000u;
    g_gfx.ctx->UpdateSubresource(dst, 0, nullptr, gdiBits, w * 4, 0);
    gotFirstFrame = true;
    return true;
}

bool MonitorCapture::Poll(ID3D11Texture2D* dst, int srcX, int srcY, int w, int h, ID3D11Texture2D* fullDst) {
    if (gdiActive) return PollGdi(dst, srcX, srcY, w, h);
    if (!pool) return false;
    Direct3D11CaptureFrame frame{ nullptr }, f{ nullptr };
    try {
        while ((f = pool.TryGetNextFrame()) != nullptr) { if (frame) frame.Close(); frame = f; }
    } catch (...) { return false; }
    if (!frame) return false;

    // monitor unplug, resolution change, sleep/resume or a GPU reset make these throw: drop the
    // session (the dock starts a new one) instead of taking the whole dock down
    ComPtr<ID3D11Texture2D> src;
    winrt::Windows::Graphics::SizeInt32 cs{};
    try {
        cs = frame.ContentSize();
        if (cs.Width != size.Width || cs.Height != size.Height) {
            size = cs;
            pool.Recreate(g_gfx.winrtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        access->GetInterface(IID_PPV_ARGS(&src));
    } catch (...) {
        Log(L"capture frame failed - restarting capture");
        Stop();
        return false;
    }
    if (!src) { frame.Close(); return false; }
    D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);

    int x0 = std::max(0, srcX), y0 = std::max(0, srcY);
    int x1 = std::min<int>(srcX + w, std::min<int>(cs.Width, sd.Width));
    int y1 = std::min<int>(srcY + h, std::min<int>(cs.Height, sd.Height));
    if (x1 > x0 && y1 > y0) {
        D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
        g_gfx.ctx->CopySubresourceRegion(dst, 0, x0 - srcX, y0 - srcY, 0, src.Get(), 0, &box);
    }
    if (fullDst) {
        D3D11_TEXTURE2D_DESC fd; fullDst->GetDesc(&fd);
        D3D11_BOX all = { 0, 0, 0, std::min<UINT>(fd.Width, sd.Width), std::min<UINT>(fd.Height, sd.Height), 1 };
        g_gfx.ctx->CopySubresourceRegion(fullDst, 0, 0, 0, 0, src.Get(), 0, &all);
    }
    frame.Close();
    gotFirstFrame = true;
    return true;
}
