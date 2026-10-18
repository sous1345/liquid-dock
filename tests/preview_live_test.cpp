// Real DWM/Win32 integration, restricted to disposable windows in this process.
// No synthetic keyboard/mouse input, no Explorer or installed settings changes.
#include "../src/common.h"
#include "../src/apps.h"
#include <dwmapi.h>
#include <cstdio>
#include <fstream>
#include <set>
#define private public
#include "../src/window_preview.h"
#undef private
#include "../src/window_preview.cpp"
Settings g_settings;
static int checks=0, failures=0;
static COLORREF liveColor=RGB(32,164,215);
static void Check(bool ok,const char* msg){++checks;if(!ok){++failures;printf("FAIL %s\n",msg);fflush(stdout);}}
static void Pump(int ms){auto end=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}while(GetTickCount64()<end);}
static LRESULT CALLBACK SourceProc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);HBRUSH b=CreateSolidBrush(liveColor);FillRect(dc,&r,b);DeleteObject(b);EndPaint(h,&ps);return 0;}return DefWindowProcW(h,m,w,l);
}
static bool Sample(WindowPreview& p,int index,COLORREF expected){
 RECT r=p.entries[index].image;HDC dc=GetDC(nullptr);COLORREF got=GetPixel(dc,p.bounds.left+(r.left+r.right)/2,p.bounds.top+(r.top+r.bottom)/2);ReleaseDC(nullptr,dc);
 bool ok=got!=CLR_INVALID&&abs((int)GetRValue(got)-GetRValue(expected))<=8&&abs((int)GetGValue(got)-GetGValue(expected))<=8&&abs((int)GetBValue(got)-GetBValue(expected))<=8;
 printf("PIXEL expected=%06lx actual=%06lx match=%d\n",expected,got,ok);return ok;
}
static void SaveBmp(RECT r,const std::wstring& path){
 int w=r.right-r.left,h=r.bottom-r.top;HDC screen=GetDC(nullptr),mem=CreateCompatibleDC(screen);
 BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
 void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(mem,bitmap);
 BitBlt(mem,0,0,w,h,screen,r.left,r.top,SRCCOPY|CAPTUREBLT);GdiFlush();
 BITMAPFILEHEADER bf{};bf.bfType=0x4d42;bf.bfOffBits=sizeof(bf)+sizeof(BITMAPINFOHEADER);bf.bfSize=bf.bfOffBits+w*h*4;
 std::ofstream f(path,std::ios::binary);f.write((char*)&bf,sizeof(bf));f.write((char*)&bi.bmiHeader,sizeof(bi.bmiHeader));f.write((char*)pixels,w*h*4);
 SelectObject(mem,old);DeleteObject(bitmap);DeleteDC(mem);ReleaseDC(nullptr,screen);
}
int wmain(int argc,wchar_t** argv){
 if(argc!=3)return 2;
 HWND original=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(original,&pid);
 if(pid!=wcstoul(argv[1],nullptr,10)){puts("Foreground changed; no test windows created.");return 2;}
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 std::vector<RECT> monitors;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT r,LPARAM p)->BOOL{((std::vector<RECT>*)p)->push_back(*r);return TRUE;},(LPARAM)&monitors);
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=SourceProc;wc.lpszClassName=L"LiquidDockPreviewFixture";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
 int cases=0;
 for(size_t mi=0;mi<monitors.size();++mi)for(Edge edge:{Edge::Bottom,Edge::Left,Edge::Right,Edge::Top}){
  liveColor=RGB(32,164,215);
  RECT m=monitors[mi];int cx=(m.left+m.right)/2,cy=(m.top+m.bottom)/2;int before=failures;
  std::vector<HWND> windows;
  for(int i=0;i<12;++i){std::wstring title=L"Preview fixture "+std::to_wstring(i+1)+L" — 창 선택 확인";
   HWND h=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,title.c_str(),WS_OVERLAPPEDWINDOW,m.left+250+i*8,m.top+180+i*8,520,340,nullptr,nullptr,wc.hInstance,nullptr);
   if(!h){puts("Window creation failed");return 2;}windows.push_back(h);ShowWindow(h,i==1?SW_SHOWMINNOACTIVE:SW_SHOWNOACTIVATE);
  }
  // Known protected own-process source: do not try to recover its pixels.
  SetWindowDisplayAffinity(windows[2],WDA_EXCLUDEFROMCAPTURE);
  Pump(200);
  auto app=std::make_shared<AppItem>();app->name=L"LiquidDock 미리보기 검증";app->windows=windows;
  RECT anchor{};switch(edge){case Edge::Left:anchor={m.left,cy-35,m.left+170,cy+35};break;case Edge::Right:anchor={m.right-170,cy-35,m.right,cy+35};break;case Edge::Top:anchor={cx-35,m.top,cx+35,m.top+170};break;case Edge::Bottom:anchor={cx-35,m.bottom-170,cx+35,m.bottom};break;}
  POINT cursor{(anchor.left+anchor.right)/2,(anchor.top+anchor.bottom)/2};float scale=std::max(.72f,std::min(m.right-m.left,m.bottom-m.top)/1440.f);
  {
   WindowPreview p;
   p.Update(windows[0],app,anchor,m,edge,scale,cursor,0,true);
   Check(!p.Open(),"dwell does not open immediately");
   p.Update(windows[0],app,anchor,m,edge,scale,cursor,.5,true);Pump(150);
   Check(p.Open(),"real preview opens");
   Check(p.bounds.left>=m.left&&p.bounds.right<=m.right&&p.bounds.top>=m.top&&p.bounds.bottom<=m.bottom,"popup stays within monitor");
   Check(edge==Edge::Left?p.bounds.left>anchor.right:edge==Edge::Right?p.bounds.right<anchor.left:edge==Edge::Top?p.bounds.top>anchor.bottom:p.bounds.bottom<anchor.top,"popup clears dock edge");
   DWORD affinity=0;Check(GetWindowDisplayAffinity(p.hwnd,&affinity)&&affinity==WDA_EXCLUDEFROMCAPTURE,"production popup requests capture exclusion");
   // Only the test popup becomes capturable so its actual DWM output can be measured.
   SetWindowDisplayAffinity(p.hwnd,WDA_NONE);Pump(100);DwmFlush();
   Check(p.entries.size()==12,"all real windows present");
   Check(p.entries[0].thumbnail!=nullptr,"real DWM relationship created");
   Check(Sample(p,0,liveColor),"DWM actually renders source pixels");
   liveColor=RGB(225,127,40);InvalidateRect(windows[0],nullptr,FALSE);Pump(150);DwmFlush();
   Check(Sample(p,0,liveColor),"DWM updates live source contents");
   Check(IsIconic(windows[1])&&!p.entries[1].thumbnail&&!p.entries[1].fallback.empty(),"preview does not restore minimized source");
   if(p.capacity>2)Check(!p.entries[2].thumbnail&&!p.entries[2].fallback.empty(),"protected source has title fallback");
   SaveBmp(p.bounds,std::wstring(argv[2])+L"\\preview-m"+std::to_wstring(mi)+L"-e"+std::to_wstring((int)edge)+L".bmp");
   POINT bridge{(p.bridge.left+p.bridge.right)/2,(p.bridge.top+p.bridge.bottom)/2};
   p.Update(windows[0],nullptr,{},m,edge,scale,bridge,.7,true);Check(p.Open()&&p.Contains(bridge),"gap crossing keeps real popup open");
   std::set<HWND> reached;for(int page=0;page<12;++page){for(int j=p.first;j<std::min(p.first+p.capacity,12);++j)reached.insert(p.entries[j].source);int old=p.first;p.ChangePage(1);if(p.first==old)break;}
   Check(reached.size()==12,"paging reaches all 12 windows");p.ChangePage(-100);
   // A source disappearing between press/release must never activate its replacement.
   RECT card=p.entries[0].card;LPARAM point=MAKELPARAM((card.left+card.right)/2,(card.top+card.bottom)/2);
   SendMessageW(p.hwnd,WM_LBUTTONDOWN,0,point);DestroyWindow(windows[0]);app->windows.erase(app->windows.begin());
   // Owner destruction also destroys the popup; this is an ownership-lifetime test.
   Check(!p.hwnd&&!p.Open(),"owner destruction destroys preview without stale HWND");
   p.Dismiss();
   // Reopen using another surviving owner, then activate the minimized source via actual input handler.
   p.candidate.reset();p.Update(windows[3],app,anchor,m,edge,scale,cursor,1,true);p.Update(windows[3],app,anchor,m,edge,scale,cursor,1.5,true);Pump(100);
   Check(p.Open(),"reopen after owner destruction");
   if(p.Open()){
    card=p.entries[0].card;point=MAKELPARAM((card.left+card.right)/2,(card.top+card.bottom)/2);
    SendMessageW(p.hwnd,WM_LBUTTONDOWN,0,point);SendMessageW(p.hwnd,WM_LBUTTONUP,0,point);Pump(250);
    Check(!IsIconic(windows[1]),"click restores minimized source");Check(GetForegroundWindow()==windows[1],"click activates selected real source");Check(!p.Open(),"selection dismisses popup");
   }
  }
  for(HWND h:windows)if(IsWindow(h))DestroyWindow(h);
  if(IsWindow(original))SetForegroundWindow(original);Pump(80);
  printf("LIVE_PREVIEW monitor=%zu edge=%d checks=%d failed=%d\n",mi,(int)edge,checks,failures-before);fflush(stdout);++cases;
 }
 printf("RESULT live_preview_cases=%d checks=%d failures=%d\n",cases,checks,failures);
 CoUninitialize();return failures?1:0;
}
