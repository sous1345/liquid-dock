#include "dock.h"
#include "system.h"
#include "sensors.h"
#include "oled.h"
#include "theme.h"
#include <d2d1effects.h>

struct GlassStyle { float sigma, tint, sat, refr, bezel, chroma, highlight, bright; };
static const GlassStyle kStyles[3] = {
    { 1.5f, 0.05f, 1.30f, 18, 18, 0.10f, 0.42f, 1.03f },   // clear
    { 6.0f, 0.10f, 1.40f, 16, 18, 0.08f, 0.36f, 1.04f },   // normal
    { 16.f, 0.20f, 1.30f, 12, 18, 0.05f, 0.30f, 1.05f },   // frosted
};

// ---------------------------------------------------------------- drawing

float Dock::InkAt(float u) const {
    float w = std::max(1.0f, barU1 - barU0);
    int i = std::clamp((int)((u - barU0) / w * 16), 0, 15);
    return inkCol[i];
}

D2D1_COLOR_F Dock::Ink(float u, float alpha) const {
    float k = InkAt(u);
    return D2D1::ColorF(lerpf(1.0f, 0.07f, k), lerpf(1.0f, 0.07f, k), lerpf(1.0f, 0.09f, k), alpha * lerpf(0.95f, 0.86f, k));
}

void Dock::DrawStartIcon(D2D1_RECT_F r, float alpha) {
    auto* dc = g_gfx.dc.Get();
    float sz = r.right - r.left;
    float rad = sz * 0.225f;
    D2D1_GRADIENT_STOP stops[2] = { { 0, D2D1::ColorF(0.30f, 0.70f, 1.0f, alpha) }, { 1, D2D1::ColorF(0.05f, 0.33f, 0.88f, alpha) } };
    ComPtr<ID2D1GradientStopCollection> sc;
    dc->CreateGradientStopCollection(stops, 2, &sc);
    ComPtr<ID2D1LinearGradientBrush> gb;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ r.left, r.top }, { r.right, r.bottom }), sc.Get(), &gb);
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    dc->FillRoundedRectangle(rr, gb.Get());
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.28f * alpha));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ { r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f }, rad, rad }, brush.Get(), 1.0f);
    float lg = sz * 0.42f, g = sz * 0.03f, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float q = (lg - g) / 2;
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.97f * alpha));
    for (int i = 0; i < 4; ++i) {
        float x = cx - lg / 2 + (i % 2) * (q + g), y = cy - lg / 2 + (i / 2) * (q + g);
        dc->FillRectangle({ x, y, x + q, y + q }, brush.Get());
    }
}

void Dock::DrawIcon(Slot& s, float u, float vb, float size, float alpha) {
    auto* dc = g_gfx.dc.Get();
    D2D1_RECT_F r = RectUV(u - size / 2, u + size / 2, vb, vb + size);
    if (!s.app) { DrawStartIcon(r, alpha); if (s.press.x > 0.02f) { brush->SetColor(D2D1::ColorF(0, 0, 0, 0.3f * s.press.x)); dc->FillRoundedRectangle({ r, size * 0.225f, size * 0.225f }, brush.Get()); } return; }
    auto* bmp = s.app->icon.Get();
    if (!bmp) {
        brush->SetColor(D2D1::ColorF(0.6f, 0.6f, 0.65f, 0.9f * alpha));
        dc->FillRoundedRectangle({ r, size * 0.22f, size * 0.22f }, brush.Get());
        return;
    }
    if (s.press.x > 0.02f) {
        float k = 1 - 0.45f * s.press.x;
        D2D1_MATRIX_5X4_F mtx = { k,0,0,0, 0,k,0,0, 0,0,k,0, 0,0,0,alpha, 0,0,0,0 };
        darken->SetInput(0, bmp);
        darken->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, mtx);
        auto bs = bmp->GetPixelSize();
        dc->SetTransform(D2D1::Matrix3x2F::Scale(size / bs.width, size / bs.height) * D2D1::Matrix3x2F::Translation(r.left, r.top));
        dc->DrawImage(darken.Get(), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    } else {
        dc->DrawBitmap(bmp, r, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    }
}

void Dock::DrawTooltip(const std::wstring& text, float u, float vBase) {
    auto* dc = g_gfx.dc.Get();
    float a = tooltipAlpha.x;
    if (a < 0.01f || text.empty()) return;
    float w = TextWidth(g_gfx.fmtTooltip.Get(), text) + 24 * scale;
    float h = 28 * scale;
    bool vert = IsVertical(g_settings.edge);
    float across = vert ? h : w, depth = vert ? w : h;
    u = clampf(u, across / 2 + 4 * scale, len - across / 2 - 4 * scale);
    float lift = (1 - a) * 6 * scale;
    D2D1_RECT_F r = RectUV(u - across / 2, u + across / 2, vBase - lift, vBase - lift + depth);
    D2D1_ROUNDED_RECT rr = { r, h / 2, h / 2 };
    brush->SetColor(D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.80f * a));
    dc->FillRoundedRectangle(rr, brush.Get());
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.16f * a));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ { r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f }, h / 2, h / 2 }, brush.Get(), 1);
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.96f * a));
    dc->DrawTextW(text.c_str(), (UINT32)text.size(), g_gfx.fmtTooltip.Get(), r, brush.Get());
}

static std::wstring TempStr(float t, bool unit = false) {
    wchar_t b[16];
    if (std::isnan(t)) wcscpy_s(b, unit ? L"--°C" : L"--°");
    else swprintf_s(b, unit ? L"%d°C" : L"%d°", (int)std::lround(t));
    return b;
}

static D2D1_COLOR_F SevColor(int sev, D2D1_COLOR_F normal, float a) {
    if (sev == 2) return D2D1::ColorF(1.0f, 0.34f, 0.30f, a);
    if (sev == 1) return D2D1::ColorF(1.0f, 0.66f, 0.22f, a);
    return normal;
}

void Dock::DrawSensorCell(float v0, float v1, float vis) {
    auto* dc = g_gfx.dc.Get();
    if (sbPeek.x < 0.001f) { v0 += sbOff; v1 += sbOff; }   // overheat hop while the dock is open
    float cu = (cells[HitSensor].u0 + cells[HitSensor].u1) * 0.5f;
    float ink = InkAt(cu);
    auto draw = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F col) {
        if (ink < 0.95f) {
            brush->SetColor(D2D1::ColorF(0, 0, 0, 0.35f * vis * (1 - ink)));
            D2D1_RECT_F rs = { r.left, r.top + scale, r.right, r.bottom + scale };
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, rs, brush.Get());
        }
        brush->SetColor(col);
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, brush.Get());
    };
    using M = SensorClient::Metric;
    // temperature when known, otherwise the load
    auto value = [](float temp, float load) {
        if (!std::isnan(temp)) return TempStr(temp);
        wchar_t b[16];
        if (std::isnan(load)) wcscpy_s(b, L"--"); else swprintf_s(b, L"%d%%", (int)std::lround(load));
        return std::wstring(b);
    };
    const auto& dd = g_sensors.Data();
    bool live = g_sensors.Live();
    std::wstring cs = value(g_sensors.Temp(M::CPU), live ? dd.cpuLoad : NAN);
    std::wstring gs = value(g_sensors.Temp(M::GPU), live && (dd.flags & LD_HAS_GPU_LOAD) ? dd.gpuLoad : NAN);
    auto cc = SevColor(g_sensors.Severity(M::CPU), Ink(cu, vis), vis);
    auto gc = SevColor(g_sensors.Severity(M::GPU), Ink(cu, vis), vis);
    if (!IsVertical(g_settings.edge)) {
        float mid = (v0 + v1) * 0.5f;
        draw(L"CPU " + cs, g_gfx.fmtClockSmall.Get(), RectUV(cells[HitSensor].u0, cells[HitSensor].u1, mid, mid + 17 * scale), cc);
        draw(L"GPU " + gs, g_gfx.fmtClockSmall.Get(), RectUV(cells[HitSensor].u0, cells[HitSensor].u1, mid - 17 * scale, mid), gc);
    } else {
        float h = (cells[HitSensor].u1 - cells[HitSensor].u0) / 4.4f, u = cells[HitSensor].u0 + h * 0.2f;
        auto lab = Ink(cu, vis * 0.62f);
        draw(L"CPU", g_gfx.fmtTiny.Get(), RectUV(u, u + h, v0, v1), lab); u += h;
        draw(cs, g_gfx.fmtClock.Get(), RectUV(u, u + h, v0, v1), cc); u += h;
        draw(L"GPU", g_gfx.fmtTiny.Get(), RectUV(u, u + h, v0, v1), lab); u += h;
        draw(gs, g_gfx.fmtClock.Get(), RectUV(u, u + h, v0, v1), gc);
    }
}

void Dock::DrawSensorPeek() {
    auto* dc = g_gfx.dc.Get();
    using M = SensorClient::Metric;
    // hottest metric relative to its threshold
    int best = M::CPU; float bestR = -1;
    for (int m : { (int)M::CPU, (int)M::GPU, (int)M::SSD }) {
        float t = g_sensors.Temp((M)m);
        if (std::isnan(t)) continue;
        float r = t / g_sensors.Threshold((M)m);
        if (r > bestR) { bestR = r; best = m; }
    }
    const wchar_t* names[] = { L"CPU", L"GPU", L"RAM", L"SSD" };
    bool vert = IsVertical(g_settings.edge);
    float across = vert ? 52 * scale : 112 * scale, depth = vert ? 84 * scale : 40 * scale;
    float cu = (cells[HitSensor].u0 + cells[HitSensor].u1) * 0.5f;
    float vb = lerpf(-depth - 12 * scale, margin + 2 * scale, clampf(sbPeek.x, 0, 1.1f)) + sbOff;
    D2D1_RECT_F r = RectUV(cu - across / 2, cu + across / 2, vb, vb + depth);
    float rad = std::min(r.right - r.left, r.bottom - r.top) * 0.5f;
    brush->SetColor(D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.92f));
    dc->FillRoundedRectangle({ r, rad, rad }, brush.Get());
    brush->SetColor(D2D1::ColorF(1, 0.4f, 0.35f, 0.55f));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ { r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f }, rad, rad }, brush.Get(), 1.2f * scale);
    auto col = SevColor(std::max(1, g_sensors.Severity((M)best)), D2D1::ColorF(1, 1, 1), 1);
    brush->SetColor(col);
    std::wstring t = vert ? std::wstring(names[best]) + L"\n" + TempStr(g_sensors.Temp((M)best))
                          : std::wstring(names[best]) + L" " + TempStr(g_sensors.Temp((M)best));
    g_gfx.fmtClock->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    dc->DrawTextW(t.c_str(), (UINT32)t.size(), g_gfx.fmtClock.Get(), r, brush.Get());
    g_gfx.fmtClock->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

void Dock::DrawPanel() {
    auto* dc = g_gfx.dc.Get();
    using M = SensorClient::Metric;
    float a = clampf(panelAlpha.x, 0, 1), s = scale;
    D2D1_RECT_F r = panelRect;
    float x0 = r.left + 16 * s, x1 = r.right - 16 * s;
    auto white = [&](float k) { return D2D1::ColorF(1, 1, 1, k * a); };
    auto text = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F rr, D2D1_COLOR_F c) {
        brush->SetColor(c);
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, rr, brush.Get());
    };
    float y = r.top + 12 * s;
    bool live = g_sensors.Live();
    text(L"시스템", g_gfx.fmtPanelLabel.Get(), { x0, y, x1, y + 20 * s }, white(0.95f));
    text(live ? L"1초마다 갱신" : L"측정 중…", g_gfx.fmtPanelSmallR.Get(), { x0, y, x1, y + 20 * s }, white(0.5f));
    y += 32 * s;
    if (!live) return;
    const auto& d = g_sensors.Data();
    wchar_t b1[96], b2[96];
    std::wstring gpuName = d.gpuName;
    for (const wchar_t* junk : { L"NVIDIA ", L"GeForce " }) { size_t p; while ((p = gpuName.find(junk)) != std::wstring::npos) gpuName.erase(p, wcslen(junk)); }

    struct Row { const wchar_t* title; std::wstring sub, big, detail; int sev; M m; D2D1_COLOR_F col; };
    Row rows[4];
    // CPU: temperature is opt-in; until then the load is the headline and the row offers to enable it
    if (g_sensors.CpuTempAvailable()) {
        swprintf_s(b1, L"사용 %d%%", (int)std::lround(d.cpuLoad));
        rows[0] = { L"CPU", d.cpuName, TempStr(g_sensors.Temp(M::CPU), true), b1, g_sensors.Severity(M::CPU), M::CPU, D2D1::ColorF(0.38f, 0.74f, 1.0f) };
    } else {
        swprintf_s(b2, L"%d%%", (int)std::lround(d.cpuLoad));
        // the helper reports why it has no temperature (unsupported CPU, PawnIO missing)
        const wchar_t* hint = !g_settings.cpuTemp ? L"온도 보기: 클릭" : (d.status[0] ? d.status : L"온도: 도우미 시작 중…");
        rows[0] = { L"CPU", d.cpuName, b2, hint, 0, M::CPU, D2D1::ColorF(0.38f, 0.74f, 1.0f) };
    }
    if (d.flags & LD_HAS_GPU) swprintf_s(b1, L"사용 %d%% · VRAM %.1f/%.0f GB", (int)std::lround(d.gpuLoad), d.gpuMemUsed, d.gpuMemTotal);
    else swprintf_s(b1, L"사용 %d%%", (int)std::lround(d.gpuLoad));
    rows[1] = { L"GPU", gpuName, (d.flags & LD_HAS_GPU) ? TempStr(g_sensors.Temp(M::GPU), true) : std::wstring(b1 + 3),
                (d.flags & LD_HAS_GPU) ? std::wstring(b1) : std::wstring(L"온도: NVIDIA만 지원"), g_sensors.Severity(M::GPU), M::GPU, D2D1::ColorF(0.45f, 0.88f, 0.52f) };
    swprintf_s(b1, L"%d%%", (int)std::lround(d.ramLoad));
    swprintf_s(b2, L"%.1f / %.0f GB", d.ramUsed, d.ramTotal);
    rows[2] = { L"RAM", L"메모리", b1, b2, 0, M::RAM, D2D1::ColorF(0.82f, 0.60f, 1.0f) };
    swprintf_s(b1, L"활동 %d%% · %.0f/%.0f GB", (int)std::lround(d.ssdActive), d.ssdUsed, d.ssdTotal);
    swprintf_s(b2, L"%d%%", (int)std::lround(d.ssdActive));
    rows[3] = { L"SSD", d.ssdName, (d.flags & LD_HAS_SSD_TEMP) ? TempStr(g_sensors.Temp(M::SSD), true) : std::wstring(b2), b1,
                g_sensors.Severity(M::SSD), M::SSD, D2D1::ColorF(1.0f, 0.76f, 0.36f) };

    for (auto& row : rows) {
        text(row.title, g_gfx.fmtPanelLabel.Get(), { x0, y, x1, y + 18 * s }, white(0.95f));
        text(row.sub, g_gfx.fmtPanelSub.Get(), { x0, y + 18 * s, x1 - 110 * s, y + 33 * s }, white(0.55f));
        text(row.big, g_gfx.fmtPanelValue.Get(), { x0, y - 3 * s, x1, y + 22 * s }, SevColor(row.sev, white(0.97f), a));
        text(row.detail, g_gfx.fmtPanelSmallR.Get(), { x0, y + 21 * s, x1, y + 35 * s }, white(0.6f));

        // usage sparkline (last 60 s)
        const float* h = g_sensors.History(row.m);
        int n = g_sensors.histCount;
        float gy0 = y + 39 * s, gy1 = y + 52 * s;
        brush->SetColor(white(0.07f));
        dc->FillRoundedRectangle({ { x0, gy0, x1, gy1 }, 3 * s, 3 * s }, brush.Get());
        if (n >= 2) {
            ComPtr<ID2D1PathGeometry> line, area;
            g_gfx.d2dFactory->CreatePathGeometry(&line);
            g_gfx.d2dFactory->CreatePathGeometry(&area);
            ComPtr<ID2D1GeometrySink> ls, as;
            line->Open(&ls); area->Open(&as);
            float step = (x1 - x0) / (SensorClient::kHist - 1);
            float xs = x1 - step * (n - 1);
            for (int i = 0; i < n; ++i) {
                int idx = (g_sensors.histHead - n + i + SensorClient::kHist) % SensorClient::kHist;
                D2D1_POINT_2F p = { xs + step * i, gy1 - (gy1 - gy0) * clampf(h[idx] / 100.0f, 0, 1) };
                if (i == 0) {
                    ls->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
                    as->BeginFigure({ p.x, gy1 }, D2D1_FIGURE_BEGIN_FILLED);
                    as->AddLine(p);
                } else { ls->AddLine(p); as->AddLine(p); }
            }
            ls->EndFigure(D2D1_FIGURE_END_OPEN);
            as->AddLine({ x1, gy1 });
            as->EndFigure(D2D1_FIGURE_END_CLOSED);
            ls->Close(); as->Close();
            auto c = row.col; c.a = 0.22f * a;
            brush->SetColor(c);
            dc->FillGeometry(area.Get(), brush.Get());
            c.a = 0.95f * a;
            brush->SetColor(c);
            dc->DrawGeometry(line.Get(), brush.Get(), 1.4f * s);
        }
        y += 60 * s;
    }
}

void Dock::Render() {
    if (!dirty || !swap) return;
    g_gfx.EnsureFonts(scale);
    dirty = false;
    bool peeking = sbPeek.x > 0.001f;
    for (auto& s : slots) if (s.peek.x > 0.001f) peeking = true;
    bool glassVisible = reveal.x > 0.002f;
    if (!glassVisible && !peeking) {
        if (clearedWhileHidden) return;
        float zero[4] = { 0, 0, 0, 0 };
        g_gfx.ctx->ClearRenderTargetView(rtv.Get(), zero);
        if (recording) g_gfx.ctx->ClearRenderTargetView(dockFrame.rtv.Get(), zero);
        swap->Present(0, 0);
        clearedWhileHidden = true;
        return;
    }
    clearedWhileHidden = false;

    float zero[4] = { 0, 0, 0, 0 };
    g_gfx.ctx->ClearRenderTargetView(rtv.Get(), zero);

    const GlassStyle& gs = kStyles[std::clamp(g_settings.glassStyle, 0, 2)];
    D2D1_RECT_F bar = RectUV(barU0, barU1, margin - hideOff, margin + barThick - hideOff);
    if (glassVisible && capture.gotFirstFrame) {
        GlassCB c = {};
        c.misc[0] = (float)winW; c.misc[1] = (float)winH;
        if (backdropDirty) {
            float sigmaHalf = std::max(0.5f, gs.sigma * scale * 0.5f);
            int taps = std::min(32, (int)std::ceil(sigmaHalf * 2.5f));
            g_gfx.UpdateCB(c);
            g_gfx.FullscreenPass(halfA.rtv.Get(), halfA.w, halfA.h, g_gfx.psDown.Get(), backdrop.srv.Get(), nullptr);
            c.blur[0] = 1.0f / halfA.w; c.blur[1] = 0; c.blur[2] = sigmaHalf; c.blur[3] = (float)taps;
            g_gfx.UpdateCB(c);
            g_gfx.FullscreenPass(halfB.rtv.Get(), halfB.w, halfB.h, g_gfx.psBlur.Get(), halfA.srv.Get(), nullptr);
            c.blur[0] = 0; c.blur[1] = 1.0f / halfA.h;
            g_gfx.UpdateCB(c);
            g_gfx.FullscreenPass(halfA.rtv.Get(), halfA.w, halfA.h, g_gfx.psBlur.Get(), halfB.srv.Get(), nullptr);
            backdropDirty = false;
        }
        c.rect[0] = bar.left; c.rect[1] = bar.top; c.rect[2] = bar.right - bar.left; c.rect[3] = bar.bottom - bar.top;
        c.shape[0] = barThick * 0.40f;
        c.shape[1] = gs.bezel * scale;
        c.shape[2] = gs.refr * scale;
        c.shape[3] = clampf(reveal.x * 1.6f, 0, 1);
        c.look[0] = gs.chroma; c.look[1] = gs.tint; c.look[2] = gs.sat; c.look[3] = gs.highlight;
        c.misc[2] = 0.20f; c.misc[3] = 9 * scale;
        c.tint[0] = 1; c.tint[1] = 1; c.tint[2] = 1; c.tint[3] = gs.bright;
        g_gfx.UpdateCB(c);
        g_gfx.FullscreenPass(rtv.Get(), winW, winH, g_gfx.psGlass.Get(), nullptr, halfA.srv.Get());

        // luminance probe (read back one frame late, never stalls)
        g_gfx.FullscreenPass(lumRT.rtv.Get(), 16, 1, g_gfx.psLum.Get(), nullptr, halfA.srv.Get());
        g_gfx.ctx->CopyResource(lumStage[lumFrame & 1].Get(), lumRT.tex.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (lumFrame > 0 && SUCCEEDED(g_gfx.ctx->Map(lumStage[(lumFrame + 1) & 1].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            const uint32_t* px = (const uint32_t*)m.pData;
            for (int i = 0; i < 16; ++i) {
                uint32_t p = px[i];
                float r = ((p >> 16) & 255) / 255.f, g = ((p >> 8) & 255) / 255.f, b = (p & 255) / 255.f;
                lumCol[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            }
            g_gfx.ctx->Unmap(lumStage[(lumFrame + 1) & 1].Get(), 0);
        }
        lumFrame++;

        // system monitor popover: darker liquid glass blended over the dock
        if (panelAlpha.x > 0.01f) {
            GlassCB p = c;
            p.rect[0] = panelRect.left; p.rect[1] = panelRect.top;
            p.rect[2] = panelRect.right - panelRect.left; p.rect[3] = panelRect.bottom - panelRect.top;
            p.shape[0] = 22 * scale; p.shape[1] = 14 * scale; p.shape[2] = 10 * scale;
            p.shape[3] = clampf(panelAlpha.x, 0, 1);
            p.look[0] = 0.05f; p.look[1] = 0.42f; p.look[2] = 1.25f; p.look[3] = 0.28f;
            p.misc[2] = 0.28f; p.misc[3] = 14 * scale;
            p.tint[0] = 0.06f; p.tint[1] = 0.06f; p.tint[2] = 0.08f; p.tint[3] = 0.92f;
            g_gfx.UpdateCB(p);
            g_gfx.FullscreenPass(rtv.Get(), winW, winH, g_gfx.psGlass.Get(), nullptr, halfA.srv.Get(), true);
        }
    }

    auto* dc = g_gfx.dc.Get();
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    float vis = clampf(reveal.x * 1.4f, 0, 1);

    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        if (dragging && (int)i == pressSlot) continue;
        float sz = s.size * clampf(s.appear.x, 0, 1.2f) * clampf(s.shrink.x, 0, 1);
        if (sz < 1) continue;
        float vNormal = margin + pad - hideOff + s.bounceOff;
        float vb = vNormal;
        float alpha = vis;
        if (s.peek.x > 0.001f) {
            float vPeek = lerpf(-s.size - 10 * scale, margin + 2 * scale, clampf(s.peek.x, 0, 1.1f)) + s.bounceOff;
            if (vPeek > vb) { vb = vPeek; alpha = 1; }
        }
        if (alpha < 0.01f) continue;
        DrawIcon(s, s.u, vb, sz, s.ghost ? alpha * clampf(s.appear.x, 0, 1) : alpha);

        // running / attention indicator
        if (s.app && s.app->Running() && vis > 0.01f) {
            float rr = 2.3f * scale;
            auto p = W(s.u, margin + pad * 0.42f - hideOff);
            bool att = s.app->attention;
            float k = InkAt(s.u);
            brush->SetColor(D2D1::ColorF(0, 0, 0, 0.30f * vis * (1 - k)));
            dc->FillEllipse(D2D1::Ellipse(p, rr + 1, rr + 1), brush.Get());
            brush->SetColor(att ? D2D1::ColorF(1.0f, 0.55f, 0.10f, vis) : Ink(s.u, vis));
            dc->FillEllipse(D2D1::Ellipse(p, rr, rr), brush.Get());
        }
    }

    if (vis > 0.01f) {
        float v0 = margin - hideOff, v1 = margin + barThick - hideOff;
        // separator
        float su = sepU + gap * 0.5f;
        auto sr = RectUV(su, su + 1, v0 + pad * 1.2f, v1 - pad * 1.2f);
        auto sc = Ink(su, vis); sc.a *= 0.42f;
        brush->SetColor(sc);
        dc->FillRectangle(sr, brush.Get());

        // hover highlight
        D2D1_RECT_F hr{};
        if (FindSmallButton(hoverKind)) {
            hr = ButtonRect(hoverKind);
            float inset = (stackButtons ? 1.5f : 2.0f) * scale;
            hr.left += inset; hr.right -= inset; hr.top += inset; hr.bottom -= inset;
        } else if (hoverKind == HitTray || hoverKind == HitClock || hoverKind == HitSensor) {
            const Cell& c = cells[hoverKind];
            hr = RectUV(c.u0 + 1, c.u1 - 1, v0 + pad * 0.6f, v1 - pad * 0.6f);
        }
        if (hr.right > hr.left) {
            float ink = InkAt(cells[hoverKind].Mid());
            brush->SetColor(D2D1::ColorF(1 - ink, 1 - ink, 1 - ink, lerpf(0.16f, 0.08f, ink) * vis));
            dc->FillRoundedRectangle({ hr, 8 * scale, 8 * scale }, brush.Get());
        }
        DrawSensorCell(v0, v1, vis);

        // "show hidden icons" chevron points away from the screen edge
        const wchar_t* chevG = g_settings.edge == Edge::Bottom ? L"" : g_settings.edge == Edge::Top ? L""
                             : g_settings.edge == Edge::Left ? L"" : L"";
        const Cell& tray = cells[HitTray];
        DrawShadowText(chevG, g_gfx.fmtGlyph.Get(), RectUV(tray.u0, tray.u1, v0, v1), tray.Mid(), vis * 0.85f);
        const Cell& clk = cells[HitClock];
        if (!IsVertical(g_settings.edge)) {
            auto r = RectUV(clk.u0, clk.u1, v0, v1);
            float timeH = TextSize(ClockFont(), g_sys.timeText).height;
            float dateH = TextSize(ClockFont(true), g_sys.dateText).height;
            float top = (r.top + r.bottom - timeH - dateH) * 0.5f;
            DrawShadowText(g_sys.timeText, ClockFont(), { r.left, top, r.right, top + timeH }, clk.Mid(), vis);
            DrawShadowText(g_sys.dateText, ClockFont(true), { r.left, top + timeH, r.right, top + timeH + dateH }, clk.Mid(), vis);
        } else {
            DrawShadowText(g_sys.timeShort, ClockFont(), RectUV(clk.u0, clk.u1, v0, v1), clk.Mid(), vis);
        }
        for (const SmallButton* b = kSmallButtons; b->kind != HitNone; ++b)
            if (Shown(*b)) (this->*b->draw)(ButtonRect(b->kind), cells[b->kind].Mid(), vis);
    }

    // dragged icon follows the cursor
    if (dragging && pressSlot >= 0 && pressSlot < (int)slots.size()) {
        auto& s = slots[pressSlot];
        float vb = clampf(cursorV - base / 2, -base, T - base);
        DrawIcon(s, cursorU, vb, base, dropRemove ? 0.45f : 0.95f);
    }

    DrawTooltip(tooltipText, tooltipU, tooltipV);
    if (panelAlpha.x > 0.01f && glassVisible) DrawPanel();
    if (sbPeek.x > 0.001f) DrawSensorPeek();

    HRESULT hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (FAILED(hr)) Log(L"EndDraw %08x", hr);
    if (snapshotPending) { snapshotPending = false; SaveSnapshot(); }
    if (recording) {
        ComPtr<ID3D11Texture2D> bb; swap->GetBuffer(0, IID_PPV_ARGS(&bb));
        g_gfx.ctx->CopyResource(dockFrame.tex.Get(), bb.Get());
    }
    swap->Present(0, 0);
}

// Debug: write (dock frame composited over the captured backdrop) to %APPDATA%\LiquidDock\snap_N.png
void Dock::SaveSnapshot() {
    auto readback = [&](ID3D11Texture2D* src, std::vector<uint32_t>& out) {
        D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(g_gfx.d3d->CreateTexture2D(&d, nullptr, &st))) return;
        g_gfx.ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_gfx.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        out.resize(d.Width * d.Height);
        for (UINT y = 0; y < d.Height; ++y) memcpy(&out[y * d.Width], (BYTE*)m.pData + y * m.RowPitch, d.Width * 4);
        g_gfx.ctx->Unmap(st.Get(), 0);
    };
    ComPtr<ID3D11Texture2D> bb; swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    std::vector<uint32_t> fg, bg;
    readback(bb.Get(), fg);
    readback(backdrop.tex.Get(), bg);
    if (fg.size() != (size_t)winW * winH) return;
    if (bg.size() != fg.size()) bg.assign(fg.size(), 0xff808080);
    std::vector<uint32_t> rawBg = bg;
    for (size_t i = 0; i < fg.size(); ++i) {
        uint32_t f = fg[i], b = bg[i], a = f >> 24, out = 0xff000000;
        for (int sh = 0; sh < 24; sh += 8) {
            uint32_t c = ((f >> sh) & 255) + ((b >> sh) & 255) * (255 - a) / 255;
            out |= std::min(c, 255u) << sh;
        }
        fg[i] = out;
    }
    auto save = [&](const std::wstring& path, std::vector<uint32_t>& px) {
        for (auto& p : px) p |= 0xff000000;
        ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
        g_gfx.wic->CreateStream(&stream);
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return;
        g_gfx.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        enc->CreateNewFrame(&frame, nullptr);
        frame->Initialize(nullptr);
        frame->SetSize(winW, winH);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&fmt);
        frame->WritePixels(winH, winW * 4, (UINT)(px.size() * 4), (BYTE*)px.data());
        frame->Commit();
        enc->Commit();
    };
    save(AppDataDir() + L"\\snap_" + std::to_wstring(index) + L".png", fg);
    save(AppDataDir() + L"\\snap_bg_" + std::to_wstring(index) + L".png", rawBg);
    Log(L"snapshot saved; layout (screen) edge=%d win=%d,%d bar=%.0f-%.0f apps-end=%.0f sens=%.0f-%.0f thick=%.0f margin=%.0f",
        (int)g_settings.edge, winX, winY, barU0, barU1, sepU, cells[HitSensor].u0, cells[HitSensor].u1, barThick, margin);
    for (auto& s : slots) if (s.app && s.app->exe.find(L"kakaotalk") != std::wstring::npos) Log(L"kakao u=%.0f", s.u);
}

void Dock::DrawShadowText(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float u, float a) {
    auto* dc = g_gfx.dc.Get();
    float ink = InkAt(u);
    if (ink < 0.95f) {
        brush->SetColor(D2D1::ColorF(0, 0, 0, 0.35f * a * (1 - ink)));
        D2D1_RECT_F rs = { r.left, r.top + scale, r.right, r.bottom + scale };
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, rs, brush.Get());
    }
    brush->SetColor(Ink(u, a));
    dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, brush.Get());
}

// Segoe Fluent glyph centred in a button cell; a little smaller when two buttons share the cell.
void Dock::DrawGlyph(const wchar_t* glyph, D2D1_RECT_F r, float u, float vis) {
    auto* dc = g_gfx.dc.Get();
    float f = ButtonIconScale();
    if (f != 1) dc->SetTransform(D2D1::Matrix3x2F::Scale(f, f, { (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f }));
    DrawShadowText(glyph, g_gfx.fmtGlyph.Get(), r, u, vis);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Dock::DrawNetwork(D2D1_RECT_F r, float u, float vis) {
    DrawGlyph(g_sys.net == 0 ? L"" : (g_sys.net == 2 ? L"" : L""), r, u, vis);
}

void Dock::DrawVolume(D2D1_RECT_F r, float u, float vis) {
    DrawGlyph(g_sys.muted ? L"" : g_sys.volume < 0.01f ? L"" : g_sys.volume < 0.34f ? L""
              : g_sys.volume < 0.67f ? L"" : L"", r, u, vis);
}

void Dock::DrawSettings(D2D1_RECT_F r, float u, float vis) { DrawGlyph(L"", r, u, vis); }

// A desktop outline, kept upright on all four edges.
void Dock::DrawDesktop(D2D1_RECT_F r, float u, float vis) {
    auto* dc = g_gfx.dc.Get();
    D2D1_POINT_2F p = { (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f };
    float s = scale * ButtonIconScale();
    brush->SetColor(Ink(u, vis));
    auto screen = D2D1::RectF(p.x - 8 * s, p.y - 6 * s, p.x + 8 * s, p.y + 4 * s);
    dc->DrawRoundedRectangle({ screen, 1.5f * s, 1.5f * s }, brush.Get(), 1.4f * s);
    dc->DrawLine({ p.x, p.y + 4 * s }, { p.x, p.y + 7 * s }, brush.Get(), 1.4f * s);
    dc->DrawLine({ p.x - 4 * s, p.y + 7 * s }, { p.x + 4 * s, p.y + 7 * s }, brush.Get(), 1.4f * s);
}

// Three bars (red, green, blue: an OLED subpixel) leaning 35 degrees inside one rounded
// outline; grey ink while the mode is off. Kept upright on every edge.
void Dock::DrawOled(D2D1_RECT_F br, float u, float vis) {
    auto* dc = g_gfx.dc.Get();
    D2D1_POINT_2F p = { (br.left + br.right) * 0.5f, (br.top + br.bottom) * 0.5f };
    float s = scale * ButtonIconScale();
    float w = 13 * s, h = 15 * s;
    D2D1_RECT_F r = { p.x - w / 2, p.y - h / 2, p.x + w / 2, p.y + h / 2 };
    ComPtr<ID2D1RoundedRectangleGeometry> shape;
    if (FAILED(g_gfx.d2dFactory->CreateRoundedRectangleGeometry({ r, 2.6f * s, 2.6f * s }, &shape))) return;

    D2D1_COLOR_F c[3];
    if (oled::Active()) {
        c[0] = D2D1::ColorF(0xFF3B30, vis); c[1] = D2D1::ColorF(0x5BC22A, vis); c[2] = D2D1::ColorF(0x4459F0, vis);
    } else {
        const float a[3] = { 0.55f, 0.75f, 0.95f };
        for (int i = 0; i < 3; ++i) { c[i] = Ink(u, vis); c[i].a *= a[i]; }
    }
    // hard colour stops: one gradient brush paints all three bars
    D2D1_GRADIENT_STOP stops[6] = { { 0, c[0] }, { 1 / 3.0f, c[0] }, { 1 / 3.0f, c[1] }, { 2 / 3.0f, c[1] }, { 2 / 3.0f, c[2] }, { 1, c[2] } };
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> fill;
    if (FAILED(dc->CreateGradientStopCollection(stops, 6, &coll)) ||
        FAILED(dc->CreateLinearGradientBrush({ { r.left, p.y }, { r.right, p.y } }, coll.Get(), &fill))) return;
    dc->SetTransform(D2D1::Matrix3x2F::Skew(-35, 0, p));
    dc->FillGeometry(shape.Get(), fill.Get());
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

// A bulb hung upside down: base and cord at the top, the round glass below. Dark mode: outline;
// light mode: filled with rays spreading from the glass. Ink colour only.
void Dock::DrawTheme(D2D1_RECT_F br, float u, float vis) {
    auto* dc = g_gfx.dc.Get();
    D2D1_POINT_2F p = { (br.left + br.right) * 0.5f, (br.top + br.bottom) * 0.5f };
    float k = 0.55f * scale * ButtonIconScale();   // drawing units: the glass is 16 units wide
    auto pt = [&](float x, float y) { return D2D1::Point2F(p.x + x * k, p.y + y * k); };
    bool lit = theme::IsLight();
    auto ink = Ink(u, vis * (lit ? 1.0f : 0.8f));
    float line = 1.8f * k;

    // glass: circle (0, 2) r 8 with a neck 9 units wide up to y = -7
    ComPtr<ID2D1PathGeometry> glass;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(g_gfx.d2dFactory->CreatePathGeometry(&glass)) || FAILED(glass->Open(&sink))) return;
    sink->BeginFigure(pt(-4.5f, -7), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(pt(-4.5f, -4.61f));
    sink->AddArc({ pt(4.5f, -4.61f), { 8 * k, 8 * k }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE });
    sink->AddLine(pt(4.5f, -7));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;

    brush->SetColor(ink);
    dc->DrawLine(pt(0, -10.5f), pt(0, -19), brush.Get(), line);        // cord
    dc->DrawLine(pt(-3.5f, -10.5f), pt(3.5f, -10.5f), brush.Get(), line);   // base
    if (lit) {
        dc->FillGeometry(glass.Get(), brush.Get());
        const float rays[5][4] = { { 0, 15, 0, 19 }, { -9.2f, 11.2f, -12, 14 }, { 9.2f, 11.2f, 12, 14 },
                                   { -12, 2, -16, 2 }, { 12, 2, 16, 2 } };
        for (auto& ray : rays) dc->DrawLine(pt(ray[0], ray[1]), pt(ray[2], ray[3]), brush.Get(), line);
    } else {
        dc->DrawGeometry(glass.Get(), brush.Get(), line);
    }
}
