// Compile the production implementation against an in-memory Explorer/registry.
// No real taskbar, registry, desktop input, or running dock is changed by these tests.
#include <winsock2.h>
#include <ws2ipdef.h>
#include "../src/system.h"
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <netlistmgr.h>
#include <iphlpapi.h>
#include <cstdio>
#include <cstdlib>

struct ShellState {
    struct Opacity { LONG_PTR style=WS_EX_TOOLWINDOW; COLORREF color=0; BYTE alpha=255; DWORD flags=LWA_ALPHA; std::unordered_map<std::wstring,HANDLE> props; };
    std::unordered_map<HWND,Opacity> opacity;
    bool failOpacityBackup=false,failOpacity=false,ignoreOpacity=false,failOpacityRestore=false;
    bool explorer = true, secondary = true, backup = true;
    bool primaryEmpty = true, secondaryEmpty = true;
    bool primaryVisible = true, secondaryVisible = true;
    bool failRegion = false, failShow = false, ignoreState = false;
    bool failWrite = false, failCreate = false, failRead = false, failDelete = false, replaceShell = false;
    UINT_PTR state = ABS_AUTOHIDE;
    DWORD saved = 0;
    HWND primary = (HWND)1;
    unsigned deletes = 0, sets = 0;
} shell;

LONG_PTR WINAPI TestWindowLong(HWND h,int) { return shell.opacity[h].style; }
LONG_PTR WINAPI TestSetWindowLong(HWND h,int,LONG_PTR value) {
    auto& s=shell.opacity[h];auto old=s.style;
    if(!(shell.failOpacityRestore && !(value&WS_EX_LAYERED)))s.style=value;
    return old;
}
HANDLE WINAPI TestGetProp(HWND h,LPCWSTR name) { auto& p=shell.opacity[h].props;auto it=p.find(name);return it==p.end()?nullptr:it->second; }
BOOL WINAPI TestSetProp(HWND h,LPCWSTR name,HANDLE value) { if(shell.failOpacityBackup)return FALSE;shell.opacity[h].props[name]=value;return TRUE; }
HANDLE WINAPI TestRemoveProp(HWND h,LPCWSTR name) {HANDLE value=TestGetProp(h,name);shell.opacity[h].props.erase(name);return value;}
BOOL WINAPI TestGetLayered(HWND h,COLORREF* color,BYTE* alpha,DWORD* flags) {
    auto& s=shell.opacity[h];if(!(s.style&WS_EX_LAYERED))return FALSE;
    *color=s.color;*alpha=s.alpha;*flags=s.flags;return TRUE;
}
BOOL WINAPI TestSetLayered(HWND h,COLORREF color,BYTE alpha,DWORD flags) {
    auto& s=shell.opacity[h];if(shell.failOpacity || !(s.style&WS_EX_LAYERED))return FALSE;
    if(!shell.ignoreOpacity){s.color=color;s.alpha=alpha;s.flags=flags;}return TRUE;
}

HWND WINAPI TestFindWindow(LPCWSTR cls, LPCWSTR) { return !wcscmp(cls,L"Shell_TrayWnd") && shell.explorer ? shell.primary : nullptr; }
HWND WINAPI TestFindWindowEx(HWND, HWND after, LPCWSTR cls, LPCWSTR) {
    return shell.explorer && shell.secondary && !after && !wcscmp(cls,L"Shell_SecondaryTrayWnd") ? (HWND)2 : nullptr;
}
BOOL WINAPI TestIsWindow(HWND h) { return shell.explorer && (h == shell.primary || (shell.secondary && h == (HWND)2)); }
BOOL WINAPI TestVisible(HWND h) { return h == (HWND)2 ? shell.secondaryVisible : shell.primaryVisible; }
int WINAPI TestGetRegion(HWND h, HRGN) { return (h == (HWND)2 ? shell.secondaryEmpty : shell.primaryEmpty) ? NULLREGION : ERROR; }
int WINAPI TestSetRegion(HWND h, HRGN r, BOOL) {
    ++shell.sets;
    if (shell.failRegion && h == (HWND)2) return 0;
    (h == (HWND)2 ? shell.secondaryEmpty : shell.primaryEmpty) = r != nullptr;
    if (r) DeleteObject(r); // match ownership transfer on successful SetWindowRgn
    return 1;
}
BOOL WINAPI TestShow(HWND h, int) {
    bool old = TestVisible(h);
    if (!shell.failShow) (h == (HWND)2 ? shell.secondaryVisible : shell.primaryVisible) = true;
    return old;
}
UINT_PTR WINAPI TestAppbar(DWORD msg, PAPPBARDATA data) {
    if (msg == ABM_GETSTATE) return shell.state;
    if (msg == ABM_SETSTATE) {
        if (!shell.ignoreState) shell.state = data->lParam;
        if (shell.replaceShell) { shell.primary = (HWND)3; shell.replaceShell = false; }
    }
    return TRUE; // Windows reports TRUE even when state is not restored yet
}
LSTATUS WINAPI TestRead(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID data, LPDWORD bytes) {
    if (shell.failRead) return ERROR_ACCESS_DENIED;
    if (!shell.backup) return ERROR_FILE_NOT_FOUND;
    *(DWORD*)data = shell.saved; *bytes = sizeof(DWORD); return ERROR_SUCCESS;
}
LSTATUS WINAPI TestCreate(HKEY,LPCWSTR,DWORD,LPWSTR,DWORD,REGSAM,const SECURITY_ATTRIBUTES*,PHKEY out,LPDWORD) {
    if (shell.failCreate) return ERROR_ACCESS_DENIED;
    *out = (HKEY)1; return ERROR_SUCCESS;
}
LSTATUS WINAPI TestWrite(HKEY,LPCWSTR,DWORD,DWORD,const BYTE* data,DWORD) {
    if (shell.failWrite) return ERROR_ACCESS_DENIED;
    shell.backup = true; shell.saved = *(const DWORD*)data; return ERROR_SUCCESS;
}
LSTATUS WINAPI TestClose(HKEY) { return ERROR_SUCCESS; }
LSTATUS WINAPI TestDelete(HKEY,LPCWSTR,LPCWSTR) {
    if (shell.failDelete) return ERROR_ACCESS_DENIED;
    if (!shell.backup) return ERROR_FILE_NOT_FOUND;
    shell.backup = false; ++shell.deletes; return ERROR_SUCCESS;
}

#define FindWindowW TestFindWindow
#define FindWindowExW TestFindWindowEx
#define IsWindow TestIsWindow
#define IsWindowVisible TestVisible
#define GetWindowRgn TestGetRegion
#define SetWindowRgn TestSetRegion
#define ShowWindow TestShow
#define SHAppBarMessage TestAppbar
#define RegGetValueW TestRead
#define RegCreateKeyExW TestCreate
#define RegSetValueExW TestWrite
#define RegCloseKey TestClose
#define RegDeleteKeyValueW TestDelete
#define GetWindowLongPtrW TestWindowLong
#define SetWindowLongPtrW TestSetWindowLong
#define GetPropW TestGetProp
#define SetPropW TestSetProp
#define RemovePropW TestRemoveProp
#define GetLayeredWindowAttributes TestGetLayered
#define SetLayeredWindowAttributes TestSetLayered
#include "../src/system.cpp"

HWND g_mainWnd = nullptr;
void Log(const wchar_t*, ...) {}
double NowSeconds() { return 0; }
namespace nativetray { void CacheTrayButtons(HWND) {} }

static void Check(bool ok, const char* label) {
    if (!ok) { std::printf("FAIL: %s\n",label); std::exit(1); }
}
static void Pass(const char* label) { std::printf("PASS: %s\n", label); }
int main() {
    {
        shell = {}; NativeTaskbar bar;
        Check(bar.Restore() && !shell.backup && !shell.primaryEmpty && !shell.secondaryEmpty && !(shell.state & ABS_AUTOHIDE), "restore all taskbars");
        Pass("restore original non-autohide state on both taskbars");
    }
    {
        shell = {}; shell.saved = ABS_AUTOHIDE; shell.state = 0; NativeTaskbar bar;
        Check(bar.Restore() && !shell.backup && (shell.state & ABS_AUTOHIDE), "original autohide");
        Pass("preserve original autohide preference");
    }
    {
        shell = {}; shell.explorer = false; NativeTaskbar bar;
        Check(!bar.Restore() && shell.backup && !shell.deletes, "missing Explorer backup");
        shell.explorer = true;
        Check(bar.Restore() && !shell.backup, "retry Explorer"); Pass("retain backup while Explorer is absent, restore after it returns");
    }
    {
        shell = {}; shell.failRegion = true; NativeTaskbar bar;
        Check(!bar.Restore() && shell.backup, "secondary failure"); shell.failRegion = false;
        Check(bar.Restore() && !shell.backup, "secondary retry"); Pass("secondary taskbar failure preserves recovery state");
    }
    {
        shell = {}; shell.ignoreState = true; NativeTaskbar bar;
        Check(!bar.Restore() && shell.backup, "read-back failure"); shell.ignoreState = false;
        Check(bar.Restore(), "read-back retry"); Pass("TRUE from ABM_SETSTATE alone never counts as recovery");
    }
    {
        shell = {}; shell.failShow = true; shell.primaryVisible = false; NativeTaskbar bar;
        Check(!bar.Restore() && shell.backup, "visibility failure"); shell.failShow = false;
        Check(bar.Restore(), "visibility retry"); Pass("invisible taskbar preserves recovery state");
    }
    {
        shell = {}; shell.backup = false; shell.failWrite = true; shell.state = 0; NativeTaskbar bar;
        Check(!bar.Hide() && !bar.hidden && !shell.sets && shell.state == 0, "backup write failure"); Pass("never hide when backup write fails");
    }
    {
        shell = {}; shell.failCreate = true; NativeTaskbar bar;
        Check(!bar.Hide() && !bar.hidden && !shell.sets, "backup key failure"); Pass("never hide when backup registry key cannot open");
    }
    {
        shell = {}; shell.failRead = true; NativeTaskbar bar;
        Check(!bar.Restore() && shell.backup && !shell.sets, "backup read failure"); Pass("unreadable backup is retained");
    }
    {
        shell = {}; shell.failDelete = true; NativeTaskbar bar;
        Check(!bar.Restore() && shell.backup, "backup deletion failure"); shell.failDelete = false;
        Check(bar.Restore() && !shell.backup, "delete retry"); Pass("backup deletion failure remains retryable");
    }
    {
        shell = {}; shell.replaceShell = true; NativeTaskbar bar;
        Check(!bar.Restore() && shell.backup, "Explorer replacement");
        Check(bar.Restore(), "new Explorer retry"); Pass("Explorer replacement during restoration requires another pass");
    }
    {
        shell = {}; shell.saved = ABS_AUTOHIDE; shell.state = 0; NativeTaskbar bar;
        Check(bar.Hide() && shell.saved == ABS_AUTOHIDE, "existing backup");
        Check(bar.Restore() && (shell.state & ABS_AUTOHIDE), "existing preference restore"); Pass("restart never overwrites a previous recovery record");
    }
    {
        shell = {}; shell.backup = false; NativeTaskbar bar;
        Check(bar.Restore() && !shell.primaryEmpty && !shell.secondaryEmpty, "no backup recovery"); Pass("restore visible regions even when no preference backup exists");
    }
    {
        shell={};NativeTaskbar bar;Check(bar.Hide(),"guard setup");
        bool ran=false;
        Check(bar.WithInvisibleTaskbars([&]{ran=true;Check(taskbarvisibility::IsProtected(shell.primary)&&taskbarvisibility::IsProtected((HWND)2),"all bars shielded before action");shell.primaryEmpty=false;return true;}),"guarded action");
        Check(ran && shell.primaryEmpty,"Explorer region rebuild is re-clipped");
        Check(bar.Restore() && !(shell.opacity[shell.primary].style&WS_EX_LAYERED) && !(shell.opacity[(HWND)2].style&WS_EX_LAYERED),"added transparency removed");
        Pass("Shell actions require an opacity shield on both taskbars and restore original styles");
    }
    {
        shell={};auto& original=shell.opacity[shell.primary];original.style|=WS_EX_LAYERED;original.alpha=91;original.color=0x224466;original.flags=LWA_ALPHA|LWA_COLORKEY;
        NativeTaskbar bar;Check(bar.Hide() && bar.Restore(),"existing layered recovery");auto& restored=shell.opacity[shell.primary];
        Check(restored.alpha==91 && restored.color==0x224466 && restored.flags==(LWA_ALPHA|LWA_COLORKEY) && (restored.style&WS_EX_LAYERED),"exact layered attributes");
        Pass("pre-existing transparency and color key survive hide and restore");
    }
    {
        shell={};{NativeTaskbar crashed;Check(crashed.Hide(),"crash setup");}
        NativeTaskbar recovery;Check(recovery.Restore() && !shell.backup && !(shell.opacity[shell.primary].style&WS_EX_LAYERED),"new process opacity recovery");
        Pass("fresh recovery instance restores opacity using window properties");
    }
    {
        shell={};NativeTaskbar bar;Check(bar.Hide(),"retry setup");shell.failOpacityRestore=true;
        Check(!bar.Restore() && shell.backup && TestGetProp(shell.primary,taskbarvisibility::kState),"opacity failure retains recovery data");
        shell.failOpacityRestore=false;Check(bar.Restore()&&!shell.backup,"opacity retry");
        Pass("opacity restoration failure preserves backups for retry");
    }
    {
        shell={};shell.failOpacityBackup=true;NativeTaskbar bar;bar.Hide();bool ran=false;
        Check(!bar.WithInvisibleTaskbars([&]{ran=true;return true;})&&!ran,"failed opacity backup blocks action");
        Check(!(shell.opacity[shell.primary].style&WS_EX_LAYERED),"no opacity mutation without backup");
        Pass("opacity backup failure never invokes Shell controls");
    }
    {
        shell={};shell.ignoreOpacity=true;NativeTaskbar bar;bar.Hide();bool ran=false;
        Check(!bar.WithInvisibleTaskbars([&]{ran=true;return true;})&&!ran,"opacity readback required");
        Pass("successful API return without zero opacity never permits Shell invocation");
    }
    {
        shell={};NativeTaskbar bar;bool ran=false;
        Check(!bar.WithInvisibleTaskbars([&]{ran=true;return true;})&&!ran,"no guard outside active dock");
        Pass("inactive dock cannot activate taskbar controls");
    }
    std::puts("20 recovery regression cases passed; real Windows taskbar and registry untouched.");
}
