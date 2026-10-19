#pragma once
#include "common.h"
#include "gfx.h"
#include "capture.h"
#include "apps.h"
#include "window_preview.h"
#include "buttons.h"
#include <shellapi.h>

extern Settings g_settings;
extern HWND g_mainWnd;
constexpr UINT WM_APP_REBUILD = WM_APP + 1;
constexpr UINT WM_APP_AUTOSTART = WM_APP + 2;
constexpr UINT WM_APP_RELAYOUT = WM_APP + 3;    // shape settings changed: lay the docks out again in place
constexpr UINT WM_APP_SETTINGS = WM_APP + 4;    // open the settings window
constexpr UINT WM_APP_HOTKEY = WM_APP + 5;      // the reveal shortcut setting changed
bool RevealHotkeyFailed();                       // the chosen shortcut belongs to another program
constexpr UINT WM_APP_APPBAR = WM_APP + 40;     // shell notifications for an always-shown dock

void OpenTaskManager();

// One dock per monitor. The implementation is split by concern:
//   dock.cpp           window, per-frame state (reveal, magnify, drag, bounce), input messages
//   dock_layout.cpp    sizes, positions and hit testing
//   dock_render.cpp    glass and Direct2D drawing
//   dock_controls.cpp  the small tray buttons, clicks and menus
class Dock {
public:
    Dock(HMONITOR mon, int index, bool primary);
    ~Dock();
    bool Create();
    void Update(double now, float dt, POINT cursor);
    void Render();
    bool Busy() const { return busy; }        // wants vsync-paced frames
    void RequestSnapshot() { snapshotPending = true; dirty = true; }
    static inline bool debugForceShow = false;
    static inline bool debugForcePanel = false;
    static inline bool settingsOpen = false;   // the shape settings window keeps every dock shown
    bool Relayout();                           // false: the window must be recreated instead
    void HotkeyReveal(double now);             // the reveal shortcut: show (or put away) this dock
    HWND hwnd = nullptr;
    HMONITOR monitor;
    int index;
    bool primary;

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Slot {
        std::shared_ptr<AppItem> app;   // null => start button
        bool ghost = false;             // removed from model; animating out
        Spring appear, gapBefore, shrink, press, peek;
        // bounce
        double bounceStart = 0; int bounceCount = 0; bool bounceLoop = false; bool bounceHiddenMode = false;
        float bounceOff = 0;
        // layout
        float u = 0, size = 0, u0 = 0, weight = 0;
    };
    enum HitKind { HitNone, HitSlot, HitTray, HitClock, HitSensor, HitPanel, HitBar, HitButton, HitKindCount };
    struct Hit {
        HitKind kind = HitNone; int slot = -1; ButtonId button = ButtonId::Count;
        bool Is(ButtonId b) const { return kind == HitButton && button == b; }
        bool operator==(const Hit& o) const { return kind == o.kind && slot == o.slot && button == o.button; }
    };
    static Hit ButtonHit(ButtonId b) { Hit h; h.kind = HitButton; h.button = b; return h; }

    // ---- small tray buttons (dock_controls.cpp): what a button does, one row per button. What it
    // looks like is in buttons.cpp (shared with the settings preview). Two buttons of a group share a
    // cell, split across the bar's thickness, when half the bar is still a comfortable click target.
    struct SmallButton {
        ButtonId id;
        std::wstring (Dock::* tooltip)() const;
        void (Dock::* click)();
        void (Dock::* rightClick)();       // nullptr: the dock menu
    };
    static const SmallButton kSmallButtons[kButtonCount];
    static const SmallButton& Button(ButtonId id) { return kSmallButtons[(int)id]; }
    // group 0: network + volume; group 1: the user's buttons, then settings
    static std::vector<ButtonId> Group(int group);
    std::wstring TipNetwork() const;
    std::wstring TipVolume() const;
    std::wstring TipDesktop() const;
    std::wstring TipTheme() const;
    std::wstring TipOled() const;
    std::wstring TipAwake() const;
    std::wstring TipScreenshot() const;
    std::wstring TipQuiet() const;
    std::wstring TipTrash() const;
    std::wstring TipDownloads() const;
    std::wstring TipAudio() const;
    std::wstring TipSff() const;
    std::wstring TipSettings() const;
    void ClickNetwork();
    void ClickVolume();
    void ClickDesktop();
    void ClickTheme();
    void ClickOled();
    void ClickAwake();
    void ClickScreenshot();
    void ClickSff();
    void ClickQuiet();
    void ClickTrash();
    void ClickDownloads();
    void ClickAudio();
    void ClickSettings();
    void MenuNetwork() { ShowTrayMenu(ButtonHit(ButtonId::Network)); }
    void MenuVolume() { ShowTrayMenu(ButtonHit(ButtonId::Volume)); }
    void MenuQuiet();
    void MenuTrash();
    void MenuDownloads();
    void MenuAudio();
    bool DropOnTrash(HDROP drop);

    // ---- layout (dock_layout.cpp)
    struct Cell { float u0 = 0, u1 = 0; int half = -1; float Mid() const { return (u0 + u1) * 0.5f; } };   // half -1: whole cell
    Cell cells[HitKindCount];              // tray, clock and sensor
    Cell buttonCells[kButtonCount];        // the small buttons
    Cell& Cell_(ButtonId b) { return buttonCells[(int)b]; }
    const Cell& Cell_(ButtonId b) const { return buttonCells[(int)b]; }
    bool stackButtons = false;
    struct LayoutMetrics {
        float icon, padding, gap, thickness;
        float separator, trayButton, clock, sensor;
        float connectivity, utilities;     // the two small-button groups, after pairing
        bool stack;
        float TrayWidth() const { return separator + trayButton + connectivity + clock + sensor + utilities; }
    };
    LayoutMetrics MeasureMetrics(float fitFactor, int thicknessPct = -1);
    float SmallButtonWidth(ButtonId b, const LayoutMetrics& m) const;
    float ArrangeGroup(int group, const LayoutMetrics& m, float x, bool assign);   // returns the end
    D2D1_RECT_F ButtonRect(ButtonId b, bool hit = false) const;
    float ButtonIconScale() const { return stackButtons ? 0.8f : 1.0f; }
    void Metrics();
    void ApplyFit(float dt);
    void Layout(double now);
    Hit HitTestUV(float u, float v);
    void ScreenToUV(POINT p, float& u, float& v) const;
    void ClientToUV(int x, int y, float& u, float& v) const;
    D2D1_POINT_2F W(float u, float v) const;
    D2D1_RECT_F RectUV(float u0, float u1, float v0, float v1) const;
    POINT ScreenPointUV(float u, float v) const;
    RECT  BarScreenRect() const;
    float TextWidth(IDWriteTextFormat* f, const std::wstring& s);
    D2D1_SIZE_F TextSize(IDWriteTextFormat* f, const std::wstring& s);
    IDWriteTextFormat* ClockFont(bool date = false) const;
    void ComputePanelRect();
    bool CursorInPanel() const;

    // ---- frame and input (dock.cpp)
    void SyncSlots();
    void UpdateRegion();
    // Always-shown dock: registered as an app bar so maximised windows stop at it, like the taskbar.
    bool AlwaysShown() const;
    void UpdateAppBar(bool want);
    bool appBar = false; RECT appBarRect{};
    void StartBounce(Slot& s, int count, bool loop, bool hiddenMode, double now);
    bool CreateDeviceResources();
    bool FullscreenAppActive() const;
    // Start, search, quick settings or the notification centre is in front on this monitor
    bool ShellPopupOpen() const;
    static bool (*isShellPopup)(HWND);     // which windows count (replaceable for tests)
    void Click(const Hit& h);
    void EndDrag(bool commit);

    // ---- menus (dock_controls.cpp)
    void ShowAppMenu(Slot& s);
    void ShowDockMenu();
    void ShowStartMenu();              // right-click on the start button: power and system
    void ShowTrayMenu(const Hit& h);   // network, volume, clock, sensor
    int  PopupMenu(HMENU m);
    void ToggleOled();

    // ---- drawing (dock_render.cpp)
    void DrawIcon(Slot& s, float u, float vBottom, float size, float alpha);
    void DrawStartIcon(D2D1_RECT_F r, float alpha);
    void DrawTooltip(const std::wstring& text, float u, float vBase);
    void DrawShadowText(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float u, float alpha);
    void DrawSensorCell(float v0, float v1, float vis);
    void DrawSensorPeek();
    void DrawPanel();
    float InkAt(float u) const;
    D2D1_COLOR_F Ink(float u, float alpha) const;

    WindowPreview windowPreview;

    RECT mon{};          // monitor rect (physical px)
    int winX = 0, winY = 0, winW = 0, winH = 0;
    float len = 0, T = 0, scale = 1;
    float base = 54, pad = 8, gap = 6, margin = 6, barThick = 70, magRange = 140, mag = 1.7f;
    float baseNominal = 54, fit = 1;   // fit < 1 shrinks the dock when it would overflow the edge

    // state
    Spring reveal, magAmount, tooltipAlpha;
    float hoverU = 0, cursorU = -1, cursorV = 1e9;
    double edgeSince = -1, outsideSince = -1;
    double hotkeyHoldUntil = 0;            // shown by the shortcut: stays until then (or while the pointer is on it)
    double lastCaptureWanted = -1e9, nextCaptureTry = 0;
    bool menuOpen = false;
    bool busy = false, dirty = true;
    int lastStructure = -1, lastSysVersion = -1;
    int hoverSlot = -1; HitKind hoverKind = HitNone; ButtonId hoverButton = ButtonId::Count;
    std::wstring tooltipText; float tooltipU = 0, tooltipV = 0;

    std::vector<Slot> slots;
    Spring trailingGap;
    float sepU = 0, barU0 = 0, barU1 = 0, trayW = 0;
    float hideOff = 0;
    float edgeReserve = 0;                 // room kept free for magnification next to an offset dock
    // system monitor: cell at the end of the dock + glass popover panel
    Spring panelAlpha;
    double panelOutsideSince = -1;
    float panelW = 0, panelH = 0;          // physical px (screen orientation)
    D2D1_RECT_F panelRect{};               // window coords
    int lastSensorVersion = -1;
    // overheat bounce of the sensor cell
    double sbStart = 0; int sbCount = 0; bool sbHidden = false; float sbOff = 0;
    Spring sbPeek;

    // input
    int pressSlot = -1; HitKind pressKind = HitNone; ButtonId pressButton = ButtonId::Count;
    POINT pressPt{};
    bool dragging = false;
    int dropIndex = -1; bool dropRemove = false;

    // region
    RECT curRegion{ -1, -1, -1, -1 };

    // gpu
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<IDCompositionTarget> compTarget;
    ComPtr<IDCompositionVisual> visual;
    RTTexture backdrop, halfA, halfB;
    bool backdropDirty = false;
    MonitorCapture capture;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Effect> darken;
    bool clearedWhileHidden = false;
    // backdrop luminance probe -> adaptive ink (0 = light ink, 1 = dark ink)
    RTTexture lumRT;
    ComPtr<ID3D11Texture2D> lumStage[2];
    int lumFrame = 0;
    float lumCol[16] = {}, inkTarget[16] = {}, inkCol[16] = {};

    bool snapshotPending = false;
    void SaveSnapshot();

public:
    // built-in screen recorder (normal recorders can't see the dock: it is excluded from capture)
    void StartRecording(RECT cropMonitorPx, float outScale, bool append = false);
    // a recording outlives the dock windows being rebuilt (position change): carried over by BuildDocks
    static inline bool carryRecording = false;
    static inline RECT carryCrop{};
    static inline float carryScale = 0.5f;
    static inline int carryFrames = 0;
    void StopRecording();
    void SuspendRecording();
    void RecordTick(double now);
    bool Recording() const { return recording; }
private:
    bool recording = false;
    double recNext = 0;
    int recFrames = 0, recW = 0, recH = 0, recSlot = 0;
    RECT recCrop{};
    FILE* recFile = nullptr;
    RTTexture dockFrame, fullTex, recRT;
    ComPtr<ID3D11Texture2D> recStage[3];
    ComPtr<ID2D1Bitmap1> recTarget;
};
