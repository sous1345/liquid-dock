#include "window_preview.h"

namespace {
constexpr double kHoverDelay = 0.45, kLeaveDelay = 0.25;
constexpr wchar_t kClass[] = L"LiquidDockWindowPreview";
int Px(float value, float scale) { return std::max(1, (int)std::lround(value * scale)); }
bool SameRect(RECT a, RECT b) { return EqualRect(&a, &b) != FALSE; }
void Fill(HDC dc, RECT r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &r, brush); DeleteObject(brush);
}
bool Usable(HWND source) {
    if (!IsWindow(source) || !IsWindowVisible(source)) return false;
    DWORD cloaked = 0;
    return FAILED(DwmGetWindowAttribute(source, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || !cloaked;
}
}

WindowPreview::~WindowPreview() {
    Hide();
    if (hwnd) DestroyWindow(hwnd);
    if (font) DeleteObject(font);
}

bool WindowPreview::Create(HWND owner) {
    if (hwnd) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClass, L"열린 창 미리보기", WS_POPUP, 0, 0, 1, 1, owner, nullptr, wc.hInstance, this);
    if (!hwnd) return false;
    // Keep the popup out of the dock's background capture, like the dock itself.
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    return true;
}

void WindowPreview::ReleaseThumbnails() {
    for (auto& e : entries) if (e.thumbnail) {
        DwmUnregisterThumbnail(e.thumbnail); e.thumbnail = nullptr;
    }
}

void WindowPreview::Hide() {
    if (hwnd) ShowWindow(hwnd, SW_HIDE);
    ReleaseThumbnails(); entries.clear(); app.reset();
    first = 0; hot = -1; pressed = nullptr; pressedPid = 0; pressedNav = 0;
    outsideSince = -1;
    if (active == this) active = nullptr;
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
}

void WindowPreview::Dismiss() {
    Hide(); suppressed = true; // require leaving the icon before reopening
}

bool WindowPreview::Contains(POINT p) const {
    return Open() && (PtInRect(&bounds, p) || PtInRect(&bridge, p) || PtInRect(&anchor, p));
}

bool WindowPreview::Open() const {
    return app != nullptr && hwnd && IsWindowVisible(hwnd);
}

bool WindowPreview::Valid(const Entry& e) const {
    DWORD pid = 0;
    GetWindowThreadProcessId(e.source, &pid);
    return app && pid && pid == e.pid && Usable(e.source) &&
        std::find(app->windows.begin(), app->windows.end(), e.source) != app->windows.end();
}

void WindowPreview::Update(HWND owner, const std::shared_ptr<AppItem>& hovered, RECT newAnchor,
    RECT newMonitor, Edge newEdge, float newScale, POINT cursor, double now, bool allowed) {
    if (!allowed) { Dismiss(); candidate = hovered; since = now; return; }
    if (candidate != hovered) {
        candidate = hovered; since = now; suppressed = false;
        if (hovered && app != hovered) Hide();
    }
    if (Open()) {
        bool outsideClick = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) &&
                            !PtInRect(&bounds, cursor);
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || outsideClick || GetForegroundWindow() != foreground) {
            Dismiss(); return;
        }
        if (hovered == app && !SameRect(anchor, newAnchor)) {
            anchor = newAnchor; Arrange();
            if (!app) return;
        }
        if (Contains(cursor) || hovered == app) outsideSince = -1;
        else if (outsideSince < 0) outsideSince = now;
        else if (now - outsideSince >= kLeaveDelay) { Hide(); return; }
        if (now >= nextRefresh) {
            nextRefresh = now + 0.2; Refresh();
            if (entries.empty()) { Dismiss(); return; }
        }
        POINT client{cursor.x - bounds.left, cursor.y - bounds.top};
        int nextHot = Hit(client);
        if (nextHot != hot) { hot = nextHot; InvalidateRect(hwnd, nullptr, FALSE); }
        return;
    }
    if (!hovered || hovered->windows.empty() || suppressed || now - since < kHoverDelay) return;
    if (!Create(owner)) { suppressed = true; return; }
    if (active && active != this) active->Dismiss();
    active = this; app = hovered; anchor = newAnchor; monitor = newMonitor; edge = newEdge;
    float fontScale = std::clamp(newScale, 0.72f, 2.5f);
    if (!font || scale != fontScale) {
        if (font) DeleteObject(font);
        scale = fontScale;
        font = CreateFontW(-Px(13, scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
    }
    foreground = GetForegroundWindow(); nextRefresh = now + 0.2;
    Refresh();
    if (entries.empty()) { Dismiss(); return; }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

void WindowPreview::Refresh() {
    if (!app) return;
    // Preserve order while the popup is open; z-order changes must not move a
    // different window under a pointer that is already over a card.
    bool changed = false;
    for (auto it = entries.begin(); it != entries.end();) {
        if (!Valid(*it)) {
            if (it->thumbnail) DwmUnregisterThumbnail(it->thumbnail);
            it = entries.erase(it); changed = true;
        } else ++it;
    }
    for (HWND source : app->windows) {
        if (!Usable(source) || std::any_of(entries.begin(), entries.end(),
            [&](const Entry& e) { return e.source == source; })) continue;
        Entry e; e.source = source; GetWindowThreadProcessId(source, &e.pid);
        if (e.pid) { entries.push_back(std::move(e)); changed = true; }
    }
    for (auto& e : entries) {
        wchar_t text[512]{}; GetWindowTextW(e.source, text, ARRAYSIZE(text));
        std::wstring title = *text ? text : app->name;
        if (title != e.title) { e.title = std::move(title); changed = true; }
    }
    // Also refresh minimized/protection state and thumbnail size without
    // reading source pixels or requesting the source app to render.
    Arrange();
    if (changed) InvalidateRect(hwnd, nullptr, FALSE);
}

void WindowPreview::Arrange() {
    if (entries.empty()) return;
    const int pad = Px(8, scale), gap = Px(8, scale), header = Px(30, scale);
    const bool vertical = IsVertical(edge);
    RECT available = monitor;
    InflateRect(&available, -pad, -pad);
    switch (edge) {
    case Edge::Left: available.left = std::max(available.left, anchor.right + gap); break;
    case Edge::Right: available.right = std::min(available.right, anchor.left - gap); break;
    case Edge::Top: available.top = std::max(available.top, anchor.bottom + gap); break;
    case Edge::Bottom: available.bottom = std::min(available.bottom, anchor.top - gap); break;
    }
    int aw = available.right - available.left, ah = available.bottom - available.top;
    // Do not overlap the dock to make a preview fit on an unusably small area.
    if (aw < 2 * pad + Px(80, scale) || ah < header + 2 * pad + Px(60, scale)) {
        Hide(); suppressed = true; return;
    }
    int cw = std::min(Px(220, scale), aw - 2 * pad);
    int ch = std::min(Px(156, scale), ah - header - 2 * pad);
    capacity = std::max(1, vertical ? (ah - header - pad) / (ch + pad) : (aw - pad) / (cw + pad));
    capacity = std::min(capacity, (int)entries.size());
    first = std::min(first / capacity, ((int)entries.size() - 1) / capacity) * capacity;
    int count = std::min(capacity, (int)entries.size() - first);
    int w = 2 * pad + (vertical ? cw : count * cw + (count - 1) * pad);
    int h = header + 2 * pad + (vertical ? count * ch + (count - 1) * pad : ch);
    int x = (anchor.left + anchor.right - w) / 2, y = (anchor.top + anchor.bottom - h) / 2;
    if (edge == Edge::Left) x = available.left;
    if (edge == Edge::Right) x = available.right - w;
    if (edge == Edge::Top) y = available.top;
    if (edge == Edge::Bottom) y = available.bottom - h;
    x = std::clamp<int>(x, available.left, available.right - w);
    y = std::clamp<int>(y, available.top, available.bottom - h);
    RECT nextBounds{x, y, x + w, y + h};
    bool moved = !SameRect(bounds, nextBounds);
    bounds = nextBounds;
    bridge = anchor;
    if (edge == Edge::Left) bridge.right = bounds.left;
    if (edge == Edge::Right) bridge.left = bounds.right;
    if (edge == Edge::Top) bridge.bottom = bounds.top;
    if (edge == Edge::Bottom) bridge.top = bounds.bottom;
    // Small padding avoids a dead seam at either end of the crossing.
    InflateRect(&bridge, 2, 2);
    int nav = Px(28, scale);
    next = {w - pad - nav, pad, w - pad, pad + header};
    previous = {next.left - nav, pad, next.left, pad + header};
    for (int i = 0; i < (int)entries.size(); ++i) {
        auto& e = entries[i];
        if (i < first || i >= first + count) {
            if (e.thumbnail) { DwmUnregisterThumbnail(e.thumbnail); e.thumbnail = nullptr; }
            e.card = {}; continue;
        }
        int j = i - first;
        int cx = pad + (vertical ? 0 : j * (cw + pad));
        int cy = pad + header + (vertical ? j * (ch + pad) : 0);
        e.card = {cx, cy, cx + cw, cy + ch};
        RECT image{cx + pad, cy + header, cx + cw - pad, cy + ch - pad};
        std::wstring fallback;
        DWORD affinity = WDA_NONE;
        if (IsIconic(e.source)) fallback = L"최소화된 창 · 클릭하여 복원";
        else if (GetWindowDisplayAffinity(e.source, &affinity) && affinity != WDA_NONE)
            fallback = L"미리보기가 제한된 창";
        if (!fallback.empty()) {
            if (e.thumbnail) { DwmUnregisterThumbnail(e.thumbnail); e.thumbnail = nullptr; }
        } else {
            if (!e.thumbnail && FAILED(DwmRegisterThumbnail(hwnd, e.source, &e.thumbnail))) e.thumbnail = nullptr;
            SIZE size{};
            if (!e.thumbnail || FAILED(DwmQueryThumbnailSourceSize(e.thumbnail, &size)) || size.cx <= 0 || size.cy <= 0)
                fallback = L"미리보기 없음 · 클릭하여 열기";
            else {
                double fit = std::min(double(image.right - image.left) / size.cx, double(image.bottom - image.top) / size.cy);
                int iw = std::max(1, (int)(size.cx * fit)), ih = std::max(1, (int)(size.cy * fit));
                int ix = (image.left + image.right - iw) / 2, iy = (image.top + image.bottom - ih) / 2;
                DWM_THUMBNAIL_PROPERTIES props{};
                props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
                props.rcDestination = {ix, iy, ix + iw, iy + ih};
                props.fVisible = TRUE; props.opacity = 255; props.fSourceClientAreaOnly = FALSE;
                if (FAILED(DwmUpdateThumbnailProperties(e.thumbnail, &props))) fallback = L"미리보기 없음 · 클릭하여 열기";
            }
            if (!fallback.empty() && e.thumbnail) { DwmUnregisterThumbnail(e.thumbnail); e.thumbnail = nullptr; }
        }
        if (fallback != e.fallback || !SameRect(image, e.image)) moved = true;
        e.image = image; e.fallback = std::move(fallback);
    }
    if (moved) {
        if (!SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE)) {
            Dismiss(); return;
        }
        HRGN rounded = CreateRoundRectRgn(0, 0, w + 1, h + 1, Px(14, scale), Px(14, scale));
        if (!SetWindowRgn(hwnd, rounded, TRUE)) DeleteObject(rounded);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

int WindowPreview::Hit(POINT p) const {
    if (first > 0 && PtInRect(&previous, p)) return -2;
    if (first + capacity < (int)entries.size() && PtInRect(&next, p)) return -3;
    for (int i = first; i < std::min(first + capacity, (int)entries.size()); ++i)
        if (PtInRect(&entries[i].card, p)) return i;
    return -1;
}

void WindowPreview::ChangePage(int delta) {
    int target = std::clamp(first + delta * capacity, 0, (((int)entries.size() - 1) / capacity) * capacity);
    if (target == first) return;
    first = target; pressed = nullptr; pressedNav = 0; hot = -1;
    ReleaseThumbnails(); Arrange(); InvalidateRect(hwnd, nullptr, FALSE);
}

void WindowPreview::Paint() {
    PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
    RECT client{}; GetClientRect(hwnd, &client);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
    auto oldBitmap = SelectObject(mem, bitmap);
    auto oldFont = SelectObject(mem, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(240, 242, 247));
    Fill(mem, client, RGB(27, 30, 37));
    if (app) {
        int pad = Px(8, scale), header = Px(30, scale);
        RECT title{pad * 2, pad, previous.left, pad + header};
        std::wstring label = app->name;
        if ((int)entries.size() > capacity) label += L"  " + std::to_wstring(first + 1) + L"–" +
            std::to_wstring(std::min(first + capacity, (int)entries.size())) + L" / " + std::to_wstring(entries.size());
        DrawTextW(mem, label.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        auto arrow = [&](RECT r, const wchar_t* text, bool enabled, int id) {
            if (hot == id) Fill(mem, r, RGB(65, 72, 87));
            SetTextColor(mem, enabled ? RGB(240, 242, 247) : RGB(83, 87, 96));
            DrawTextW(mem, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        if ((int)entries.size() > capacity) {
            arrow(previous, L"‹", first > 0, -2);
            arrow(next, L"›", first + capacity < (int)entries.size(), -3);
        }
        for (int i = first; i < std::min(first + capacity, (int)entries.size()); ++i) {
            auto& e = entries[i];
            Fill(mem, e.card, hot == i ? RGB(64, 77, 97) : RGB(40, 44, 54));
            SetTextColor(mem, RGB(240, 242, 247));
            RECT titleRect{e.card.left + pad, e.card.top, e.card.right - pad, e.card.top + header};
            DrawTextW(mem, e.title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            Fill(mem, e.image, RGB(20, 23, 29));
            if (!e.fallback.empty()) {
                SetTextColor(mem, RGB(183, 192, 205)); RECT textRect = e.image;
                DrawTextW(mem, e.fallback.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
    }
    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont); SelectObject(mem, oldBitmap); DeleteObject(bitmap); DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WindowPreview::Proc(HWND h, UINT message, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<WindowPreview*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<WindowPreview*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(h, message, wp, lp);
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: self->Paint(); return 0;
    case WM_LBUTTONDOWN: {
        int hit = self->Hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        self->pressed = hit >= 0 ? self->entries[hit].source : nullptr;
        self->pressedPid = hit >= 0 ? self->entries[hit].pid : 0;
        self->pressedNav = hit < -1 ? hit : 0;
        SetCapture(h); return 0;
    }
    case WM_LBUTTONUP: {
        HWND pressed = self->pressed; DWORD pid = self->pressedPid; int nav = self->pressedNav;
        self->pressed = nullptr; self->pressedNav = 0;
        if (GetCapture() == h) ReleaseCapture();
        int hit = self->Hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (nav && nav == hit) self->ChangePage(nav == -2 ? -1 : 1);
        else if (hit >= 0 && self->entries[hit].source == pressed && self->entries[hit].pid == pid && self->Valid(self->entries[hit])) {
            auto app = self->app; self->Dismiss();
            g_apps.ActivateWindow(pressed); app->attention = false;
        }
        return 0;
    }
    case WM_CAPTURECHANGED: self->pressed = nullptr; self->pressedNav = 0; return 0;
    case WM_MOUSEWHEEL:
        if (self->app && !self->entries.empty() && GET_WHEEL_DELTA_WPARAM(wp))
            self->ChangePage(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
        return 0;
    case WM_CLOSE: self->Dismiss(); return 0;
    case WM_NCDESTROY:
        self->hwnd = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, message, wp, lp);
}
