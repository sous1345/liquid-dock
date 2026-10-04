#include "apps.h"
#include "gfx.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <fstream>
#include <sstream>

AppModel g_apps;
extern Settings g_settings;

// ---------------------------------------------------------------- helpers

static std::wstring WindowClass(HWND h) {
    wchar_t c[256] = {}; GetClassNameW(h, c, 256); return c;
}

static std::wstring ProcessExe(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t buf[MAX_PATH * 2]; DWORD n = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
        CloseHandle(p);
    }
    return ToLower(out);
}

static std::wstring ProcessAumid(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH]; UINT32 n = ARRAYSIZE(buf);
        if (GetApplicationUserModelId(p, &n, buf) == ERROR_SUCCESS) out = buf;
        CloseHandle(p);
    }
    return out;
}

static std::wstring WindowAumid(HWND h) {
    std::wstring out;
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
        PropVariantClear(&pv);
    }
    return out;
}

static bool IsCloaked(HWND h) {
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked != 0;
}

// The same heuristic the shell uses to decide whether a window gets a taskbar button.
static bool IsTaskbarWindow(HWND h) {
    if (!IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    HWND owner = GetWindow(h, GW_OWNER);
    if (ex & WS_EX_APPWINDOW) { /* always */ }
    else {
        if (owner) return false;
        if (ex & WS_EX_TOOLWINDOW) return false;
        if (ex & WS_EX_NOACTIVATE) return false;
    }
    if (IsCloaked(h)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    std::wstring cls = WindowClass(h);
    if (cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd")
        return false;
    if (GetWindowTextLengthW(h) == 0 && cls != L"CabinetWClass") return false;
    return true;
}

static std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return {};
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return {};
    struct LangCp { WORD lang, cp; } *tr = nullptr; UINT trLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &trLen) || trLen < sizeof(LangCp)) return {};
    wchar_t q[128];
    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
    wchar_t* val = nullptr; UINT vlen = 0;
    if (VerQueryValueW(buf.data(), q, (void**)&val, &vlen) && vlen > 1) return std::wstring(val);
    return {};
}

static std::wstring FileStem(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring n = s == std::wstring::npos ? path : path.substr(s + 1);
    size_t d = n.find_last_of(L'.');
    if (d != std::wstring::npos) n = n.substr(0, d);
    return n;
}

static bool EndsWithI(const std::wstring& s, const wchar_t* suf) {
    size_t n = wcslen(suf);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

static ComPtr<ID2D1Bitmap1> BitmapFromHBITMAP(HBITMAP hbm) {
    DIBSECTION ds = {};
    if (!GetObjectW(hbm, sizeof(ds), &ds) || !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 32) {
        // not a DIB section: copy through GetDIBits
        BITMAP bm = {};
        if (!GetObjectW(hbm, sizeof(bm), &bm)) return nullptr;
        ds.dsBm = bm;
        ds.dsBmih.biHeight = -bm.bmHeight;
        std::vector<uint32_t> px(bm.bmWidth * bm.bmHeight);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth; bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        HDC hdc = GetDC(nullptr);
        GetDIBits(hdc, hbm, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, hdc);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        bool anyA = false;
        for (auto p : px) if (p >> 24) { anyA = true; break; }
        if (!anyA) for (auto& p : px) p |= 0xff000000;
        ComPtr<ID2D1Bitmap1> b;
        g_gfx.dc->CreateBitmap(D2D1::SizeU(bm.bmWidth, bm.bmHeight), px.data(), bm.bmWidth * 4, bp, &b);
        return b;
    }
    int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    bool bottomUp = ds.dsBmih.biHeight > 0;
    std::vector<uint32_t> px(w * h);
    for (int y = 0; y < h; ++y) {
        const uint32_t* src = (const uint32_t*)((const BYTE*)ds.dsBm.bmBits + ds.dsBm.bmWidthBytes * (bottomUp ? (h - 1 - y) : y));
        memcpy(&px[y * w], src, w * 4);
    }
    bool anyA = false, needPremul = false;
    for (auto p : px) {
        uint32_t a = p >> 24;
        if (a) anyA = true;
        if (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a) needPremul = true;
    }
    if (!anyA) for (auto& p : px) p |= 0xff000000;
    else if (needPremul) {
        for (auto& p : px) {
            uint32_t a = p >> 24;
            uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
            p = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> b;
    g_gfx.dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, bp, &b);
    return b;
}

static ComPtr<ID2D1Bitmap1> IconFromShellItem(IShellItem* si) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(si->QueryInterface(IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP hbm = nullptr;
    if (FAILED(f->GetImage({ 256, 256 }, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) || !hbm) return nullptr;
    auto b = BitmapFromHBITMAP(hbm);
    DeleteObject(hbm);
    return b;
}

// HICON -> premultiplied D2D bitmap at the icon's native size (handles alpha and mask-only icons).
static ComPtr<ID2D1Bitmap1> IconFromHICON(HICON hi) {
    if (!hi) return nullptr;
    ICONINFO ii = {};
    if (!GetIconInfo(hi, &ii)) return nullptr;
    ComPtr<ID2D1Bitmap1> result;
    BITMAP bm = {};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0) {
        int w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        std::vector<uint32_t> px(w * h), mask(w * h);
        HDC hdc = GetDC(nullptr);
        GetDIBits(hdc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);
        bool anyA = false;
        for (auto p : px) if (p >> 24) { anyA = true; break; }
        if (!anyA && ii.hbmMask) {
            GetDIBits(hdc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS);
            for (int i = 0; i < w * h; ++i) px[i] = (px[i] & 0xffffff) | ((mask[i] & 0xffffff) ? 0 : 0xff000000u);
        }
        ReleaseDC(nullptr, hdc);
        for (auto& p : px) {
            uint32_t a = p >> 24;
            uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
            p = (a << 24) | (r << 16) | (g << 8) | b;
        }
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        g_gfx.dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, bp, &result);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

// Largest icon in a PE/ico file (what the taskbar uses); avoids the shell's "small icon in a frame" thumbnails.
static ComPtr<ID2D1Bitmap1> IconFromFile(const std::wstring& path, int index) {
    if (path.empty()) return nullptr;
    wchar_t expanded[MAX_PATH * 2];
    ExpandEnvironmentStringsW(path.c_str(), expanded, ARRAYSIZE(expanded));
    HICON hi = nullptr; UINT id = 0;
    if (PrivateExtractIconsW(expanded, index, 256, 256, &hi, &id, 1, LR_DEFAULTCOLOR) != 1 || !hi) return nullptr;
    auto b = IconFromHICON(hi);
    DestroyIcon(hi);
    return b;
}

static ComPtr<IShellItem> AppsFolderItem(const std::wstring& aumid, std::wstring* nameOut) {
    if (aumid.empty()) return nullptr;
    ComPtr<IShellItem> si;
    if (FAILED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, aumid.c_str(), IID_PPV_ARGS(&si))))
        return nullptr;
    PWSTR dn = nullptr;
    std::wstring name;
    if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &dn))) { name = dn; CoTaskMemFree(dn); }
    if (name.empty() || name == aumid) return nullptr;   // not a real Start-menu entry
    if (nameOut) *nameOut = name;
    return si;
}

// ---------------------------------------------------------------- model

std::shared_ptr<AppItem> AppModel::NewItem() {
    auto a = std::make_shared<AppItem>();
    a->id = nextId++;
    return a;
}

static bool LinkIconLocation(const std::wstring& lnk, std::wstring& path, int& index) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    ComPtr<IPersistFile> pf; sl.As(&pf);
    if (FAILED(pf->Load(lnk.c_str(), STGM_READ))) return false;
    wchar_t p[MAX_PATH * 2] = {};
    if (FAILED(sl->GetIconLocation(p, ARRAYSIZE(p), &index)) || !p[0]) return false;
    path = p;
    return true;
}

void AppModel::LoadIconAndName(AppItem& a, HWND sample) {
    std::wstring nm;
    bool packaged = a.exe.empty() || a.exe.find(L"\\windowsapps\\") != std::wstring::npos;
    if (!packaged) {
        std::wstring ip; int ii = 0;
        if (EndsWithI(a.launch, L".lnk") && LinkIconLocation(a.launch, ip, ii)) a.icon = IconFromFile(ip, ii);
        if (!a.icon) a.icon = IconFromFile(a.exe, 0);
    }
    if (!a.icon && !a.aumid.empty()) {
        if (auto si = AppsFolderItem(a.aumid, &nm)) {
            a.icon = IconFromShellItem(si.Get());
            if (a.name.empty()) a.name = nm;
        }
    }
    if (!a.icon && EndsWithI(a.launch, L".lnk")) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(a.launch.c_str(), nullptr, IID_PPV_ARGS(&si)))) a.icon = IconFromShellItem(si.Get());
    }
    if (!a.icon && !a.exe.empty()) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(a.exe.c_str(), nullptr, IID_PPV_ARGS(&si)))) a.icon = IconFromShellItem(si.Get());
    }
    if (!a.icon && sample) {
        HICON hi = nullptr;
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(sample, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &res) && res) hi = (HICON)res;
        if (!hi) hi = (HICON)GetClassLongPtrW(sample, GCLP_HICON);
        a.icon = IconFromHICON(hi);
    }
    if (a.name.empty() && !a.exe.empty()) {
        a.name = FileDescription(a.exe);
        if (a.name.empty()) a.name = FileStem(a.exe);
    }
    if (a.name.empty() && sample) {
        wchar_t t[256] = {}; GetWindowTextW(sample, t, 256); a.name = t;
    }
}

void AppModel::ReloadIcons() {
    for (auto& a : items) {
        a->icon.Reset();
        LoadIconAndName(*a, a->windows.empty() ? nullptr : a->windows.front());
    }
    structureVersion++;
}

void AppModel::Init() {
    LoadPins();
    MigratePins();
    Refresh();
}

int AppModel::PinnedCount() const {
    int n = 0; for (auto& a : items) if (a->pinned) ++n; return n;
}

void AppModel::LoadPins() {
    std::wstring path = AppDataDir() + L"\\pins.txt";
    std::ifstream f(path);
    if (!f) {
        ImportTaskbarPins();
        SavePins();
        g_settings.pinsVersion = 2;
        g_settings.Save();
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        std::wstring w = FromUtf8(line);
        std::vector<std::wstring> parts;
        size_t s = 0, t;
        while ((t = w.find(L'\t', s)) != std::wstring::npos) { parts.push_back(w.substr(s, t - s)); s = t + 1; }
        parts.push_back(w.substr(s));
        if (parts.size() < 4) continue;
        auto a = NewItem();
        a->pinned = true;
        a->name = parts[0]; a->aumid = parts[1]; a->exe = ToLower(parts[2]); a->launch = parts[3];
        LoadIconAndName(*a, nullptr);
        items.push_back(a);
    }
    structureVersion++;
}

void AppModel::SavePins() {
    std::ofstream f(AppDataDir() + L"\\pins.txt", std::ios::trunc);
    for (auto& a : items) {
        if (!a->pinned) continue;
        f << ToUtf8(a->name + L"\t" + a->aumid + L"\t" + a->exe + L"\t" + a->launch) << "\n";
    }
}

static bool ReadShortcut(const std::wstring& lnk, std::wstring& exe, std::wstring& aumid) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    ComPtr<IPersistFile> pf; sl.As(&pf);
    if (FAILED(pf->Load(lnk.c_str(), STGM_READ))) return false;
    wchar_t target[MAX_PATH * 2] = {};
    if (SUCCEEDED(sl->GetPath(target, ARRAYSIZE(target), nullptr, 0)) && target[0]) {
        wchar_t expanded[MAX_PATH * 2];
        ExpandEnvironmentStringsW(target, expanded, ARRAYSIZE(expanded));
        exe = ToLower(expanded);
    }
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(sl.As(&ps))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) aumid = pv.pwszVal;
        PropVariantClear(&pv);
    }
    return true;
}

// The taskbar's own pin list (binary PIDL blob). Store (MSIX) apps are pinned only here - as
// "PackageFamily!AppId" - while desktop apps also have a .lnk; the blob gives the pin order.
static std::wstring TaskbandBlob() {
    DWORD size = 0;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"Favorites", RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2) return {};
    std::wstring s(size / 2, L'\0');
    RegGetValueW(HKEY_CURRENT_USER, key, L"Favorites", RRF_RT_REG_BINARY, nullptr, s.data(), &size);
    return s;
}

// AUMIDs of pinned store apps, in pin order, with their position in the blob
static std::vector<std::pair<size_t, std::wstring>> PackagedPins(const std::wstring& blob) {
    std::vector<std::pair<size_t, std::wstring>> out;
    auto isName = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_'; };
    for (size_t bang = blob.find(L'!'); bang != std::wstring::npos; bang = blob.find(L'!', bang + 1)) {
        size_t b = bang; while (b > 0 && isName(blob[b - 1])) --b;
        size_t e = bang + 1; while (e < blob.size() && isName(blob[e])) ++e;
        std::wstring fam = blob.substr(b, bang - b), app = blob.substr(bang + 1, e - bang - 1);
        size_t us = fam.rfind(L'_');   // PackageName_publisherId (13 chars)
        if (us == std::wstring::npos || fam.size() - us - 1 != 13 || us < 2 || app.empty()) continue;
        std::wstring id = fam + L"!" + app;
        if (std::none_of(out.begin(), out.end(), [&](auto& p) { return p.second == id; })) out.push_back({ b, id });
    }
    return out;
}

static std::wstring PackageFamily(const std::wstring& aumid) {
    size_t bang = aumid.find(L'!');
    return bang == std::wstring::npos ? std::wstring() : ToLower(aumid.substr(0, bang));
}

void AppModel::ImportTaskbarPins() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) return;
    std::wstring dir = std::wstring(p) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\";
    CoTaskMemFree(p);
    std::wstring blob = TaskbandBlob();
    std::vector<std::pair<size_t, std::shared_ptr<AppItem>>> found;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring lnk = dir + fd.cFileName;
            auto a = NewItem();
            a->pinned = true;
            a->name = FileStem(lnk);
            a->launch = lnk;
            ReadShortcut(lnk, a->exe, a->aumid);
            size_t pos = blob.find(fd.cFileName);
            found.push_back({ pos == std::wstring::npos ? SIZE_MAX : pos, a });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto& [pos, id] : PackagedPins(blob)) {
        auto a = NewItem();
        a->pinned = true;
        a->aumid = id;
        a->launch = L"shell:AppsFolder\\" + id;
        found.push_back({ pos, a });
    }
    std::stable_sort(found.begin(), found.end(), [](auto& x, auto& y) { return x.first < y.first; });
    for (auto& [pos, a] : found) {
        LoadIconAndName(*a, nullptr);
        items.push_back(a);
        Log(L"imported pin %s (%s | %s)", a->name.c_str(), a->exe.c_str(), a->aumid.c_str());
    }
}

// Pins saved by versions that did not know about store apps: add the missing ones once.
void AppModel::MigratePins() {
    if (g_settings.pinsVersion >= 2) return;
    std::wstring blob = TaskbandBlob();
    int added = 0;
    for (auto& [pos, id] : PackagedPins(blob)) {
        std::wstring fam = PackageFamily(id);
        bool have = std::any_of(items.begin(), items.end(), [&](auto& it) { return !it->aumid.empty() && PackageFamily(it->aumid) == fam; });
        if (have) continue;
        auto a = NewItem();
        a->pinned = true;
        a->aumid = id;
        a->launch = L"shell:AppsFolder\\" + id;
        LoadIconAndName(*a, nullptr);
        items.insert(items.begin() + PinnedCount(), a);
        added++;
        Log(L"migrated store pin %s (%s)", a->name.c_str(), id.c_str());
    }
    if (added) { structureVersion++; SavePins(); }
    g_settings.pinsVersion = 2;
    g_settings.Save();
}

void AppModel::PinFile(const std::wstring& path, int index) {
    auto a = NewItem();
    a->pinned = true;
    a->launch = path;
    if (EndsWithI(path, L".lnk")) { ReadShortcut(path, a->exe, a->aumid); a->name = FileStem(path); }
    else a->exe = ToLower(path);
    // same app already present? just pin/move it
    for (auto& it : items) {
        if ((!a->aumid.empty() && it->aumid == a->aumid) || (!a->exe.empty() && it->exe == a->exe)) {
            MoveTo(*it, index, true);
            return;
        }
    }
    LoadIconAndName(*a, nullptr);
    int pc = PinnedCount();
    index = std::clamp(index, 0, pc);
    items.insert(items.begin() + index, a);
    structureVersion++;
    SavePins();
    dirty = true;
}

void AppModel::SetPinned(AppItem& a, bool pinned) {
    if (a.pinned == pinned) return;
    if (pinned) {
        if (a.launch.empty()) {
            std::wstring nm;
            if (!a.aumid.empty() && AppsFolderItem(a.aumid, &nm)) a.launch = L"shell:AppsFolder\\" + a.aumid;
            else a.launch = a.exe;
        }
        MoveTo(a, PinnedCount(), true);
    } else {
        a.pinned = false;
        // move to the running section (or drop entirely when not running)
        auto it = std::find_if(items.begin(), items.end(), [&](auto& p) { return p.get() == &a; });
        if (it == items.end()) return;
        auto sp = *it;
        items.erase(it);
        if (sp->Running()) items.insert(items.begin() + PinnedCount(), sp);
        structureVersion++;
        SavePins();
    }
}

void AppModel::MoveTo(AppItem& a, int index, bool pin) {
    auto it = std::find_if(items.begin(), items.end(), [&](auto& p) { return p.get() == &a; });
    if (it == items.end()) return;
    auto sp = *it;
    items.erase(it);
    if (pin) {
        if (sp->launch.empty()) {
            std::wstring nm;
            if (!sp->aumid.empty() && AppsFolderItem(sp->aumid, &nm)) sp->launch = L"shell:AppsFolder\\" + sp->aumid;
            else sp->launch = sp->exe;
        }
        sp->pinned = true;
        index = std::clamp(index, 0, PinnedCount());
    } else {
        int pc = PinnedCount();
        index = std::clamp(index, pc, (int)items.size());
    }
    items.insert(items.begin() + index, sp);
    structureVersion++;
    SavePins();
}

std::shared_ptr<AppItem> AppModel::FindByWindow(HWND h) {
    for (auto& a : items)
        for (HWND w : a->windows) if (w == h) return a;
    return nullptr;
}

void AppModel::Refresh() {
    lastRefresh = NowSeconds();
    dirty = false;

    std::vector<HWND> wins;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (IsTaskbarWindow(h)) ((std::vector<HWND>*)lp)->push_back(h);
        return TRUE;
    }, (LPARAM)&wins);

    std::unordered_map<HWND, WinCache> newCache;
    std::vector<size_t> prevCounts;
    for (auto& a : items) { prevCounts.push_back(a->windows.size()); a->windows.clear(); }

    bool structural = false;
    for (HWND h : wins) {
        WinCache wc;
        auto f = winCache.find(h);
        if (f != winCache.end()) wc = f->second;
        else {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            wc.exe = ProcessExe(pid);
            wc.aumid = WindowAumid(h);
            std::wstring procAumid = ProcessAumid(pid);
            if (wc.aumid.empty()) wc.aumid = procAumid;
            // packaged apps may label their windows with their own AUMID; the package identifies them
            wc.family = PackageFamily(!procAumid.empty() ? procAumid : wc.aumid);
            if (EndsWithI(wc.exe, L"\\applicationframehost.exe")) {
                // UWP: the real app lives in a child CoreWindow from another process
                struct Ctx { DWORD host; DWORD found; } c{ pid, 0 };
                EnumChildWindows(h, [](HWND ch, LPARAM lp) -> BOOL {
                    auto* c = (Ctx*)lp; DWORD p = 0; GetWindowThreadProcessId(ch, &p);
                    if (p != c->host) { c->found = p; return FALSE; }
                    return TRUE;
                }, (LPARAM)&c);
                if (c.found) {
                    wc.exe = ProcessExe(c.found);
                    std::wstring pa = ProcessAumid(c.found);
                    if (wc.aumid.empty()) wc.aumid = pa;
                    if (!pa.empty()) wc.family = PackageFamily(pa);
                }
                if (wc.aumid.empty()) continue;   // suspended frame we can't identify yet
            }
            wc.valid = true;
        }
        newCache[h] = wc;

        std::shared_ptr<AppItem> match;
        if (!wc.aumid.empty())
            for (auto& a : items) if (_wcsicmp(a->aumid.c_str(), wc.aumid.c_str()) == 0) { match = a; break; }
        if (!match && !wc.family.empty())
            for (auto& a : items) if (!a->aumid.empty() && PackageFamily(a->aumid) == wc.family) { match = a; break; }
        if (!match && !wc.exe.empty())
            for (auto& a : items)
                if (a->exe == wc.exe && (a->aumid.empty() || wc.aumid.empty())) { match = a; break; }
        if (!match) {
            match = NewItem();
            match->aumid = wc.aumid;
            match->exe = wc.exe;
            LoadIconAndName(*match, h);
            items.push_back(match);
            Log(L"running app: %s (aumid %s, package %s)", match->name.c_str(), wc.aumid.c_str(), wc.family.c_str());
            prevCounts.push_back(0);
            structural = true;
        }
        match->windows.push_back(h);
    }
    winCache.swap(newCache);

    // drop unpinned apps whose windows are gone
    double now = NowSeconds();
    for (size_t i = 0; i < items.size(); ++i) {
        auto& a = items[i];
        if (a->launchStarted > 0) {
            if (a->windows.size() > a->launchWindowCount || now - a->launchStarted > 8.0) a->launchStarted = 0;
        }
    }
    size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(), [](auto& a) { return !a->pinned && !a->Running(); }), items.end());
    if (items.size() != before) structural = true;

    // clear attention for the foreground app
    HWND fg = GetForegroundWindow();
    for (auto& a : items)
        if (a->attention && std::find(a->windows.begin(), a->windows.end(), fg) != a->windows.end()) a->attention = false;

    if (structural) structureVersion++;
}

void AppModel::Tick(double now) {
    if (dirty || now - lastRefresh > 1.0) Refresh();
}

void AppModel::OnShellHook(WPARAM code, LPARAM lp) {
    HWND h = (HWND)lp;
    switch (code) {
    case HSHELL_WINDOWCREATED:
    case HSHELL_WINDOWDESTROYED:
    case HSHELL_WINDOWREPLACED:
    case HSHELL_MONITORCHANGED:
        dirty = true;
        break;
    case HSHELL_WINDOWACTIVATED:
    case HSHELL_RUDEAPPACTIVATED: {
        if (auto a = FindByWindow(h)) a->attention = false;
        dirty = true;
        break;
    }
    case HSHELL_FLASH: {
        auto a = FindByWindow(h);
        if (!a) { Refresh(); a = FindByWindow(h); }
        if (!a) break;
        if (GetForegroundWindow() == h) break;
        double now = NowSeconds();
        bool fresh = !a->attention;
        a->attention = true;
        // bounce for a new notification; a window that keeps flashing re-bounces at most every 30 s
        if (fresh || now - a->lastAttentionBounce > 30.0) {
            a->lastAttentionBounce = now;
            attentionEvents.push_back({ a->id });
            Log(L"attention: %s", a->name.c_str());
        }
        break;
    }
    }
}

// ---------------------------------------------------------------- actions

void AppModel::ActivateWindow(HWND h) {
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    if (!SetForegroundWindow(h) || GetForegroundWindow() != h) {
        HWND fg = GetForegroundWindow();
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD me = GetCurrentThreadId();
        if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, TRUE);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, FALSE);
    }
}

void AppModel::Launch(AppItem& a, int dockIndex) {
    std::wstring target = a.launch;
    if (target.empty()) target = !a.aumid.empty() ? L"shell:AppsFolder\\" + a.aumid : a.exe;
    if (EndsWithI(target, L".lnk") && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES && !a.exe.empty())
        target = a.exe;
    if (target.empty()) return;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_ASYNCOK | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = target.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        a.launchStarted = NowSeconds();
        a.launchDock = dockIndex;
        a.launchWindowCount = a.windows.size();
    } else Log(L"launch failed %s (%lu)", target.c_str(), GetLastError());
}

void AppModel::Click(AppItem& a, int dockIndex) {
    if (a.windows.empty()) { Launch(a, dockIndex); return; }
    HWND fg = GetForegroundWindow();
    auto it = std::find(a.windows.begin(), a.windows.end(), fg);
    if (it != a.windows.end()) {
        if (a.windows.size() == 1) {
            PostMessageW(fg, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        } else {
            // cycle: bring the bottom-most window of this app to the front
            ActivateWindow(a.windows.back());
        }
    } else {
        ActivateWindow(a.windows.front());
    }
    a.attention = false;
}

void AppModel::CloseAll(AppItem& a) {
    for (HWND h : a.windows) PostMessageW(h, WM_CLOSE, 0, 0);
}
