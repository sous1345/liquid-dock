#pragma once
#include "apps.h"
#include <dwmapi.h>

// The preview owns its popup and DWM relationships. It never touches Explorer's
// taskbar, restores a source window for capture, or takes screenshots of it.
class WindowPreview {
public:
    ~WindowPreview();
    WindowPreview() = default;
    WindowPreview(const WindowPreview&) = delete;
    WindowPreview& operator=(const WindowPreview&) = delete;
    void Update(HWND owner, const std::shared_ptr<AppItem>& hovered, RECT anchor,
                RECT monitor, Edge edge, float scale, POINT cursor, double now, bool allowed);
    void Dismiss();
    bool Open() const;
    bool Contains(POINT cursor) const;

private:
    struct Entry {
        HWND source = nullptr;
        DWORD pid = 0;
        std::wstring title, fallback;
        HTHUMBNAIL thumbnail = nullptr;
        RECT card{}, image{};
    };
    bool Create(HWND owner);
    void Hide();
    void ReleaseThumbnails();
    void Refresh();
    void Arrange();
    void Paint();
    void ChangePage(int delta);
    int Hit(POINT client) const;
    bool Valid(const Entry& entry) const;
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd = nullptr;
    HFONT font = nullptr;
    std::shared_ptr<AppItem> candidate, app;
    std::vector<Entry> entries;
    RECT anchor{}, monitor{}, bounds{}, bridge{}, previous{}, next{};
    Edge edge = Edge::Bottom;
    float scale = 1;
    int first = 0, capacity = 1, hot = -1;
    HWND pressed = nullptr, foreground = nullptr;
    DWORD pressedPid = 0;
    int pressedNav = 0;
    double since = 0, outsideSince = -1, nextRefresh = 0;
    bool suppressed = false;
    static inline WindowPreview* active = nullptr;
};
