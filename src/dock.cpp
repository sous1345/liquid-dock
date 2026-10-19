#include "dock.h"
#include "system.h"
#include "sensors.h"
#include "native_tray.h"
#include <shellscalingapi.h>
#include <thread>
#include <shellapi.h>
#include <d2d1effects.h>
#include "oled.h"
#include "theme.h"

extern HWND g_mainWnd;
constexpr UINT WM_APP_REBUILD = WM_APP + 1;
constexpr UINT WM_APP_AUTOSTART = WM_APP + 2;

static const wchar_t* kDockClass = L"LiquidDockBar";

struct GlassStyle { float sigma, tint, sat, refr, bezel, chroma, highlight, bright; };
static const GlassStyle kStyles[3] = {
    { 1.5f, 0.05f, 1.30f, 18, 18, 0.10f, 0.42f, 1.03f },   // clear
    { 6.0f, 0.10f, 1.40f, 16, 18, 0.08f, 0.36f, 1.04f },   // normal
    { 16.f, 0.20f, 1.30f, 12, 18, 0.05f, 0.30f, 1.05f },   // frosted
};

static LRESULT CALLBACK DockProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = (Dock*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        auto cs = (CREATESTRUCTW*)l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(h, m, w, l);
    }
    if (d) return d->WndProc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

Dock::Dock(HMONITOR m, int i, bool p) : monitor(m), index(i), primary(p) {}

Dock::~Dock() {
    windowPreview.Dismiss();
    capture.Stop();
    if (hwnd) DestroyWindow(hwnd);
}

void Dock::Metrics() {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    mon = mi.rcMonitor;
    // Sized relative to the screen, not to Windows' DPI setting: the dock (icons, text, margins)
    // takes the same share of every monitor. 1.0 = a screen whose short side is 1440 px.
    float shortSide = (float)std::min(mon.right - mon.left, mon.bottom - mon.top);
    // floor: on small laptop screens (768 px) pure proportion would make text unreadable
    scale = std::max(0.72f, shortSide / 1440.0f);
    baseNominal = std::round(g_settings.iconSize * scale);
    margin = std::round(6 * scale);
    mag = std::max(1.0f, g_settings.magnification);
    g_gfx.EnsureFonts(scale);
    fit = 1;
    ApplyFit(0);
    bool vert = IsVertical(g_settings.edge);
    len = (float)(vert ? (mon.bottom - mon.top) : (mon.right - mon.left));
    float headroom = std::max(base * (mag - 1), base * 0.65f) + (vert ? 240 * scale : 46 * scale);
    panelW = std::round(300 * scale);
    panelH = std::round(292 * scale);
    float panelNeed = margin + barThick + 14 * scale + (vert ? panelW : panelH) + 28 * scale;
    T = std::ceil(std::max(margin + barThick + headroom + 10 * scale, panelNeed));
    int t = (int)T;
    switch (g_settings.edge) {
    case Edge::Bottom: winX = mon.left; winY = mon.bottom - t; winW = mon.right - mon.left; winH = t; break;
    case Edge::Top:    winX = mon.left; winY = mon.top;        winW = mon.right - mon.left; winH = t; break;
    case Edge::Left:   winX = mon.left; winY = mon.top;        winW = t; winH = mon.bottom - mon.top; break;
    case Edge::Right:  winX = mon.right - t; winY = mon.top;   winW = t; winH = mon.bottom - mon.top; break;
    }
    g_gfx.EnsureFonts(scale);
}

bool Dock::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = DockProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kDockClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    Metrics();
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_ACCEPTFILES,
                           kDockClass, L"Liquid Dock", WS_POPUP, winX, winY, winW, winH,
                           nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    SetWindowRgn(hwnd, CreateRectRgn(0, 0, 0, 0), FALSE);
    curRegion = { 0, 0, 0, 0 };
    if (!CreateDeviceResources()) return false;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    reveal.snap(0);
    magAmount.snap(0);
    trailingGap.snap(0);
    SyncSlots();
    for (auto& s : slots) { s.appear.snap(1); s.shrink.snap(1); }
    return true;
}

bool Dock::CreateDeviceResources() {
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = winW; sd.Height = winH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    HRESULT hr = g_gfx.factory->CreateSwapChainForComposition(g_gfx.d3d.Get(), &sd, nullptr, &swap);
    if (FAILED(hr)) { Log(L"swapchain failed %08x", hr); return false; }
    ComPtr<ID3D11Texture2D> bb;
    swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_gfx.d3d->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    ComPtr<IDXGISurface> surf;
    swap->GetBuffer(0, IID_PPV_ARGS(&surf));
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_gfx.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target);

    g_gfx.dcomp->CreateTargetForHwnd(hwnd, TRUE, &compTarget);
    g_gfx.dcomp->CreateVisual(&visual);
    visual->SetContent(swap.Get());
    compTarget->SetRoot(visual.Get());
    g_gfx.dcomp->Commit();

    backdrop.Create(winW, winH, false);
    halfA.Create((winW + 1) / 2, (winH + 1) / 2, true);
    halfB.Create((winW + 1) / 2, (winH + 1) / 2, true);

    lumRT.Create(16, 1, true);
    for (auto& st : lumStage) {
        D3D11_TEXTURE2D_DESC d; lumRT.tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_gfx.d3d->CreateTexture2D(&d, nullptr, &st);
    }
    for (int i = 0; i < 16; ++i) lumCol[i] = 0.3f;

    g_gfx.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
    g_gfx.dc->CreateEffect(CLSID_D2D1ColorMatrix, &darken);
    return true;
}

// ---------------------------------------------------------------- coordinates
// u runs along the screen edge, v is the distance from the edge into the screen.

D2D1_POINT_2F Dock::W(float u, float v) const {
    switch (g_settings.edge) {
    case Edge::Bottom: return { u, T - v };
    case Edge::Top:    return { u, v };
    case Edge::Left:   return { v, u };
    default:           return { T - v, u };
    }
}

D2D1_RECT_F Dock::RectUV(float u0, float u1, float v0, float v1) const {
    auto a = W(u0, v0), b = W(u1, v1);
    return { std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y) };
}

void Dock::ScreenToUV(POINT p, float& u, float& v) const {
    switch (g_settings.edge) {
    case Edge::Bottom: u = float(p.x - mon.left); v = mon.bottom - p.y - 0.5f; break;
    case Edge::Top:    u = float(p.x - mon.left); v = p.y - mon.top + 0.5f; break;
    case Edge::Left:   u = float(p.y - mon.top);  v = p.x - mon.left + 0.5f; break;
    case Edge::Right:  u = float(p.y - mon.top);  v = mon.right - p.x - 0.5f; break;
    }
}

void Dock::ClientToUV(int x, int y, float& u, float& v) const {
    POINT p = { x + winX, y + winY };
    ScreenToUV(p, u, v);
}

// ---------------------------------------------------------------- model sync

void Dock::SyncSlots() {
    if (lastStructure == g_apps.structureVersion && !slots.empty()) return;
    lastStructure = g_apps.structureVersion;

    std::vector<Slot> old;
    old.swap(slots);
    std::vector<bool> used(old.size(), false);

    // start button always first
    {
        Slot st; bool had = false;
        for (size_t i = 0; i < old.size(); ++i) if (!old[i].app) { st = old[i]; used[i] = true; had = true; break; }
        if (!had) { st.appear.snap(1); st.shrink.snap(1); }
        slots.push_back(st);
    }
    for (auto& a : g_apps.items) {
        bool found = false;
        for (size_t i = 0; i < old.size(); ++i) {
            if (!used[i] && old[i].app == a) {
                slots.push_back(old[i]); used[i] = true; found = true;
                slots.back().ghost = false;
                slots.back().appear.target = 1;
                break;
            }
        }
        if (!found) {
            Slot s; s.app = a;
            s.appear.x = 0; s.appear.target = 1;
            s.shrink.snap(1);
            slots.push_back(s);
        }
    }
    // keep removed items as ghosts that shrink away, next to their old neighbour
    for (size_t i = 0; i < old.size(); ++i) {
        if (used[i] || !old[i].app) continue;
        Slot g = old[i];
        g.ghost = true; g.appear.target = 0;
        size_t pos = 1;
        for (size_t j = i; j-- > 0;) {
            auto it = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) { return s.app && s.app == old[j].app; });
            if (it != slots.end()) { pos = (it - slots.begin()) + 1; break; }
            if (!old[j].app) break;
        }
        slots.insert(slots.begin() + std::min(pos, slots.size()), g);
    }
    pressSlot = -1;
    dirty = true;
}

// ---------------------------------------------------------------- layout

IDWriteTextFormat* Dock::ClockFont(bool date) const {
    if (date) return g_settings.clockSize == 1 ? g_gfx.fmtDateLarge.Get() : g_gfx.fmtDate.Get();
    return g_settings.clockSize == 1 ? g_gfx.fmtTimeLarge.Get() : g_gfx.fmtTime.Get();
}

Dock::LayoutMetrics Dock::MeasureMetrics(float fitFactor) {
    LayoutMetrics m{};
    constexpr float spacing[] = { 0.22f, 0.32f, 0.44f };
    float thick = baseNominal * fitFactor * 1.30f;
    m.icon = std::max(1.0f, std::round(thick * 0.60f));
    m.gap = std::round(m.icon * spacing[std::clamp(g_settings.iconSpacing, 0, 2)]);

    bool vert = IsVertical(g_settings.edge);
    const auto time = TextSize(ClockFont(), vert ? L"88:88" : g_sys.timeText);
    const auto date = TextSize(ClockFont(true), g_sys.dateText);
    float clockDepth = vert ? time.width : time.height + date.height;
    // Keep the clock readable even when many apps require smaller icons. The
    // extra space is padding; clock size does not change the icon-size setting.
    m.padding = std::ceil(std::max((thick - m.icon) * 0.5f,
                                   (clockDepth + 8 * scale - m.icon) * 0.5f));
    m.thickness = m.icon + 2 * m.padding;
    m.separator = 1 + m.gap;
    const float minButton = 24 * scale;
    m.trayButton = m.icon * 0.45f;
    // pair the small buttons while half the bar still makes a comfortable target
    m.stack = m.thickness * 0.5f >= 22 * scale;
    m.network = m.volume = std::max(m.icon * 0.55f, minButton);
    m.connectivity = m.stack ? std::max(m.network, m.volume) : m.network + m.volume;
    m.clock = vert ? std::max(m.icon * 0.8f, time.height + 8 * scale)
                   : std::max(time.width, date.width) + 12 * scale;
    m.sensor = vert ? m.icon * 1.3f
                    : TextWidth(g_gfx.fmtClockSmall.Get(), L"CPU 100°") + 16 * scale;
    m.desktop = g_settings.showDesktopButton ? std::max(m.icon * 0.55f, minButton) : 0;
    m.themeBtn = g_settings.showThemeButton ? std::max(m.icon * 0.55f, minButton) : 0;
    m.oled = g_settings.showOledButton ? std::max(m.icon * 0.55f, minButton) : 0;
    // Always available, with a generous cell independent of crowded app icons.
    m.settings = std::max(m.icon * 0.65f, 32 * scale);
    m.utilities = 0;
    float pending = -1;   // width of a button still waiting for its partner
    for (float w : { m.desktop, m.themeBtn, m.oled, m.settings }) {
        if (w <= 0) continue;
        if (!m.stack) m.utilities += w;
        else if (pending < 0) pending = w;
        else { m.utilities += std::max(pending, w); pending = -1; }
    }
    if (pending >= 0) m.utilities += pending;
    return m;
}

void Dock::ApplyFit(float dt) {
    // Use the same measured tray and spacing as Layout, including the clock
    // and optional desktop button. Fit never depends on the cursor position.
    if (dt > 0 && len > 0) {
        float n = 0;
        for (auto& s : slots) n += clampf(s.appear.target, 0, 1);
        auto required = [&](float factor) {
            const auto m = MeasureMetrics(factor);
            // Upper bound for the cosine expansion budget on equally spaced
            // slots. Reserve it before hover so a full dock cannot jump in size.
            float expansion = m.icon * (mag - 1) *
                std::min(n, 1 + m.icon * 2.9f / (m.icon + m.gap));
            return 2 * m.padding + n * (m.icon + m.gap) + m.TrayWidth() + expansion;
        };
        float target = 1;
        const float avail = len - 24 * scale;
        if (required(1) > avail) {
            // The user prefers further icon reduction to losing items off-screen.
            // Keep a physical-pixel floor; the clock retains its selected font.
            float lo = 0, hi = 1;
            for (int i = 0; i < 16; ++i) {
                float mid = (lo + hi) * 0.5f;
                if (required(mid) <= avail) lo = mid; else hi = mid;
            }
            target = lo;
        }
        float k = std::min(1.0f, dt * 10);
        fit += (target - fit) * k;
        if (std::fabs(target - fit) < 0.001f) fit = target;
    }
    const auto m = MeasureMetrics(fit);
    base = m.icon; pad = m.padding; gap = m.gap; barThick = m.thickness;
    magRange = base * 2.9f;
}

void Dock::Layout(double) {
    float unit = base + gap;
    const auto metrics = MeasureMetrics(fit);
    trayW = metrics.TrayWidth();

    std::vector<float> c0(slots.size());
    float x = pad;
    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        s.weight = clampf(s.appear.x, 0, 1.2f) * clampf(s.shrink.x, 0, 1);
        x += s.gapBefore.x * unit;
        c0[i] = x + base * s.weight * 0.5f;
        x += unit * s.weight;
    }
    x += trailingGap.x * unit;
    float total0 = x + trayW + pad;
    float start0 = std::round(len * 0.5f - total0 * 0.5f);

    // Keep the expansion budget independent of cursor position so that the dock
    // stays centered and its ends do not slide while the pointer moves along it.
    std::vector<float> sz(slots.size(), base);
    float m = magAmount.x;
    if (m > 0.0005f && mag > 1.001f && !c0.empty()) {
        float hu = clampf(hoverU - start0, c0.front(), c0.back());
        auto bell = [](float distance, float radius) {
            return distance < radius ? 0.5f * (1 + std::cos(kPi * distance / radius)) : 0.0f;
        };
        auto weightSum = [&](float center, float radius) {
            float sum = 0;
            for (size_t i = 0; i < slots.size(); ++i)
                sum += slots[i].weight * bell(std::fabs(center - c0[i]), radius);
            return sum;
        };
        float budget = weightSum((c0.front() + c0.back()) * 0.5f, magRange);
        float lo = 0.001f, hi = magRange;
        for (int i = 0; i < 16 && weightSum(hu, hi) < budget; ++i) hi *= 2;
        // Near either end, widen the bell toward the remaining icons rather than
        // translating the whole dock or adding empty padding beside the tray.
        for (int i = 0; i < 14; ++i) {
            float mid = (lo + hi) * 0.5f;
            if (weightSum(hu, mid) < budget) lo = mid; else hi = mid;
        }
        float sum = weightSum(hu, hi);
        float correction = sum > 0 ? std::min(1.0f, budget / sum) : 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            float f = bell(std::fabs(hu - c0[i]), hi) * correction;
            sz[i] = base * (1 + (mag - 1) * f * m);
        }
    }
    float xm = pad, total;
    for (size_t i = 0; i < slots.size(); ++i) xm += slots[i].gapBefore.x * unit + (sz[i] + gap) * slots[i].weight;
    xm += trailingGap.x * unit;
    total = xm + trayW + pad;

    float start = (len - total) * 0.5f;
    if (total < len - 8 * scale) start = clampf(start, 4 * scale, len - 4 * scale - total);

    x = start + pad;
    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        x += s.gapBefore.x * unit;
        s.u0 = x;
        s.size = sz[i];
        s.u = x + sz[i] * s.weight * 0.5f;
        x += (sz[i] + gap) * s.weight;
    }
    x += trailingGap.x * unit;
    sepU = x; x += metrics.separator;
    trayU0 = x; trayU1 = x + metrics.trayButton;
    stackButtons = metrics.stack;
    netU0 = trayU1;
    if (stackButtons) {
        netU1 = netU0 + metrics.connectivity; volU0 = netU0; volU1 = netU1; netHalf = 0; volHalf = 1;
    } else {
        netU1 = netU0 + metrics.network; volU0 = netU1; volU1 = volU0 + metrics.volume; netHalf = volHalf = -1;
    }
    clkU0 = volU1; clkU1 = clkU0 + metrics.clock;
    sensU0 = clkU1; sensU1 = sensU0 + metrics.sensor;
    // utility buttons: in order, two to a cell when stacking (an odd one keeps a whole cell)
    struct Util { float w; float* u0; float* u1; int* half; } utils[] = {
        { metrics.desktop, &desktopU0, &desktopU1, &desktopHalf }, { metrics.themeBtn, &themeU0, &themeU1, &themeHalf },
        { metrics.oled, &oledU0, &oledU1, &oledHalf }, { metrics.settings, &settingsU0, &settingsU1, &settingsHalf } };
    x = sensU1;
    Util* waiting = nullptr;
    for (auto& b : utils) {
        *b.half = -1;
        if (b.w <= 0) { *b.u0 = *b.u1 = x; continue; }
        if (!stackButtons) { *b.u0 = x; *b.u1 = x += b.w; continue; }
        if (!waiting) { waiting = &b; *b.u0 = x; *b.u1 = x + b.w; continue; }
        float cell = std::max(waiting->w, b.w);
        *waiting->u1 = x + cell; *waiting->half = 0;
        *b.u0 = x; *b.u1 = x + cell; *b.half = 1;
        x += cell; waiting = nullptr;
    }
    if (waiting) x = *waiting->u1;
    barU0 = start; barU1 = x + pad;
}

POINT Dock::ScreenPointUV(float u, float v) const {
    auto p = W(u, v);
    return { winX + (LONG)p.x, winY + (LONG)p.y };
}

RECT Dock::BarScreenRect() const {
    auto r = RectUV(barU0, barU1, margin - hideOff, margin + barThick - hideOff);
    return { winX + (LONG)r.left, winY + (LONG)r.top, winX + (LONG)r.right, winY + (LONG)r.bottom };
}

void Dock::ComputePanelRect() {
    bool vert = IsVertical(g_settings.edge);
    float across = vert ? panelH : panelW, depth = vert ? panelW : panelH;
    float u = (sensU0 + sensU1) * 0.5f;
    u = clampf(u, across / 2 + 8 * scale, len - across / 2 - 8 * scale);
    float v0 = margin + barThick + 12 * scale - hideOff - (1 - panelAlpha.x) * 10 * scale;
    panelRect = RectUV(u - across / 2, u + across / 2, v0, v0 + depth);
}

bool Dock::CursorInPanel() const {
    if (panelAlpha.x < 0.05f && panelAlpha.target < 0.5f) return false;
    auto p = W(cursorU, cursorV);
    return p.x >= panelRect.left - 4 && p.x <= panelRect.right + 4 && p.y >= panelRect.top - 4 && p.y <= panelRect.bottom + 4;
}

D2D1_SIZE_F Dock::TextSize(IDWriteTextFormat* f, const std::wstring& s) {
    if (!f || s.empty()) return {};
    static std::unordered_map<std::wstring, D2D1_SIZE_F> cache;
    std::wstring key = std::to_wstring((uintptr_t)f) + L"|" + s;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    ComPtr<IDWriteTextLayout> tl;
    if (FAILED(g_gfx.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 200, &tl))) return {};
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(tl->GetMetrics(&tm))) return {};
    if (cache.size() > 512) cache.clear();
    D2D1_SIZE_F size = { tm.widthIncludingTrailingWhitespace, tm.height };
    cache[key] = size;
    return size;
}

float Dock::TextWidth(IDWriteTextFormat* f, const std::wstring& s) {
    return TextSize(f, s).width;
}

D2D1_RECT_F Dock::ButtonRect(HitKind kind, bool hit) const {
    float u0 = 0, u1 = 0; int half = -1;
    switch (kind) {
    case HitNet:      u0 = netU0; u1 = netU1; half = netHalf; break;
    case HitVol:      u0 = volU0; u1 = volU1; half = volHalf; break;
    case HitDesktop:  u0 = desktopU0; u1 = desktopU1; half = desktopHalf; break;
    case HitTheme:    u0 = themeU0; u1 = themeU1; half = themeHalf; break;
    case HitOled:     u0 = oledU0; u1 = oledU1; half = oledHalf; break;
    case HitSettings: u0 = settingsU0; u1 = settingsU1; half = settingsHalf; break;
    default: return {};
    }
    if (u1 <= u0) return {};
    if (hit && u1 >= settingsU1 - 0.5f) u1 = std::max(u1, barU1);   // the last cell also takes clicks on the end padding
    auto r = RectUV(u0, u1, margin - hideOff, margin + barThick - hideOff);
    if (half >= 0) {
        // Split in screen coordinates: the first button is left (side docks) or on top
        // (top and bottom docks) on every edge.
        if (IsVertical(g_settings.edge)) { float mid = (r.left + r.right) * 0.5f; (half ? r.left : r.right) = mid; }
        else { float mid = (r.top + r.bottom) * 0.5f; (half ? r.top : r.bottom) = mid; }
    }
    return r;
}

Dock::Hit Dock::HitTestUV(float u, float v) {
    Hit h;
    if (panelAlpha.x > 0.3f) {
        auto p = W(u, v);
        if (p.x >= panelRect.left && p.x <= panelRect.right && p.y >= panelRect.top && p.y <= panelRect.bottom) { h.kind = HitPanel; return h; }
    }
    float vb0 = margin - hideOff, vb1 = margin + barThick - hideOff;
    if (u < barU0 - 2 || u > barU1 + 2) return h;
    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        if (s.ghost || s.weight < 0.3f) continue;
        float top = std::max(vb1, margin + pad + s.size - hideOff);
        if (u >= s.u0 - gap * 0.5f && u <= s.u0 + s.size * s.weight + gap * 0.5f && v >= vb0 - 4 && v <= top) {
            h.kind = HitSlot; h.slot = (int)i; return h;
        }
    }
    if (v < vb0 - 4 || v > vb1) return h;
    const auto point = W(u, v);
    for (HitKind kind : { HitNet, HitVol, HitDesktop, HitTheme, HitOled, HitSettings }) {
        if ((kind == HitDesktop && !g_settings.showDesktopButton) || (kind == HitTheme && !g_settings.showThemeButton) ||
            (kind == HitOled && !g_settings.showOledButton)) continue;
        auto r = ButtonRect(kind, true);
        if (point.x >= r.left && point.x < r.right && point.y >= r.top && point.y < r.bottom) {
            h.kind = kind; return h;
        }
    }
    if (u >= trayU0 && u < trayU1) h.kind = HitTray;
    else if (u >= clkU0 && u < clkU1) h.kind = HitClock;
    else if (u >= sensU0 && u < sensU1) h.kind = HitSensor;
    else h.kind = HitBar;
    return h;
}

bool Dock::FullscreenAppActive() const {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == hwnd) return false;
    wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return false;
    if (MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != monitor) return false;
    RECT r; GetWindowRect(fg, &r);
    if (r.left > mon.left || r.top > mon.top || r.right < mon.right || r.bottom < mon.bottom) return false;
    if (IsZoomed(fg)) return false;
    LONG_PTR st = GetWindowLongPtrW(fg, GWL_STYLE);
    return (st & WS_CAPTION) != WS_CAPTION;
}

void Dock::StartBounce(Slot& s, int count, bool loop, bool hiddenMode, double now) {
    if (s.bounceCount > 0 || s.bounceLoop) {
        // already bouncing: extend
        if (loop) s.bounceLoop = true;
        else {
            double phase = (now - s.bounceStart) / 0.56;
            s.bounceCount = std::max(s.bounceCount, (int)std::ceil(phase) + count - 1);
        }
        s.bounceHiddenMode = s.bounceHiddenMode || hiddenMode;
        return;
    }
    s.bounceStart = now;
    s.bounceCount = count;
    s.bounceLoop = loop;
    s.bounceHiddenMode = hiddenMode;
}

// ---------------------------------------------------------------- per-frame update

void Dock::Update(double now, float dt, POINT cursor) {
    g_gfx.EnsureFonts(scale);   // text formats for this monitor's scale
    SyncSlots();
    {
        float before = fit;
        ApplyFit(dt);
        if (std::fabs(before - fit) > 0.0005f) dirty = true;
    }
    bool wasVisible = reveal.x > 0.002f;
    float prevU = cursorU, prevV = cursorV;
    ScreenToUV(cursor, cursorU, cursorV);
    bool inMonitor = PtInRect(&mon, cursor) != 0;
    if (!inMonitor) { cursorV = 1e9f; }
    if (g_sys.version != lastSysVersion) { lastSysVersion = g_sys.version; dirty = true; }
    if (g_sensors.version != lastSensorVersion) { lastSensorVersion = g_sensors.version; dirty = true; }

    // overheat: the sensor cell bounces once (3 hops) on the primary monitor
    if (primary && g_sensors.alertPending && sbCount == 0) {
        sbStart = now; sbCount = 3; sbHidden = reveal.target < 0.5f;
        lastCaptureWanted = now;
    }

    // notifications bounce on the primary monitor only
    if (primary) {
        for (auto& ev : g_apps.attentionEvents) {
            for (auto& s : slots) {
                if (s.app && s.app->id == ev.id && !s.ghost) {
                    if (!FullscreenAppActive()) {
                        StartBounce(s, 3, false, reveal.target < 0.5f, now);
                        if (s.bounceHiddenMode) lastCaptureWanted = now;
                    }
                }
            }
        }
    }
    // launch feedback bounces until the app shows a window
    for (auto& s : slots) {
        if (!s.app) continue;
        bool launching = s.app->launchStarted > 0 && s.app->launchDock == index;
        if (launching && !s.bounceLoop) StartBounce(s, 0, true, false, now);
        if (!launching && s.bounceLoop) {
            double phase = (now - s.bounceStart) / 0.56;
            s.bounceLoop = false;
            s.bounceCount = std::max(1, (int)std::ceil(phase));
        }
    }

    // ---- show / hide
    bool atEdge = inMonitor && cursorV < 2.0f;
    float magExtent = base * (mag - 1) * magAmount.x;
    bool insideDock = inMonitor && reveal.target > 0.5f &&
        ((cursorU >= barU0 - 14 * scale && cursorU <= barU1 + 14 * scale &&
          cursorV <= margin + barThick + std::max(magExtent, 0.0f) + 12 * scale) ||
         CursorInPanel() ||
         (panelAlpha.target > 0.5f && cursorU >= sensU0 - 4 * scale && cursorU <= sensU1 + 4 * scale &&
          cursorV <= margin + barThick + 16 * scale));   // bridge between cell and panel
    bool keep = menuOpen || dragging || pressSlot >= 0 || debugForceShow;
    bool fullscreen = FullscreenAppActive();
    bool inPreview = windowPreview.Contains(cursor);
    if (debugForceShow) reveal.target = 1;

    if (reveal.target < 0.5f) {
        if (atEdge) {
            if (edgeSince < 0) edgeSince = now;
            lastCaptureWanted = now;        // pre-warm capture
            if (now - edgeSince >= g_settings.showDelay && !fullscreen) {
                reveal.target = 1;
                outsideSince = -1;
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        } else edgeSince = -1;
        if (inMonitor && cursorV < 60 * scale) lastCaptureWanted = now;
    } else {
        // The edge that revealed the dock also keeps it open, including corners.
        if (keep || (!fullscreen && (insideDock || atEdge || inPreview))) outsideSince = -1;
        else {
            if (outsideSince < 0) outsideSince = now;
            if (now - outsideSince >= g_settings.hideDelay) { reveal.target = 0; edgeSince = -1; }
        }
    }

    // ---- magnification
    float appEnd = sepU;
    bool overApps = insideDock && cursorU >= barU0 && cursorU <= appEnd;
    bool overBar = overApps && cursorV <= margin + barThick + 2 * scale;
    bool overMag = overApps && cursorV <= margin + pad + base * (1 + (mag - 1) * magAmount.x) + 6 * scale;
    // Once expanded, hold the layout while crossing into the fixed controls.
    // Otherwise collapsing the apps moves the clock/desktop button away from
    // the pointer in the middle of an attempted click.
    bool overControls = insideDock && cursorU >= appEnd && cursorU <= barU1 &&
                        cursorV <= margin + barThick + 2 * scale;
    if (mag > 1.01f && reveal.x > 0.55f && !dragging && !menuOpen &&
        (overBar || (magAmount.target > 0.5f && (overMag || overControls)))) {
        magAmount.target = 1;
        if (overApps) hoverU = cursorU;
    } else if (!(inPreview && !fullscreen && !dragging && !menuOpen)) magAmount.target = 0;

    // ---- drag
    if (pressSlot >= 0 && !dragging && pressSlot < (int)slots.size() && slots[pressSlot].app &&
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        float dx = float(cursor.x - pressPt.x), dy = float(cursor.y - pressPt.y);
        if (dx * dx + dy * dy > 36 * scale * scale) {
            dragging = true;
            slots[pressSlot].shrink.target = 0;
            slots[pressSlot].press.target = 0;
            magAmount.target = 0;
        }
    }
    if (dragging && pressSlot >= 0 && pressSlot < (int)slots.size()) {
        auto& ds = slots[pressSlot];
        dropRemove = ds.app->pinned && cursorV > T + 20 * scale;
        int k = 0, count = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            if ((int)i == pressSlot || slots[i].ghost || !slots[i].app) continue;
            if (slots[i].u < cursorU) k++;
            count++;
        }
        dropIndex = k;
        int j = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            if ((int)i == pressSlot || slots[i].ghost || !slots[i].app) { slots[i].gapBefore.target = 0; continue; }
            slots[i].gapBefore.target = (!dropRemove && j == k) ? 1.0f : 0.0f;
            j++;
        }
        trailingGap.target = (!dropRemove && k == count) ? 1.0f : 0.0f;
    }

    // ---- springs
    reveal.step(dt, 230, reveal.target > 0.5f ? 0.72f : 1.0f);
    magAmount.step(dt, 380, 1.0f);
    trailingGap.step(dt, 320, 0.9f);
    bool anim = !reveal.settled() || !magAmount.settled() || !trailingGap.settled() || dragging;
    for (auto& s : slots) {
        s.appear.step(dt, 200, 0.72f);
        s.gapBefore.step(dt, 320, 0.9f);
        s.shrink.step(dt, 320, 0.9f);
        s.press.step(dt, 700, 1.0f);
        s.peek.step(dt, 170, 0.8f);
        // bounce: ballistic hops, 0.56 s each
        if (s.bounceCount > 0 || s.bounceLoop) {
            double phase = (now - s.bounceStart) / 0.56;
            if (!s.bounceLoop && phase >= s.bounceCount) {
                s.bounceCount = 0; s.bounceOff = 0; s.peek.target = 0; s.bounceHiddenMode = false;
            } else {
                float fr = float(phase - std::floor(phase));
                float hgt = base * (s.bounceLoop ? 0.5f : 0.9f);   // launch: small hops, notification: big ones
                s.bounceOff = hgt * 4 * fr * (1 - fr);
                s.peek.target = (s.bounceHiddenMode && reveal.target < 0.5f) ? 1.0f : 0.0f;
            }
            anim = true;
        }
        if (!s.appear.settled() || !s.gapBefore.settled() || !s.shrink.settled() || !s.press.settled() || !s.peek.settled()) anim = true;
    }
    // sensor cell bounce
    if (sbCount > 0) {
        double phase = (now - sbStart) / 0.56;
        if (phase >= sbCount) { sbCount = 0; sbOff = 0; sbPeek.target = 0; sbHidden = false; }
        else {
            float fr = float(phase - std::floor(phase));
            sbOff = base * 0.9f * 4 * fr * (1 - fr);
            sbPeek.target = (sbHidden && reveal.target < 0.5f) ? 1.0f : 0.0f;
        }
        anim = true;
    }
    sbPeek.step(dt, 170, 0.8f);
    if (!sbPeek.settled()) anim = true;
    // drop finished ghosts
    for (size_t i = slots.size(); i-- > 0;) {
        if (slots[i].ghost && slots[i].appear.x < 0.01f && slots[i].appear.target == 0) {
            slots.erase(slots.begin() + i);
            if (pressSlot == (int)i) pressSlot = -1; else if (pressSlot > (int)i) pressSlot--;
        }
    }

    // adaptive ink with hysteresis, eased
    for (int i = 0; i < 16; ++i) {
        if (lumCol[i] > 0.52f) inkTarget[i] = 1; else if (lumCol[i] < 0.42f) inkTarget[i] = 0;
        float k = std::min(1.0f, dt * 7);
        float nv = inkCol[i] + (inkTarget[i] - inkCol[i]) * k;
        if (std::fabs(nv - inkCol[i]) > 0.002f) anim = true;
        inkCol[i] = nv;
    }

    hideOff = (1 - reveal.x) * (margin + barThick + 34 * scale);
    Layout(now);
    ComputePanelRect();

    // ---- hover + tooltip
    hoverSlot = -1; hoverKind = HitNone;
    if (reveal.x > 0.9f && inMonitor && !dragging) {
        Hit h = HitTestUV(cursorU, cursorV);
        hoverKind = h.kind; hoverSlot = h.slot;
    }

    // DWM previews live in a separate, non-activating owned popup. Only the
    // hovered app is passed in; app discovery and activation remain in AppModel.
    std::shared_ptr<AppItem> previewApp;
    RECT previewAnchor{};
    if (hoverKind == HitSlot && hoverSlot >= 0 && !slots[hoverSlot].ghost) {
        auto& s = slots[hoverSlot];
        previewApp = s.app;
        float half = std::max(s.size * s.weight * 0.5f, 8 * scale);
        auto r = RectUV(s.u - half, s.u + half, 0,
            margin + pad + base * mag + 12 * scale);
        previewAnchor = {winX + (LONG)std::floor(r.left), winY + (LONG)std::floor(r.top),
            winX + (LONG)std::ceil(r.right), winY + (LONG)std::ceil(r.bottom)};
    }
    windowPreview.Update(hwnd, previewApp, previewAnchor, mon, g_settings.edge, scale, cursor, now,
        reveal.x > 0.95f && reveal.target > 0.5f && !fullscreen && !menuOpen && !dragging && pressKind == HitNone);

    // ---- system monitor panel: opens on hovering the sensor cell, stays while over cell/panel
    bool wantPanel = (debugForcePanel && reveal.target > 0.5f) || reveal.target > 0.5f && !menuOpen && !dragging &&
        (hoverKind == HitSensor || hoverKind == HitPanel || (panelAlpha.target > 0.5f && insideDock &&
         cursorU >= sensU0 - 4 * scale && cursorU <= sensU1 + 4 * scale));
    if (wantPanel) { panelAlpha.target = 1; panelOutsideSince = -1; }
    else if (panelAlpha.target > 0.5f) {
        if (panelOutsideSince < 0) panelOutsideSince = now;
        if (now - panelOutsideSince > 0.25) panelAlpha.target = 0;
    }
    panelAlpha.step(dt, 320, 0.85f);
    if (!panelAlpha.settled()) anim = true;
    std::wstring tip;
    if (hoverKind == HitSlot && hoverSlot >= 0) {
        auto& s = slots[hoverSlot];
        tip = s.app ? s.app->name : L"시작";
        tooltipU = s.u;
        tooltipV = margin + pad + s.size * s.weight + s.bounceOff + 10 * scale - hideOff;
    } else if (hoverKind == HitVol) {
        wchar_t b[64]; swprintf_s(b, g_sys.muted ? L"음소거" : L"볼륨 %d%%", (int)std::round(g_sys.volume * 100)); tip = b;
        tooltipU = (volU0 + volU1) * 0.5f; tooltipV = margin + barThick + 8 * scale - hideOff;
    } else if (hoverKind == HitTray) {
        tip = L"숨겨진 아이콘 표시";
        tooltipU = (trayU0 + trayU1) * 0.5f; tooltipV = margin + barThick + 8 * scale - hideOff;
    } else if (hoverKind == HitNet) {
        tip = g_sys.net == 0 ? L"인터넷 연결 없음" : (g_sys.net == 2 ? L"Wi-Fi 연결됨" : L"이더넷 연결됨");
        tooltipU = (netU0 + netU1) * 0.5f; tooltipV = margin + barThick + 8 * scale - hideOff;
    } else if (hoverKind == HitClock) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t d[128] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, d, 128, nullptr);
        tip = d;
        tooltipU = (clkU0 + clkU1) * 0.5f; tooltipV = margin + barThick + 8 * scale - hideOff;
    } else if (hoverKind == HitDesktop) {
        tip = L"바탕화면 보기 / 창 복원";
        tooltipU = (desktopU0 + desktopU1) * 0.5f; tooltipV = margin + barThick + 8 * scale - hideOff;
    } else if (hoverKind == HitTheme) {
        tip = theme::IsLight() ? L"다크 모드로 전환" : L"라이트 모드로 전환";
        tooltipU = (themeU0 + themeU1) * 0.5f; tooltipV = margin + barThick + 8 * scale - hideOff;
    } else if (hoverKind == HitOled) {
        tip = oled::Active() ? L"OLED 모드 끄기" : L"OLED 모드 켜기 (검은 배경 · 아이콘 숨김)";
        tooltipU = (oledU0 + oledU1) * 0.5f; tooltipV = margin + barThick + 8 * scale - hideOff;
    } else if (hoverKind == HitSettings) {
        tip = L"설정";
        tooltipU = (settingsU0 + settingsU1) * 0.5f; tooltipV = margin + barThick + 8 * scale - hideOff;
    }
    if (windowPreview.Open()) { tip.clear(); tooltipAlpha.snap(0); }
    if (!tip.empty()) { tooltipText = tip; tooltipAlpha.target = 1; }
    else tooltipAlpha.target = 0;
    tooltipAlpha.step(dt, 500, 1.0f);
    if (!tooltipAlpha.settled()) anim = true;

    // ---- capture lifetime
    bool visibleNow = reveal.x > 0.002f || reveal.target > 0.5f;
    if (visibleNow || recording) lastCaptureWanted = now;
    if (now - lastCaptureWanted < 0.05) {
        // a failing start (RDP, protected display...) is retried every few seconds, not every frame
        if (!capture.Active() && now >= nextCaptureTry && !capture.Start(monitor)) nextCaptureTry = now + 3.0;
    }
    else if (capture.Active() && now - lastCaptureWanted > 6.0) capture.Stop();
    if (capture.Active()) {
        if (capture.Poll(backdrop.tex.Get(), winX - mon.left, winY - mon.top, winW, winH, recording ? fullTex.tex.Get() : nullptr)) {
            backdropDirty = true;
            if (visibleNow) dirty = true;
        }
    }

    bool peeking = sbPeek.x > 0.001f || sbPeek.target > 0;
    for (auto& s : slots) if (s.peek.x > 0.001f || s.peek.target > 0) peeking = true;
    if (anim) dirty = true;
    if (visibleNow && (prevU != cursorU || prevV != cursorV)) dirty = true;
    if (wasVisible != (reveal.x > 0.002f)) dirty = true;
    busy = anim || visibleNow || peeking || recording;
    UpdateRegion();
}

void Dock::UpdateRegion() {
    RECT r = { 0, 0, 0, 0 };
    auto add = [&](D2D1_RECT_F f) {
        RECT q = { (LONG)std::floor(f.left), (LONG)std::floor(f.top), (LONG)std::ceil(f.right), (LONG)std::ceil(f.bottom) };
        if (IsRectEmpty(&r)) r = q; else UnionRect(&r, &r, &q);
    };
    if (reveal.x > 0.002f || reveal.target > 0.5f) {
        float q = 32 * scale;
        float u0 = std::floor((barU0 - 14 * scale) / q) * q, u1 = std::ceil((barU1 + 14 * scale) / q) * q;
        float vTop = margin + barThick + 14 * scale;
        if (magAmount.x > 0.01f || magAmount.target > 0.5f) vTop = margin + pad + base * mag + 10 * scale;
        for (auto& s : slots) if (s.bounceOff > 0 || s.bounceCount > 0 || s.bounceLoop) vTop = std::max(vTop, margin + pad + s.size + base * 0.95f + 4 * scale);
        if (sbCount > 0) vTop = std::max(vTop, margin + barThick + base * 0.95f);
        vTop = std::ceil(vTop / q) * q;
        add(RectUV(u0, u1, 0, std::min(vTop, T)));
        if (tooltipAlpha.x > 0.01f || tooltipAlpha.target > 0.5f) {
            float w = TextWidth(g_gfx.fmtTooltip.Get(), tooltipText) + 28 * scale;
            float ext = IsVertical(g_settings.edge) ? w : 34 * scale;
            float across = IsVertical(g_settings.edge) ? 34 * scale : w;
            add(RectUV(tooltipU - across / 2 - 4, tooltipU + across / 2 + 4, tooltipV - 4, std::min(tooltipV + ext + 4, T)));
        }
    }
    for (auto& s : slots) {
        if (s.peek.x > 0.001f || s.peek.target > 0) {
            float sz = base * 1.0f;
            add(RectUV(s.u - sz / 2 - 6 * scale, s.u + sz / 2 + 6 * scale, 0, margin + pad + sz + base * 0.95f + 8 * scale));
        }
    }
    if (panelAlpha.x > 0.01f || panelAlpha.target > 0.5f) {
        D2D1_RECT_F pr = panelRect;
        pr.left -= 20 * scale; pr.top -= 20 * scale; pr.right += 20 * scale; pr.bottom += 24 * scale;
        add(pr);
    }
    if (sbPeek.x > 0.001f || sbPeek.target > 0) {
        bool vert = IsVertical(g_settings.edge);
        float c = (sensU0 + sensU1) * 0.5f, a = vert ? 52 * scale : 112 * scale, depth = vert ? 84 * scale : 40 * scale;
        add(RectUV(c - a / 2 - 6 * scale, c + a / 2 + 6 * scale, 0, margin + depth + base * 0.95f + 10 * scale));
    }
    RECT win = { 0, 0, winW, winH };
    IntersectRect(&r, &r, &win);
    if (!EqualRect(&r, &curRegion)) {
        curRegion = r;
        HRGN rg = CreateRectRgn(r.left, r.top, r.right, r.bottom);
        if (!SetWindowRgn(hwnd, rg, FALSE)) DeleteObject(rg);   // the window owns it on success
    }
}

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
    float cu = (sensU0 + sensU1) * 0.5f;
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
        draw(L"CPU " + cs, g_gfx.fmtClockSmall.Get(), RectUV(sensU0, sensU1, mid, mid + 17 * scale), cc);
        draw(L"GPU " + gs, g_gfx.fmtClockSmall.Get(), RectUV(sensU0, sensU1, mid - 17 * scale, mid), gc);
    } else {
        float h = (sensU1 - sensU0) / 4.4f, u = sensU0 + h * 0.2f;
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
    float cu = (sensU0 + sensU1) * 0.5f;
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

        auto hover = [&](HitKind k, float a0, float a1) {
            if (hoverKind != k) return;
            auto r = RectUV(a0 + 1, a1 - 1, v0 + pad * 0.6f, v1 - pad * 0.6f);
            if (k == HitNet || k == HitVol || k == HitDesktop || k == HitTheme || k == HitOled || k == HitSettings) {
                r = ButtonRect(k);
                float inset = (stackButtons ? 1.5f : 2.0f) * scale;
                r.left += inset; r.right -= inset; r.top += inset; r.bottom -= inset;
            }
            float ink = InkAt((a0 + a1) * 0.5f);
            brush->SetColor(D2D1::ColorF(1 - ink, 1 - ink, 1 - ink, lerpf(0.16f, 0.08f, ink) * vis));
            dc->FillRoundedRectangle({ r, 8 * scale, 8 * scale }, brush.Get());
        };
        hover(HitTray, trayU0, trayU1); hover(HitNet, netU0, netU1); hover(HitVol, volU0, volU1); hover(HitClock, clkU0, clkU1); hover(HitSensor, sensU0, sensU1);
        if (g_settings.showDesktopButton) hover(HitDesktop, desktopU0, desktopU1);
        if (g_settings.showThemeButton) hover(HitTheme, themeU0, themeU1);
        if (g_settings.showOledButton) hover(HitOled, oledU0, oledU1);
        hover(HitSettings, settingsU0, settingsU1);
        DrawSensorCell(v0, v1, vis);

        auto shadowText = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float u, float a) {
            float ink = InkAt(u);
            if (ink < 0.95f) {
                brush->SetColor(D2D1::ColorF(0, 0, 0, 0.35f * a * (1 - ink)));
                D2D1_RECT_F rs = { r.left, r.top + scale, r.right, r.bottom + scale };
                dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, rs, brush.Get());
            }
            brush->SetColor(Ink(u, a));
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, brush.Get());
        };
        const wchar_t* netG = g_sys.net == 0 ? L"" : (g_sys.net == 2 ? L"" : L"");
        const wchar_t* volG = g_sys.muted ? L"" : g_sys.volume < 0.01f ? L"" : g_sys.volume < 0.34f ? L"" : g_sys.volume < 0.67f ? L"" : L"";
        // "show hidden icons" chevron points away from the screen edge
        const wchar_t* chevG = g_settings.edge == Edge::Bottom ? L"" : g_settings.edge == Edge::Top ? L""
                             : g_settings.edge == Edge::Left ? L"" : L"";
        shadowText(chevG, g_gfx.fmtGlyph.Get(), RectUV(trayU0, trayU1, v0, v1), (trayU0 + trayU1) / 2, vis * 0.85f);
        // glyph buttons shrink a little when two share a cell
        auto glyph = [&](const wchar_t* t, HitKind k, float u) {
            auto r = ButtonRect(k);
            float f = ButtonIconScale();
            if (f != 1) dc->SetTransform(D2D1::Matrix3x2F::Scale(f, f, { (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f }));
            shadowText(t, g_gfx.fmtGlyph.Get(), r, u, vis);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        };
        glyph(netG, HitNet, (netU0 + netU1) / 2);
        glyph(volG, HitVol, (volU0 + volU1) / 2);
        glyph(L"\uE713", HitSettings, (settingsU0 + settingsU1) / 2);
        float cu = (clkU0 + clkU1) / 2;
        if (!IsVertical(g_settings.edge)) {
            auto r = RectUV(clkU0, clkU1, v0, v1);
            float timeH = TextSize(ClockFont(), g_sys.timeText).height;
            float dateH = TextSize(ClockFont(true), g_sys.dateText).height;
            float top = (r.top + r.bottom - timeH - dateH) * 0.5f;
            shadowText(g_sys.timeText, ClockFont(), { r.left, top, r.right, top + timeH }, cu, vis);
            shadowText(g_sys.dateText, ClockFont(true), { r.left, top + timeH, r.right, top + timeH + dateH }, cu, vis);
        } else {
            shadowText(g_sys.timeShort, ClockFont(), RectUV(clkU0, clkU1, v0, v1), cu, vis);
        }
        if (g_settings.showDesktopButton) {
            // A desktop outline, kept upright on all four edges. Drawing and
            // hit testing use the same cell; the entire cell is clickable.
            float du = (desktopU0 + desktopU1) * 0.5f;
            auto br = ButtonRect(HitDesktop);
            D2D1_POINT_2F p = { (br.left + br.right) * 0.5f, (br.top + br.bottom) * 0.5f };
            float scale = this->scale * ButtonIconScale();
            brush->SetColor(Ink(du, vis));
            auto screen = D2D1::RectF(p.x - 8 * scale, p.y - 6 * scale,
                                      p.x + 8 * scale, p.y + 4 * scale);
            dc->DrawRoundedRectangle({ screen, 1.5f * scale, 1.5f * scale }, brush.Get(), 1.4f * scale);
            dc->DrawLine({ p.x, p.y + 4 * scale }, { p.x, p.y + 7 * scale }, brush.Get(), 1.4f * scale);
            dc->DrawLine({ p.x - 4 * scale, p.y + 7 * scale }, { p.x + 4 * scale, p.y + 7 * scale }, brush.Get(), 1.4f * scale);
        }
        if (g_settings.showThemeButton) DrawThemeButton(v0, v1, vis);
        if (g_settings.showOledButton) DrawOledButton(v0, v1, vis);
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
        (int)g_settings.edge, winX, winY, barU0, barU1, sepU, sensU0, sensU1, barThick, margin);
    for (auto& s : slots) if (s.app && s.app->exe.find(L"kakaotalk") != std::wstring::npos) Log(L"kakao u=%.0f", s.u);
}

// ---------------------------------------------------------------- input

void Dock::Click(const Hit& h) {
    switch (h.kind) {
    case HitSlot: {
        if (h.slot < 0 || h.slot >= (int)slots.size()) return;
        auto& s = slots[h.slot];
        if (!s.app) { SendKeyCombo({ VK_LWIN }); return; }
        auto app = s.app;
        g_apps.Click(*app, index);
        break;
    }
    // Windows' own tray windows rather than re-implementations
    case HitTray: {
        float u = (trayU0 + trayU1) * 0.5f;
        if (!nativetray::OpenOverflow(ScreenPointUV(u, margin + barThick * 0.5f), g_settings.edge, BarScreenRect()))
            MessageBoxW(hwnd, L"숨겨진 아이콘 창을 열지 못했습니다. 잠시 후 다시 시도해 주세요.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case HitNet: nativetray::OpenQuickSettings(); break;
    case HitVol: {
        // volume slider just outside the dock, centred on the icon
        float u = (volU0 + volU1) * 0.5f;
        float depth = IsVertical(g_settings.edge) ? 70 * scale : 260 * scale;
        nativetray::OpenVolume(ScreenPointUV(u, margin + barThick + 12 * scale + depth));
        break;
    }
    case HitClock: nativetray::OpenCalendar(); break;
    case HitDesktop:
        if (g_settings.showDesktopButton) nativetray::ToggleDesktop();
        break;
    case HitTheme:
        if (g_settings.showThemeButton && !theme::Toggle())
            MessageBoxW(hwnd, L"다크/라이트 모드를 바꾸지 못했습니다.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
        dirty = true;
        break;
    case HitOled:
        if (g_settings.showOledButton) ToggleOled();
        break;
    case HitSettings: ShowDockMenu(); break;
    case HitSensor: case HitPanel:
        if (g_sensors.CpuTempAvailable() || g_settings.cpuTemp) SendKeyCombo({ VK_CONTROL, VK_SHIFT, VK_ESCAPE });   // Task Manager
        else { SetForegroundWindow(hwnd); g_sensors.EnableCpuTemp(hwnd); }
        break;
    default: break;
    }
}

void Dock::EndDrag(bool commit) {
    if (!dragging) return;
    dragging = false;
    if (pressSlot >= 0 && pressSlot < (int)slots.size() && commit) {
        auto app = slots[pressSlot].app;
        if (app) {
            if (dropRemove) g_apps.SetPinned(*app, false);
            else {
                int pcOthers = g_apps.PinnedCount() - (app->pinned ? 1 : 0);
                g_apps.MoveTo(*app, dropIndex, dropIndex <= pcOthers || app->pinned);
            }
        }
    }
    for (auto& s : slots) { s.gapBefore.target = 0; s.shrink.target = 1; }
    trailingGap.target = 0;
    pressSlot = -1;
    dirty = true;
}

void Dock::ShowAppMenu(Slot& s) {
    auto app = s.app;
    if (!app) return;
    HMENU m = CreatePopupMenu();
    int n = 0;
    for (HWND w : app->windows) {
        if (n >= 15) break;
        wchar_t t[128] = {}; GetWindowTextW(w, t, 128);
        std::wstring title = t; if (title.empty()) title = app->name;
        if (title.size() > 60) title = title.substr(0, 57) + L"...";
        AppendMenuW(m, MF_STRING | (w == GetForegroundWindow() ? MF_CHECKED : 0), 1000 + n, title.c_str());
        n++;
    }
    if (n) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 2, app->Running() ? L"새 창 열기" : L"열기");
    AppendMenuW(m, MF_STRING, app->pinned ? 3 : 4, app->pinned ? L"독에서 제거" : L"독에 고정");
    if (app->Running()) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); AppendMenuW(m, MF_STRING, 5, n > 1 ? L"모든 창 닫기" : L"창 닫기"); }

    POINT pt; GetCursorPos(&pt);
    menuOpen = true;
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    menuOpen = false;
    DestroyMenu(m);
    if (cmd >= 1000 && cmd - 1000 < (int)app->windows.size()) g_apps.ActivateWindow(app->windows[cmd - 1000]);
    else if (cmd == 2) g_apps.Launch(*app, index);
    else if (cmd == 3) g_apps.SetPinned(*app, false);
    else if (cmd == 4) g_apps.SetPinned(*app, true);
    else if (cmd == 5) g_apps.CloseAll(*app);
}

int Dock::PopupMenu(HMENU m) {
    POINT pt; GetCursorPos(&pt);
    menuOpen = true;
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    menuOpen = false;
    DestroyMenu(m);
    return cmd;
}

static void OpenUri(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

// Right-click on the tray items: their own menus instead of the dock settings.
void Dock::ShowTrayMenu(HitKind kind) {
    HMENU m = CreatePopupMenu();
    // same items as the Windows 11 taskbar's own menus (plus mute, which the dock can toggle directly)
    switch (kind) {
    case HitVol: {
        AppendMenuW(m, MF_STRING, 3, L"소리 설정 열기(&E)");
        AppendMenuW(m, MF_STRING, 2, L"볼륨 믹서 열기(&M)");
        HMENU spk = CreatePopupMenu();
        AppendMenuW(spk, MF_STRING, 5, L"공간 음향 설정 열기");
        AppendMenuW(spk, MF_STRING, 6, L"출력 장치 속성");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)spk, L"스피커 설정");
        AppendMenuW(m, MF_STRING, 7, L"소리(&S)");
        AppendMenuW(m, MF_STRING, 4, L"소리 문제 해결(&T)");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (g_sys.muted ? MF_CHECKED : 0), 1, L"음소거");
        break;
    }
    case HitNet:
        AppendMenuW(m, MF_STRING, 12, L"문제 해결(&T)");
        AppendMenuW(m, MF_STRING, 11, L"네트워크 및 인터넷 설정 열기(&N)");
        break;
    case HitClock:
        AppendMenuW(m, MF_STRING, 21, L"날짜 및 시간 조정");
        AppendMenuW(m, MF_STRING, 22, L"알림 설정");
        break;
    default:   // system monitor cell / panel
        AppendMenuW(m, MF_STRING, 30, L"작업 관리자");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (g_settings.cpuTemp ? MF_CHECKED : 0), 31, L"CPU 온도 표시 (관리자 도우미 + PawnIO)");
        break;
    }
    switch (PopupMenu(m)) {
    case 1: g_sys.ToggleMute(); break;
    case 2: OpenUri(L"ms-settings:apps-volume"); break;
    case 3: OpenUri(L"ms-settings:sound"); break;
    case 4: case 12: OpenUri(L"ms-settings:troubleshoot"); break;
    case 5: OpenUri(L"ms-settings:sound-devices"); break;
    case 6: OpenUri(L"ms-settings:sound"); break;
    case 7: ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl", nullptr, SW_SHOWNORMAL); break;
    case 11: OpenUri(L"ms-settings:network"); break;
    case 21: OpenUri(L"ms-settings:dateandtime"); break;
    case 22: OpenUri(L"ms-settings:notifications"); break;
    case 30: SendKeyCombo({ VK_CONTROL, VK_SHIFT, VK_ESCAPE }); break;
    case 31:
        if (g_settings.cpuTemp) { g_settings.cpuTemp = false; g_settings.Save(); }
        else { SetForegroundWindow(hwnd); g_sensors.EnableCpuTemp(hwnd); }
        break;
    }
}

void Dock::ToggleOled() {
    bool ok = oled::Active() ? oled::Disable() : oled::Enable(g_settings.oledOneMonitor ? monitor : nullptr);
    if (!ok) MessageBoxW(hwnd, L"OLED 모드를 바꾸지 못했습니다. 잠시 후 다시 시도해 주세요.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
    dirty = true;
}

// Three bars (red, green, blue: an OLED subpixel) leaning 35 degrees inside one rounded
// outline; grey ink while the mode is off. Kept upright on every edge, like the desktop button.
void Dock::DrawOledButton(float v0, float v1, float vis) {
    auto* dc = g_gfx.dc.Get();
    float ou = (oledU0 + oledU1) * 0.5f;
    auto br = ButtonRect(HitOled);
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
        for (int i = 0; i < 3; ++i) { c[i] = Ink(ou, vis); c[i].a *= a[i]; }
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

// Design 2 hung upside down: base and cord at the top, the round glass below. Dark mode: outline;
// light mode: filled with rays spreading from the glass. Ink colour only.
void Dock::DrawThemeButton(float v0, float v1, float vis) {
    auto* dc = g_gfx.dc.Get();
    float tu = (themeU0 + themeU1) * 0.5f;
    auto br = ButtonRect(HitTheme);
    D2D1_POINT_2F p = { (br.left + br.right) * 0.5f, (br.top + br.bottom) * 0.5f };
    float k = 0.55f * scale * ButtonIconScale();   // drawing units: the glass is 16 units wide
    auto pt = [&](float x, float y) { return D2D1::Point2F(p.x + x * k, p.y + y * k); };
    bool lit = theme::IsLight();
    auto ink = Ink(tu, vis * (lit ? 1.0f : 0.8f));
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
        for (auto& r : rays) dc->DrawLine(pt(r[0], r[1]), pt(r[2], r[3]), brush.Get(), line);
    } else {
        dc->DrawGeometry(glass.Get(), brush.Get(), line);
    }
}

void Dock::ShowDockMenu() {
    HMENU m = CreatePopupMenu();
    HMENU pos = CreatePopupMenu(), size = CreatePopupMenu(), mg = CreatePopupMenu(), gl = CreatePopupMenu(), dl = CreatePopupMenu();
    HMENU clock = CreatePopupMenu(), spacing = CreatePopupMenu(), om = CreatePopupMenu();
    auto chk = [](bool b) { return MF_STRING | (b ? MF_CHECKED : 0); };
    AppendMenuW(pos, chk(g_settings.edge == Edge::Bottom), 10, L"아래");
    AppendMenuW(pos, chk(g_settings.edge == Edge::Left), 11, L"왼쪽");
    AppendMenuW(pos, chk(g_settings.edge == Edge::Right), 12, L"오른쪽");
    AppendMenuW(pos, chk(g_settings.edge == Edge::Top), 13, L"위");
    AppendMenuW(size, chk(g_settings.iconSize < 40), 20, L"작게");
    AppendMenuW(size, chk(g_settings.iconSize >= 40 && g_settings.iconSize < 49), 21, L"보통");
    AppendMenuW(size, chk(g_settings.iconSize >= 49), 22, L"크게");
    AppendMenuW(mg, chk(g_settings.magnification < 1.05f), 30, L"끄기");
    AppendMenuW(mg, chk(g_settings.magnification >= 1.05f && g_settings.magnification < 1.55f), 31, L"약하게");
    AppendMenuW(mg, chk(g_settings.magnification >= 1.55f && g_settings.magnification < 1.9f), 32, L"보통");
    AppendMenuW(mg, chk(g_settings.magnification >= 1.9f), 33, L"강하게");
    AppendMenuW(gl, chk(g_settings.glassStyle == 0), 40, L"맑게");
    AppendMenuW(gl, chk(g_settings.glassStyle == 1), 41, L"보통");
    AppendMenuW(gl, chk(g_settings.glassStyle == 2), 42, L"뿌옇게");
    AppendMenuW(dl, chk(g_settings.showDelay < 0.1f), 50, L"즉시");
    AppendMenuW(dl, chk(g_settings.showDelay >= 0.1f && g_settings.showDelay < 0.3f), 51, L"짧게");
    AppendMenuW(dl, chk(g_settings.showDelay >= 0.3f), 52, L"길게");
    AppendMenuW(clock, chk(g_settings.clockSize == 0), 80, L"보통");
    AppendMenuW(clock, chk(g_settings.clockSize == 1), 81, L"크게");
    AppendMenuW(spacing, chk(g_settings.iconSpacing == 0), 90, L"좁게");
    AppendMenuW(spacing, chk(g_settings.iconSpacing == 1), 91, L"보통");
    AppendMenuW(spacing, chk(g_settings.iconSpacing == 2), 92, L"넓게");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pos, L"위치");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)size, L"아이콘 크기");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)spacing, L"아이콘 간격");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)clock, L"시계 글자 크기");
    AppendMenuW(m, chk(g_settings.showDesktopButton), 82, L"바탕화면 보기 버튼");
    AppendMenuW(om, chk(oled::Active()), 83, L"OLED 모드 켜기 (검은 배경 · 아이콘 숨김)");
    AppendMenuW(om, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(om, chk(!g_settings.oledOneMonitor), 84, L"모든 모니터");
    AppendMenuW(om, chk(g_settings.oledOneMonitor), 85, L"이 모니터만");
    AppendMenuW(om, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(om, chk(g_settings.showOledButton), 86, L"독에 OLED 버튼 표시");
    AppendMenuW(m, chk(g_settings.showThemeButton), 87, L"다크/라이트 전환 버튼");
    AppendMenuW(m, MF_POPUP | (oled::Active() ? MF_CHECKED : 0), (UINT_PTR)om, L"OLED 모드");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mg, L"확대 효과");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)gl, L"유리 질감");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)dl, L"나타나는 속도");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, chk(g_settings.autostart), 60, L"Windows 시작 시 자동 실행");
    AppendMenuW(m, chk(g_settings.cpuTemp), 70, L"CPU 온도 표시 (관리자 도우미 + PawnIO)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 99, L"Liquid Dock 종료 (기본 작업표시줄 복원)");

    POINT pt; GetCursorPos(&pt);
    menuOpen = true;
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    menuOpen = false;
    DestroyMenu(m);

    bool rebuild = false;
    switch (cmd) {
    case 10: case 11: case 12: case 13: g_settings.edge = (Edge)(cmd - 10); rebuild = true; break;
    case 20: g_settings.iconSize = 36; rebuild = true; break;
    case 21: g_settings.iconSize = 44; rebuild = true; break;
    case 22: g_settings.iconSize = 54; rebuild = true; break;
    case 30: g_settings.magnification = 1.0f; rebuild = true; break;
    case 31: g_settings.magnification = 1.4f; rebuild = true; break;
    case 32: g_settings.magnification = 1.7f; rebuild = true; break;
    case 33: g_settings.magnification = 2.1f; rebuild = true; break;
    case 40: case 41: case 42: g_settings.glassStyle = cmd - 40; backdropDirty = true; dirty = true; break;
    case 50: g_settings.showDelay = 0.0f; break;
    case 51: g_settings.showDelay = 0.15f; break;
    case 52: g_settings.showDelay = 0.45f; break;
    case 60: g_settings.autostart = !g_settings.autostart; PostMessageW(g_mainWnd, WM_APP_AUTOSTART, 0, 0); break;
    case 70:
        if (g_settings.cpuTemp) g_settings.cpuTemp = false;   // the helper's readings are ignored from now on
        else { SetForegroundWindow(hwnd); g_sensors.EnableCpuTemp(hwnd); }
        break;
    case 99: PostMessageW(g_mainWnd, WM_CLOSE, 0, 0); break;
    case 80: case 81: g_settings.clockSize = cmd - 80; rebuild = true; break;
    case 82: g_settings.showDesktopButton = !g_settings.showDesktopButton; rebuild = true; break;
    case 83: ToggleOled(); break;
    case 84: case 85: g_settings.oledOneMonitor = cmd == 85; break;
    case 87: g_settings.showThemeButton = !g_settings.showThemeButton; rebuild = true; break;
    case 86: g_settings.showOledButton = !g_settings.showOledButton; rebuild = true; break;
    case 90: case 91: case 92: g_settings.iconSpacing = cmd - 90; rebuild = true; break;
    }
    if (cmd) g_settings.Save();
    if (cmd == 40 || cmd == 41 || cmd == 42) {
        // other docks need their blur redone too
        PostMessageW(g_mainWnd, WM_APP_REBUILD, 1, 0);
    }
    if (rebuild) PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0);
}

LRESULT Dock::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_LBUTTONDOWN: {
        windowPreview.Dismiss();
        float u, v; ClientToUV(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), u, v);
        Hit h = HitTestUV(u, v);
        pressKind = h.kind; pressSlot = h.kind == HitSlot ? h.slot : -1;
        GetCursorPos(&pressPt);
        if (pressSlot >= 0) slots[pressSlot].press.target = 1;
        SetCapture(hwnd);
        dirty = true;
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        float u, v; ClientToUV(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), u, v);
        for (auto& s : slots) s.press.target = 0;
        if (dragging) { EndDrag(true); return 0; }
        Hit h = HitTestUV(u, v);
        int ps = pressSlot; HitKind pk = pressKind;
        pressSlot = -1; pressKind = HitNone;
        if (h.kind == pk && (h.kind != HitSlot || h.slot == ps)) Click(h);
        dirty = true;
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging && (HWND)lp != hwnd) EndDrag(true);
        return 0;
    case WM_RBUTTONUP: {
        windowPreview.Dismiss();
        float u, v; ClientToUV(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), u, v);
        Hit h = HitTestUV(u, v);
        if (h.kind == HitSlot && slots[h.slot].app) ShowAppMenu(slots[h.slot]);
        else if (h.kind == HitNet || h.kind == HitVol || h.kind == HitClock || h.kind == HitSensor || h.kind == HitPanel) ShowTrayMenu(h.kind);
        else ShowDockMenu();
        return 0;
    }
    case WM_MBUTTONUP: {
        windowPreview.Dismiss();
        float u, v; ClientToUV(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), u, v);
        Hit h = HitTestUV(u, v);
        if (h.kind == HitSlot && slots[h.slot].app) { auto app = slots[h.slot].app; g_apps.Launch(*app, index); }
        else if (h.kind == HitVol) g_sys.ToggleMute();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        float u, v; ScreenToUV(p, u, v);
        Hit h = HitTestUV(u, v);
        if (h.kind == HitVol) g_sys.AddVolume(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 0.02f);
        return 0;
    }
    case WM_DROPFILES: {
        windowPreview.Dismiss();
        HDROP hd = (HDROP)wp;
        POINT p; DragQueryPoint(hd, &p);
        float u, v; ClientToUV(p.x, p.y, u, v);
        int idx = 0;
        for (auto& s : slots) if (s.app && !s.ghost && s.u < u) idx++;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 2];
            if (DragQueryFileW(hd, i, path, ARRAYSIZE(path))) g_apps.PinFile(path, idx++);
        }
        DragFinish(hd);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); dirty = true; return 0; }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
