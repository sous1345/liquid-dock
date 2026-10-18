// Production preview logic with an in-memory window/DWM model. No real windows,
// input, taskbar settings, source capture, or user app activation are involved.
#include "../src/common.h"
#include "../src/apps.h"
#include <dwmapi.h>
#include <map>
#include <set>
#include <cstdio>
#define private public
#include "../src/window_preview.h"
#undef private

struct Source { DWORD pid = 3; bool alive = true, minimized = false, protectedContent = false; };
static std::map<HWND, Source> sources;
static std::set<HTHUMBNAIL> thumbnails;
static bool shown = false, failDwm = false;
static int updates = 0, checks = 0, activations = 0;
static HWND foreground = (HWND)0x9090;
static SHORT escapeState = 0;
static WindowPreview* current = nullptr;
static HWND captured = nullptr;
static LONG_PTR WINAPI FakeUserData(HWND, int) { return reinterpret_cast<LONG_PTR>(current); }
static HWND WINAPI FakeCapture(HWND h) { HWND old = captured; captured = h; return old; }
static HWND WINAPI FakeCaptured() { return captured; }
static BOOL WINAPI FakeRelease() { captured = nullptr; return TRUE; }
static BOOL WINAPI FakeIsWindow(HWND h) { return sources.count(h) && sources[h].alive; }
static BOOL WINAPI FakeVisible(HWND h) { return h == (HWND)0x8080 ? shown : FakeIsWindow(h); }
static BOOL WINAPI FakeShow(HWND, int cmd) { shown = cmd != SW_HIDE; return TRUE; }
static DWORD WINAPI FakePid(HWND h, LPDWORD pid) { if (pid) *pid = FakeIsWindow(h) ? sources[h].pid : 0; return 1; }
static BOOL WINAPI FakeIconic(HWND h) { return sources[h].minimized; }
static BOOL WINAPI FakeAffinity(HWND h, DWORD* value) { *value = sources[h].protectedContent ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE; return TRUE; }
static HRESULT WINAPI FakeAttribute(HWND, DWORD, PVOID value, DWORD) { *static_cast<DWORD*>(value) = 0; return S_OK; }
static HRESULT WINAPI FakeRegister(HWND, HWND h, HTHUMBNAIL* out) {
    if (failDwm) return E_FAIL; *out = (HTHUMBNAIL)h; thumbnails.insert(*out); return S_OK;
}
static HRESULT WINAPI FakeUnregister(HTHUMBNAIL h) { thumbnails.erase(h); return S_OK; }
static HRESULT WINAPI FakeSize(HTHUMBNAIL, SIZE* size) { *size = {1920, 1080}; return S_OK; }
static HRESULT WINAPI FakeUpdate(HTHUMBNAIL, const DWM_THUMBNAIL_PROPERTIES* props) {
    ++updates;
    if (props->rcDestination.right <= props->rcDestination.left || props->rcDestination.bottom <= props->rcDestination.top) std::abort();
    return S_OK;
}
static BOOL WINAPI FakePos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
static int WINAPI FakeRegion(HWND, HRGN r, BOOL) { DeleteObject(r); return 1; }
static BOOL WINAPI FakeInvalidate(HWND, const RECT*, BOOL) { return TRUE; }
static int WINAPI FakeText(HWND, LPWSTR text, int count) { wcscpy_s(text, count, L"Fixture window"); return 14; }
static HWND WINAPI FakeForeground() { return foreground; }
static SHORT WINAPI FakeKey(int key) { return key == VK_ESCAPE ? escapeState : 0; }
static BOOL WINAPI FakeDestroy(HWND) { return TRUE; }

#define IsWindow FakeIsWindow
#define IsWindowVisible FakeVisible
#define ShowWindow FakeShow
#define GetWindowThreadProcessId FakePid
#define IsIconic FakeIconic
#define GetWindowDisplayAffinity FakeAffinity
#define DwmGetWindowAttribute FakeAttribute
#define DwmRegisterThumbnail FakeRegister
#define DwmUnregisterThumbnail FakeUnregister
#define DwmQueryThumbnailSourceSize FakeSize
#define DwmUpdateThumbnailProperties FakeUpdate
#define SetWindowPos FakePos
#define SetWindowRgn FakeRegion
#define InvalidateRect FakeInvalidate
#define GetWindowTextW FakeText
#define GetForegroundWindow FakeForeground
#define GetAsyncKeyState FakeKey
#define DestroyWindow FakeDestroy
#define GetWindowLongPtrW FakeUserData
#define SetCapture FakeCapture
#define GetCapture FakeCaptured
#define ReleaseCapture FakeRelease
#include "../src/window_preview.cpp"

AppModel g_apps;
void AppModel::ActivateWindow(HWND) { ++activations; }
static void Check(bool value, const char* message) {
    ++checks; if (!value) { std::printf("FAIL: %s\n", message); std::exit(1); }
}
static std::shared_ptr<AppItem> MakeApp(int count) {
    auto app = std::make_shared<AppItem>(); app->name = L"Fixture";
    sources.clear();
    for (int i = 0; i < count; ++i) { HWND h = (HWND)(INT_PTR)(0x1000 + i); sources[h] = {}; app->windows.push_back(h); }
    return app;
}
int main() {
    for (RECT mon : {RECT{0,0,3840,2160}, RECT{567,2160,3127,3760}, RECT{-1280,-800,0,0}, RECT{0,0,800,600}})
    for (Edge edge : {Edge::Left, Edge::Right, Edge::Top, Edge::Bottom})
    for (int count : {1,2,8,40}) {
        auto app = MakeApp(count);
        WindowPreview p; p.hwnd = (HWND)0x8080; p.app = app; p.monitor = mon; p.edge = edge; p.scale = 1;
        int cx = (mon.left+mon.right)/2, cy = (mon.top+mon.bottom)/2;
        switch(edge) {
        case Edge::Left: p.anchor={mon.left,cy-24,mon.left+100,cy+24}; break;
        case Edge::Right: p.anchor={mon.right-100,cy-24,mon.right,cy+24}; break;
        case Edge::Top: p.anchor={cx-24,mon.top,cx+24,mon.top+100}; break;
        case Edge::Bottom: p.anchor={cx-24,mon.bottom-100,cx+24,mon.bottom}; break;
        }
        p.Refresh(); shown=true;
        std::set<HWND> reached;
        for (int page=0; page<count; ++page) {
            Check(p.bounds.left>=mon.left && p.bounds.right<=mon.right && p.bounds.top>=mon.top && p.bounds.bottom<=mon.bottom,"popup stays on selected monitor");
            Check(edge==Edge::Left ? p.bounds.left>p.anchor.right : edge==Edge::Right ? p.bounds.right<p.anchor.left : edge==Edge::Top ? p.bounds.top>p.anchor.bottom : p.bounds.bottom<p.anchor.top,"popup clears dock on all four edges");
            for(int i=p.first;i<std::min(p.first+p.capacity,count);++i) {
                auto& e=p.entries[i]; reached.insert(e.source);
                POINT center{(e.card.left+e.card.right)/2,(e.card.top+e.card.bottom)/2};
                Check(p.Hit(center)==i,"each displayed card selects its own source");
                Check(e.thumbnail!=nullptr,"visible ordinary window gets DWM relationship");
            }
            Check(thumbnails.size()==(size_t)std::min(p.capacity,count-p.first),"only current page retains thumbnails");
            int before=p.first;p.ChangePage(1);if(before==p.first)break;
        }
        Check((int)reached.size()==count,"all windows remain reachable through pages");
        p.Dismiss();Check(thumbnails.empty()&&!shown,"dismiss releases all DWM handles and hides popup");
    }
    {
        auto app=MakeApp(3);sources[app->windows[0]].minimized=true;sources[app->windows[1]].protectedContent=true;
        WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,1920,1080};p.anchor={900,980,950,1080};
        p.Refresh();
        Check(!p.entries[0].thumbnail&&!p.entries[0].fallback.empty(),"minimized window stays minimized with selectable fallback");
        Check(!p.entries[1].thumbnail&&!p.entries[1].fallback.empty(),"known capture restriction does not request thumbnail");
        Check(p.entries[2].thumbnail!=nullptr,"ordinary sibling still has preview");
        sources[app->windows[2]].pid=9;Check(!p.Valid(p.entries[2]),"recycled handle from another process cannot be activated");
        sources[app->windows[2]].alive=false;p.Refresh();Check(p.entries.size()==2,"closed window is removed");
        failDwm=true;sources[app->windows[0]].minimized=false;p.Refresh();
        Check(!p.entries[0].thumbnail&&!p.entries[0].fallback.empty(),"DWM failure keeps a selectable title");
        failDwm=false;
    }
    {
        auto app=MakeApp(1);WindowPreview p;p.hwnd=(HWND)0x8080;
        RECT anchor{0,400,100,450},mon{0,0,1920,1080};POINT cursor{50,425};
        auto tick=[&](std::shared_ptr<AppItem> hovered,POINT pos,double time,bool allowed=true){p.Update(nullptr,hovered,anchor,mon,Edge::Left,1,pos,time,allowed);};
        tick(app,cursor,0);tick(app,cursor,0.44);Check(!p.Open(),"no popup before dwell threshold");
        tick(app,cursor,0.46);Check(p.Open(),"popup opens after dwell without activation");
        POINT bridge{(p.bridge.left+p.bridge.right)/2,(p.bridge.top+p.bridge.bottom)/2};
        tick(nullptr,bridge,0.5);Check(p.Open()&&p.Contains(bridge),"crossing gap keeps popup open");
        tick(nullptr,{1800,900},0.6);tick(nullptr,{1800,900},0.9);Check(!p.Open(),"leaving closes after grace period");
        tick(app,cursor,1);tick(app,cursor,1.5);escapeState=(SHORT)0x8000;tick(app,cursor,1.6);escapeState=0;
        tick(app,cursor,3);Check(!p.Open(),"escape does not immediately reopen under stationary cursor");
        tick(nullptr,{1800,900},3.1);tick(app,cursor,3.2);tick(app,cursor,3.7);Check(p.Open(),"leaving and returning permits reopening");
        foreground=(HWND)0x9999;tick(app,cursor,3.8);Check(!p.Open(),"foreground switch dismisses popup");
        tick(nullptr,{1800,900},4);tick(app,cursor,4.1);tick(app,cursor,4.6);tick(app,cursor,4.7,false);
        Check(!p.Open(),"fullscreen or drag inhibition closes popup");
    }
    {
        auto app=MakeApp(2);WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,1920,1080};p.anchor={900,980,950,1080};
        p.Refresh();shown=true;current=&p;
        RECT card=p.entries[0].card;LPARAM point=MAKELPARAM((card.left+card.right)/2,(card.top+card.bottom)/2);
        WindowPreview::Proc(p.hwnd,WM_LBUTTONDOWN,0,point);
        sources[p.entries[0].source].pid=77;
        WindowPreview::Proc(p.hwnd,WM_LBUTTONUP,0,point);
        Check(activations==0,"a changed source between press and release cannot be activated");
        sources[p.entries[0].source].pid=p.entries[0].pid;
        WindowPreview::Proc(p.hwnd,WM_LBUTTONDOWN,0,point);
        WindowPreview::Proc(p.hwnd,WM_LBUTTONUP,0,point);
        Check(activations==1&&!p.Open(),"matching press and release routes activation once and dismisses");
        current=nullptr;
    }
    Check(thumbnails.empty()&&activations==1,"fixtures route to fake activation and release all thumbnails");
    std::printf("PASS: %d preview checks, %d thumbnail updates\n",checks,updates);
}
