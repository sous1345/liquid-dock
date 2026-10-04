#pragma once
#include "common.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

// Captures one monitor via Windows.Graphics.Capture. The dock windows are excluded
// from capture (WDA_EXCLUDEFROMCAPTURE), so the frames show exactly what lies behind them.
class MonitorCapture {
public:
    ~MonitorCapture() { Stop(); }
    bool Start(HMONITOR mon);
    void Stop();
    bool Active() const { return session != nullptr || gdiActive; }
    static bool UseGdi();   // Windows 10: no borderless capture -> GDI fallback (dock region only)
    // Copies the newest frame's region (monitor-relative) into dst at (0,0). Returns true when a new frame arrived.
    bool Poll(ID3D11Texture2D* dst, int srcX, int srcY, int w, int h, ID3D11Texture2D* fullDst = nullptr);
    double startedAt = 0;
    bool gotFirstFrame = false;

    static void RequestBorderlessAccess();   // call once at startup

private:
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{ nullptr };
    winrt::Windows::Graphics::SizeInt32 size{};

    bool PollGdi(ID3D11Texture2D* dst, int srcX, int srcY, int w, int h);
    bool gdiActive = false;
    RECT gdiMon{};
    double gdiLast = 0;
    HDC gdiMem = nullptr;
    HBITMAP gdiBmp = nullptr;
    void* gdiBits = nullptr;
    int gdiW = 0, gdiH = 0;
};
