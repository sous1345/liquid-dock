#pragma once
#include "common.h"
#include "gfx.h"
#include "capture.h"
#include "apps.h"
#include "window_preview.h"

extern Settings g_settings;

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
    enum HitKind { HitNone, HitSlot, HitTray, HitNet, HitVol, HitClock, HitSensor, HitPanel, HitBar, HitDesktop, HitOled, HitSettings };
    struct Hit { HitKind kind = HitNone; int slot = -1; };

    void Metrics();
    void SyncSlots();
    void Layout(double now);
    void UpdateRegion();
    Hit HitTestUV(float u, float v);
    void ScreenToUV(POINT p, float& u, float& v) const;
    void ClientToUV(int x, int y, float& u, float& v) const;
    D2D1_POINT_2F W(float u, float v) const;
    D2D1_RECT_F RectUV(float u0, float u1, float v0, float v1) const;
    void StartBounce(Slot& s, int count, bool loop, bool hiddenMode, double now);
    bool CreateDeviceResources();
    void DrawIcon(Slot& s, float u, float vBottom, float size, float alpha);
    void DrawStartIcon(D2D1_RECT_F r, float alpha);
    void DrawTooltip(const std::wstring& text, float u, float vBase);
    void ShowAppMenu(Slot& s);
    void ShowDockMenu();
    void ShowTrayMenu(HitKind kind);
    int  PopupMenu(HMENU m);
    void Click(const Hit& h);
    void ToggleOled();
    void DrawOledButton(float v0, float v1, float vis);
    void EndDrag(bool commit);
    float TextWidth(IDWriteTextFormat* f, const std::wstring& s);
    D2D1_SIZE_F TextSize(IDWriteTextFormat* f, const std::wstring& s);
    IDWriteTextFormat* ClockFont(bool date = false) const;
    D2D1_RECT_F ConnectivityRect(HitKind kind) const;
    struct LayoutMetrics {
        float icon, padding, gap, thickness;
        float separator, trayButton, network, volume, connectivity, clock, sensor, desktop, oled, settings;
        float TrayWidth() const { return separator + trayButton + connectivity + clock + sensor + desktop + oled + settings; }
    };
    LayoutMetrics MeasureMetrics(float fitFactor);
    bool FullscreenAppActive() const;
    WindowPreview windowPreview;

    RECT mon{};          // monitor rect (physical px)
    int winX = 0, winY = 0, winW = 0, winH = 0;
    float len = 0, T = 0, scale = 1;
    float base = 54, pad = 8, gap = 6, margin = 6, barThick = 70, magRange = 140, mag = 1.7f;
    float baseNominal = 54, fit = 1;   // fit < 1 shrinks the dock when it would overflow the edge
    void ApplyFit(float dt);

    // state
    Spring reveal, magAmount, tooltipAlpha;
    float hoverU = 0, cursorU = -1, cursorV = 1e9;
    double edgeSince = -1, outsideSince = -1;
    double lastCaptureWanted = -1e9, nextCaptureTry = 0;
    bool menuOpen = false;
    bool busy = false, dirty = true;
    int lastStructure = -1, lastSysVersion = -1;
    int hoverSlot = -1; HitKind hoverKind = HitNone;
    std::wstring tooltipText; float tooltipU = 0, tooltipV = 0;

    std::vector<Slot> slots;
    Spring trailingGap;
    // tray layout
    float trayU0 = 0, trayU1 = 0;
    POINT ScreenPointUV(float u, float v) const;
    RECT  BarScreenRect() const;
    float sepU = 0, netU0 = 0, netU1 = 0, volU0 = 0, volU1 = 0, clkU0 = 0, clkU1 = 0, sensU0 = 0, sensU1 = 0;
    float desktopU0 = 0, desktopU1 = 0;
    float oledU0 = 0, oledU1 = 0;
    float settingsU0 = 0, settingsU1 = 0;
    // system monitor: cell at the end of the dock + glass popover panel
    Spring panelAlpha;
    double panelOutsideSince = -1;
    float panelW = 0, panelH = 0;          // physical px (screen orientation)
    D2D1_RECT_F panelRect{};               // window coords
    int lastSensorVersion = -1;
    void ComputePanelRect();
    bool CursorInPanel() const;
    void DrawSensorCell(float v0, float v1, float vis);
    void DrawSensorPeek();
    void DrawPanel();
    // overheat bounce of the sensor cell
    double sbStart = 0; int sbCount = 0; bool sbHidden = false; float sbOff = 0;
    Spring sbPeek;
    float barU0 = 0, barU1 = 0, trayW = 0;
    float hideOff = 0;

    // input
    int pressSlot = -1; HitKind pressKind = HitNone;
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
    float InkAt(float u) const;
    D2D1_COLOR_F Ink(float u, float alpha) const;

    bool snapshotPending = false;
    void SaveSnapshot();

public:
    // built-in screen recorder (normal recorders can't see the dock: it is excluded from capture)
    void StartRecording(RECT cropMonitorPx, float outScale);
    void StopRecording();
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
