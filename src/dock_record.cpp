// Built-in recorder: composites the desktop capture with the dock's own frames (which normal
// screen recorders can't see) and writes raw BGRA frames for ffmpeg.
#include "dock.h"

static constexpr double kRecFps = 30.0;

void Dock::StartRecording(RECT crop, float outScale, bool append) {
    if (recording) return;
    RECT full = { 0, 0, mon.right - mon.left, mon.bottom - mon.top };
    IntersectRect(&crop, &crop, &full);
    recCrop = crop;
    recW = ((int)((crop.right - crop.left) * outScale)) & ~1;
    recH = ((int)((crop.bottom - crop.top) * outScale)) & ~1;
    if (recW < 16 || recH < 16) return;
    dockFrame.Create(winW, winH, true);
    fullTex.Create(mon.right - mon.left, mon.bottom - mon.top, false);
    recRT.Create(recW, recH, true);
    for (auto& st : recStage) {
        D3D11_TEXTURE2D_DESC d; recRT.tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        st.Reset();
        g_gfx.d3d->CreateTexture2D(&d, nullptr, &st);
    }
    ComPtr<IDXGISurface> surf; recRT.tex.As(&surf);
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    recTarget.Reset();
    g_gfx.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &recTarget);

    std::wstring dir = AppDataDir() + L"\\rec";
    CreateDirectoryW(dir.c_str(), nullptr);
    _wfopen_s(&recFile, (dir + L"\\frames.raw").c_str(), append ? L"ab" : L"wb");
    carryRecording = true; carryCrop = crop; carryScale = outScale;
    if (!append) carryFrames = 0;
    if (!recFile) return;
    float zero[4] = {};
    g_gfx.ctx->ClearRenderTargetView(dockFrame.rtv.Get(), zero);
    recFrames = 0; recSlot = 0;
    recNext = NowSeconds();
    recording = true;
    dirty = true;
    Log(L"recording %dx%d from crop %d,%d %dx%d", recW, recH, crop.left, crop.top, crop.right - crop.left, crop.bottom - crop.top);
}

void Dock::RecordTick(double now) {
    if (!recording || now < recNext) return;
    recNext += 1.0 / kRecFps;
    if (now - recNext > 0.5) recNext = now;    // fell far behind: don't burst

    GlassCB c = {};
    c.rect[0] = (float)recCrop.left; c.rect[1] = (float)recCrop.top;
    c.rect[2] = (float)(recCrop.right - recCrop.left); c.rect[3] = (float)(recCrop.bottom - recCrop.top);
    c.shape[0] = (float)recW; c.shape[1] = (float)recH;
    c.misc[0] = (float)(mon.right - mon.left); c.misc[1] = (float)(mon.bottom - mon.top);
    c.blur[0] = (float)(winX - mon.left); c.blur[1] = (float)(winY - mon.top); c.blur[2] = (float)winW; c.blur[3] = (float)winH;
    g_gfx.UpdateCB(c);
    g_gfx.FullscreenPass(recRT.rtv.Get(), recW, recH, g_gfx.psRecord.Get(), fullTex.srv.Get(), dockFrame.srv.Get());

    // the dock hides the real cursor from capture; draw a classic arrow where it is
    POINT cp; GetCursorPos(&cp);
    float sx = (float)recW / (recCrop.right - recCrop.left);
    float px = (cp.x - mon.left - recCrop.left) * sx, py = (cp.y - mon.top - recCrop.top) * sx;
    auto* dc = g_gfx.dc.Get();
    dc->SetTarget(recTarget.Get());
    dc->BeginDraw();
    {
        float k = 1.15f * scale * sx * 1.4f;
        D2D1_POINT_2F pts[] = { {0,0},{0,17},{4.2f,13.2f},{7,19.6f},{9.6f,18.5f},{6.9f,12.2f},{12.4f,12.2f} };
        ComPtr<ID2D1PathGeometry> g; g_gfx.d2dFactory->CreatePathGeometry(&g);
        ComPtr<ID2D1GeometrySink> sk; g->Open(&sk);
        sk->BeginFigure({ px + pts[0].x * k, py + pts[0].y * k }, D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < 7; ++i) sk->AddLine({ px + pts[i].x * k, py + pts[i].y * k });
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
        sk->Close();
        ComPtr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b);
        dc->FillGeometry(g.Get(), b.Get());
        b->SetColor(D2D1::ColorF(1, 1, 1, 1));
        dc->DrawGeometry(g.Get(), b.Get(), 1.2f * k);
    }
    dc->EndDraw();
    dc->SetTarget(nullptr);

    // readback two frames late so the GPU never stalls us
    g_gfx.ctx->CopyResource(recStage[recSlot % 3].Get(), recRT.tex.Get());
    if (recSlot >= 2) {
        auto* st = recStage[(recSlot - 2) % 3].Get();
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_gfx.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < recH; ++y) fwrite((BYTE*)m.pData + y * m.RowPitch, 4, recW, recFile);
            g_gfx.ctx->Unmap(st, 0);
            recFrames++;
        }
    }
    recSlot++;
}

// The window is going away while recording (docks rebuilt): close the file, keep the count, and let
// the new primary dock append (BuildDocks).
void Dock::SuspendRecording() {
    if (!recording) return;
    recording = false;
    if (recFile) { fclose(recFile); recFile = nullptr; }
    carryFrames += recFrames;
    dockFrame = {}; fullTex = {}; recRT = {}; recTarget.Reset();
    for (auto& st : recStage) st.Reset();
}

void Dock::StopRecording() {
    if (!recording) return;
    recording = false;
    carryRecording = false;
    if (recFile) { fclose(recFile); recFile = nullptr; }
    recFrames += carryFrames; carryFrames = 0;
    FILE* f = nullptr;
    _wfopen_s(&f, (AppDataDir() + L"\\rec\\info.txt").c_str(), L"w");
    if (f) { fprintf(f, "%d %d %d %d\n", recW, recH, (int)kRecFps, recFrames); fclose(f); }
    Log(L"recording stopped: %d frames", recFrames);
    dockFrame = {}; fullTex = {}; recRT = {}; recTarget.Reset();
    for (auto& st : recStage) st.Reset();
}
