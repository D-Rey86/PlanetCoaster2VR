// Actual button transport with measured native stand-ins, no game/window/UI.
#include <windows.h>
namespace fixture {ULONGLONG now=1000;ULONGLONG Tick(){return now;}}
#define GetTickCount64 fixture::Tick
#include "../../Loader/src/world_click.cpp"
#undef GetTickCount64
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
using namespace pc2vr::pointer;
bool sourceReady=true,removed{},held{},accepting=true,consume=true,cancelBeforeRead{},handled{};
unsigned forwarded{},wakes{},sourceInstalls{};
std::vector<USHORT> edges;
void Check(bool value,const char* why){if(!value){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
UINT WINAPI Original(HRAWINPUT,UINT,LPVOID,PUINT,UINT){++forwarded;Check(GetLastError()==123,"physical raw LastError before original changed");SetLastError(456);return 1234;}
bool OriginalUi(int x,int y){Check(x==23 && y==45,"native UI arguments changed");SetLastError(567);return handled;}
void Consume(HRAWINPUT token){
    if(!consume)return;
    if(cancelBeforeRead){click::Clear();cancelBeforeRead=false;}
    RAWINPUT raw{};UINT size=sizeof(raw);SetLastError(789);
    Check(click::Data(token,RID_INPUT,&raw,&size,24)==48 && GetLastError()==789,"own raw read/LastError changed");
    Check(raw.header.hDevice==click::packets.Device(),"own device identity lost");
    edges.push_back(raw.data.mouse.usButtonFlags);
    if(raw.data.mouse.usButtonFlags&RI_MOUSE_LEFT_BUTTON_DOWN)held=true;
    if(raw.data.mouse.usButtonFlags&RI_MOUSE_LEFT_BUTTON_UP)held=false;
}
}
namespace pc2vr::pointer::native {
uintptr_t Address(Landmark){return 0;}
bool VerifyRetail(){return false;}
bool Install(Site,void*,Hook&){return false;}
}
namespace pc2vr::pointer::sources {
bool Install(HANDLE){++sourceInstalls;return false;}
bool Enable(){return false;}
bool Available(){SetLastError(801);return sourceReady && !removed;}
bool OwnNeutral(){return !held;}
Snapshot Read(){SetLastError(802);return{27,4,true,true,false,removed,held,held?1:0};}
}
namespace pc2vr::pointer::window {
bool Attach(HWND,ReleaseFn,PumpFn){return false;}
bool Accepting(){return accepting;}
void Wake(){++wakes;SetLastError(803);}
bool DeliverInput(HRAWINPUT token){Consume(token);return true;}
}
int main(){
    using namespace pc2vr::pointer;
    Check(!click::Prepare() && !sourceInstalls,"foreign process installed native source hooks");
    Check(!click::BindWindow(nullptr),"null native window accepted");
    click::dataOriginal=reinterpret_cast<void*>(Original);UINT size{};SetLastError(123);
    Check(click::Data(reinterpret_cast<HRAWINPUT>(uintptr_t{9}),RID_INPUT,nullptr,&size,24)==1234 &&
        forwarded==1 && GetLastError()==456,"physical raw path changed");
    click::prepared=click::bound=true;click::targetThread=GetCurrentThreadId();
    Check(click::UiOwnsInput(),"unknown UI state did not retain native controls");
    click::uiHook.original.store(reinterpret_cast<void*>(OriginalUi));
    Check(!click::Publish(true,1000),"startup held trigger accepted");
    Check(click::Publish(false,1000),"startup neutral rejected");
    Check(!click::Publish(true,1000),"unknown UI hover allowed menu press");
    Check(!click::UiMove(23,45) && GetLastError()==567,"native UI unhandled result/LastError changed");
    Check(!click::UiOwnsInput(),"unhandled native UI blocked world controls");
    Check(click::Publish(false,1000) && click::Publish(true,1000) && click::Publish(false,1000),"ordered tap enqueue failed");
    Check(edges.empty(),"render publisher dispatched native input");click::Pump(1000,true);
    Check(edges==std::vector<USHORT>{RI_MOUSE_LEFT_BUTTON_DOWN,RI_MOUSE_LEFT_BUTTON_UP} && !held,"short tap edges collapsed");
    fixture::now=1001;
    const auto idleWakes=wakes;Check(click::Publish(false,1001) && wakes==idleWakes,"neutral frame posts unnecessary wake");
    Check(click::Publish(true,1001),"world press rejected");click::Pump(1001,true);Check(held,"world hold missing");
    handled=true;Check(click::UiMove(23,45) && GetLastError()==567,"native UI handled result changed");
    Check(click::UiOwnsInput(),"native UI did not retain A input");
    click::Pump(1001,true);Check(!held,"entering native UI did not cancel world hold");
    Check(!click::Publish(true,1001),"native menu received world trigger press");
    handled=false;click::UiMove(23,45);Check(!click::Publish(true,1001),"held trigger rearmed after leaving UI");
    Check(click::Publish(false,1001) && click::Publish(true,1001),"neutral UI recovery failed");
    click::Pump(1001,true);Check(held,"recovered press missing");click::Pump(1001,false);Check(!held,"focus loss retained native hold");
    Check(!click::Publish(true,1001),"focus cancellation revived held trigger");
    Check(click::Publish(false,1001) && click::Publish(true,1001),"tracking recovery failed");click::Pump(1001,true);
    fixture::now=1252;click::Pump(1252,true);Check(!held && !click::stream.valid,"stale XR source retained world hold");
    fixture::now=1300;Check(click::Publish(false,1300) && click::Publish(true,1300),"fresh source recovery failed");
    cancelBeforeRead=true;click::Pump(1300,true);Check(!held && edges.back()==0,"cancelled buffered DOWN reached native input");
    Check(click::Publish(false,1300),"buffered cancellation recovery failed");
    const auto old=click::packets.Acquire(true,click::stream.generation);click::Clear();click::Publish(false,1300);
    RAWINPUT raw{};size=sizeof(raw);SetLastError(789);
    Check(click::Data(old,RID_INPUT,&raw,&size,24)==48 && !raw.data.mouse.usButtonFlags && GetLastError()==789,"new interval revived old DOWN");
    Check(click::Publish(true,1300),"overflow setup rejected");click::Pump(1300,true);Check(held,"overflow setup not held");
    for(int i=0;i<8;++i)Check(click::Publish(false,1300) && click::Publish(true,1300),"edge capacity smaller than contract");
    Check(!click::Publish(false,1300),"edge overflow accepted");click::Pump(1300,true);Check(!held,"overflow retained native hold");
    Check(!click::Publish(true,1300) && click::Publish(false,1300),"overflow recovery bypassed neutral rearm");
    removed=true;const auto gone=click::packets.Acquire(false);size=sizeof(raw);
    Check(click::Data(gone,RID_INPUT,&raw,&size,24)==UINT(-1) && GetLastError()==ERROR_INVALID_HANDLE && forwarded==1,"removed token reached native fallback");
    removed=false;Check(click::Publish(false,1300) && click::Publish(true,1300),"pre-thread control failed");click::Pump(1300,true);Check(held,"pre-thread hold missing");
    const auto foreign=click::packets.Acquire(true,click::stream.generation);UINT result{};DWORD error{};
    std::thread thread([&]{RAWINPUT other{};UINT bytes=sizeof(other);result=click::Data(foreign,RID_INPUT,&other,&bytes,24);error=GetLastError();});thread.join();
    Check(result==UINT(-1) && error==ERROR_INVALID_THREAD_ID && !click::Available(),"unknown async raw consumer accepted");
    click::Pump(1300,true);Check(!held,"transport fault prevented final UP");
    std::puts("PASS actual world buttons: UI rejection, native forwarding, short taps, neutral rearm, focus/stale release, generations and removal/thread safety");
}
