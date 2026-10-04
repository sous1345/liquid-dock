#pragma once
// Hardware readings that need no administrator rights: CPU/RAM load, NVIDIA GPU (NVML, temperature
// included), GPU load for any vendor (PDH), system drive activity/capacity/name and - when the drive
// allows it - its temperature. Shared by the dock and the elevated helper.
#include <windows.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>
#include "sensor_shared.h"

namespace hw {

struct Nvml {
    typedef int (*init_t)();
    typedef int (*handle_t)(unsigned, void**);
    typedef int (*temp_t)(void*, int, unsigned*);
    struct Util { unsigned gpu, memory; };
    typedef int (*util_t)(void*, Util*);
    struct Mem { unsigned long long total, free, used; };
    typedef int (*mem_t)(void*, Mem*);
    typedef int (*name_t)(void*, char*, unsigned);
    temp_t temp = nullptr; util_t util = nullptr; mem_t mem = nullptr;
    void* dev = nullptr;
    std::wstring name;

    bool Init() {
        // System32 or NVIDIA's own folder only - never the application directory / search path
        HMODULE m = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) { wchar_t p[MAX_PATH]; ExpandEnvironmentStringsW(L"%ProgramFiles%\\NVIDIA Corporation\\NVSMI\\nvml.dll", p, MAX_PATH); m = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); }
        if (!m) return false;
        auto init = (init_t)GetProcAddress(m, "nvmlInit_v2");
        auto hnd = (handle_t)GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
        auto nm = (name_t)GetProcAddress(m, "nvmlDeviceGetName");
        temp = (temp_t)GetProcAddress(m, "nvmlDeviceGetTemperature");
        util = (util_t)GetProcAddress(m, "nvmlDeviceGetUtilizationRates");
        mem = (mem_t)GetProcAddress(m, "nvmlDeviceGetMemoryInfo");
        if (!init || !hnd || !temp || !util || !mem) return false;
        if (init() != 0 || hnd(0, &dev) != 0) return false;
        char n[96] = {};
        if (nm && nm(dev, n, sizeof(n)) == 0) {
            wchar_t w[96]; MultiByteToWideChar(CP_UTF8, 0, n, -1, w, 96); name = w;
        }
        return true;
    }
};

inline std::wstring CpuName() {
    std::wstring n = L"CPU";
    wchar_t buf[128]; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        n = buf;
        for (const wchar_t* junk : { L"AMD ", L"Intel(R) ", L"Core(TM) ", L"(R)", L"(TM)", L" Processor", L" CPU",
                                     L" 4-Core", L" 6-Core", L" 8-Core", L" 12-Core", L" 16-Core", L" 24-Core" }) {
            size_t p; while ((p = n.find(junk)) != std::wstring::npos) n.erase(p, wcslen(junk));
        }
        size_t at = n.find(L" @"); if (at != std::wstring::npos) n.erase(at);
        while (!n.empty() && n.back() == L' ') n.pop_back();
        while (!n.empty() && n.front() == L' ') n.erase(0, 1);
    }
    return n;
}

inline int SystemDiskNumber() {
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    wchar_t vol[] = L"\\\\.\\C:"; vol[4] = win[0];
    HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    STORAGE_DEVICE_NUMBER n = {}; DWORD br = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof(n), &br, nullptr);
    CloseHandle(h);
    return ok ? (int)n.DeviceNumber : 0;
}

inline HANDLE OpenDisk(int n) {
    wchar_t p[64]; swprintf_s(p, L"\\\\.\\PhysicalDrive%d", n);
    return CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

inline bool DiskTemp(HANDLE h, float& t) {
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {}; DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, nullptr)) return false;
    auto* d = (STORAGE_TEMPERATURE_DATA_DESCRIPTOR*)buf;
    if (d->InfoCount == 0) return false;
    t = (float)d->TemperatureInfo[0].Temperature;
    return t > -40 && t < 150;
}

inline std::wstring DiskName(HANDLE h) {
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
    BYTE buf[2048] = {}; DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, nullptr)) return L"SSD";
    auto* d = (STORAGE_DEVICE_DESCRIPTOR*)buf;
    std::string s;
    if (d->ProductIdOffset && d->ProductIdOffset < br) s = (const char*)buf + d->ProductIdOffset;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) return L"SSD";
    wchar_t w[128]; MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w, 128);
    return w;
}

class Sampler {
public:
    void Init() {
        haveNv = nv.Init();
        cpuName = CpuName();
        diskNo = SystemDiskNumber();
        disk = OpenDisk(diskNo);
        diskName = disk != INVALID_HANDLE_VALUE ? DiskName(disk) : L"SSD";
        wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH); sysRoot[0] = win[0];
        if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\% Idle Time", 0, &diskIdle);
            if (!haveNv) PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gpu3d);
            PdhCollectQueryData(q);
        }
        GetSystemTimes(&pi, &pk, &pu);
    }

    // Fills everything except the CPU temperature.
    void Sample(LDSensorBlock& s) {
        FILETIME ci, ck, cu; GetSystemTimes(&ci, &ck, &cu);
        uint64_t di = U(ci) - U(pi), dk = U(ck) - U(pk), du = U(cu) - U(pu);
        pi = ci; pk = ck; pu = cu;
        uint64_t tot = dk + du;
        s.cpuLoad = tot ? 100.0f * float(tot - di) / float(tot) : 0;

        MEMORYSTATUSEX ms = { sizeof(ms) }; GlobalMemoryStatusEx(&ms);
        s.ramTotal = ms.ullTotalPhys / 1073741824.0f;
        s.ramUsed = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0f;
        s.ramLoad = (float)ms.dwMemoryLoad;

        if (haveNv) {
            unsigned t = 0; Nvml::Util u = {}; Nvml::Mem m = {};
            if (nv.temp(nv.dev, 0, &t) == 0) { s.gpuTemp = (float)t; s.flags |= LD_HAS_GPU; }
            if (nv.util(nv.dev, &u) == 0) s.gpuLoad = (float)u.gpu;
            if (nv.mem(nv.dev, &m) == 0) { s.gpuMemUsed = m.used / 1073741824.0f; s.gpuMemTotal = m.total / 1073741824.0f; }
        }

        if (disk != INVALID_HANDLE_VALUE && DiskTemp(disk, s.ssdTemp)) s.flags |= LD_HAS_SSD_TEMP;
        ULARGE_INTEGER fa, tb, tf;
        if (GetDiskFreeSpaceExW(sysRoot, &fa, &tb, &tf)) {
            s.ssdTotal = tb.QuadPart / 1073741824.0f;
            s.ssdUsed = (tb.QuadPart - tf.QuadPart) / 1073741824.0f;
        }
        if (q && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            wchar_t prefix[16]; swprintf_s(prefix, L"%d ", diskNo);
            ForEach(diskIdle, [&](const wchar_t* name, double v) {
                if (wcsncmp(name, prefix, wcslen(prefix)) == 0) s.ssdActive = (float)std::clamp(100.0 - v, 0.0, 100.0);
            });
            if (gpu3d) {
                double sum = 0;
                ForEach(gpu3d, [&](const wchar_t*, double v) { sum += v; });
                s.gpuLoad = (float)std::min(100.0, sum);
                s.flags |= LD_HAS_GPU_LOAD;
            }
        }
        if (haveNv) s.flags |= LD_HAS_GPU_LOAD;

        wcsncpy_s(s.cpuName, cpuName.c_str(), _TRUNCATE);
        wcsncpy_s(s.gpuName, nv.name.empty() ? L"GPU" : nv.name.c_str(), _TRUNCATE);
        wcsncpy_s(s.ssdName, diskName.c_str(), _TRUNCATE);
    }

private:
    template <class F> void ForEach(PDH_HCOUNTER c, F f) {
        if (!c) return;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) return;
        std::vector<BYTE> buf(bytes);
        auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return;
        for (DWORD i = 0; i < count; ++i)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                f(items[i].szName, items[i].FmtValue.doubleValue);
    }
    static uint64_t U(FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

    Nvml nv; bool haveNv = false;
    std::wstring cpuName, diskName;
    int diskNo = 0;
    HANDLE disk = INVALID_HANDLE_VALUE;
    wchar_t sysRoot[4] = L"C:\\";
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER diskIdle = nullptr, gpu3d = nullptr;
    FILETIME pi{}, pk{}, pu{};
};

} // namespace hw
