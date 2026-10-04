#pragma once
#include "common.h"

struct AppItem {
    int id = 0;
    std::wstring name;
    std::wstring aumid;     // AppUserModelID (may be empty)
    std::wstring exe;       // lowercase full path (may be empty)
    std::wstring launch;    // what to ShellExecute (lnk / exe / shell:AppsFolder\aumid)
    bool pinned = false;
    std::vector<HWND> windows;     // z-order, topmost first
    ComPtr<ID2D1Bitmap1> icon;
    bool attention = false;        // window flashed and not yet activated
    double lastAttentionBounce = -1e9;
    double launchStarted = 0;      // >0 while waiting for a launched app's window
    int launchDock = -1;
    size_t launchWindowCount = 0;
    bool Running() const { return !windows.empty(); }
};

struct AttentionEvent { int id; };

class AppModel {
public:
    std::vector<std::shared_ptr<AppItem>> items;   // pinned first (pin order), then running unpinned
    std::vector<AttentionEvent> attentionEvents;    // consumed by the primary dock each frame
    int structureVersion = 0;                       // bumps when items are added/removed/reordered

    void Init();
    void ReloadIcons();                             // after a GPU reset (bitmaps belong to the old device)
    void Tick(double now);                          // periodic refresh
    void MarkDirty() { dirty = true; }
    void OnShellHook(WPARAM code, LPARAM lp);

    void Click(AppItem& a, int dockIndex);
    void Launch(AppItem& a, int dockIndex);
    void ActivateWindow(HWND h);
    void CloseAll(AppItem& a);
    void SetPinned(AppItem& a, bool pinned);
    void MoveTo(AppItem& a, int index, bool pin);
    void PinFile(const std::wstring& path, int index);
    int  PinnedCount() const;
    std::shared_ptr<AppItem> FindByWindow(HWND h);

private:
    void Refresh();
    void SavePins();
    void LoadPins();
    void ImportTaskbarPins();
    void MigratePins();
    std::shared_ptr<AppItem> NewItem();
    void LoadIconAndName(AppItem& a, HWND sample);

    struct WinCache { std::wstring aumid, family, exe; bool valid = false; };   // family: store-app package
    std::unordered_map<HWND, WinCache> winCache;
    int nextId = 1;
    bool dirty = true;
    double lastRefresh = 0;
};

extern AppModel g_apps;
