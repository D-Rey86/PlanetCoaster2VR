#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace fixture {
HWND hwnd=reinterpret_cast<HWND>(uintptr_t{23}),foreground=hwnd;
ULONGLONG now=1000;
unsigned posted{},pumps{},moves{},raws{},destroyed{};
bool accepting{};
LPARAM lastMove{};
HWND Foreground(){return foreground;}
ULONGLONG Tick(){return now;}
BOOL WINAPI Post(HWND,UINT,WPARAM,LPARAM){++posted;return TRUE;}
BOOL WINAPI Kill(HWND,UINT_PTR){++destroyed;return TRUE;}
LRESULT WINAPI Call(WNDPROC proc,HWND window,UINT message,WPARAM wparam,LPARAM lparam){return proc(window,message,wparam,lparam);}
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
}
#define GetForegroundWindow fixture::Foreground
#define GetTickCount64 fixture::Tick
#define PostMessageW fixture::Post
#define KillTimer fixture::Kill
#define CallWindowProcW fixture::Call
#include "../../Loader/src/window_pointer.cpp"
#undef GetForegroundWindow
#undef GetTickCount64
#undef PostMessageW
#undef KillTimer
#undef CallWindowProcW
namespace fixture {
LRESULT CALLBACK Previous(HWND,UINT message,WPARAM,LPARAM lparam){
    pc2vr::pointer::window::Read(); // Callouts must be outside the window lock.
    if(message==WM_MOUSEMOVE){++moves;lastMove=lparam;}
    if(message==WM_INPUT)++raws;
    return 123;
}
void Pump(ULONGLONG time,bool active){
    ++pumps;accepting=active;Check(time==now,"pump timestamp changed");
    const auto sample=pc2vr::pointer::window::Read();
    Check(!sample.active && !sample.releasing,"button pump acquired cursor-position ownership");
    Check(pc2vr::pointer::window::DeliverInput(reinterpret_cast<HRAWINPUT>(uintptr_t{99})),"window-thread button delivery rejected");
}
}
int main(){
    using namespace pc2vr::pointer::window;using fixture::Check;
    Check(!Attach(nullptr,nullptr,fixture::Pump),"invalid window accepted");
    s.hwnd=fixture::hwnd;s.thread=GetCurrentThreadId();s.installed=s.focused=true;s.pump=fixture::Pump;
    s.message=0xc105;s.timer=123;s.width=s.height=944;previous.store(fixture::Previous);
    Check(!DeliverInput(reinterpret_cast<HRAWINPUT>(uintptr_t{99})),"button dispatched outside pump");
    Wake();Wake();Check(fixture::posted==1,"wake requests not coalesced");
    Procedure(s.hwnd,s.message,reinterpret_cast<WPARAM>(&s),0);
    Check(fixture::pumps==1 && fixture::accepting && fixture::raws==1 && fixture::moves==0,"wake moved native cursor or lost button");
    const auto coordinates=MAKELPARAM(318,418);
    Check(Procedure(s.hwnd,WM_MOUSEMOVE,77,coordinates)==123 && fixture::moves==1 && fixture::lastMove==coordinates,
        "button-only mode rewrote ordinary mouse coordinates");
    Procedure(s.hwnd,WM_TIMER,s.timer,0);Check(fixture::pumps==2 && fixture::moves==1,"timer moved cursor");
    fixture::foreground=nullptr;Procedure(s.hwnd,WM_KILLFOCUS,0,0);
    Check(!fixture::accepting && !Accepting(),"focus loss not delivered to button pump");
    fixture::foreground=fixture::hwnd;Procedure(s.hwnd,WM_SETFOCUS,0,0);
    Check(fixture::accepting && Accepting(),"focus recovery not delivered");
    Procedure(s.hwnd,WM_NCDESTROY,0,0);
    Check(!fixture::accepting && !s.installed && !s.hwnd && fixture::destroyed==1,"window destruction did not invalidate pump");
    std::puts("PASS actual window button pump: no cursor ownership/moves, native forwarding, reentrancy, coalesced wake, timer/focus/destruction");
}
