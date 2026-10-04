// LiquidDockSensors: optional elevated helper. Reads what needs a driver (CPU temperature through the
// PawnIO driver; AMD Zen and Intel) plus the drive temperature when it needs admin, and publishes it
// to shared memory for the dock.
//   (no args)    run the sensor loop (exits ~30 s after the dock is gone)
//   --install    register the on-demand elevated scheduled task, then run
//   --uninstall  remove the scheduled task
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <sddl.h>
#define SECURITY_WIN32
#include <security.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>
#include "../src/hw_sample.h"
#include "resource.h"

// The helper runs elevated, so it lives - and is registered as a scheduled task - only in an
// admin-only folder (%ProgramFiles%\LiquidDock). A copy in the user's install folder is used just
// once, through UAC, to put it there (--install).
static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) { s = p; CoTaskMemFree(p); }
    return s;
}
static std::wstring SecureDir() { return KnownDir(FOLDERID_ProgramFiles) + L"\\LiquidDock"; }
static std::wstring SecurePath() { return SecureDir() + L"\\LiquidDockSensors.exe"; }
static std::wstring Schtasks() { return KnownDir(FOLDERID_System) + L"\\schtasks.exe"; }
static std::wstring SelfPath() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); return p; }
static bool RunningFromSecureDir() { return _wcsicmp(SelfPath().c_str(), SecurePath().c_str()) == 0; }

static FILE* g_log = nullptr;
static void Log(const wchar_t* fmt, ...) {
    // an elevated process must not write into user-writable folders: log only from the secure copy
    if (!g_log && RunningFromSecureDir())
        g_log = _wfsopen((SecureDir() + L"\\sensors.log").c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfwprintf(g_log, fmt, ap);
    va_end(ap);
    fputwc(L'\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- PawnIO (CPU temperature)

typedef HRESULT(WINAPI* pawnio_open_t)(PHANDLE);
typedef HRESULT(WINAPI* pawnio_load_t)(HANDLE, const UCHAR*, SIZE_T);
typedef HRESULT(WINAPI* pawnio_execute_t)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT(WINAPI* pawnio_close_t)(HANDLE);

struct CpuTemp {
    enum Vendor { None, Amd, Intel } vendor = None;
    HMODULE lib = nullptr;
    HANDLE h = nullptr;
    HANDLE pciMutex = nullptr;
    pawnio_execute_t exec = nullptr;
    pawnio_close_t close = nullptr;
    int tjMax = 100;

    static Vendor DetectVendor() {
        int r[4]; __cpuid(r, 0);
        char v[13] = {}; memcpy(v, &r[1], 4); memcpy(v + 4, &r[3], 4); memcpy(v + 8, &r[2], 4);
        if (!strcmp(v, "AuthenticAMD")) return Amd;
        if (!strcmp(v, "GenuineIntel")) return Intel;
        return None;
    }

    bool Init() {
        Vendor v = DetectVendor();
        if (v == None) { Log(L"unsupported CPU vendor"); return false; }
        // fixed, admin-only location only (never the search path)
        lib = LoadLibraryExW((KnownDir(FOLDERID_ProgramFiles) + L"\\PawnIO\\PawnIOLib.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib) { Log(L"PawnIOLib.dll not found (PawnIO not installed)"); return false; }
        auto open = (pawnio_open_t)GetProcAddress(lib, "pawnio_open");
        auto load = (pawnio_load_t)GetProcAddress(lib, "pawnio_load");
        exec = (pawnio_execute_t)GetProcAddress(lib, "pawnio_execute");
        close = (pawnio_close_t)GetProcAddress(lib, "pawnio_close");
        if (!open || !load || !exec || !close) return false;
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(v == Amd ? IDR_AMD17 : IDR_INTELMSR), RT_RCDATA);
        if (!r) return false;
        const UCHAR* blob = (const UCHAR*)LockResource(LoadResource(nullptr, r));
        DWORD size = SizeofResource(nullptr, r);
        HRESULT hr = open(&h);
        if (FAILED(hr)) { Log(L"pawnio_open failed %08x", hr); h = nullptr; return false; }
        hr = load(h, blob, size);
        if (FAILED(hr)) { Log(L"pawnio_load failed %08x (CPU family not supported?)", hr); close(h); h = nullptr; return false; }
        if (v == Amd) {
            // same mutex HWiNFO / LibreHardwareMonitor use for PCI config access
            SECURITY_ATTRIBUTES sa = { sizeof(sa) };
            ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
            pciMutex = CreateMutexW(&sa, FALSE, L"Global\\Access_PCI");
            if (!pciMutex) pciMutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
            LocalFree(sa.lpSecurityDescriptor);
        } else {
            ULONG64 target = 0;
            if (Call("ioctl_read_msr", 0x1A2, target)) {   // IA32_TEMPERATURE_TARGET
                int t = int((target >> 16) & 0xFF);
                if (t >= 60 && t <= 125) tjMax = t;
            }
        }
        vendor = v;
        return true;
    }

    bool Call(const char* fn, ULONG64 in, ULONG64& out) {
        SIZE_T rs = 0;
        return SUCCEEDED(exec(h, fn, &in, 1, &out, 1, &rs)) && rs == 1;
    }

    bool Read(float& t) {
        if (!h) return false;
        ULONG64 v = 0;
        if (vendor == Amd) {
            DWORD w = pciMutex ? WaitForSingleObject(pciMutex, 100) : WAIT_OBJECT_0;
            if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return false;
            bool ok = Call("ioctl_read_smn", 0x00059800, v);   // THM_TCON_CUR_TMP
            if (pciMutex) ReleaseMutex(pciMutex);
            if (!ok) return false;
            uint32_t r = (uint32_t)v;
            t = ((r >> 21) & 0x7FF) / 8.0f;                      // 0.125 °C units
            if (r & (1u << 19)) t -= 49.0f;                      // extended range
        } else {
            if (!Call("ioctl_read_msr", 0x1B1, v)) return false; // IA32_PACKAGE_THERM_STATUS
            t = float(tjMax - int((v >> 16) & 0x7F));
        }
        return t > -20 && t < 150;
    }
};

// ---------------------------------------------------------------- scheduled task

static int RunHidden(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'&') o += L"&amp;"; else if (c == L'<') o += L"&lt;"; else if (c == L'>') o += L"&gt;"; else if (c == L'"') o += L"&quot;"; else o += c;
    }
    return o;
}

// End a running secure copy so its file can be replaced or deleted.
static void StopSecureCopy() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\LiquidDockSensors.Instance");
    if (!m) return;
    CloseHandle(m);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"LiquidDockSensors.exe") || pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n) && !_wcsicmp(path, SecurePath().c_str())) {
            TerminateProcess(p, 0);
            WaitForSingleObject(p, 3000);
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
}

static bool InstallTask() {
    wchar_t user[256]; ULONG ul = 256;
    if (!GetUserNameExW(NameSamCompatible, user, &ul)) return false;
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>Liquid Dock hardware sensor helper (CPU temperature)</Description></RegistrationInfo>\r\n"
        L"<Triggers />\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + XmlEscape(user) + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable><IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"" + XmlEscape(SecurePath()) + L"\"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n";
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"LiquidDockSensors.xml";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD bw; WORD bom = 0xFEFF;
    WriteFile(f, &bom, 2, &bw, nullptr);
    WriteFile(f, xml.data(), DWORD(xml.size() * 2), &bw, nullptr);
    CloseHandle(f);
    int rc = RunHidden(L"\"" + Schtasks() + L"\" /Create /F /TN " LD_SENSOR_TASK L" /XML \"" + path + L"\"");
    DeleteFileW(path.c_str());
    Log(L"task install rc=%d", rc);
    return rc == 0;
}

// ---------------------------------------------------------------- main loop

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    std::wstring args = cmd ? cmd : L"";
    if (args.find(L"--uninstall") != std::wstring::npos) {
        int rc = RunHidden(L"\"" + Schtasks() + L"\" /Delete /F /TN " LD_SENSOR_TASK);
        StopSecureCopy();
        DeleteFileW(SecurePath().c_str());
        DeleteFileW((SecureDir() + L"\\sensors.log").c_str());
        RemoveDirectoryW(SecureDir().c_str());
        return rc == 0 ? 0 : 1;
    }
    if (args.find(L"--install") != std::wstring::npos) {
        // copy into the admin-only folder, register the task there, start it, and leave
        StopSecureCopy();
        CreateDirectoryW(SecureDir().c_str(), nullptr);
        if (!RunningFromSecureDir() && !CopyFileW(SelfPath().c_str(), SecurePath().c_str(), FALSE)) return 1;
        if (!InstallTask()) return 1;
        RunHidden(L"\"" + Schtasks() + L"\" /Run /TN " LD_SENSOR_TASK);
        return 0;
    }
    if (!RunningFromSecureDir()) return 1;   // never run the elevated loop from a user-writable folder

    HANDLE inst = CreateMutexW(nullptr, TRUE, L"Local\\LiquidDockSensors.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // readable by the (non-elevated) dock running as the same interactive user
    SECURITY_ATTRIBUTES sa = { sizeof(sa) };
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;BA)(A;;GA;;;SY)(A;;GR;;;IU)S:(ML;;NW;;;ME)",
                                                         SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(LDSensorBlock), LD_SENSOR_MAP);
    LocalFree(sa.lpSecurityDescriptor);
    if (!map) { Log(L"CreateFileMapping failed %lu", GetLastError()); return 1; }
    auto* blk = (LDSensorBlock*)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(LDSensorBlock));
    if (!blk) return 1;
    ZeroMemory(blk, sizeof(*blk));
    blk->magic = LD_SENSOR_MAGIC;
    blk->size = sizeof(LDSensorBlock);

    CpuTemp cpu; bool haveCpu = cpu.Init();
    hw::Sampler sampler; sampler.Init();
    Log(L"cpu temp=%d vendor=%d", haveCpu, (int)cpu.vendor);
    ULONGLONG lastDock = GetTickCount64();

    for (;;) {
        Sleep(1000);
        LDSensorBlock s = {};
        sampler.Sample(s);
        if (haveCpu && cpu.Read(s.cpuTemp)) s.flags |= LD_HAS_CPU_TEMP;
        if (!haveCpu) wcsncpy_s(s.status, L"CPU 온도: 지원하지 않는 CPU이거나 PawnIO가 없습니다", _TRUNCATE);

        // seqlock publish
        blk->seq++;
        MemoryBarrier();
        uint32_t seq = blk->seq;
        s.magic = LD_SENSOR_MAGIC; s.size = sizeof(LDSensorBlock); s.tick = GetTickCount64();
        s.seq = seq;
        memcpy((void*)blk, &s, sizeof(s));
        MemoryBarrier();
        blk->seq = seq + 1;

        // exit once the dock has been gone for 30 s
        HANDLE dockMutex = OpenMutexW(SYNCHRONIZE, FALSE, L"LiquidDock.SingleInstance");
        if (dockMutex) { CloseHandle(dockMutex); lastDock = GetTickCount64(); }
        else if (GetTickCount64() - lastDock > 30000) break;
    }
    if (cpu.h) cpu.close(cpu.h);
    UnmapViewOfFile(blk);
    CloseHandle(map);
    if (inst) CloseHandle(inst);
    return 0;
}
