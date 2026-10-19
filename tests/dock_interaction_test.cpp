// Runs the production Dock implementation with controlled monitor/cursor/foreground inputs.
// Never sends keyboard or mouse input, starts apps, hides taskbars, or alters user data.
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/capture.h"
#include "../src/apps.h"
#include "../src/system.h"
#include "../src/sensor_shared.h"
#include "../src/native_tray.h"
#include <shellscalingapi.h>
#include <shellapi.h>
#include <d2d1effects.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <map>
#include <limits>
#define private public
#include "../src/sensors.h"
#include "../src/dock.h"
#undef private

Settings g_settings;
HWND g_mainWnd = nullptr;
struct MonitorCase { HMONITOR handle; RECT rect; bool primary; bool actual; const char* name; };
static std::vector<MonitorCase> monitors;
static bool fgEnabled=false, fgMaximized=false;
static RECT fgRect{};
static HMONITOR fgMonitor=nullptr;
static LONG_PTR fgStyle=WS_POPUP;
static const wchar_t* fgClass=L"MatrixFullscreen";
static HWND fgHandle=(HWND)(uintptr_t)0xF00DFACE;
static BOOL WINAPI MatrixMonitorInfo(HMONITOR m,LPMONITORINFO info) {
 for(const auto& entry:monitors) if(entry.handle==m) {info->rcMonitor=info->rcWork=entry.rect;info->dwFlags=entry.primary?MONITORINFOF_PRIMARY:0;return TRUE;}
 return ::GetMonitorInfoW(m,info);
}
static HWND WINAPI MatrixForeground(){return fgEnabled?fgHandle:nullptr;}
static HMONITOR WINAPI MatrixMonitorFromWindow(HWND h,DWORD flags){return h==fgHandle ? fgMonitor : ::MonitorFromWindow(h,flags);}
static BOOL WINAPI MatrixWindowRect(HWND h,LPRECT rect){if(h==fgHandle){*rect=fgRect;return TRUE;}return ::GetWindowRect(h,rect);}
static BOOL WINAPI MatrixIsZoomed(HWND h){return h==fgHandle ? fgMaximized : ::IsZoomed(h);}
static LONG_PTR WINAPI MatrixWindowLong(HWND h,int index){return h==fgHandle ? fgStyle : ::GetWindowLongPtrW(h,index);}
static int WINAPI MatrixClassName(HWND h,LPWSTR text,int capacity){if(h==fgHandle){wcsncpy_s(text,capacity,fgClass,_TRUNCATE);return (int)wcslen(text);}return ::GetClassNameW(h,text,capacity);}
static BOOL WINAPI MatrixShowWindow(HWND,int){return TRUE;} // keep fixture windows invisible
static SHORT WINAPI MatrixKeyState(int){return 0;}
static int settingsMenus=0;
static BOOL WINAPI MatrixForegroundSet(HWND){return TRUE;}
static BOOL WINAPI MatrixPopupMenu(HMENU,UINT,int,int,int,HWND,const RECT*){++settingsMenus;return 0;}
static int desktopActions=0;
namespace nativetray {
static bool MatrixToggleDesktop(){++desktopActions;return true;}
}
// OLED mode stubs: the matrix never changes the real wallpaper or desktop icons
static int oledActions=0; static bool oledOn=false;
namespace oled {
bool Active(){return oledOn;}
bool Enable(HMONITOR){++oledActions;oledOn=true;return true;}
bool Disable(){++oledActions;oledOn=false;return true;}
void Reapply(){}
}
static int themeActions=0; static bool themeLight=false;
namespace theme {
bool IsLight(){return themeLight;}
bool Toggle(){++themeActions;themeLight=!themeLight;return true;}
}
#define GetMonitorInfoW MatrixMonitorInfo
#define GetForegroundWindow MatrixForeground
#define MonitorFromWindow MatrixMonitorFromWindow
#define GetWindowRect MatrixWindowRect
#define IsZoomed MatrixIsZoomed
#define GetWindowLongPtrW MatrixWindowLong
#define GetClassNameW MatrixClassName
#define ShowWindow MatrixShowWindow
#define GetAsyncKeyState MatrixKeyState
#define SetForegroundWindow MatrixForegroundSet
#define TrackPopupMenu MatrixPopupMenu
#define ToggleDesktop MatrixToggleDesktop
#include "../src/dock.cpp"
#include "../src/dock_layout.cpp"
#include "../src/dock_render.cpp"
#include "../src/dock_controls.cpp"
#undef GetMonitorInfoW
#undef GetForegroundWindow
#undef MonitorFromWindow
#undef GetWindowRect
#undef IsZoomed
#undef GetWindowLongPtrW
#undef GetClassNameW
#undef ShowWindow
#undef GetAsyncKeyState
#undef SetForegroundWindow
#undef TrackPopupMenu
#undef ToggleDesktop

static int checks=0, failures=0, cases=0, caseFailures=0;
static std::string caseLabel;
static std::ofstream report;
static double simTime=10;
static void Check(bool ok,const char* name) {
 ++checks;
 if(!ok){++failures;++caseFailures;report<<"FAIL\t"<<caseLabel<<"\t"<<name<<"\n";if(failures<=25)printf("FAIL %s: %s\n",caseLabel.c_str(),name);}
}
static POINT EdgePoint(const RECT& r,Edge edge,float fraction,int depth) {
 LONG alongX=r.left+(LONG)((r.right-r.left-1)*fraction),alongY=r.top+(LONG)((r.bottom-r.top-1)*fraction);
 switch(edge){case Edge::Bottom:return {alongX,r.bottom-1-depth};case Edge::Top:return {alongX,r.top+depth};case Edge::Left:return {r.left+depth,alongY};default:return {r.right-1-depth,alongY};}
}
static POINT Outside(const RECT& r){return {r.left-1200,r.top-1200};}
static void Advance(Dock& d,POINT cursor,double seconds,float dt=1.f/120.f) {
 for(int i=0;i<(int)std::ceil(seconds/dt);++i){simTime+=dt;d.Update(simTime,dt,cursor);}
}
static void ResetHidden(Dock& d) {
 fgEnabled=false;fgMaximized=false;fgStyle=WS_POPUP;fgClass=L"MatrixFullscreen";
 d.reveal.snap(0);d.magAmount.snap(0);d.panelAlpha.snap(0);d.tooltipAlpha.snap(0);
 d.edgeSince=d.outsideSince=d.panelOutsideSince=-1;d.menuOpen=false;d.dragging=false;d.pressSlot=-1;
 for(auto& s:d.slots){s.bounceCount=0;s.bounceLoop=false;s.peek.snap(0);s.bounceOff=0;}
 d.sbCount=0;d.sbPeek.snap(0);d.nextCaptureTry=std::numeric_limits<double>::max();
 Dock::debugForceShow=Dock::debugForcePanel=false;
 Advance(d,Outside(d.mon),0.2);
}
static void Apps(int count) {
 g_apps.items.clear();g_apps.attentionEvents.clear();++g_apps.structureVersion;
 for(int i=0;i<count;++i){auto item=std::make_shared<AppItem>();item->id=i+1;item->name=L"Test App "+std::to_wstring(i+1);item->pinned=true;g_apps.items.push_back(item);}
}
static void InitializeLayout(Dock& d) {
 d.Metrics();d.SyncSlots();
 for(auto& s:d.slots){s.appear.snap(1);s.shrink.snap(1);}
 d.trailingGap.snap(0);ResetHidden(d);
 Advance(d,Outside(d.mon),1.5);
}
static void AssertHitGeometry(Dock& d) {
 // Check what is drawn against the product's independent input classification and OS region coordinates.
 const float v=d.margin+d.pad+d.base*0.4f-d.hideOff;
 for(size_t i=0;i<d.slots.size();++i) {
  auto& slot=d.slots[i];auto px=d.W(slot.u,v);
  float u2,v2;d.ClientToUV((int)px.x,(int)px.y,u2,v2);
  auto hit=d.HitTestUV(u2,v2);
  Check(hit.kind==Dock::HitSlot && hit.slot==(int)i,"rendered icon center maps to its own hit target");
  POINT local={(LONG)px.x,(LONG)px.y};Check(PtInRect(&d.curRegion,local)!=0,"icon center inside active input region");
  Check(slot.u0>=-1 && slot.u0+slot.size*slot.weight<=d.len+1,"icons remain inside the monitor edge");
 }
 struct Item {float start,end;Dock::HitKind kind;};
 Item buttons[]={{d.cells[Dock::HitTray].u0,d.cells[Dock::HitTray].u1,Dock::HitTray},{d.cells[Dock::HitClock].u0,d.cells[Dock::HitClock].u1,Dock::HitClock},{d.cells[Dock::HitSensor].u0,d.cells[Dock::HitSensor].u1,Dock::HitSensor}};
 for(const auto& item:buttons){
  auto px=d.W((item.start+item.end)/2,d.margin+d.barThick/2-d.hideOff);
  float u2,v2;d.ClientToUV((int)px.x,(int)px.y,u2,v2);Check(d.HitTestUV(u2,v2).kind==item.kind,"tray control rendered center maps to its own hit target");
  Check(item.start>=-1 && item.end<=d.len+1,"tray controls remain inside monitor edge");
 }
 auto network=d.ButtonRect(Dock::HitNet),volume=d.ButtonRect(Dock::HitVol);
 if(d.stackButtons){
  Check(d.cells[Dock::HitNet].u0==d.cells[Dock::HitVol].u0 && d.cells[Dock::HitNet].u1==d.cells[Dock::HitVol].u1 && d.cells[Dock::HitClock].u0==d.cells[Dock::HitVol].u1,"stacked connectivity shares one cell before clock");
  if(IsVertical(g_settings.edge))Check(network.right<=volume.left+0.1f,"side docks: network left of volume");
  else Check(network.bottom<=volume.top+0.1f,"top and bottom docks: network above volume");
 }else Check(d.cells[Dock::HitNet].u1==d.cells[Dock::HitVol].u0 && d.cells[Dock::HitClock].u0==d.cells[Dock::HitVol].u1,"unstacked connectivity keeps two adjacent cells before clock");
 // every small button: inside the window, a comfortable size, its own click area, no overlap
 struct Btn{Dock::HitKind kind;bool on;float u0,u1;} btns[]={{Dock::HitNet,true,d.cells[Dock::HitNet].u0,d.cells[Dock::HitNet].u1},{Dock::HitVol,true,d.cells[Dock::HitVol].u0,d.cells[Dock::HitVol].u1},
  {Dock::HitDesktop,g_settings.showDesktopButton,d.cells[Dock::HitDesktop].u0,d.cells[Dock::HitDesktop].u1},{Dock::HitTheme,g_settings.showThemeButton,d.cells[Dock::HitTheme].u0,d.cells[Dock::HitTheme].u1},
  {Dock::HitOled,g_settings.showOledButton,d.cells[Dock::HitOled].u0,d.cells[Dock::HitOled].u1},{Dock::HitSettings,true,d.cells[Dock::HitSettings].u0,d.cells[Dock::HitSettings].u1}};
 std::vector<D2D1_RECT_F> seen;
 for(auto& b:btns){
  if(!b.on){Check(b.u0==b.u1,"hidden button reserves no space");continue;}
  auto r=d.ButtonRect(b.kind);
  Check(r.left>=0 && r.top>=0 && r.right<=d.winW && r.bottom<=d.winH,"small buttons stay inside dock window");
  Check(std::min(r.right-r.left,r.bottom-r.top)>=22*d.scale-1,"small buttons keep a comfortable click size");
  Check(b.kind==Dock::HitNet||b.kind==Dock::HitVol||b.u0>=d.cells[Dock::HitSensor].u1,"utility buttons follow the sensor cell");
  for(auto& o:seen){
   float ix=std::min(r.right,o.right)-std::max(r.left,o.left),iy=std::min(r.bottom,o.bottom)-std::max(r.top,o.top);
   Check(ix<=0.5f||iy<=0.5f,"small buttons never overlap");
  }
  seen.push_back(r);
  for(float x:{0.15f,0.5f,0.85f})for(float y:{0.15f,0.5f,0.85f}){
   POINT local={(LONG)lerpf(r.left,r.right,x),(LONG)lerpf(r.top,r.bottom,y)};
   float u,v;d.ClientToUV(local.x,local.y,u,v);
   Check(d.HitTestUV(u,v).kind==b.kind,"small button drawing matches its click target across the cell");
   Check(PtInRect(&d.curRegion,local)!=0,"small button remains in active input region");
  }
 }
 if(g_settings.showOledButton){
  int before=oledActions;d.Click({Dock::HitOled,-1});d.Click({Dock::HitOled,-1});
  Check(oledActions-before==2 && !oledOn,"OLED button toggles on and back off");
 }
 if(g_settings.showThemeButton){
  int before=themeActions;bool was=themeLight;d.Click({Dock::HitTheme,-1});d.Click({Dock::HitTheme,-1});
  Check(themeActions-before==2 && themeLight==was,"theme button flips the mode and back");
 }
 Check(d.cells[Dock::HitSettings].u1-d.cells[Dock::HitSettings].u0>=32*d.scale-0.1f,"settings button retains its minimum target width in crowded layouts");
 {
  auto r=d.ButtonRect(Dock::HitSettings,true);
  POINT local={(LONG)((r.right+r.left)/2),(LONG)((r.top+r.bottom)/2)};
  Check(PtInRect(&d.curRegion,local)!=0,"settings target stays in active input region");
 }
 {
  // the end padding beside the gear (in the gear's half when it shares a cell)
  auto pr=d.ButtonRect(Dock::HitSettings,true);
  auto pe=d.W((d.cells[Dock::HitSettings].u1+d.barU1)*0.5f,d.margin+d.barThick*0.5f-d.hideOff);
  POINT pp=IsVertical(g_settings.edge)?POINT{(LONG)((pr.left+pr.right)*0.5f),(LONG)pe.y}:POINT{(LONG)pe.x,(LONG)((pr.top+pr.bottom)*0.5f)};
  float u,v;d.ClientToUV(pp.x,pp.y,u,v);
  Check(d.HitTestUV(u,v).kind==Dock::HitSettings,"dock-end padding also opens settings");
 }
 Check(d.curRegion.left>=0 && d.curRegion.top>=0 && d.curRegion.right<=d.winW && d.curRegion.bottom<=d.winH,"input region stays inside its window");
 Check(d.barU0>=-1 && d.barU1<=d.len+1,"glass bar stays inside monitor edge");
}
static void StateTests(Dock& d) {
 auto center=EdgePoint(d.mon,g_settings.edge,0.5f,0);
 // Only pixels inside this monitor count; its four corners and the shared monitor boundary matter.
 for(float fraction:{0.f,0.5f,1.f}) {
  ResetHidden(d);auto edge=EdgePoint(d.mon,g_settings.edge,fraction,0);
  Advance(d,edge,0.025);Check(d.reveal.target==0,"show delay prevents immediate reveal");
  Advance(d,edge,0.2);Check(d.reveal.x>0.5f,"edge and corner begin revealing after delay");
  int flips=0;float target=d.reveal.target;
  for(int frame=0;frame<240;++frame){Advance(d,edge,1.f/120.f);if(d.reveal.target!=target){++flips;target=d.reveal.target;}}
  Check(flips==0,"stationary edge pointer does not repeatedly toggle visibility");
  d.reveal.snap(1);d.outsideSince=-1;
  Advance(d,Outside(d.mon),0.12);Check(d.reveal.target==1,"hide delay keeps dock briefly visible");
  Advance(d,Outside(d.mon),1.6);Check(d.reveal.x<0.002f && IsRectEmpty(&d.curRegion),"leaving monitor hides dock and releases input region");
 }
 for(int depth:{-1,3,60}){ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,depth),0.8);Check(d.reveal.target==0,"outside or merely near an edge does not reveal");}
 ResetHidden(d);Advance(d,center,0.8);AssertHitGeometry(d);
 for(size_t index:{(size_t)0,d.slots.size()-1}) {
  ResetHidden(d);Advance(d,center,0.8);
  auto p=d.ScreenPointUV(d.slots[index].u,d.margin+d.barThick/2);
  float u,v;d.ScreenToUV(p,u,v);
  auto initial=d.HitTestUV(u,v);
  Check(initial.kind==Dock::HitSlot && initial.slot==(int)index,"hover test starts on the rendered icon");
  bool keptTarget=true;
  for(int frame=0;frame<120;++frame){
   Advance(d,p,1.f/120.f);d.ScreenToUV(p,u,v);auto hit=d.HitTestUV(u,v);
   keptTarget &= hit.kind==Dock::HitSlot && hit.slot==(int)index;
  }
  Check(keptTarget,"first or last icon stays under pointer on every animation frame");
  if(d.reveal.target>0.5f && d.reveal.x>0.9f)AssertHitGeometry(d);
  Check(d.reveal.target==1,"hovering first or last icon keeps dock visible");
  if(d.reveal.target!=1)report<<"DETAIL\t"<<caseLabel<<"\ticon="<<index<<" cursorU="<<d.cursorU<<" cursorV="<<d.cursorV<<" bar="<<d.barU0<<","<<d.barU1<<" reveal="<<d.reveal.x<<"\n";
 }
 // The sensor popup must be reachable and contained in the target monitor in every orientation.
 ResetHidden(d);d.reveal.snap(1);d.hideOff=0;d.Layout(simTime);d.ComputePanelRect();
 auto sensor=d.ScreenPointUV((d.cells[Dock::HitSensor].u0+d.cells[Dock::HitSensor].u1)/2,d.margin+d.barThick/2);
 Advance(d,sensor,0.9);Check(d.panelAlpha.x>0.95f,"sensor hover opens panel");
 Check(d.panelRect.left>=0 && d.panelRect.top>=0 && d.panelRect.right<=d.winW && d.panelRect.bottom<=d.winH,"sensor panel remains inside monitor window");
 auto panel=POINT{d.winX+(LONG)((d.panelRect.left+d.panelRect.right)/2),d.winY+(LONG)((d.panelRect.top+d.panelRect.bottom)/2)};
 Advance(d,panel,1.0);Check(d.panelAlpha.target==1 && d.reveal.target==1,"moving into sensor panel keeps it open");
 Advance(d,Outside(d.mon),2.0);Check(d.reveal.x<0.002f && d.panelAlpha.x<0.01f,"panel and dock close after leaving");
 // Interaction locks and fullscreen suppression are controlled inputs, not OS input injection.
 ResetHidden(d);Advance(d,center,0.8);d.menuOpen=true;Advance(d,Outside(d.mon),1.0);Check(d.reveal.target==1,"open menu prevents hiding");d.menuOpen=false;
 d.dragging=true;Advance(d,Outside(d.mon),1.0);Check(d.reveal.target==1,"drag in progress prevents hiding");d.dragging=false;
 d.pressSlot=0;Advance(d,Outside(d.mon),1.0);Check(d.reveal.target==1,"pressed icon prevents hiding");d.pressSlot=-1;
 Advance(d,Outside(d.mon),1.6);Check(d.reveal.x<0.002f,"releasing interaction lock permits hiding");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;Advance(d,center,1.0);Check(d.reveal.target==0,"fullscreen on this monitor suppresses reveal");
 fgMonitor=(HMONITOR)(uintptr_t)0xD00D;Advance(d,center,1.0);Check(d.reveal.target==1,"fullscreen on another monitor does not suppress this dock");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;fgMaximized=true;Advance(d,center,0.8);Check(d.reveal.target==1,"maximized ordinary window permits reveal");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;fgStyle=WS_CAPTION;Advance(d,center,0.8);Check(d.reveal.target==1,"captioned full-sized window permits reveal");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;fgClass=L"Progman";Advance(d,center,0.8);Check(d.reveal.target==1,"desktop window permits reveal");
 ResetHidden(d);auto corner=EdgePoint(d.mon,g_settings.edge,0.f,0);Advance(d,corner,0.8);
 fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;Advance(d,corner,1.6);
 Check(d.reveal.target==0 && d.reveal.x<0.002f,"fullscreen starting at a corner still permits the dock to hide");
 ResetHidden(d);g_apps.attentionEvents.push_back({1});Advance(d,Outside(d.mon),0.05);g_apps.attentionEvents.clear();
 Check((d.slots[1].bounceCount>0)==d.primary,"attention notification is restricted to primary monitor");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;g_apps.attentionEvents.push_back({1});Advance(d,Outside(d.mon),0.05);g_apps.attentionEvents.clear();Check(d.slots[1].bounceCount==0,"fullscreen suppresses app attention bounce");
 ResetHidden(d);
}

static void SweepTests(Dock& d) {
 const POINT center=EdgePoint(d.mon,g_settings.edge,0.5f,0);
 ResetHidden(d);Advance(d,center,1.5);
 const float from=d.slots.front().u,to=d.slots.back().u;
 const float referenceCenter=(d.barU0+d.barU1)*0.5f;
 const float referenceLength=d.barU1-d.barU0;
 float maxCenterError=0,maxLengthError=0;
 bool remainedVisible=true,sizesBounded=true,ordered=true;
 // Fixed physical points from the displayed first/last icons, not points that
 // follow a moving dock. Several return trips expose frame-to-frame feedback.
 for(int lap=0;lap<3;++lap)for(int direction=0;direction<2;++direction) {
  for(int frame=0;frame<=120;++frame) {
   float u=lerpf(direction?to:from,direction?from:to,frame/120.f);
   auto p=d.ScreenPointUV(u,d.margin+d.barThick/2);
   simTime+=1.f/120.f;d.Update(simTime,1.f/120.f,p);
   maxCenterError=std::max(maxCenterError,std::fabs((d.barU0+d.barU1)*0.5f-referenceCenter));
   maxLengthError=std::max(maxLengthError,std::fabs(d.barU1-d.barU0-referenceLength));
   remainedVisible &= d.reveal.target>0.5f;
   for(size_t i=0;i<d.slots.size();++i){
    const auto& s=d.slots[i];sizesBounded &= std::isfinite(s.size) && s.size>=d.base-0.01f && s.size<=d.base*d.mag+0.01f;
    if(i)ordered &= d.slots[i-1].u0+d.slots[i-1].size*d.slots[i-1].weight<=s.u0+0.01f;
   }
  }
 }
 Check(maxCenterError<=0.1f,"full-length return trips preserve dock center");
 Check(maxLengthError<=0.1f,"full-length return trips preserve enlarged dock length");
 Check(remainedVisible,"full-length return trips never hide the dock");
 Check(sizesBounded,"sweeps keep every icon within configured magnification");
 Check(ordered,"sweeps never overlap or reorder icons");
 if(maxCenterError>0.1f || maxLengthError>0.1f)report<<"SWEEP_DETAIL\t"<<caseLabel<<"\tcenter="<<maxCenterError<<" length="<<maxLengthError<<"\n";
 Advance(d,Outside(d.mon),2);
 Check(d.reveal.x<0.002f && IsRectEmpty(&d.curRegion),"dock hides normally after pointer leaves a sweep");
 Advance(d,center,1.5);
 Check(std::fabs((d.barU0+d.barU1)*0.5f-referenceCenter)<=0.1f,"reopening after a sweep returns to the same center");
 ResetHidden(d);
}

static void ReachabilityTests(Dock& d) {
 for(size_t index=0;index<d.slots.size();++index){
  ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.5);
  bool reachable=false;POINT p{};
  for(int attempt=0;attempt<6 && !reachable;++attempt){
   p=d.ScreenPointUV(d.slots[index].u,d.margin+d.barThick/2);
   Advance(d,p,0.3);float u,v;d.ScreenToUV(p,u,v);auto hit=d.HitTestUV(u,v);
   reachable=hit.kind==Dock::HitSlot && hit.slot==(int)index && d.reveal.target>0.5f;
  }
  Check(reachable,"each icon can be reached after magnification settles");
  if(reachable){
   Advance(d,p,0.6);float u,v;d.ScreenToUV(p,u,v);auto hit=d.HitTestUV(u,v);
   Check(hit.kind==Dock::HitSlot && hit.slot==(int)index,"reached icon remains the stable input target");
  }
 }
 ResetHidden(d);
}

static void OptionsTests(Dock& d) {
 ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.5);
 AssertHitGeometry(d);
 auto clock=d.RectUV(d.cells[Dock::HitClock].u0,d.cells[Dock::HitClock].u1,d.margin,d.margin+d.barThick);
 bool vertical=IsVertical(g_settings.edge);
 auto time=d.TextSize(d.ClockFont(),vertical?g_sys.timeShort:g_sys.timeText);
 auto date=d.TextSize(d.ClockFont(true),g_sys.dateText);
 Check(time.width<=clock.right-clock.left+0.1f,"clock text width fits without clipping");
 Check((vertical?time.height:time.height+date.height)<=clock.bottom-clock.top+0.1f,"clock text height fits without clipping");
 if(!vertical)Check(date.width<=clock.right-clock.left+0.1f,"date text width fits without clipping");
 Check(d.ClockFont()->GetFontSize()>g_gfx.fmtClock->GetFontSize(),"new normal and large clocks exceed former clock font size");
 Check(std::fabs(g_gfx.fmtClock->GetFontSize()-13*d.scale)<0.1f,"sensor font is independent of clock setting");
 // With no pressure to fit, clock size and button visibility must not enlarge icons.
 auto normalMetrics=d.MeasureMetrics(1);
 Check(d.base<=normalMetrics.icon,"clock space never increases app icon size");
 const int before=desktopActions;
 d.Click({Dock::HitDesktop,-1}); // stubbed Shell action: never minimizes real windows
 Check(desktopActions-before==(g_settings.showDesktopButton?1:0),"desktop button routes exactly one Shell action only when enabled");
 if(g_settings.showDesktopButton){
  auto br=d.ButtonRect(Dock::HitDesktop);POINT p={d.winX+(LONG)((br.left+br.right)*0.5f),d.winY+(LONG)((br.top+br.bottom)*0.5f)};
  Advance(d,p,0.8);
  Check(d.hoverKind==Dock::HitDesktop && d.reveal.target==1,"desktop button stays reachable with dock visible");
  Check(d.panelAlpha.target==0,"desktop hover does not open sensor panel");
 }
 auto sr=d.ButtonRect(Dock::HitSettings);POINT settingsPoint={d.winX+(LONG)((sr.left+sr.right)*0.5f),d.winY+(LONG)((sr.top+sr.bottom)*0.5f)};
 Advance(d,settingsPoint,0.8);
 Check(d.hoverKind==Dock::HitSettings && d.reveal.target==1,"gear remains reachable after leaving magnified apps");
 Check(d.panelAlpha.target==0,"settings hover does not open the sensor panel");
 int menuBefore=settingsMenus,desktopBefore=desktopActions;
 d.Click({Dock::HitSettings,-1}); // popup display is intercepted; no real menu or focus changes
 Check(settingsMenus==menuBefore+1 && desktopActions==desktopBefore,"gear click opens the existing settings menu, not the desktop action");
 for(auto kind:{Dock::HitNet,Dock::HitVol}){
  auto r=d.ButtonRect(kind);
  POINT p={d.winX+(LONG)((r.left+r.right)*0.5f),d.winY+(LONG)((r.top+r.bottom)*0.5f)};
  Advance(d,p,0.8);
  Check(d.hoverKind==kind && d.reveal.target==1,"connectivity hover selects its own tooltip and keeps dock visible");
  Check(!d.tooltipText.empty() && d.tooltipAlpha.target==1,"connectivity tooltip remains available in paired row");
 }
 ResetHidden(d);
}

static void DragSpacingTests(Dock& d) {
 ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.5);
 auto moved=d.slots[1].app;const size_t count=g_apps.items.size();
 d.pressSlot=1;d.dragging=true;d.slots[1].shrink.target=0;
 // Controlled cursor inputs go through production drop-index/layout logic.
 auto target=d.ScreenPointUV(d.sepU,d.margin+d.barThick/2);
 Advance(d,target,0.7);
 Check(!d.dropRemove && d.dropIndex==(int)count-1,"drag reaches final insertion position at every spacing");
 d.EndDrag(true);Advance(d,target,1.0);
 Check(g_apps.items.size()==count && g_apps.items.back()==moved && moved->pinned,"drag changes order without losing or unpinning an app");
 auto cancelFirst=g_apps.items.front();
 d.pressSlot=1;d.dragging=true;d.slots[1].shrink.target=0;
 Advance(d,d.ScreenPointUV(d.sepU,d.margin+d.barThick/2),0.4);
 d.EndDrag(false);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.0);
 Check(g_apps.items.front()==cancelFirst && g_apps.items.back()==moved,"cancelled drag preserves app order");
 AssertHitGeometry(d);ResetHidden(d);
}

static void RenderFixture(const MonitorCase& monitor,int edge,int index,int variant=0) {
 g_settings=Settings{};g_settings.edge=(Edge)edge;Apps(12);fgEnabled=false;
 if(variant==1 || variant==2){g_settings.clockSize=1;g_settings.iconSpacing=2;}
 if(variant==2)g_settings.showDesktopButton=false;
 if(variant==3)g_settings.iconSpacing=0;
 g_sys.timeText=variant?L"오후 12:59":L"21:30";g_sys.timeShort=variant?L"12:59":L"21:30";
 g_sys.dateText=variant?L"12/31 수요일":L"2026-10-07";
 g_sensors.live=true;g_sensors.d={};g_sensors.d.flags=LD_HAS_GPU|LD_HAS_GPU_LOAD|LD_HAS_SSD_TEMP;
 g_sensors.d.cpuLoad=46;g_sensors.d.gpuLoad=72;g_sensors.d.gpuTemp=65;g_sensors.d.gpuMemUsed=5.5f;g_sensors.d.gpuMemTotal=16;
 g_sensors.d.ramUsed=18.5f;g_sensors.d.ramTotal=32;g_sensors.d.ramLoad=58;g_sensors.d.ssdTemp=43;g_sensors.d.ssdActive=12;g_sensors.d.ssdUsed=500;g_sensors.d.ssdTotal=1000;
 wcscpy_s(g_sensors.d.cpuName,L"Sample CPU");wcscpy_s(g_sensors.d.gpuName,L"Sample GPU");wcscpy_s(g_sensors.d.ssdName,L"Sample SSD");
 g_sensors.histHead=0;g_sensors.histCount=60;
 for(int metric=0;metric<4;++metric)for(int sample=0;sample<60;++sample)g_sensors.hist[metric][sample]=45+20*std::sin(sample*0.23f+metric);
 Dock d(monitor.handle,index,monitor.primary);
 if(!d.Create()){Check(false,"GPU fixture created");return;}
 d.nextCaptureTry=std::numeric_limits<double>::max();
 // An artificial background exercises the existing glass shader without capturing user content.
 std::vector<uint32_t> pixels(d.winW*d.winH);
 for(int y=0;y<d.winH;++y)for(int x=0;x<d.winW;++x){unsigned grid=((x/80+y/80)&1)?28:0;unsigned r=45+grid+(90*x/d.winW),g=65+grid+(80*y/d.winH),b=115+grid;pixels[y*d.winW+x]=0xff000000|(r<<16)|(g<<8)|b;}
 g_gfx.ctx->UpdateSubresource(d.backdrop.tex.Get(),0,nullptr,pixels.data(),d.winW*4,0);
 d.capture.gotFirstFrame=true;d.backdropDirty=true;
 Advance(d,EdgePoint(d.mon,(Edge)edge,0.5f,0),1.0);
 d.snapshotPending=true;d.dirty=true;d.Render();
 Check(!g_gfx.DeviceLost(),"GPU remains available after normal dock rendering");
 d.index=index+1000;Dock::debugForcePanel=true;d.reveal.snap(1);
 Advance(d,d.ScreenPointUV((d.cells[Dock::HitSensor].u0+d.cells[Dock::HitSensor].u1)/2,d.margin+d.barThick/2),1.0);
 d.snapshotPending=true;d.dirty=true;d.Render();
 Check(!g_gfx.DeviceLost(),"GPU remains available after sensor panel rendering");
 Dock::debugForcePanel=false;
 for(int end=0;end<2;++end){
  ResetHidden(d);Advance(d,EdgePoint(d.mon,(Edge)edge,0.5f,0),1.5);
  const auto& slot=end?d.slots.back():d.slots.front();
  Advance(d,d.ScreenPointUV(slot.u,d.margin+d.barThick/2),1.0);
  d.index=index+(end?3000:2000);d.snapshotPending=true;d.dirty=true;d.Render();
  Check(!g_gfx.DeviceLost(),"GPU remains available after end-icon magnification rendering");
 }
}

static void RenderOptions(int actualCount) {
 for(int variant=0;variant<4;++variant)for(int i=0;i<actualCount;++i)for(int edge=0;edge<4;++edge){
  caseLabel=std::string("render ")+monitors[i].name+" edge="+std::to_string(edge)+" variant="+std::to_string(variant);
  RenderFixture(monitors[i],edge,100+variant*10000+i*10+edge,variant);
 }
}

static void TraceReproductions(int actualCount) {
 std::ofstream trace(AppDataDir()+L"\\reproduction-trace.csv");
 trace<<"monitor,edge,scenario,seconds,cursor_u,cursor_v,bar_start,bar_end,reveal,reveal_target,magnification,first_icon_center,hit_kind,hit_slot\n";
 for(int mi=0;mi<actualCount;++mi)for(int edge=0;edge<4;++edge)for(int scenario=0;scenario<2;++scenario){
  const auto& monitor=monitors[mi];g_settings=Settings{};g_settings.edge=(Edge)edge;
  if(scenario==1){g_settings.iconSize=54;g_settings.magnification=2.1f;}
  Apps(12);Dock d(monitor.handle,0,monitor.primary);InitializeLayout(d);
  POINT cursor;
  if(scenario==0)cursor=EdgePoint(d.mon,(Edge)edge,0.f,0);
  else {Advance(d,EdgePoint(d.mon,(Edge)edge,0.5f,0),0.8);cursor=d.ScreenPointUV(d.slots[0].u,d.margin+d.barThick/2);}
  float initialU,initialV;d.ScreenToUV(cursor,initialU,initialV);auto initialHit=d.HitTestUV(initialU,initialV);
  int flips=0;float previous=d.reveal.target;
  for(int frame=0;frame<=360;++frame){
   if(frame)Advance(d,cursor,1.f/120.f);
   float u,v;d.ScreenToUV(cursor,u,v);auto hit=d.HitTestUV(u,v);
   if(d.reveal.target!=previous){++flips;previous=d.reveal.target;}
   trace<<monitor.name<<","<<edge<<","<<(scenario?"first-icon-large-strong":"stationary-corner")<<","<<frame/120.0<<","<<u<<","<<v<<","<<d.barU0<<","<<d.barU1<<","<<d.reveal.x<<","<<d.reveal.target<<","<<d.magAmount.x<<","<<d.slots[0].u<<","<<(int)hit.kind<<","<<hit.slot<<"\n";
  }
  printf("TRACE %s edge=%d scenario=%s initial_hit_kind=%d initial_slot=%d visibility_flips=%d final_target=%.0f\n",monitor.name,edge,scenario?"first-icon-large-strong":"stationary-corner",(int)initialHit.kind,initialHit.slot,flips,d.reveal.target);
 }
}

static void PreviewCouplingTests(int actualCount) {
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"LiquidDockPreviewCouplingSource";wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
 for(int monitorIndex=0;monitorIndex<actualCount;++monitorIndex)for(int edge=0;edge<4;++edge){
  const auto& monitor=monitors[monitorIndex];++cases;caseFailures=0;caseLabel="preview coupling monitor="+std::to_string(monitorIndex)+" edge="+std::to_string(edge);
  HWND source=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Preview coupling fixture",WS_OVERLAPPEDWINDOW,monitor.rect.left+200,monitor.rect.top+200,400,250,nullptr,nullptr,wc.hInstance,nullptr);
  ShowWindow(source,SW_SHOWNOACTIVATE);
  g_settings=Settings{};g_settings.edge=(Edge)edge;Apps(12);
  {
   Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);d.slots[1].app->windows={source};
   Advance(d,EdgePoint(d.mon,g_settings.edge,.5f,0),1.5);
   POINT icon{};for(int attempt=0;attempt<6;++attempt){icon=d.ScreenPointUV(d.slots[1].u,d.margin+d.barThick/2);Advance(d,icon,.6);}
   Check(d.windowPreview.Open(),"hover opens preview through production Dock update");
   if(d.windowPreview.Open()){
    HWND popup=FindWindowW(L"LiquidDockWindowPreview",nullptr);RECT r{};GetWindowRect(popup,&r);
    POINT card{(r.left+r.right)/2,(r.top+r.bottom)/2};
    float center=(d.barU0+d.barU1)*.5f,length=d.barU1-d.barU0;
    for(int step=0;step<=90;++step){POINT cursor{icon.x+(card.x-icon.x)*step/90,icon.y+(card.y-icon.y)*step/90};Advance(d,cursor,1.f/120);}
    Advance(d,card,.8);
    Check(d.windowPreview.Open(),"crossing from icon to popup keeps preview open");
    Check(d.reveal.target==1,"preview hover keeps dock revealed");
    Check(std::fabs((d.barU0+d.barU1)*.5f-center)<.1f&&std::fabs(d.barU1-d.barU0-length)<.1f,"preview hover preserves dock center and expanded length");
    d.menuOpen=true;Advance(d,card,.1);Check(!d.windowPreview.Open(),"menu suppresses preview");d.menuOpen=false;
    Advance(d,Outside(d.mon),1);Check(d.reveal.target==0,"leaving preview hides dock normally");
    Advance(d,EdgePoint(d.mon,g_settings.edge,.5f,0),1.5);
    icon=d.ScreenPointUV(d.slots[1].u,d.margin+d.barThick/2);Advance(d,icon,1);
    fgEnabled=true;fgMonitor=monitor.handle;fgRect=monitor.rect;fgStyle=WS_POPUP;fgClass=L"MatrixFullscreen";
    Advance(d,icon,.8);Check(!d.windowPreview.Open()&&d.reveal.target==0,"fullscreen closes preview and hides dock");fgEnabled=false;
   }
  }
  DestroyWindow(source);printf("PREVIEW_COUPLING monitor=%d edge=%d failures=%d\n",monitorIndex,edge,caseFailures);fflush(stdout);
 }
}

int wmain(int argc,wchar_t** argv) {
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 winrt::init_apartment(winrt::apartment_type::single_threaded);
 if(!g_gfx.Init()){puts("GPU initialization failed");return 2;}
 g_sys.timeText=L"21:30";g_sys.timeShort=L"21:30";g_sys.dateText=L"2026-10-07";
 EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR h,HDC,LPRECT,LPARAM)->BOOL{
  MONITORINFO mi={sizeof(mi)};GetMonitorInfoW(h,&mi);
  monitors.push_back({h,mi.rcMonitor,(mi.dwFlags&MONITORINFOF_PRIMARY)!=0,true,(mi.dwFlags&MONITORINFOF_PRIMARY)?"actual-primary":"actual-secondary"});return TRUE;
 },0);
 const int actualCount=(int)monitors.size();
 if(argc>1 && wcscmp(argv[1],L"--preview-coupling")==0){
  report.open(AppDataDir()+L"\\preview-coupling.tsv");PreviewCouplingTests(actualCount);
  printf("RESULT preview_coupling_cases=%d checks=%d failures=%d; synthetic cursor, production Dock update and real DWM popup\n",cases,checks,failures);return failures?1:0;
 }
 if(argc>1 && wcscmp(argv[1],L"--trace")==0){TraceReproductions(actualCount);return 0;}
 if(argc>1 && wcscmp(argv[1],L"--render-only")==0){
  report.open(AppDataDir()+L"\\render-checks.tsv");
  RenderOptions(actualCount);
  printf("RENDER checks=%d failed=%d frames=%d; sample data only\n",checks,failures,actualCount*64);return failures?1:0;
 }
 report.open(AppDataDir()+L"\\matrix-results.tsv");
 monitors.push_back({(HMONITOR)(uintptr_t)0x1001,{-1920,0,0,1080},false,false,"simulated-left-negative"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1002,{0,-1440,2560,0},false,false,"simulated-above-negative"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1003,{3840,-200,5280,2360},false,false,"simulated-portrait"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1004,{0,0,1366,768},true,false,"simulated-small-laptop"});
 // common resolutions: the dock scales with the short side of the screen
 monitors.push_back({(HMONITOR)(uintptr_t)0x1005,{0,0,1280,720},true,false,"simulated-720p"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1006,{0,0,1920,1080},true,false,"simulated-1080p"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1007,{0,0,2560,1440},true,false,"simulated-1440p"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1008,{0,0,3840,2160},true,false,"simulated-4k"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1009,{0,0,3440,1440},true,false,"simulated-ultrawide"});
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(float size:{36.f,44.f,54.f})for(float mag:{1.f,1.7f,2.1f})for(int count:{12,40}) {
  ++cases;caseFailures=0;
  std::ostringstream label;label<<monitor.name<<" edge="<<edge<<" size="<<size<<" magnification="<<mag<<" apps="<<count;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.iconSize=size;g_settings.magnification=mag;
  Apps(count);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);StateTests(d);SweepTests(d);
  if(monitor.actual && ((size==44 && mag==1.7f && count==12)||(size==54 && mag==2.1f && count==40)))ReachabilityTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
  if(cases%50==0){printf("Progress: %d cases, %d checks, %d failed checks\n",cases,checks,failures);fflush(stdout);}
 }
 // Focused setting combinations in addition to the unchanged baseline matrix:
 // small/default-density and large/crowded, all edges and monitor geometries.
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(int clock=0;clock<2;++clock)
 for(int spacing=0;spacing<3;++spacing)for(bool desktop:{false,true})for(bool crowded:{false,true}){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"options "<<monitor.name<<" edge="<<edge<<" clock="<<clock<<" spacing="<<spacing<<" desktop="<<desktop<<" crowded="<<crowded;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.clockSize=clock;g_settings.iconSpacing=spacing;g_settings.showDesktopButton=desktop;g_settings.showOledButton=((edge+clock+spacing)&1)!=0;g_settings.showThemeButton=((edge+spacing)&1)==0;
  g_settings.iconSize=crowded?54.f:36.f;g_settings.magnification=crowded?2.1f:1.7f;
  Apps(crowded?40:12);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);OptionsTests(d);SweepTests(d);DragSpacingTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Extremes: the largest icon size a settings file accepts, very many apps, every button on or
 // every optional button off. Fit, button pairing and click targets must still hold everywhere.
 struct Extreme{const char* name;float size,mag;int apps;bool buttons;int spacing,clock;};
 const Extreme extremes[]={{"max-icons-crowded",96,2.1f,60,true,2,1},{"ini-magnification-above-menu",96,2.5f,60,true,2,1},{"min-icons-crowded",32,1.7f,80,true,0,0},
  {"large-icons-few",54,2.1f,5,false,1,1},{"large-icons-all-buttons",54,2.1f,25,true,1,0}};
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(const auto& x:extremes){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"extreme "<<x.name<<" "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.iconSize=x.size;g_settings.magnification=x.mag;
  g_settings.iconSpacing=x.spacing;g_settings.clockSize=x.clock;
  g_settings.magnification=std::min(g_settings.magnification,2.1f);   // what Settings::Load accepts
  g_settings.showDesktopButton=g_settings.showThemeButton=g_settings.showOledButton=x.buttons;
  Apps(x.apps);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  // let the fit settle as it would after a few frames on screen
  for(int i=0;i<60;++i){d.ApplyFit(1.f/60.f);d.Layout(0);}
  Check(d.barU0>=-1 && d.barU1<=d.len+1,"extreme settings still fit the monitor edge");
  StateTests(d);OptionsTests(d);SweepTests(d);
  if(monitor.actual)ReachabilityTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 RenderOptions(actualCount);
 report<<"TOTAL\t"<<cases<<"\t"<<checks<<"\t"<<failures<<"\n";report.close();
 printf("RESULT cases=%d checks=%d failed_checks=%d; synthetic input tests; render fixtures=%d\n",cases,checks,failures,actualCount*64);
 return failures?1:0;
}
