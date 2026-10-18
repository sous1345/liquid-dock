#include "../src/common.h"
#include "../src/system.h"
#include "../src/taskbar_visibility.h"
#include <thread>
#include <cstdio>
#include "../src/native_tray.cpp" // exercise both cold and cached production UIA paths
HWND g_mainWnd=nullptr;
struct Monitor {HMONITOR h;RECT bounds,work;};
struct Fixture {HWND h;RECT bounds;bool maximized;};
static void Pump(int ms){auto end=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5);}while(GetTickCount64()<end);}
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 2;HWND original=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(original,&pid);
 if(pid!=wcstoul(argv[1],nullptr,10)){puts("Foreground changed; no test started.");return 2;}
 HWND popup=FindWindowW(L"TopLevelWindowForOverflowXamlIsland",nullptr);
 if(popup&&IsWindowVisible(popup)){puts("User tray popup is open; test not started.");return 2;}
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 std::vector<Monitor> monitors;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR h,HDC,LPRECT,LPARAM p)->BOOL{MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(h,&mi);((std::vector<Monitor>*)p)->push_back({h,mi.rcMonitor,mi.rcWork});return TRUE;},(LPARAM)&monitors);
 std::vector<HWND> bars;for(auto cls:{L"Shell_TrayWnd",L"Shell_SecondaryTrayWnd"}){HWND h=nullptr;while((h=FindWindowExW(nullptr,h,cls,nullptr)))bars.push_back(h);}
 for(auto h:bars)if(!taskbarvisibility::IsProtected(h)){puts("Installed dock protection absent.");return 2;}
 if(!g_taskbar.Hide())return 2;
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"LiquidDockWorkspaceFixture";wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
 std::vector<Fixture> fixtures;
 for(auto& m:monitors)for(int maximized=0;maximized<2;++maximized){
  HWND h=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"LiquidDock work-area verification",WS_OVERLAPPEDWINDOW,m.bounds.left+150,m.bounds.top+150,500,300,nullptr,nullptr,wc.hInstance,nullptr);
  if(!h)return 2;ShowWindow(h,SW_SHOWNOACTIVATE);
  if(maximized){WINDOWPLACEMENT wp{sizeof(wp)};GetWindowPlacement(h,&wp);wp.showCmd=SW_SHOWMAXIMIZED;SetWindowPlacement(h,&wp);}
  fixtures.push_back({h,{},maximized!=0});
 }
 Pump(200);for(auto& f:fixtures)GetWindowRect(f.h,&f.bounds);
 int failures=0,cases=0;
 for(size_t idx=0;idx<monitors.size();++idx)for(int edge=0;edge<4;++edge)for(int cold=1;cold>=0;--cold){
  if(cold){for(auto& button:nativetray::g_button)button.Reset();nativetray::g_buttonTray=nullptr;}
  auto m=monitors[idx].bounds;int cx=(m.left+m.right)/2,cy=(m.top+m.bottom)/2;RECT dock{};POINT anchor{};
  switch((Edge)edge){case Edge::Bottom:dock={cx-250,m.bottom-90,cx+250,m.bottom-5};anchor={cx,m.bottom-45};break;case Edge::Left:dock={m.left+5,cy-250,m.left+90,cy+250};anchor={m.left+45,cy};break;case Edge::Right:dock={m.right-90,cy-250,m.right-5,cy+250};anchor={m.right-45,cy};break;case Edge::Top:dock={cx-250,m.top+5,cx+250,m.top+90};anchor={cx,m.top+45};break;}
  std::atomic<bool> stop=false;std::atomic<int> samples=0,workChanges=0,windowChanges=0,opacityFailures=0;
  std::vector<RECT> changedAreas(monitors.size());std::vector<bool> sawArea(monitors.size());
  std::thread observer([&]{while(!stop){++samples;for(size_t k=0;k<monitors.size();++k){auto& monitor=monitors[k];MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(monitor.h,&mi)||!EqualRect(&monitor.work,&mi.rcWork)){++workChanges;if(!sawArea[k]){changedAreas[k]=mi.rcWork;sawArea[k]=true;}}}for(auto& f:fixtures){RECT r;if(!GetWindowRect(f.h,&r)||!EqualRect(&r,&f.bounds))++windowChanges;}for(auto h:bars)if(!taskbarvisibility::IsProtected(h))++opacityFailures;Sleep(2);}});
  bool opened=nativetray::OpenOverflow(anchor,(Edge)edge,dock);Pump(180);
  popup=FindWindowW(L"TopLevelWindowForOverflowXamlIsland",nullptr);
  bool visible=popup&&IsWindowVisible(popup);bool close=visible&&nativetray::OpenOverflow(anchor,(Edge)edge,dock);auto closeStart=GetTickCount64();Pump(180);
  bool closedAt180=!popup||!IsWindowVisible(popup);
  while(popup&&IsWindowVisible(popup)&&GetTickCount64()-closeStart<1500)Pump(25);
  bool closed=!popup||!IsWindowVisible(popup);stop=true;observer.join();
  bool final=true;for(auto& f:fixtures){RECT r{};GetWindowRect(f.h,&r);final&=EqualRect(&r,&f.bounds)!=FALSE;}
  bool ok=opened&&visible&&close&&closed&&final&&!workChanges&&!windowChanges&&!opacityFailures;
  printf("WORKSPACE monitor=%zu edge=%d cold=%d opened=%d closed=%d samples=%d work_changes=%d window_changes=%d opacity_failures=%d final_unchanged=%d pass=%d\n",idx,edge,cold,opened,closed,(int)samples,(int)workChanges,(int)windowChanges,(int)opacityFailures,final,ok);fflush(stdout);
  printf("CLOSE closed_at_180=%d closed_at_deadline=%d elapsed_ms=%llu\n",closedAt180,closed,GetTickCount64()-closeStart);
  for(size_t k=0;k<monitors.size();++k)if(sawArea[k]){auto a=monitors[k].work,b=changedAreas[k];printf("AREA monitor=%zu before=%ld,%ld,%ld,%ld observed=%ld,%ld,%ld,%ld\n",k,a.left,a.top,a.right,a.bottom,b.left,b.top,b.right,b.bottom);}
  if(!ok)++failures;++cases;if(!closed){DWORD_PTR ignored;SendMessageTimeoutW(popup,WM_CLOSE,0,0,SMTO_ABORTIFHUNG,500,&ignored);Pump(100);}
 }
 for(auto& f:fixtures)DestroyWindow(f.h);if(IsWindow(original))SetForegroundWindow(original);
 printf("RESULT workspace_cases=%d failures=%d\n",cases,failures);
 // Installed dock still owns the taskbar. Do not restore it from this fixture.
 CoUninitialize();return failures?1:0;
}
