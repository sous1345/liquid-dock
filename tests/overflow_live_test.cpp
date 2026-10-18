// Interactive native-flyout integration test. Requires the current installed
// dock to have already protected both taskbars. Never starts from a bare desktop.
#include "../src/native_tray.h"
#include "../src/system.h"
#include "../src/taskbar_visibility.h"
#include <shellapi.h>
#include <thread>
#include <cstdio>
HWND g_mainWnd=nullptr;
int wmain(int argc,wchar_t** argv){
 if(argc!=3)return 2;int edge=_wtoi(argv[1]),monitorIndex=_wtoi(argv[2]);
 if(edge<0||edge>3)return 2;
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 std::vector<RECT> monitors;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT r,LPARAM p)->BOOL{reinterpret_cast<std::vector<RECT>*>(p)->push_back(*r);return TRUE;},reinterpret_cast<LPARAM>(&monitors));
 if(monitorIndex<0||monitorIndex>=(int)monitors.size())return 2;
 std::vector<HWND> taskbars;for(auto cls:{L"Shell_TrayWnd",L"Shell_SecondaryTrayWnd"}){HWND h=nullptr;while((h=FindWindowExW(nullptr,h,cls,nullptr)))taskbars.push_back(h);}
 if(taskbars.empty())return 2;
 for(auto h:taskbars)if(!taskbarvisibility::IsProtected(h)){puts("Installed dock has not applied opacity protection; no flyout opened.");return 2;}
 HWND existing=FindWindowW(L"TopLevelWindowForOverflowXamlIsland",nullptr);
 if(existing&&IsWindowVisible(existing)){puts("Flyout already open; test did not disturb it.");return 2;}
 // Reuse the persisted recovery record and existing window-property backups.
 // The running dock remains their owner; this fixture must not restore them.
 if(!g_taskbar.Hide())return 2;
 RECT m=monitors[monitorIndex],bar{};POINT anchor{};
 int cx=(m.left+m.right)/2,cy=(m.top+m.bottom)/2;
 switch((Edge)edge){
 case Edge::Bottom:bar={cx-250,m.bottom-85,cx+250,m.bottom-5};anchor={cx,m.bottom-45};break;
 case Edge::Top:bar={cx-250,m.top+5,cx+250,m.top+85};anchor={cx,m.top+45};break;
 case Edge::Left:bar={m.left+5,cy-250,m.left+85,cy+250};anchor={m.left+45,cy};break;
 case Edge::Right:bar={m.right-85,cy-250,m.right-5,cy+250};anchor={m.right-45,cy};break;
 }
 std::atomic<bool> stop=false;std::atomic<int> samples=0,exposures=0;
 std::thread observe([&]{while(!stop){for(auto h:taskbars){++samples;if(!taskbarvisibility::IsProtected(h))++exposures;}Sleep(2);}});
 bool opened=nativetray::OpenOverflow(anchor,(Edge)edge,bar);Sleep(200);
 HWND popup=FindWindowW(L"TopLevelWindowForOverflowXamlIsland",nullptr);RECT r{};
 bool visible=popup&&IsWindowVisible(popup)&&GetWindowRect(popup,&r);
 bool within=visible&&r.left>=m.left&&r.top>=m.top&&r.right<=m.right&&r.bottom<=m.bottom;
 bool beside=visible;
 switch((Edge)edge){case Edge::Bottom:beside &= r.bottom<=bar.top;break;case Edge::Top:beside &= r.top>=bar.bottom;break;case Edge::Left:beside &= r.left>=bar.right;break;case Edge::Right:beside &= r.right<=bar.left;break;}
 bool closeInvoked=visible&&nativetray::OpenOverflow(anchor,(Edge)edge,bar);Sleep(250);
 bool closed=!popup||!IsWindowVisible(popup);
 if(!closed){DWORD_PTR unused;SendMessageTimeoutW(popup,WM_CLOSE,0,0,SMTO_ABORTIFHUNG,500,&unused);}
 stop=true;observe.join();
 printf("LIVE_OVERFLOW edge=%d monitor=%d opened=%d visible=%d within=%d beside=%d close_invoked=%d closed=%d opacity_samples=%d opacity_failures=%d\n",edge,monitorIndex,opened,visible,within,beside,closeInvoked,closed,(int)samples,(int)exposures);
 CoUninitialize();return opened&&visible&&within&&beside&&closeInvoked&&closed&&exposures==0?0:1;
}
