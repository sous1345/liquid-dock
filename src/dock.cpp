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

static const wchar_t* kDockClass = L"LiquidDockBar";

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
    SuspendRecording();
    UpdateAppBar(false);
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
    margin = std::round(std::clamp(g_settings.edgeGap, 0, 40) * scale);
    mag = std::max(1.0f, g_settings.magnification);
    g_gfx.EnsureFonts(scale);
    fit = 1;
    ApplyFit(0);
    bool vert = IsVertical(g_settings.edge);
    len = (float)(vert ? (mon.bottom - mon.top) : (mon.right - mon.left));
    float headroom = std::max(base * (mag - 1), base * 0.65f) + (vert ? 240 * scale : 46 * scale);
    panelW = std::round(300 * scale);
    panelH = std::round(292 * scale);
    // The window is sized for the largest edge gap and thickness, so those settings can change live
    // (Relayout) without recreating the window, its swap chain and the screen capture.
    float marginMax = std::round(40 * scale), thickMax = MeasureMetrics(1, 150).thickness;
    float panelNeed = marginMax + thickMax + 14 * scale + (vert ? panelW : panelH) + 28 * scale;
    T = std::ceil(std::max(marginMax + thickMax + headroom + 10 * scale, panelNeed));
    int t = (int)T;
    switch (g_settings.edge) {
    case Edge::Bottom: winX = mon.left; winY = mon.bottom - t; winW = mon.right - mon.left; winH = t; break;
    case Edge::Top:    winX = mon.left; winY = mon.top;        winW = mon.right - mon.left; winH = t; break;
    case Edge::Left:   winX = mon.left; winY = mon.top;        winW = t; winH = mon.bottom - mon.top; break;
    case Edge::Right:  winX = mon.right - t; winY = mon.top;   winW = t; winH = mon.bottom - mon.top; break;
    }
    g_gfx.EnsureFonts(scale);
}

bool Dock::AlwaysShown() const { return !g_settings.autoHide && !oled::Active(); }   // OLED mode always hides

void Dock::UpdateAppBar(bool want) {
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = hwnd;
    if (!want || !hwnd) {
        if (appBar) { SHAppBarMessage(ABM_REMOVE, &abd); appBar = false; appBarRect = {}; }
        return;
    }
    // the strip from the screen edge to the far side of the glass, in steps of 4 px so a crowded
    // dock shrinking by a pixel does not make every window on the screen move again
    int band = ((int)std::ceil(margin + barThick) + 3) / 4 * 4;
    RECT r = mon;
    UINT e = ABE_BOTTOM;
    switch (g_settings.edge) {
    case Edge::Bottom: e = ABE_BOTTOM; r.top = r.bottom - band; break;
    case Edge::Top:    e = ABE_TOP;    r.bottom = r.top + band; break;
    case Edge::Left:   e = ABE_LEFT;   r.right = r.left + band; break;
    case Edge::Right:  e = ABE_RIGHT;  r.left = r.right - band; break;
    }
    if (appBar && EqualRect(&r, &appBarRect)) return;
    if (!appBar) {
        abd.uCallbackMessage = WM_APP_APPBAR;
        if (!SHAppBarMessage(ABM_NEW, &abd)) { Log(L"appbar: register failed"); appBarRect = r; return; }
        appBar = true;
    }
    abd.uEdge = e;
    abd.rc = r;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    switch (e) {   // keep the dock's own thickness from whatever edge the shell returned
    case ABE_BOTTOM: abd.rc.top = abd.rc.bottom - band; break;
    case ABE_TOP:    abd.rc.bottom = abd.rc.top + band; break;
    case ABE_LEFT:   abd.rc.right = abd.rc.left + band; break;
    case ABE_RIGHT:  abd.rc.left = abd.rc.right - band; break;
    }
    SHAppBarMessage(ABM_SETPOS, &abd);
    appBarRect = r;
}

void Dock::HotkeyReveal(double now) {
    if (reveal.target > 0.5f) { reveal.target = 0; hotkeyHoldUntil = 0; edgeSince = -1; return; }   // pressed again: away
    hotkeyHoldUntil = now + 3;
    reveal.target = 1; outsideSince = -1;
    lastCaptureWanted = now;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    dirty = true;
}

bool Dock::Relayout() {
    int x = winX, y = winY, w = winW, h = winH;
    float keepFit = fit;   // a crowded dock keeps its current shrink instead of animating it again
    Metrics();
    if (x != winX || y != winY || w != winW || h != winH) return false;
    fit = keepFit; ApplyFit(0);
    backdropDirty = dirty = true;
    return true;
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

    reveal.snap(settingsOpen ? 1.0f : 0.0f);   // recreated while the shape settings are open: no pop-in
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

    // the quiet button holds back notification and overheat bounces (launch hops still show)
    const bool quiet = features::Quiet(now);

    // overheat: the sensor cell bounces once (3 hops) on the primary monitor
    if (primary && !quiet && g_sensors.alertPending && sbCount == 0) {
        sbStart = now; sbCount = 3; sbHidden = reveal.target < 0.5f;
        lastCaptureWanted = now;
    }

    // notifications bounce on the primary monitor only
    if (primary && !quiet) {
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
         (panelAlpha.target > 0.5f && cursorU >= cells[HitSensor].u0 - 4 * scale && cursorU <= cells[HitSensor].u1 + 4 * scale &&
          cursorV <= margin + barThick + 16 * scale));   // bridge between cell and panel
    bool keep = menuOpen || dragging || pressSlot >= 0 || debugForceShow || settingsOpen || now < hotkeyHoldUntil;
    bool fullscreen = FullscreenAppActive();
    bool inPreview = windowPreview.Contains(cursor);
    if (debugForceShow || settingsOpen) reveal.target = 1;
    bool alwaysShown = AlwaysShown();
    UpdateAppBar(alwaysShown);

    if (alwaysShown) {
        // auto-hide off: only a fullscreen app (a game, a video) puts the dock away
        if (fullscreen && !keep) reveal.target = 0;
        else if (reveal.target < 0.5f) {
            reveal.target = 1;
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        outsideSince = edgeSince = -1;
    } else if (reveal.target < 0.5f) {
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
    hoverSlot = -1; hoverKind = HitNone; hoverButton = ButtonId::Count;
    if (reveal.x > 0.9f && inMonitor && !dragging) {
        Hit h = HitTestUV(cursorU, cursorV);
        hoverKind = h.kind; hoverSlot = h.slot; hoverButton = h.button;
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
         cursorU >= cells[HitSensor].u0 - 4 * scale && cursorU <= cells[HitSensor].u1 + 4 * scale));
    if (wantPanel) { panelAlpha.target = 1; panelOutsideSince = -1; }
    else if (panelAlpha.target > 0.5f) {
        if (panelOutsideSince < 0) panelOutsideSince = now;
        if (now - panelOutsideSince > 0.25) panelAlpha.target = 0;
    }
    panelAlpha.step(dt, 320, 0.85f);
    if (!panelAlpha.settled()) anim = true;
    std::wstring tip;
    const float barTip = margin + barThick + 8 * scale - hideOff;   // tooltips of the tray controls sit just above the bar
    if (hoverKind == HitSlot && hoverSlot >= 0) {
        auto& s = slots[hoverSlot];
        tip = s.app ? s.app->name : L"시작";
        tooltipU = s.u;
        tooltipV = margin + pad + s.size * s.weight + s.bounceOff + 10 * scale - hideOff;
    } else if (hoverKind == HitTray) {
        tip = L"숨겨진 아이콘 표시";
        tooltipU = cells[HitTray].Mid(); tooltipV = barTip;
    } else if (hoverKind == HitClock) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t d[128] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, d, 128, nullptr);
        tip = d;
        tooltipU = cells[HitClock].Mid(); tooltipV = barTip;
    } else if (hoverKind == HitButton) {
        tip = (this->*Button(hoverButton).tooltip)();
        tooltipU = Cell_(hoverButton).Mid(); tooltipV = barTip;
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
        float c = (cells[HitSensor].u0 + cells[HitSensor].u1) * 0.5f, a = vert ? 52 * scale : 112 * scale, depth = vert ? 84 * scale : 40 * scale;
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

// ---------------------------------------------------------------- input

void Dock::Click(const Hit& h) {
    switch (h.kind) {
    case HitSlot: {
        if (h.slot < 0 || h.slot >= (int)slots.size()) return;
        auto& s = slots[h.slot];
        if (!s.app) { SendKeyCombo({ VK_LWIN }); return; }
        auto app = s.app;
        if (g_settings.shiftRunsAsAdmin && (GetKeyState(VK_SHIFT) & 0x8000)) g_apps.Launch(*app, index, true);
        else g_apps.Click(*app, index);
        break;
    }
    // Windows' own tray windows rather than re-implementations
    case HitTray: {
        float u = (cells[HitTray].u0 + cells[HitTray].u1) * 0.5f;
        if (!nativetray::OpenOverflow(ScreenPointUV(u, margin + barThick * 0.5f), g_settings.edge, BarScreenRect()))
            MessageBoxW(hwnd, L"숨겨진 아이콘 창을 열지 못했습니다. 잠시 후 다시 시도해 주세요.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case HitClock: nativetray::OpenCalendar(); break;
    case HitSensor: case HitPanel:
        if (g_sensors.CpuTempAvailable() || g_settings.cpuTemp) OpenTaskManager();
        else { SetForegroundWindow(hwnd); g_sensors.EnableCpuTemp(hwnd); }
        break;
    case HitButton:
        if (h.button != ButtonId::Count) (this->*Button(h.button).click)();
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

LRESULT Dock::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_LBUTTONDOWN: {
        windowPreview.Dismiss();
        float u, v; ClientToUV(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), u, v);
        Hit h = HitTestUV(u, v);
        pressKind = h.kind; pressSlot = h.kind == HitSlot ? h.slot : -1; pressButton = h.button;
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
        int ps = pressSlot; HitKind pk = pressKind; ButtonId pb = pressButton;
        pressSlot = -1; pressKind = HitNone; pressButton = ButtonId::Count;
        if (h.kind == pk && (h.kind != HitSlot || h.slot == ps) && (h.kind != HitButton || h.button == pb)) Click(h);
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
        else if (h.kind == HitSlot && g_settings.startMenuExtras) ShowStartMenu();
        else if (h.kind == HitButton && Button(h.button).rightClick) (this->*Button(h.button).rightClick)();
        else if (h.kind == HitClock || h.kind == HitSensor || h.kind == HitPanel) ShowTrayMenu(h);
        else ShowDockMenu();
        return 0;
    }
    case WM_MBUTTONUP: {
        windowPreview.Dismiss();
        float u, v; ClientToUV(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), u, v);
        Hit h = HitTestUV(u, v);
        if (h.kind == HitSlot && slots[h.slot].app) { auto app = slots[h.slot].app; g_apps.Launch(*app, index); }
        else if (h.Is(ButtonId::Volume)) g_sys.ToggleMute();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        float u, v; ScreenToUV(p, u, v);
        Hit h = HitTestUV(u, v);
        if (h.Is(ButtonId::Volume)) g_sys.AddVolume(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 0.02f);
        return 0;
    }
    case WM_DROPFILES: {
        windowPreview.Dismiss();
        HDROP hd = (HDROP)wp;
        POINT p; DragQueryPoint(hd, &p);
        float u, v; ClientToUV(p.x, p.y, u, v);
        if (HitTestUV(u, v).Is(ButtonId::Trash)) { DropOnTrash(hd); DragFinish(hd); return 0; }
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
    case WM_APP_APPBAR:   // another app bar or the taskbar moved: claim the strip again
        if (wp == ABN_POSCHANGED) appBarRect = {};
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); dirty = true; return 0; }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
