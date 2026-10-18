// Explicit interactive integration test. Uses the production Shell action,
// creates only disposable fixture windows, and never injects keyboard input.
#include "../src/native_tray.h"
#include "../src/system.h"
#include <cstdio>
#include <cstdlib>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
void SendKeyCombo(std::initializer_list<WORD>) { std::abort(); } // forbidden in this fixture
NativeTaskbar g_taskbar;
bool NativeTaskbar::WithInvisibleTaskbars(const std::function<bool()>&) { std::abort(); }
static void Pump(int ms) {
    const auto end = GetTickCount64() + ms;
    do { MSG msg; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg);DispatchMessageW(&msg);} Sleep(10); } while(GetTickCount64()<end);
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2) {puts("Supply expected foreground process ID to authorize this interactive fixture.");return 2;}
    HWND foreground=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(foreground,&pid);
    if(pid!=wcstoul(argv[1],nullptr,10)){puts("Foreground changed; test not started.");return 2;}
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT r,LPARAM p)->BOOL{
        reinterpret_cast<std::vector<RECT>*>(p)->push_back(*r);return TRUE;
    },reinterpret_cast<LPARAM>(&monitors));
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"LiquidDockDesktopTest";wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
    std::vector<HWND> windows;
    for(const auto& r:monitors){
        HWND h=CreateWindowExW(WS_EX_APPWINDOW,wc.lpszClassName,L"LiquidDock verification - temporary window",WS_OVERLAPPEDWINDOW,
            r.left+150,r.top+150,400,240,nullptr,nullptr,wc.hInstance,nullptr);
        if(!h)return 2;windows.push_back(h);ShowWindow(h,SW_SHOWNOACTIVATE);
    }
    HWND minimized=CreateWindowW(wc.lpszClassName,L"LiquidDock verification - already minimized",WS_OVERLAPPEDWINDOW,
        300,300,350,220,nullptr,nullptr,wc.hInstance,nullptr);
    if(!minimized)return 2;ShowWindow(minimized,SW_SHOWMINNOACTIVE);
    SetForegroundWindow(windows.front());
    Pump(1500);
    if(argc>2 && wcscmp(argv[2],L"--wait-activation")==0){
        puts("WAITING for fixture activation");fflush(stdout);
        const auto deadline=GetTickCount64()+60000;
        while(GetTickCount64()<deadline && std::find(windows.begin(),windows.end(),GetForegroundWindow())==windows.end())Pump(100);
        if(std::find(windows.begin(),windows.end(),GetForegroundWindow())==windows.end()){
            for(auto h:windows)DestroyWindow(h);DestroyWindow(minimized);puts("Fixture was not activated; no Shell action performed.");return 2;
        }
        Pump(500);
    }
    if(std::find(windows.begin(),windows.end(),GetForegroundWindow())==windows.end()){
        for(auto h:windows)DestroyWindow(h);DestroyWindow(minimized);
        puts("Fixture did not receive foreground; Shell test not performed.");return 2;
    }
    // Record pre-existing normal user windows solely for failure cleanup. Do
    // not restore windows that were already minimized before the test.
    struct UserWindow {HWND h;WINDOWPLACEMENT placement;};
    std::vector<UserWindow> userNormal;
    EnumWindows([](HWND h,LPARAM p)->BOOL{
        DWORD pid=0;GetWindowThreadProcessId(h,&pid);
        LONG_PTR style=GetWindowLongPtrW(h,GWL_STYLE),ex=GetWindowLongPtrW(h,GWL_EXSTYLE);
        if(pid!=GetCurrentProcessId()&&IsWindowVisible(h)&&!IsIconic(h)&&!(ex&WS_EX_TOOLWINDOW)&&(style&WS_CAPTION)){
            WINDOWPLACEMENT placement{sizeof(placement)};
            if(GetWindowPlacement(h,&placement))((std::vector<UserWindow>*)p)->push_back({h,placement});
        }
        return TRUE;
    },(LPARAM)&userNormal);
    auto trace = [&](const char* phase) {
        wchar_t cls[128]{};GetClassNameW(GetForegroundWindow(),cls,128);
        printf("TRACE %s foreground_class=%ls\n",phase,cls);
        for(auto h:windows){DWORD cloak=0;DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloak,sizeof(cloak));
            printf("  fixture iconic=%d visible=%d cloaked=%lu style=%llx\n",IsIconic(h),IsWindowVisible(h),cloak,(unsigned long long)GetWindowLongPtrW(h,GWL_STYLE));}
    };
    trace("before");
    bool baseline=IsIconic(minimized)!=0;
    for(auto h:windows)baseline &= IsWindowVisible(h) && !IsIconic(h);
    if(!baseline){for(auto h:windows)DestroyWindow(h);DestroyWindow(minimized);return 2;}
    bool called=nativetray::ToggleDesktop();Pump(800);
    trace("shown-desktop");
    bool hidden=called;
    for(auto h:windows)hidden &= IsIconic(h)!=0;
    bool minimizedKept=IsIconic(minimized)!=0;
    // Restore through the matching Shell toggle, not ShowWindow on user windows.
    bool restoredCall=called && nativetray::ToggleDesktop();Pump(2500);
    trace("restored");
    bool restored=restoredCall;
    for(auto h:windows)restored &= IsWindowVisible(h) && !IsIconic(h);
    minimizedKept &= IsIconic(minimized)!=0;
    printf("SHELL monitors=%zu call=%d hidden=%d restore_call=%d restored=%d existing_minimized_preserved=%d\n",
        monitors.size(),called,hidden,restoredCall,restored,minimizedKept);
    int cleanupRestores=0;
    for(auto& entry:userNormal)if(IsWindow(entry.h)&&IsIconic(entry.h)){SetWindowPlacement(entry.h,&entry.placement);++cleanupRestores;}
    printf("CLEANUP preexisting_windows_recovered=%d (not counted as Shell restore success)\n",cleanupRestores);
    for(auto h:windows)DestroyWindow(h);DestroyWindow(minimized);
    if(IsWindow(foreground))SetForegroundWindow(foreground);
    CoUninitialize();
    return hidden && restored && minimizedKept ? 0 : 1;
}
