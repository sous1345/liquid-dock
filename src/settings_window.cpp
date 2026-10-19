#include "settings_window.h"
#include "dock.h"
#include "theme.h"
#include "oled.h"
#include "buttons.h"
#include "sensors.h"
#include "version.h"
#include <dwmapi.h>

namespace settingswindow {
namespace {

using D2D1::ColorF;

// ---------------------------------------------------------------- the options

// How a change reaches the docks.
enum class Apply {
    None,        // read when needed (delays, OLED scope)
    Live,        // laid out again in place while the control moves (falls back to a rebuild)
    OnRelease,   // the dock windows change size: rebuilt once, when the slider is let go
    Rebuild,     // discrete choice that changes the windows
};

struct Control {
    enum Type { Segmented, Slider, Toggle, Button } type;
    const wchar_t* label;
    const wchar_t* hint = nullptr;                      // one line under the label
    Apply apply = Apply::None;
    // segmented
    std::vector<const wchar_t*> options;
    std::function<int()> index;
    std::function<void(int)> choose;
    // slider
    float lo = 0, hi = 1, step = 1;
    std::function<float()> value;
    std::function<void(float)> set;
    std::function<std::wstring(float)> text;
    // toggle
    std::function<bool()> on;
    std::function<void(bool)> toggle;
    // button
    std::function<void()> press;
};

struct Page { const wchar_t* title; const wchar_t* glyph; std::vector<Control> controls; std::function<void()> reset; };

HWND g_wnd = nullptr;
HMONITOR g_monitor = nullptr;
std::vector<Page> g_pages;

std::wstring Num(float v, int decimals, const wchar_t* unit) {
    wchar_t b[32]; swprintf_s(b, L"%.*f%s", decimals, v, unit);
    return b;
}

void BuildPages() {
    g_pages.clear();

    Page shape{ L"모양", L"" };
    shape.controls = {
        { Control::Segmented, L"위치", nullptr, Apply::Rebuild, { L"아래", L"왼쪽", L"오른쪽", L"위" },
          [] { const int order[] = { 0, 1, 2, 3 }; return order[(int)g_settings.edge]; },
          [](int i) { g_settings.edge = (Edge)i; } },
        { Control::Slider, L"아이콘 크기", nullptr, Apply::OnRelease, {}, {}, {}, 32, 64, 2,
          [] { return g_settings.iconSize; }, [](float v) { g_settings.iconSize = v; },
          [](float v) { return Num(v, 0, v == 44 ? L" (기본)" : L""); } },
        { Control::Slider, L"아이콘 간격", nullptr, Apply::Live, {}, {}, {}, 10, 60, 2,
          [] { constexpr int preset[] = { 22, 32, 44 }; return (float)(g_settings.iconGap > 0 ? g_settings.iconGap : preset[std::clamp(g_settings.iconSpacing, 0, 2)]); },
          [](float v) { g_settings.iconGap = (int)v; },
          [](float v) { return Num(v, 0, v == 32 ? L"% (기본)" : L"%"); } },
        { Control::Slider, L"독 두께", L"아이콘 크기는 그대로 두고 유리 두께만 바뀝니다", Apply::Live, {}, {}, {}, 75, 150, 5,
          [] { return (float)g_settings.dockThickness; }, [](float v) { g_settings.dockThickness = (int)v; },
          [](float v) { return Num(v, 0, v == 100 ? L"% (기본)" : L"%"); } },
        { Control::Slider, L"화면 가장자리와 거리", nullptr, Apply::Live, {}, {}, {}, 0, 40, 2,
          [] { return (float)g_settings.edgeGap; }, [](float v) { g_settings.edgeGap = (int)v; },
          [](float v) { return v == 0 ? std::wstring(L"붙임") : Num(v, 0, L" px"); } },
        { Control::Slider, L"가장자리 따라 위치", nullptr, Apply::Live, {}, {}, {}, -100, 100, 5,
          [] { return (float)g_settings.edgeOffset; }, [](float v) { g_settings.edgeOffset = (int)v; },
          [](float v) {
              if (v == 0) return std::wstring(L"가운데");
              bool vert = IsVertical(g_settings.edge);
              return std::wstring(v < 0 ? (vert ? L"위로 " : L"왼쪽으로 ") : (vert ? L"아래로 " : L"오른쪽으로 ")) + Num(std::fabs(v), 0, L"%");
          } },
        { Control::Segmented, L"유리 질감", nullptr, Apply::Live, { L"맑게", L"보통", L"뿌옇게" },
          [] { return std::clamp(g_settings.glassStyle, 0, 2); }, [](int i) { g_settings.glassStyle = i; } },
        { Control::Segmented, L"시계 글자", nullptr, Apply::Live, { L"보통", L"크게" },
          [] { return std::clamp(g_settings.clockSize, 0, 1); }, [](int i) { g_settings.clockSize = i; } },
    };
    shape.reset = [] {
        Settings d; auto& s = g_settings;
        s.iconSize = d.iconSize; s.iconGap = d.iconGap; s.iconSpacing = d.iconSpacing; s.dockThickness = d.dockThickness;
        s.edgeGap = d.edgeGap; s.edgeOffset = d.edgeOffset; s.glassStyle = d.glassStyle; s.clockSize = d.clockSize;
    };

    Page behaviour{ L"동작", L"" };
    behaviour.controls = {
        { Control::Toggle, L"자동 숨김", L"끄면 독이 항상 보이고 창이 독을 가리지 않습니다 (OLED 모드에서는 항상 숨김)", Apply::Live, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.autoHide; }, [](bool v) { g_settings.autoHide = v; } },
        { Control::Slider, L"확대 효과", L"아이콘에 마우스를 올렸을 때 커지는 정도", Apply::OnRelease, {}, {}, {}, 1.0f, 2.1f, 0.1f,
          [] { return g_settings.magnification; }, [](float v) { g_settings.magnification = v; },
          [](float v) { return v < 1.05f ? std::wstring(L"끄기") : Num(v, 1, L"배"); } },
        { Control::Slider, L"나타나는 속도", L"화면 가장자리에 마우스를 대고 독이 나올 때까지", Apply::None, {}, {}, {}, 0, 0.6f, 0.05f,
          [] { return g_settings.showDelay; }, [](float v) { g_settings.showDelay = v; },
          [](float v) { return v < 0.025f ? std::wstring(L"즉시") : Num(v, 2, L"초"); } },
        { Control::Slider, L"숨는 속도", L"마우스가 독을 벗어나고 독이 들어갈 때까지", Apply::None, {}, {}, {}, 0.1f, 1.5f, 0.05f,
          [] { return g_settings.hideDelay; }, [](float v) { g_settings.hideDelay = v; },
          [](float v) { return Num(v, 2, L"초"); } },
    };
    behaviour.reset = [] { Settings d; g_settings.autoHide = d.autoHide; g_settings.magnification = d.magnification; g_settings.showDelay = d.showDelay; g_settings.hideDelay = d.hideDelay; };

    Page buttons{ L"버튼", L"" };
    buttons.controls.clear();
    // one switch per optional button (the drag-and-drop preview replaces these)
    for (int i = 0; i < kButtonCount; ++i) {
        const ButtonInfo& bi = Info((ButtonId)i);
        if (bi.fixed) continue;
        ButtonId id = bi.id;
        buttons.controls.push_back({ Control::Toggle, bi.name, bi.description, Apply::Live, {}, {}, {}, 0, 1, 1, {}, {}, {},
            [id] { return HasButton(id); }, [id](bool v) { SetButton(id, v); } });
    }
    buttons.controls.push_back({ Control::Segmented, L"OLED 모드를 적용할 모니터", nullptr, Apply::None, { L"모든 모니터", L"설정을 연 모니터만" },
        [] { return g_settings.oledOneMonitor ? 1 : 0; }, [](int i) { g_settings.oledOneMonitor = i == 1; } });
    buttons.controls.push_back({ Control::Toggle, L"OLED 모드 켜기", L"지금 바로 검은 배경으로 바꿉니다", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
        [] { return oled::Active(); },
        [](bool v) { if (v) oled::Enable(g_settings.oledOneMonitor ? g_monitor : nullptr); else oled::Disable(); } });
    buttons.controls.push_back({ Control::Toggle, L"Windows 다크 모드", nullptr, Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
        [] { return !theme::IsLight(); }, [](bool v) { if (v == theme::IsLight()) theme::Toggle(); } });
    buttons.reset = [] { Settings d; g_settings.trayButtons = d.trayButtons; g_settings.oledOneMonitor = d.oledOneMonitor; };

    Page system{ L"시스템", L"" };
    system.controls = {
        { Control::Toggle, L"Windows 시작 시 자동 실행", nullptr, Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.autostart; },
          [](bool v) { g_settings.autostart = v; PostMessageW(g_mainWnd, WM_APP_AUTOSTART, 0, 0); } },
        { Control::Segmented, L"독 열기 단축키", L"다른 프로그램이 이미 쓰는 조합이면 동작하지 않습니다", Apply::None,
          { L"끄기", L"Ctrl+Alt+D", L"Ctrl+Shift+Space", L"Win+Shift+D" },
          [] { return std::clamp(g_settings.revealHotkey, 0, 3); },
          [](int i) { g_settings.revealHotkey = i; PostMessageW(g_mainWnd, WM_APP_HOTKEY, 0, 0); } },
        { Control::Toggle, L"시작 버튼 우클릭 메뉴", L"작업 관리자, 실행, 잠금·절전·다시 시작·종료", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.startMenuExtras; }, [](bool v) { g_settings.startMenuExtras = v; } },
        { Control::Toggle, L"Shift+클릭으로 관리자 권한 실행", L"독의 앱 아이콘을 Shift를 누른 채 클릭 (스토어 앱 제외)", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.shiftRunsAsAdmin; }, [](bool v) { g_settings.shiftRunsAsAdmin = v; } },
        { Control::Toggle, L"CPU 온도 표시", L"관리자 승인 한 번과 PawnIO 드라이버가 필요합니다", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.cpuTemp; },
          [](bool v) { if (!v) g_settings.cpuTemp = false; else g_sensors.EnableCpuTemp(g_wnd); } },
        { Control::Button, L"Liquid Dock 종료", L"Windows 기본 작업표시줄로 돌아갑니다", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {}, {}, {},
          [] { PostMessageW(g_mainWnd, WM_CLOSE, 0, 0); } },
    };
    g_pages = { shape, behaviour, buttons, system };
}

// ---------------------------------------------------------------- window state

int g_page = 0;
int g_hover = -1, g_hoverSub = -1;     // control (or -10 - nav item / -2 reset button) under the pointer
int g_drag = -1;                       // slider being dragged
bool g_pressed = false;
float g_scroll = 0, g_contentH = 0;
float g_dpi = 96;
bool g_acrylic = false, g_dark = true;
D2D1_COLOR_F g_accent = ColorF(0x4C9AFF);

ComPtr<IDXGISwapChain1> g_swap;
ComPtr<ID2D1Bitmap1> g_target;
ComPtr<IDCompositionTarget> g_compTarget;
ComPtr<IDCompositionVisual> g_visual;
ComPtr<ID2D1SolidColorBrush> g_brush;
ComPtr<IDWriteTextFormat> g_fTitle, g_fNavTitle, g_fNav, g_fLabel, g_fValue, g_fHint, g_fGlyph, g_fSeg, g_fSegSmall;

// layout in DIPs
constexpr float kW = 660, kH = 600, kNav = 176, kPad = 32;

struct Hot { D2D1_RECT_F r; int control; int sub; };
std::vector<Hot> g_hots;
D2D1_RECT_F g_sliderTrack[32];         // per control, for dragging

D2D1_COLOR_F Text(float a = 1) { return g_dark ? ColorF(0.96f, 0.96f, 0.97f, a) : ColorF(0.10f, 0.10f, 0.11f, a); }
D2D1_COLOR_F Muted() { return g_dark ? ColorF(0.70f, 0.71f, 0.75f) : ColorF(0.36f, 0.37f, 0.40f); }
D2D1_COLOR_F Wash(float a) { return g_dark ? ColorF(1, 1, 1, a) : ColorF(0, 0, 0, a); }

void ReadAccent() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        g_accent = ColorF((v & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, ((v >> 16) & 0xFF) / 255.f);   // ABGR
    // on a dark backdrop a dark accent loses contrast: lift it
    if (g_dark) {
        float l = 0.2126f * g_accent.r + 0.7152f * g_accent.g + 0.0722f * g_accent.b;
        if (l < 0.45f) { float k = (0.45f - l) / (1 - l); g_accent.r += (1 - g_accent.r) * k; g_accent.g += (1 - g_accent.g) * k; g_accent.b += (1 - g_accent.b) * k; }
    }
}

ComPtr<IDWriteTextFormat> Font(float size, DWRITE_FONT_WEIGHT w, const wchar_t* family = L"Segoe UI Variable Text") {
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(g_gfx.dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ko-kr", &f)))
        g_gfx.dw->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ko-kr", &f);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

bool CreateTarget() {
    g_target.Reset();
    ComPtr<IDXGISurface> surf;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;
    auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), g_dpi, g_dpi);
    return SUCCEEDED(g_gfx.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &g_target));
}

bool CreateResources() {
    RECT rc; GetClientRect(g_wnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = std::max<LONG>(1, rc.right); sd.Height = std::max<LONG>(1, rc.bottom);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(g_gfx.factory->CreateSwapChainForComposition(g_gfx.d3d.Get(), &sd, nullptr, &g_swap))) return false;
    if (!CreateTarget()) return false;
    g_gfx.dcomp->CreateTargetForHwnd(g_wnd, TRUE, &g_compTarget);
    g_gfx.dcomp->CreateVisual(&g_visual);
    g_visual->SetContent(g_swap.Get());
    g_compTarget->SetRoot(g_visual.Get());
    g_gfx.dcomp->Commit();
    g_gfx.dc->CreateSolidColorBrush(ColorF(1, 1, 1), &g_brush);
    g_fTitle = Font(26, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI Variable Display");
    g_fNavTitle = Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fNav = Font(15, DWRITE_FONT_WEIGHT_NORMAL);
    g_fLabel = Font(15, DWRITE_FONT_WEIGHT_NORMAL);
    g_fValue = Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fValue->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fHint = Font(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fSeg = Font(14, DWRITE_FONT_WEIGHT_NORMAL);
    g_fSeg->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fSegSmall = Font(12, DWRITE_FONT_WEIGHT_NORMAL);   // long choices (key combinations) in narrow segments
    g_fSegSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    // the icon font the dock resolved (Segoe Fluent Icons, or MDL2 Assets on Windows 10)
    wchar_t family[64] = L"Segoe Fluent Icons";
    if (g_gfx.fmtGlyph) g_gfx.fmtGlyph->GetFontFamilyName(family, ARRAYSIZE(family));
    g_fGlyph = Font(16, DWRITE_FONT_WEIGHT_NORMAL, family);
    return true;
}

void Release() {
    g_target.Reset(); g_swap.Reset(); g_visual.Reset(); g_compTarget.Reset(); g_brush.Reset();
    g_fTitle.Reset(); g_fNavTitle.Reset(); g_fNav.Reset(); g_fLabel.Reset(); g_fValue.Reset(); g_fHint.Reset(); g_fSeg.Reset(); g_fSegSmall.Reset(); g_fGlyph.Reset();
}

// ---------------------------------------------------------------- drawing

void Fill(D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
    g_brush->SetColor(c);
    if (radius > 0) g_gfx.dc->FillRoundedRectangle({ r, radius, radius }, g_brush.Get());
    else g_gfx.dc->FillRectangle(r, g_brush.Get());
}

void Draw(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    g_brush->SetColor(c);
    g_gfx.dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float RowHeight(const Control& c) {
    float h = c.type == Control::Segmented ? 72.f : c.type == Control::Slider ? 62.f : 56.f;
    return h + (c.hint ? 18 : 0);
}

void Render() {
    if (!g_target) return;
    auto* dc = g_gfx.dc.Get();
    float oldX, oldY; dc->GetDpi(&oldX, &oldY);
    dc->SetTarget(g_target.Get());
    // the shared context works in pixels for the docks; this window is laid out in DIPs
    auto oldUnits = dc->GetUnitMode();
    dc->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    dc->SetDpi(g_dpi, g_dpi);
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    // over the acrylic only a light tint (as the taskbar); opaque where there is no backdrop
    dc->Clear(g_acrylic ? (g_dark ? ColorF(0.11f, 0.11f, 0.12f, 0.30f) : ColorF(0.96f, 0.96f, 0.97f, 0.35f))
                        : (g_dark ? ColorF(0.13f, 0.13f, 0.14f) : ColorF(0.95f, 0.95f, 0.96f)));
    g_hots.clear();

    // navigation pane
    Fill({ 0, 0, kNav, kH }, Wash(g_dark ? 0.035f : 0.03f));
    Draw(L"Liquid Dock", g_fNavTitle.Get(), { 22, 18, kNav, 44 }, Text());
    Draw(L"버전 " LD_VERSION_WSTR, g_fHint.Get(), { 22, 42, kNav, 60 }, Muted());
    for (int i = 0; i < (int)g_pages.size(); ++i) {
        D2D1_RECT_F r = { 10, 78 + i * 44.f, kNav - 10, 78 + i * 44.f + 38 };
        bool sel = i == g_page, hot = g_hover == -10 - i;
        if (sel || hot) Fill(r, Wash(sel ? 0.09f : 0.05f), 6);
        if (sel) Fill({ r.left, r.top + 10, r.left + 3, r.bottom - 10 }, g_accent, 1.5f);
        Draw(g_pages[i].glyph, g_fGlyph.Get(), { r.left + 14, r.top, r.left + 40, r.bottom }, sel ? Text() : Muted());
        Draw(g_pages[i].title, g_fNav.Get(), { r.left + 44, r.top, r.right, r.bottom }, sel ? Text() : Text(0.85f));
        g_hots.push_back({ r, -10 - i, 0 });
    }

    // page
    const Page& page = g_pages[g_page];
    float x0 = kNav + kPad, x1 = kW - kPad;
    dc->PushAxisAlignedClip({ kNav, 0, kW, kH }, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = 26 - g_scroll;
    Draw(page.title, g_fTitle.Get(), { x0, y, x1, y + 40 }, Text());
    y += 62;
    for (int ci = 0; ci < (int)page.controls.size(); ++ci) {
        const Control& c = page.controls[ci];
        bool hot = g_hover == ci;
        switch (c.type) {
        case Control::Segmented: {
            Draw(c.label, g_fLabel.Get(), { x0, y, x1, y + 22 }, Text());
            float top = y + 28;
            if (c.hint) { Draw(c.hint, g_fHint.Get(), { x0, top - 4, x1, top + 14 }, Muted()); top += 18; }
            D2D1_RECT_F bar = { x0, top, x1, top + 36 };
            Fill(bar, Wash(0.06f), 6);
            int n = (int)c.options.size(), cur = c.index();
            float w = (bar.right - bar.left - 4) / n;
            for (int i = 0; i < n; ++i) {
                D2D1_RECT_F r = { bar.left + 2 + i * w, bar.top + 2, bar.left + 2 + (i + 1) * w, bar.bottom - 2 };
                if (i == cur) Fill(r, g_dark ? ColorF(1, 1, 1, 0.16f) : ColorF(1, 1, 1, 0.95f), 4);
                else if (hot && g_hoverSub == i) Fill(r, Wash(0.05f), 4);
                Draw(c.options[i], (r.right - r.left) < wcslen(c.options[i]) * 8.5f ? g_fSegSmall.Get() : g_fSeg.Get(), r, i == cur ? Text() : Text(0.8f));
                if (i == cur) Fill({ (r.left + r.right) / 2 - 8, r.bottom - 3, (r.left + r.right) / 2 + 8, r.bottom - 1 }, g_accent, 1);
                g_hots.push_back({ r, ci, i });
            }
            break;
        }
        case Control::Slider: {
            float v = c.value();
            Draw(c.label, g_fLabel.Get(), { x0, y, x1 - 140, y + 22 }, Text());
            g_brush->SetColor(g_accent);
            Draw(c.text(v), g_fValue.Get(), { x1 - 200, y, x1, y + 22 }, g_accent);
            float top = y + 26;
            if (c.hint) { Draw(c.hint, g_fHint.Get(), { x0, top - 2, x1, top + 16 }, Muted()); top += 18; }
            float cy = top + 16, t = (std::clamp(v, c.lo, c.hi) - c.lo) / (c.hi - c.lo);
            D2D1_RECT_F track = { x0 + 10, cy - 2, x1 - 10, cy + 2 };
            float kx = track.left + (track.right - track.left) * t;
            Fill(track, Wash(0.22f), 2);
            Fill({ track.left, track.top, kx, track.bottom }, g_accent, 2);
            float kr = (hot || g_drag == ci) ? 10.f : 9.f;
            g_brush->SetColor(g_dark ? ColorF(0.27f, 0.27f, 0.29f) : ColorF(1, 1, 1));
            dc->FillEllipse({ { kx, cy }, kr, kr }, g_brush.Get());
            g_brush->SetColor(Wash(0.12f));
            dc->DrawEllipse({ { kx, cy }, kr, kr }, g_brush.Get(), 1);
            g_brush->SetColor(g_accent);
            float inner = g_drag == ci ? 4.5f : hot ? 6 : 5;
            dc->FillEllipse({ { kx, cy }, inner, inner }, g_brush.Get());
            if (ci < 32) g_sliderTrack[ci] = track;
            g_hots.push_back({ { x0, cy - 16, x1, cy + 16 }, ci, 0 });
            break;
        }
        case Control::Toggle: {
            bool on = c.on();
            float sw = 44, sh = 22, tx = x1 - sw, ty = y + 6;
            Draw(c.label, g_fLabel.Get(), { x0, y, tx - 12, y + 22 }, Text());
            if (c.hint) Draw(c.hint, g_fHint.Get(), { x0, y + 24, tx - 12, y + 42 }, Muted());
            D2D1_RECT_F r = { tx, ty, tx + sw, ty + sh };
            if (on) Fill(r, g_accent, sh / 2);
            else { Fill(r, Wash(hot ? 0.08f : 0.03f), sh / 2); g_brush->SetColor(Muted()); dc->DrawRoundedRectangle({ { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, sh / 2, sh / 2 }, g_brush.Get(), 1); }
            float kx = on ? r.right - sh / 2 : r.left + sh / 2, kr = hot ? 7.f : 6.f;
            g_brush->SetColor(on ? (g_dark ? ColorF(0, 0, 0, 0.85f) : ColorF(1, 1, 1)) : Muted());
            dc->FillEllipse({ { kx, ty + sh / 2 }, kr, kr }, g_brush.Get());
            g_hots.push_back({ { x0, y - 4, x1, y + RowHeight(c) - 12 }, ci, 0 });
            break;
        }
        case Control::Button: {
            Draw(c.label, g_fLabel.Get(), { x0, y, x1, y + 22 }, Text());
            if (c.hint) Draw(c.hint, g_fHint.Get(), { x0, y + 24, x1, y + 42 }, Muted());
            D2D1_RECT_F r = { x1 - 96, y + 2, x1, y + 34 };
            Fill(r, Wash(hot ? 0.12f : 0.07f), 6);
            Draw(L"종료", g_fSeg.Get(), r, Text());
            g_hots.push_back({ r, ci, 0 });
            break;
        }
        }
        y += RowHeight(c);
    }
    if (page.reset) {
        y += 4;
        D2D1_RECT_F r = { x0, y, x0 + 150, y + 34 };
        Fill(r, Wash(g_hover == -2 ? 0.12f : 0.07f), 6);
        Draw(L"이 탭 기본값으로", g_fSeg.Get(), r, Text());
        g_hots.push_back({ r, -2, 0 });
        y += 50;
    }
    g_contentH = y + g_scroll;
    dc->PopAxisAlignedClip();

    HRESULT hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    dc->SetDpi(oldX, oldY);
    dc->SetUnitMode(oldUnits);
    if (FAILED(hr)) { Log(L"settings EndDraw %08x", hr); return; }
    g_swap->Present(1, 0);
}

// ---------------------------------------------------------------- input

float ClientH() { RECT rc; GetClientRect(g_wnd, &rc); return rc.bottom * 96.f / g_dpi; }
void ClampScroll() { g_scroll = std::clamp(g_scroll, 0.0f, std::max(0.0f, g_contentH - ClientH())); }

void Changed(const Control& c, bool released) {
    switch (c.apply) {
    case Apply::Live: PostMessageW(g_mainWnd, WM_APP_RELAYOUT, 0, 0); break;
    case Apply::Rebuild: PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0); break;
    case Apply::OnRelease: if (released) PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0); break;
    case Apply::None: break;
    }
    if (released) g_settings.Save();
}

void SetSlider(int ci, float x) {
    const Control& c = g_pages[g_page].controls[ci];
    const auto& t = g_sliderTrack[ci];
    float v = c.lo + std::clamp((x - t.left) / (t.right - t.left), 0.0f, 1.0f) * (c.hi - c.lo);
    v = c.lo + std::round((v - c.lo) / c.step) * c.step;
    v = std::clamp(std::round(v * 1000) / 1000, c.lo, c.hi);
    if (std::fabs(v - c.value()) < c.step * 0.01f) return;
    c.set(v);
    Changed(c, false);
}

const Hot* HotAt(float x, float y) {
    for (auto& h : g_hots) if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return &h;
    return nullptr;
}

void Press(const Hot& h, float x) {
    if (h.control <= -10) { g_page = -10 - h.control; g_scroll = 0; return; }
    const Page& page = g_pages[g_page];
    if (h.control == -2) { page.reset(); g_settings.Save(); PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0); return; }
    const Control& c = page.controls[h.control];
    switch (c.type) {
    case Control::Segmented: if (c.index() != h.sub) { c.choose(h.sub); Changed(c, true); } break;
    case Control::Slider: g_drag = h.control; SetCapture(g_wnd); SetSlider(h.control, x); break;
    case Control::Toggle: c.toggle(!c.on()); Changed(c, true); break;
    case Control::Button: c.press(); break;
    }
}

// ---------------------------------------------------------------- size

constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, kExStyle = WS_EX_NOREDIRECTIONBITMAP;
HMONITOR g_scaledFor = nullptr;

// Same rule as the dock: sized by the screen, not by Windows' scale setting (1.0 = a 1440 px short side).
float ContentDpi(HMONITOR m) {
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(m, &mi);
    float shortSide = (float)std::min(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    return 96 * std::max(0.72f, shortSide / 1440.0f);
}

// Scales the content for monitor m and resizes the window around it, keeping its top-left at (x, y)
// unless centre is set. The title bar keeps Windows' own size for that monitor.
void ScaleFor(HMONITOR m, int x, int y, bool centre) {
    g_scaledFor = m;
    g_dpi = ContentDpi(m);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(m, &mi);
    float workH = (mi.rcWork.bottom - mi.rcWork.top) * 96.f / g_dpi - 60;
    RECT rc = { 0, 0, (LONG)std::ceil(kW * g_dpi / 96), (LONG)std::ceil(std::min(kH, workH) * g_dpi / 96) };
    AdjustWindowRectExForDpi(&rc, kStyle, FALSE, kExStyle, GetDpiForWindow(g_wnd));
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (centre) { x = (mi.rcWork.left + mi.rcWork.right - w) / 2; y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2; }
    SetWindowPos(g_wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_swap) { g_target.Reset(); CreateTarget(); ClampScroll(); Render(); }   // new DPI even if the size stayed
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto dip = [&](LPARAM p, float& x, float& y) { x = GET_X_LPARAM(p) * 96.f / g_dpi; y = GET_Y_LPARAM(p) * 96.f / g_dpi; };
    switch (m) {
    case WM_MOUSEMOVE: {
        float x, y; dip(l, x, y);
        if (g_drag >= 0) { SetSlider(g_drag, x); Render(); return 0; }
        const Hot* hot = HotAt(x, y);
        int nh = hot ? hot->control : -1, ns = hot ? hot->sub : -1;
        if (nh != g_hover || ns != g_hoverSub) { g_hover = nh; g_hoverSub = ns; Render(); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover != -1) { g_hover = g_hoverSub = -1; Render(); } return 0;
    case WM_LBUTTONDOWN: {
        float x, y; dip(l, x, y);
        if (const Hot* hot = HotAt(x, y)) { Hot copy = *hot; Press(copy, x); }
        Render();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag >= 0) {
            Changed(g_pages[g_page].controls[g_drag], true);
            g_drag = -1; ReleaseCapture(); Render();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_drag >= 0 && (HWND)l != h) { Changed(g_pages[g_page].controls[g_drag], true); g_drag = -1; Render(); }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        float x = p.x * 96.f / g_dpi, y = p.y * 96.f / g_dpi;
        int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        const Hot* hot = HotAt(x, y);
        if (hot && hot->control >= 0 && g_pages[g_page].controls[hot->control].type == Control::Slider) {
            // the wheel nudges the slider under the pointer one step at a time
            const Control& c = g_pages[g_page].controls[hot->control];
            float v = std::clamp(c.value() + notches * c.step, c.lo, c.hi);
            c.set(std::round(v * 1000) / 1000);
            Changed(c, true);
        } else {
            g_scroll -= notches * 60; ClampScroll();
        }
        Render();
        return 0;
    }
    case WM_KEYDOWN: if (w == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_ACTIVATE: case WM_SETTINGCHANGE: case WM_DWMCOLORIZATIONCOLORCHANGED:
        if (m != WM_ACTIVATE) ReadAccent();
        Render();
        break;
    case WM_DPICHANGED: {   // moved to a monitor with another scale: size for that monitor's resolution
        auto r = (RECT*)l;
        ScaleFor(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), r->left, r->top, false);
        return 0;
    }
    case WM_EXITSIZEMOVE: {   // moved to a monitor with the same scale but another resolution
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        if (mon != g_scaledFor) { RECT r; GetWindowRect(h, &r); ScaleFor(mon, r.left, r.top, false); }
        return 0;
    }
    case WM_SIZE:
        if (g_swap) {
            g_target.Reset();
            g_swap->ResizeBuffers(0, std::max(1, (int)LOWORD(l)), std::max(1, (int)HIWORD(l)), DXGI_FORMAT_UNKNOWN, 0);
            CreateTarget(); ClampScroll(); Render();
        }
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Render(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (g_drag >= 0) { Changed(g_pages[g_page].controls[g_drag], true); g_drag = -1; }
        g_settings.Save();
        Dock::settingsOpen = false;
        Release();
        g_wnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}   // namespace

bool IsOpen() { return g_wnd != nullptr; }
void Close() { if (g_wnd) DestroyWindow(g_wnd); }
bool PreTranslate(MSG* msg) {
    if (!g_wnd || msg->hwnd != g_wnd || msg->message != WM_KEYDOWN || msg->wParam != VK_ESCAPE) return false;
    DestroyWindow(g_wnd);
    return true;
}

void Show(HMONITOR monitor) {
    if (g_wnd) { ShowWindow(g_wnd, SW_RESTORE); SetForegroundWindow(g_wnd); return; }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"LiquidDockSettings";
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        registered = true;
    }
    g_monitor = monitor;
    g_dark = !theme::IsLight();
    ReadAccent();
    BuildPages();
    g_page = 0; g_scroll = 0; g_hover = g_hoverSub = g_drag = -1;

    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(monitor, &mi);
    // created on its monitor first (so Windows picks that monitor's title bar size), then sized
    g_wnd = CreateWindowExW(kExStyle, L"LiquidDockSettings", L"Liquid Dock 설정", kStyle,
                            mi.rcWork.left + 40, mi.rcWork.top + 40, 200, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_wnd) return;
    ScaleFor(monitor, 0, 0, true);
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    // the acrylic of the taskbar and flyouts behind the whole window, title bar included (Windows 11)
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_wnd, &margins);
    int backdrop = 3 /* DWMSBT_TRANSIENTWINDOW */;
    g_acrylic = SUCCEEDED(DwmSetWindowAttribute(g_wnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop)));
    if (!CreateResources()) { Log(L"settings window: no swap chain"); DestroyWindow(g_wnd); return; }
    Dock::settingsOpen = true;
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    Render();
}

}   // namespace settingswindow
