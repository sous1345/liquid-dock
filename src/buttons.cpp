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
    { ButtonId::Sff,        "sff",        L"SFF 갤러리",        L"디시인사이드 SFF 마이너 갤러리를 엽니다",    false },
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
    // the Recycle Bin query takes a few milliseconds: only while the trash button is on the dock
    if (now < g_nextTrashPoll || !HasButton(ButtonId::Trash)) return;
    g_nextTrashPoll = now + 10;
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

std::vector<RecentFile> RecentDownloads(size_t max) { return RecentFilesIn(DownloadsFolder(), max); }

std::vector<RecentFile> RecentFilesIn(const std::wstring& dir, size_t max) {
    struct Entry { RecentFile f; ULONGLONG t; };
    std::vector<Entry> all;
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

// The SFF gallery mark (an isometric cube), traced from the gallery cover: outlines in units of half its
// height, centred on the origin; filled with the alternate rule, so the inner outlines are holes.
const D2D1_POINT_2F kSff0[] = {
    {-0.006f,-0.983f}, {-0.112f,-0.961f}, {-0.302f,-0.850f}, {-0.373f,-0.819f}, {-0.432f,-0.780f}, {-0.740f,-0.619f},
    {-0.858f,-0.546f}, {-0.910f,-0.485f}, {-0.922f,-0.450f}, {-0.930f,-0.367f}, {-0.930f,0.331f}, {-0.922f,0.438f},
    {-0.905f,0.492f}, {-0.811f,0.574f}, {-0.751f,0.600f}, {-0.692f,0.640f}, {-0.621f,0.670f}, {-0.574f,0.704f},
    {-0.505f,0.734f}, {-0.314f,0.845f}, {-0.254f,0.870f}, {-0.183f,0.916f}, {-0.077f,0.970f}, {-0.030f,0.983f},
    {0.030f,0.983f}, {0.077f,0.971f}, {0.822f,0.570f}, {0.893f,0.509f}, {0.910f,0.485f}, {0.930f,0.391f},
    {0.932f,-0.367f}, {0.920f,-0.462f}, {0.897f,-0.509f}, {0.858f,-0.548f}, {0.750f,-0.615f}, {0.680f,-0.646f},
    {0.620f,-0.686f}, {0.434f,-0.781f}, {0.243f,-0.892f}, {0.183f,-0.917f}, {0.136f,-0.950f}, {0.077f,-0.973f},
};
const D2D1_POINT_2F kSff1[] = {
    {0.006f,-0.805f}, {-0.041f,-0.797f}, {-0.136f,-0.762f}, {-0.195f,-0.721f}, {-0.290f,-0.675f}, {-0.349f,-0.628f},
    {-0.361f,-0.604f}, {-0.337f,-0.572f}, {-0.231f,-0.514f}, {-0.195f,-0.503f}, {-0.148f,-0.505f}, {-0.065f,-0.549f},
    {0.006f,-0.561f}, {0.065f,-0.550f}, {0.136f,-0.508f}, {0.195f,-0.501f}, {0.243f,-0.515f}, {0.349f,-0.577f},
    {0.365f,-0.604f}, {0.349f,-0.633f}, {0.278f,-0.686f}, {0.207f,-0.718f}, {0.136f,-0.764f},
};
const D2D1_POINT_2F kSff2[] = {
    {-0.550f,-0.523f}, {-0.633f,-0.490f}, {-0.673f,-0.462f}, {-0.707f,-0.414f}, {-0.704f,-0.391f}, {-0.694f,-0.379f},
    {-0.621f,-0.347f}, {-0.065f,-0.044f}, {-0.030f,-0.035f}, {0.018f,-0.033f}, {0.065f,-0.043f}, {0.538f,-0.297f},
    {0.692f,-0.375f}, {0.712f,-0.402f}, {0.699f,-0.438f}, {0.657f,-0.479f}, {0.550f,-0.523f}, {0.527f,-0.522f},
    {0.444f,-0.490f}, {0.254f,-0.378f}, {0.183f,-0.346f}, {0.124f,-0.305f}, {0.006f,-0.268f}, {-0.112f,-0.302f},
    {-0.183f,-0.349f}, {-0.373f,-0.446f}, {-0.432f,-0.487f}, {-0.527f,-0.522f},
};
const D2D1_POINT_2F kSff3[] = {
    {-0.716f,-0.226f}, {-0.747f,-0.213f}, {-0.759f,-0.189f}, {-0.755f,-0.059f}, {-0.669f,0.018f}, {-0.570f,0.071f},
    {-0.531f,0.130f}, {-0.521f,0.189f}, {-0.533f,0.237f}, {-0.562f,0.257f}, {-0.621f,0.251f}, {-0.716f,0.210f},
    {-0.738f,0.213f}, {-0.751f,0.225f}, {-0.757f,0.331f}, {-0.733f,0.402f}, {-0.704f,0.432f}, {-0.586f,0.500f},
    {-0.515f,0.530f}, {-0.456f,0.570f}, {-0.278f,0.658f}, {-0.077f,0.774f}, {0.018f,0.786f}, {0.089f,0.770f},
    {0.716f,0.426f}, {0.744f,0.391f}, {0.757f,0.355f}, {0.754f,0.225f}, {0.740f,0.212f}, {0.716f,0.210f},
    {0.621f,0.254f}, {0.562f,0.256f}, {0.537f,0.237f}, {0.526f,0.166f}, {0.546f,0.107f}, {0.574f,0.071f},
    {0.680f,0.015f}, {0.752f,-0.047f}, {0.764f,-0.083f}, {0.764f,-0.178f}, {0.753f,-0.213f}, {0.740f,-0.224f},
    {0.704f,-0.227f}, {0.659f,-0.213f}, {0.479f,-0.105f}, {0.408f,-0.075f}, {0.349f,-0.035f}, {0.278f,-0.002f},
    {0.101f,0.102f}, {0.041f,0.119f}, {-0.006f,0.122f}, {-0.101f,0.099f}, {-0.148f,0.065f}, {-0.467f,-0.102f},
    {-0.527f,-0.142f}, {-0.598f,-0.173f}, {-0.657f,-0.214f},
};
const D2D1_POINT_2F kSff4[] = {
    {-0.266f,0.236f}, {-0.302f,0.256f}, {-0.316f,0.296f}, {-0.312f,0.379f}, {-0.296f,0.426f}, {-0.278f,0.443f},
    {-0.219f,0.469f}, {-0.148f,0.515f}, {-0.053f,0.559f}, {-0.018f,0.572f}, {0.030f,0.571f}, {0.160f,0.512f},
    {0.254f,0.457f}, {0.300f,0.426f}, {0.318f,0.379f}, {0.319f,0.284f}, {0.290f,0.244f}, {0.243f,0.243f},
    {0.148f,0.301f}, {0.041f,0.345f}, {0.006f,0.351f}, {-0.030f,0.347f}, {-0.124f,0.310f}, {-0.231f,0.245f},
};
struct Outline { const D2D1_POINT_2F* p; size_t n; };
const Outline kSffMark[] = { { kSff0, std::size(kSff0) }, { kSff1, std::size(kSff1) }, { kSff2, std::size(kSff2) }, { kSff3, std::size(kSff3) }, { kSff4, std::size(kSff4) } };

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
    case ButtonId::Sff: {
        // the gallery's cube mark, as tall as the other icons
        float k = 8.6f * s;
        ComPtr<ID2D1PathGeometry> mark; ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(g_gfx.d2dFactory->CreatePathGeometry(&mark)) || FAILED(mark->Open(&sink))) break;
        sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        for (auto& o : kSffMark) {
            sink->BeginFigure({ c.x + o.p[0].x * k, c.y + o.p[0].y * k }, D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < o.n; ++i) sink->AddLine({ c.x + o.p[i].x * k, c.y + o.p[i].y * k });
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }
        if (FAILED(sink->Close())) break;
        dc->FillGeometry(mark.Get(), brush);
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
