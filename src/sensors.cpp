#include "sensors.h"
#include "hw_sample.h"
#include <shellapi.h>

SensorClient g_sensors;
extern Settings g_settings;

static std::wstring HelperPath() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\') + 1) + L"LiquidDockSensors.exe";
}

static int RunHidden(const std::wstring& cmd, DWORD timeoutMs) {
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

void SensorClient::Start() {
    sampler = std::thread([this] {
        hw::Sampler s;
        s.Init();
        for (;;) {
            Sleep(1000);
            LDSensorBlock b = {};
            s.Sample(b);
            { std::lock_guard<std::mutex> lk(mu); localBlk = b; }
            localNew = true;
        }
    });
    sampler.detach();
}

bool SensorClient::PawnIOInstalled() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"PawnIO", SERVICE_QUERY_STATUS);
    bool ok = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// Must run on the UI thread right after a user click: Windows only shows a UAC prompt in
// front for the foreground app (otherwise it just flashes on the - hidden - taskbar).
bool SensorClient::LaunchInstall(HWND owner) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    std::wstring path = HelperPath();
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = path.c_str();
    sei.lpParameters = L"--install";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) { Log(L"sensor helper install not started (%lu)", GetLastError()); return false; }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

void SensorClient::EnableCpuTemp(HWND owner) {
    if (!PawnIOInstalled()) {
        // no owner: the dock window is topmost/no-activate and would leave the box hidden behind it
        int r = MessageBoxW(nullptr,
            L"CPU 온도를 표시하려면 PawnIO 드라이버가 필요합니다.\n\n"
            L"PawnIO는 LibreHardwareMonitor, FanControl 등이 쓰는 무료 오픈소스 하드웨어 접근 드라이버입니다.\n"
            L"공식 페이지를 열까요? 설치한 뒤 이 칸을 다시 클릭해 주세요.",
            L"Liquid Dock - CPU 온도", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        if (r == IDYES) ShellExecuteW(nullptr, L"open", L"https://pawnio.eu/", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (LaunchInstall(owner)) {
        g_settings.cpuTemp = true;
        g_settings.Save();
        lastHelperTry = NowSeconds();
    }
}

// Start the elevated helper through its scheduled task (no UAC prompt once installed).
void SensorClient::EnsureHelper() {
    if (GetFileAttributesW(HelperPath().c_str()) == INVALID_FILE_ATTRIBUTES) return;
    int rc = RunHidden(SystemExe(L"schtasks.exe") + L" /Run /TN " LD_SENSOR_TASK, 8000);
    Log(L"sensor task run rc=%d", rc);
}

float SensorClient::Temp(Metric m) const {
    if (!live) return NAN;
    switch (m) {
    case CPU: return (d.flags & LD_HAS_CPU_TEMP) ? d.cpuTemp : NAN;
    case GPU: return (d.flags & LD_HAS_GPU) ? d.gpuTemp : NAN;
    case SSD: return (d.flags & LD_HAS_SSD_TEMP) ? d.ssdTemp : NAN;
    default: return NAN;
    }
}

int SensorClient::Severity(Metric m) const {
    float t = Temp(m);
    if (std::isnan(t)) return 0;
    float th = Threshold(m);
    return t >= th ? 2 : (t >= th - 10 ? 1 : 0);
}

void SensorClient::ReadHelper(double now) {
    if (!view) {
        map = OpenFileMappingW(FILE_MAP_READ, FALSE, LD_SENSOR_MAP);
        if (map) view = (const LDSensorBlock*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(LDSensorBlock));
        if (!view && map) { CloseHandle(map); map = nullptr; }
    }
    if (!view) {
        helperLive = false;
        if (g_settings.cpuTemp && now - lastHelperTry > 20.0) {
            lastHelperTry = now;
            std::thread([this] { EnsureHelper(); }).detach();
        }
        return;
    }
    LDSensorBlock tmp;
    for (int tries = 0; tries < 4; ++tries) {
        uint32_t s0 = view->seq;
        if (s0 & 1) { Sleep(0); continue; }
        MemoryBarrier();
        memcpy(&tmp, (const void*)view, sizeof(tmp));
        MemoryBarrier();
        if (view->seq == s0) {
            if (tmp.magic == LD_SENSOR_MAGIC && s0 != lastSeq) {
                lastSeq = s0; helperBlk = tmp; helperLive = true; lastHelperSample = now;
            }
            break;
        }
    }
    if (helperLive && now - lastHelperSample > 5.0) {   // helper died
        helperLive = false;
        UnmapViewOfFile(view); view = nullptr;
        CloseHandle(map); map = nullptr;
    }
}

void SensorClient::Tick(double now) {
    if (now - lastPoll < 0.25) return;
    lastPoll = now;
    ReadHelper(now);
    if (!localNew.exchange(false)) return;

    { std::lock_guard<std::mutex> lk(mu); d = localBlk; }
    if (helperLive && g_settings.cpuTemp) {
        if (helperBlk.flags & LD_HAS_CPU_TEMP) { d.cpuTemp = helperBlk.cpuTemp; d.flags |= LD_HAS_CPU_TEMP; }
        if (!(d.flags & LD_HAS_SSD_TEMP) && (helperBlk.flags & LD_HAS_SSD_TEMP)) { d.ssdTemp = helperBlk.ssdTemp; d.flags |= LD_HAS_SSD_TEMP; }
        wcsncpy_s(d.status, helperBlk.status, _TRUNCATE);
    }
    live = true;

    hist[CPU][histHead] = d.cpuLoad;
    hist[GPU][histHead] = (d.flags & LD_HAS_GPU_LOAD) ? d.gpuLoad : 0;
    hist[RAM][histHead] = d.ramLoad;
    hist[SSD][histHead] = d.ssdActive;
    histHead = (histHead + 1) % kHist;
    histCount = std::min(histCount + 1, kHist);
    version++;

    // overheat: alert once, re-arm after cooling 5 °C or 10 minutes
    for (int m = 0; m < Count; ++m) {
        float t = Temp((Metric)m);
        if (std::isnan(t)) continue;
        float th = Threshold((Metric)m);
        if (t < th - 5) armed[m] = true;
        if (t >= th && (armed[m] || now - lastAlert[m] > 600)) {
            armed[m] = false;
            lastAlert[m] = now;
            alertPending = true;
            Log(L"overheat alert metric %d: %.1f", m, t);
        }
    }
}
