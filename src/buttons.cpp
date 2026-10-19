#include "buttons.h"
#include "gfx.h"
#include "system.h"
#include "oled.h"
#include "theme.h"
#include <shellapi.h>
#include <shlobj.h>

// ---------------------------------------------------------------- catalogue

static const ButtonInfo kInfo[kButtonCount] = {
    { ButtonId::Network,    "network",    L"네트워크",          L"Wi-Fi와 빠른 설정",                          true },
    { ButtonId::Volume,     "volume",     L"볼륨",              L"볼륨 조절, 휠로 바로 조절",                  true },
    { ButtonId::Desktop,    "desktop",    L"바탕화면 보기",     L"모든 창을 내리고 다시 누르면 되돌립니다",    false },
    { ButtonId::Theme,      "theme",      L"다크/라이트",       L"Windows와 앱의 모드를 한 번에 바꿉니다",     false },
    { ButtonId::Oled,       "oled",       L"OLED 모드",         L"검은 배경과 바탕화면 아이콘 숨김(번인 방지)", false },
    { ButtonId::Awake,      "awake",      L"화면 꺼짐 방지",    L"켜 둔 동안 화면이 꺼지거나 절전되지 않습니다", false },
    { ButtonId::Screenshot, "screenshot", L"스크린샷",          L"Windows 캡처 도구를 엽니다",                 false },
    { ButtonId::Quiet,      "quiet",      L"알림 튐 끄기",      L"켜 둔 동안 알림이 와도 아이콘이 튀지 않습니다", false },
    { ButtonId::Trash,      "trash",      L"휴지통",            L"파일을 끌어다 놓으면 휴지통으로, 우클릭으로 비우기", false },
    { ButtonId::Downloads,  "downloads",  L"다운로드",          L"최근 받은 파일 목록",                        false },
    { ButtonId::Audio,      "audio",      L"오디오 장치",       L"스피커와 헤드셋 사이를 바로 바꿉니다",       false },
    // always available, with a generous cell independent of crowded app icons
    { ButtonId::Settings,   "settings",   L"설정",              L"Liquid Dock 설정",                           true, 0.65f, 32 },
};

const ButtonInfo& Info(ButtonId id) { return kInfo[std::clamp((int)id, 0, kButtonCount - 1)]; }

const ButtonInfo* FindButton(const std::string& key) {
    for (auto& i : kInfo) if (key == i.key) return &i;
    return nullptr;
}

std::vector<ButtonId> ParseButtons(const std::string& list) {
    std::vector<ButtonId> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        std::string key = list.substr(pos, comma - pos);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        if (auto b = FindButton(key); b && !b->fixed && std::find(out.begin(), out.end(), b->id) == out.end()) out.push_back(b->id);
        pos = comma + 1;
    }
    return out;
}

std::string FormatButtons(const std::vector<ButtonId>& ids) {
    std::string s;
    for (auto id : ids) { if (!s.empty()) s += ','; s += Info(id).key; }
    return s;
}

extern Settings g_settings;

const std::vector<ButtonId>& ChosenButtons() {
    static std::string parsedFrom = "\x01";
    static std::vector<ButtonId> ids;
    if (g_settings.trayButtons != parsedFrom) { parsedFrom = g_settings.trayButtons; ids = ParseButtons(parsedFrom); }
    return ids;
}

bool HasButton(ButtonId id) {
    if (Info(id).fixed) return true;
    auto& ids = ChosenButtons();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void SetButton(ButtonId id, bool on) {
    if (Info(id).fixed || HasButton(id) == on) return;
    auto ids = ChosenButtons();
    if (on) ids.push_back(id); else ids.erase(std::find(ids.begin(), ids.end(), id));
    g_settings.trayButtons = FormatButtons(ids);
}

float PlanButtons(const std::vector<ButtonId>& ids, float icon, float scale, bool stack, float x, std::vector<PlannedCell>* out) {
    if (out) out->clear();
    int waiting = -1; float waitingW = 0;
    for (ButtonId id : ids) {
        const auto& bi = Info(id);
        float w = std::max(icon * bi.iconFactor, bi.minWidth * scale);
        if (!stack) { if (out) out->push_back({ id, x, x + w, -1 }); x += w; continue; }
        if (waiting < 0) { waitingW = w; if (out) { waiting = (int)out->size(); out->push_back({ id, x, x + w, -1 }); } else waiting = 0; continue; }
        float cell = std::max(waitingW, w);
        if (out) { (*out)[waiting].u1 = x + cell; (*out)[waiting].half = 0; out->push_back({ id, x, x + cell, 1 }); }
        x += cell; waiting = -1;
    }
    return waiting >= 0 ? x + waitingW : x;
}

// ---------------------------------------------------------------- live state

namespace features {
static bool g_awake = false;
static double g_quietUntil = 0;
static bool g_trashFull = false;
static double g_nextTrashPoll = 0;

bool Awake() { return g_awake; }
void SetAwake(bool on) {
    g_awake = on;
    // the display and the system stay on while this thread (the dock's UI thread) asks for it
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
    Log(L"awake: %d", (int)on);
}
bool Quiet(double now) { return now < g_quietUntil; }
void SetQuiet(double until) { g_quietUntil = until; }
bool TrashFull() { return g_trashFull; }
void Tick(double now) {
    if (now < g_nextTrashPoll) return;
    g_nextTrashPoll = now + 5;
    SHQUERYRBINFO q = { sizeof(q) };
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &q))) g_trashFull = q.i64NumItems > 0;
}
std::wstring DownloadsFolder() {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) s = p;
    CoTaskMemFree(p);
    return s;
}

std::vector<RecentFile> RecentDownloads(size_t max) {
    struct Entry { RecentFile f; ULONGLONG t; };
    std::vector<Entry> all;
    std::wstring dir = DownloadsFolder();
    if (dir.empty()) return {};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        std::wstring name = fd.cFileName, low = ToLower(name);
        bool partial = false;
        for (const wchar_t* ext : { L".crdownload", L".part", L".tmp", L".partial", L".download" })
            if (low.size() > wcslen(ext) && low.compare(low.size() - wcslen(ext), std::wstring::npos, ext) == 0) partial = true;
        if (partial || low == L"desktop.ini") continue;
        // the later of "written" and "created": a file copied in keeps an old write time
        ULONGLONG w = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        ULONGLONG c = ((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime;
        all.push_back({ { dir + L"\\" + name, name, L"" }, std::max(w, c) });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return a.t > b.t; });
    if (all.size() > max) all.resize(max);
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    std::vector<RecentFile> out;
    for (auto& e : all) {
        long long mins = now > e.t ? (long long)((now - e.t) / 600000000ULL) : 0;
        wchar_t b[32];
        if (mins < 1) wcscpy_s(b, L"방금");
        else if (mins < 60) swprintf_s(b, L"%lld분 전", mins);
        else if (mins < 60 * 24) swprintf_s(b, L"%lld시간 전", mins / 60);
        else swprintf_s(b, L"%lld일 전", mins / (60 * 24));
        e.f.ago = b;
        out.push_back(e.f);
    }
    return out;
}}   // namespace features

IconState CurrentIconState(double now) {
    IconState s;
    s.net = g_sys.net; s.volume = g_sys.volume; s.muted = g_sys.muted;
    s.oled = oled::Active(); s.light = theme::IsLight();
    s.awake = features::Awake(); s.quiet = features::Quiet(now); s.trashFull = features::TrashFull();
    return s;
}

// ---------------------------------------------------------------- icons

namespace {
struct Pen {
    ID2D1DeviceContext* dc; ID2D1SolidColorBrush* brush; D2D1_POINT_2F c; float k;
    D2D1_POINT_2F P(float x, float y) const { return { c.x + x * k, c.y + y * k }; }
    void Line(float x0, float y0, float x1, float y1, float w) const { dc->DrawLine(P(x0, y0), P(x1, y1), brush, w * k); }
};

void Glyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, IDWriteTextFormat* f, const wchar_t* g, D2D1_RECT_F r,
           float zoom, D2D1_COLOR_F ink, float shadow) {
    D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
    if (zoom != 1) dc->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom, { (r.left + r.right) / 2, (r.top + r.bottom) / 2 }) * old);
    UINT32 n = (UINT32)wcslen(g);
    if (shadow > 0) {
        float off = std::max(1.0f, (r.bottom - r.top) / 40);
        brush->SetColor(D2D1::ColorF(0, 0, 0, shadow));
        dc->DrawTextW(g, n, f, { r.left, r.top + off, r.right, r.bottom + off }, brush);
    }
    brush->SetColor(ink);
    dc->DrawTextW(g, n, f, r, brush);
    dc->SetTransform(old);
}
}

void DrawButtonIcon(ButtonId id, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, IDWriteTextFormat* glyphFont,
                    D2D1_RECT_F r, float scale, float zoom, D2D1_COLOR_F ink, float shadow, const IconState& st) {
    D2D1_POINT_2F c = { (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f };
    float s = scale * zoom;
    auto glyph = [&](const wchar_t* g) { Glyph(dc, brush, glyphFont, g, r, zoom, ink, shadow); };
    brush->SetColor(ink);
    switch (id) {
    case ButtonId::Network: glyph(st.net == 0 ? L"" : (st.net == 2 ? L"" : L"")); break;
    case ButtonId::Volume:
        glyph(st.muted ? L"" : st.volume < 0.01f ? L"" : st.volume < 0.34f ? L"" : st.volume < 0.67f ? L"" : L"");
        break;
    case ButtonId::Settings: glyph(L""); break;
    case ButtonId::Screenshot: glyph(L""); break;
    case ButtonId::Downloads: glyph(L""); break;
    case ButtonId::Audio: glyph(L""); break;
    case ButtonId::Trash: {
        glyph(L"");
        if (st.trashFull) {   // something to empty: a small dot over the can
            brush->SetColor(ink);
            dc->FillEllipse({ { c.x + 6.5f * s, c.y - 6.5f * s }, 2.2f * s, 2.2f * s }, brush);
        }
        break;
    }
    case ButtonId::Quiet: {
        // a bell; struck through while bounces are off
        glyph(L"");
        if (st.quiet) {
            brush->SetColor(ink);
            dc->DrawLine({ c.x - 8 * s, c.y - 8 * s }, { c.x + 8 * s, c.y + 8 * s }, brush, 1.6f * s);
        }
        break;
    }
    case ButtonId::Awake: {
        // a cup; steam rises while the screen is kept on
        Pen p{ dc, brush, c, s };
        float w = 1.5f;
        ComPtr<ID2D1PathGeometry> cup; ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(g_gfx.d2dFactory->CreatePathGeometry(&cup)) || FAILED(cup->Open(&sink))) break;
        sink->BeginFigure(p.P(-6, -1), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(p.P(-5.2f, 5.5f));
        sink->AddQuadraticBezier({ p.P(-4.8f, 7.5f), p.P(-2.8f, 7.5f) });
        sink->AddLine(p.P(2.8f, 7.5f));
        sink->AddQuadraticBezier({ p.P(4.8f, 7.5f), p.P(5.2f, 5.5f) });
        sink->AddLine(p.P(6, -1));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        dc->DrawGeometry(cup.Get(), brush, w * s);
        dc->DrawEllipse({ p.P(7.6f, 2.6f), 2.4f * s, 2.6f * s }, brush, w * s);   // handle
        if (st.awake) {
            dc->FillGeometry(cup.Get(), brush);
            for (float x : { -2.5f, 1.5f }) {
                ComPtr<ID2D1PathGeometry> steam; ComPtr<ID2D1GeometrySink> ss;
                if (FAILED(g_gfx.d2dFactory->CreatePathGeometry(&steam)) || FAILED(steam->Open(&ss))) continue;
                ss->BeginFigure(p.P(x, -3.5f), D2D1_FIGURE_BEGIN_HOLLOW);
                ss->AddBezier({ p.P(x - 1.8f, -5.5f), p.P(x + 1.8f, -7.5f), p.P(x, -9.5f) });
                ss->EndFigure(D2D1_FIGURE_END_OPEN);
                ss->Close();
                dc->DrawGeometry(steam.Get(), brush, w * s);
            }
        }
        break;
    }
    case ButtonId::Desktop: {
        // a monitor outline
        auto screen = D2D1::RectF(c.x - 8 * s, c.y - 6 * s, c.x + 8 * s, c.y + 4 * s);
        dc->DrawRoundedRectangle({ screen, 1.5f * s, 1.5f * s }, brush, 1.4f * s);
        dc->DrawLine({ c.x, c.y + 4 * s }, { c.x, c.y + 7 * s }, brush, 1.4f * s);
        dc->DrawLine({ c.x - 4 * s, c.y + 7 * s }, { c.x + 4 * s, c.y + 7 * s }, brush, 1.4f * s);
        break;
    }
    case ButtonId::Oled: {
        // three bars (an OLED subpixel: red, green, blue) leaning 35 degrees in one rounded outline; ink while off
        float w = 13 * s, h = 15 * s;
        D2D1_RECT_F b = { c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2 };
        ComPtr<ID2D1RoundedRectangleGeometry> shape;
        if (FAILED(g_gfx.d2dFactory->CreateRoundedRectangleGeometry({ b, 2.6f * s, 2.6f * s }, &shape))) break;
        D2D1_COLOR_F col[3];
        if (st.oled) { col[0] = D2D1::ColorF(0xFF3B30, ink.a); col[1] = D2D1::ColorF(0x5BC22A, ink.a); col[2] = D2D1::ColorF(0x4459F0, ink.a); }
        else { const float a[3] = { 0.55f, 0.75f, 0.95f }; for (int i = 0; i < 3; ++i) { col[i] = ink; col[i].a *= a[i]; } }
        D2D1_GRADIENT_STOP stops[6] = { { 0, col[0] }, { 1 / 3.0f, col[0] }, { 1 / 3.0f, col[1] }, { 2 / 3.0f, col[1] }, { 2 / 3.0f, col[2] }, { 1, col[2] } };
        ComPtr<ID2D1GradientStopCollection> coll; ComPtr<ID2D1LinearGradientBrush> fill;
        if (FAILED(dc->CreateGradientStopCollection(stops, 6, &coll)) ||
            FAILED(dc->CreateLinearGradientBrush({ { b.left, c.y }, { b.right, c.y } }, coll.Get(), &fill))) break;
        D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
        dc->SetTransform(D2D1::Matrix3x2F::Skew(-35, 0, c) * old);
        dc->FillGeometry(shape.Get(), fill.Get());
        dc->SetTransform(old);
        break;
    }
    case ButtonId::Theme: {
        // a bulb hung upside down: outline in dark mode, lit with rays in light mode
        float k = 0.55f * s;
        auto pt = [&](float x, float y) { return D2D1::Point2F(c.x + x * k, c.y + y * k); };
        auto inkT = ink; if (!st.light) inkT.a *= 0.8f;
        float line = 1.8f * k;
        ComPtr<ID2D1PathGeometry> glass; ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(g_gfx.d2dFactory->CreatePathGeometry(&glass)) || FAILED(glass->Open(&sink))) break;
        sink->BeginFigure(pt(-4.5f, -7), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(pt(-4.5f, -4.61f));
        sink->AddArc({ pt(4.5f, -4.61f), { 8 * k, 8 * k }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE });
        sink->AddLine(pt(4.5f, -7));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) break;
        brush->SetColor(inkT);
        dc->DrawLine(pt(0, -10.5f), pt(0, -19), brush, line);
        dc->DrawLine(pt(-3.5f, -10.5f), pt(3.5f, -10.5f), brush, line);
        if (st.light) {
            dc->FillGeometry(glass.Get(), brush);
            const float rays[5][4] = { { 0, 15, 0, 19 }, { -9.2f, 11.2f, -12, 14 }, { 9.2f, 11.2f, 12, 14 }, { -12, 2, -16, 2 }, { 12, 2, 16, 2 } };
            for (auto& ray : rays) dc->DrawLine(pt(ray[0], ray[1]), pt(ray[2], ray[3]), brush, line);
        } else {
            dc->DrawGeometry(glass.Get(), brush, line);
        }
        break;
    }
    default: break;
    }
}
