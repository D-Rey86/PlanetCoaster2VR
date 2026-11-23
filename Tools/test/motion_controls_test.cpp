#include "../../Loader/src/motion_pad.h"
#include "../../Loader/src/laser_controls.h"
#include "../../Loader/src/menu_pointer.h"
#include "../../Loader/src/alternate_eyes.h"
#include "../../Loader/src/pointer_geometry.h"
#include "../../Loader/src/mouse_events.h"
#include "../../Loader/src/pointer_packets.h"
#include "../../Loader/src/pointer_device.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include "../../Loader/src/selection_panel_state.h"
#include <string>
#include <vector>

namespace pc2vr::log { void Write(const char*, ...) {} }
namespace {
using namespace pc2vr;
void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "%s\n", why); std::exit(1); } }
UINT listCalls{},infoCalls{};
UINT WINAPI RealListFixture(PRAWINPUTDEVICELIST data, UINT* count, UINT stride) {
    ++listCalls;
    if (!count || stride!=sizeof(RAWINPUTDEVICELIST)) { SetLastError(ERROR_INVALID_PARAMETER);return UINT(-1); }
    if (!data) { *count=2;SetLastError(741);return 0; }
    if (*count<2) { *count=2;SetLastError(ERROR_INSUFFICIENT_BUFFER);return UINT(-1); }
    data[0]={reinterpret_cast<HANDLE>(uintptr_t{10}),RIM_TYPEMOUSE};
    data[1]={reinterpret_cast<HANDLE>(uintptr_t{20}),RIM_TYPEKEYBOARD};
    SetLastError(741);return 2;
}
UINT WINAPI RealInfoFixture(HANDLE, UINT, void*, UINT*) { ++infoCalls;SetLastError(742);return 123; }
void PointerMetadata() {
    pointer::RawPackets packets;pointer::RawDeviceApi api(packets);UINT count=0;
    Check(api.List(nullptr,&count,sizeof(RAWINPUTDEVICELIST),RealListFixture)==0 && count==2 &&
        GetLastError()==741,"disabled pointer changed raw list");
    api.Expose(true);count=0;
    Check(api.List(nullptr,&count,sizeof(RAWINPUTDEVICELIST),RealListFixture)==0 && count==3 &&
        GetLastError()==741,"pointer list size query");
    std::array<RAWINPUTDEVICELIST,4> devices{};devices[3].hDevice=reinterpret_cast<HANDLE>(uintptr_t{0xfeed});
    count=2;Check(api.List(devices.data(),&count,sizeof(devices[0]),RealListFixture)==UINT(-1) &&
        count==3 && GetLastError()==ERROR_INSUFFICIENT_BUFFER,"pointer list capacity retry");
    count=0;Check(api.List(devices.data(),&count,sizeof(devices[0]),RealListFixture)==UINT(-1) &&
        count==3 && GetLastError()==ERROR_INSUFFICIENT_BUFFER,"zero-capacity pointer list");
    count=4;Check(api.List(devices.data(),&count,sizeof(devices[0]),RealListFixture)==3 && count==4 &&
        devices[0].hDevice==reinterpret_cast<HANDLE>(uintptr_t{10}) && devices[1].dwType==RIM_TYPEKEYBOARD &&
        devices[2].hDevice==packets.Device() && devices[2].dwType==RIM_TYPEMOUSE &&
        devices[3].hDevice==reinterpret_cast<HANDLE>(uintptr_t{0xfeed}) && GetLastError()==741,"pointer list overwrote real devices/capacity/canary");
    count=9;Check(api.List(devices.data(),&count,1,RealListFixture)==UINT(-1) &&
        count==9 && GetLastError()==ERROR_INVALID_PARAMETER,"invalid list stride changed");
    Check(api.List(devices.data(),nullptr,sizeof(devices[0]),RealListFixture)==UINT(-1),"null list count changed");
    UINT size=0;
    Check(api.Info(packets.Device(),RIDI_DEVICENAME,nullptr,&size,RealInfoFixture)==0 && size>1 && size<256,"pointer name query");
    std::array<char,256> name;name.fill(char(0x55));const UINT needed=size;size=needed-1;
    Check(api.Info(packets.Device(),RIDI_DEVICENAME,name.data(),&size,RealInfoFixture)==UINT(-1) &&
        size==needed && name[0]==char(0x55),"short pointer name written");
    SetLastError(743);size=256;
    Check(api.Info(packets.Device(),RIDI_DEVICENAME,name.data(),&size,RealInfoFixture)==needed &&
        !std::strcmp(name.data(),"PC2VR\\MotionPointer") && name[needed]==char(0x55) && GetLastError()==743,"pointer name contract");
    for (const DWORD inputSize : {0u,32u,0xa5a5a5a5u,0xffffffffu}) {
        RID_DEVICE_INFO info{};info.cbSize=inputSize;size=sizeof(info);
        Check(api.Info(packets.Device(),RIDI_DEVICEINFO,&info,&size,RealInfoFixture)==sizeof(info) &&
            info.cbSize==sizeof(info) && info.dwType==RIM_TYPEMOUSE && info.mouse.dwNumberOfButtons==5,"native uninitialized cbSize rejected");
    }
    const auto calls=infoCalls;size=17;
    Check(api.Info(reinterpret_cast<HANDLE>(uintptr_t{123}),RIDI_DEVICEINFO,name.data(),&size,RealInfoFixture)==123 &&
        infoCalls==calls+1 && size==17 && GetLastError()==742,"physical mouse info changed");
    Check(api.Info(packets.Device(),RIDI_DEVICEINFO,name.data(),nullptr,RealInfoFixture)==UINT(-1) &&
        GetLastError()==ERROR_INVALID_PARAMETER,"null pointer metadata size accepted");
    api.Expose(false);count=0;
    Check(api.List(nullptr,&count,sizeof(devices[0]),RealListFixture)==0 && count==2,"removed pointer still enumerated");
}
void PointerPackets() {
    pointer::RawPackets packets;
    std::array<HRAWINPUT,pointer::RawPackets::kCapacity> tokens{};
    for (size_t i=0;i<tokens.size();++i) {
        tokens[i]=packets.Acquire((i%2)==0);
        Check(tokens[i] && packets.Owns(tokens[i]) && packets.Pending(tokens[i]),"raw packet allocation");
        for (size_t j=0;j<i;++j)Check(tokens[i]!=tokens[j],"pending raw token reused");
    }
    Check(!packets.Acquire(false),"full packet queue overwritten");
    Check(!packets.Owns(nullptr) && !packets.Owns(reinterpret_cast<HRAWINPUT>(uintptr_t{0x1234})),"physical raw handle claimed");
    UINT size=0;
    Check(packets.Read(tokens[0],RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER))==0 && size==48 &&
        packets.Pending(tokens[0]),"raw size query consumed packet");
    RAWINPUTHEADER header{};size=sizeof(header);
    Check(packets.Read(tokens[0],RID_HEADER,&header,&size,sizeof(header))==sizeof(header) &&
        packets.Pending(tokens[0]) && header.dwType==RIM_TYPEMOUSE && header.dwSize==48 &&
        header.hDevice==packets.Device(),"raw header query contract");
    std::array<unsigned char,64> bytes;bytes.fill(0xa5);size=47;
    Check(packets.Read(tokens[0],RID_INPUT,bytes.data(),&size,sizeof(header))==UINT(-1) &&
        GetLastError()==ERROR_INSUFFICIENT_BUFFER && size==48 && packets.Pending(tokens[0]),"short raw buffer consumed packet");
    for (auto byte : bytes)Check(byte==0xa5,"short raw buffer written");
    for (size_t i=0;i<tokens.size();++i) {
        size=64;SetLastError(123);
        Check(packets.Read(tokens[i],RID_INPUT,bytes.data(),&size,sizeof(header))==48 && size==48 &&
            GetLastError()==123 && !packets.Pending(tokens[i]),"raw full read contract");
        RAWINPUT input{};std::memcpy(&input,bytes.data(),48);
        Check(input.header.hDevice==packets.Device() && input.header.dwType==RIM_TYPEMOUSE &&
            input.data.mouse.usFlags==0 && input.data.mouse.lLastX==0 && input.data.mouse.lLastY==0 &&
            input.data.mouse.usButtonFlags==((i%2)==0 ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_UP),"raw payload changed or reordered");
        for (size_t j=48;j<bytes.size();++j)Check(bytes[j]==0xa5,"raw read exceeded declared size");
    }
    size=64;Check(packets.Read(tokens[0],RID_INPUT,bytes.data(),&size,sizeof(header))==UINT(-1),"consumed token accepted");
    const auto cancelled=packets.Acquire(true);packets.Cancel(cancelled);
    Check(!packets.Pending(cancelled),"failed-post packet not cancelled");
    const auto token=packets.Acquire(false);size=64;
    Check(packets.Read(token,RID_INPUT,bytes.data(),&size,4)==UINT(-1) && packets.Pending(token),"wrong raw header size accepted");
    Check(packets.Read(token,0,bytes.data(),&size,sizeof(header))==UINT(-1) && packets.Pending(token),"invalid raw command consumed packet");
    Check(packets.Read(token,RID_INPUT,bytes.data(),nullptr,sizeof(header))==UINT(-1) && packets.Pending(token),"null raw size consumed packet");
    packets.Cancel(token);
    for (const bool down : {true,false}) {
        const auto stale=packets.Acquire(down);RAWINPUT input{};size=sizeof(input);
        Check(packets.Read(stale,RID_INPUT,&input,&size,sizeof(header),false)==48 &&
            input.data.mouse.usButtonFlags==(down ? 0 : RI_MOUSE_LEFT_BUTTON_UP),"stale pointer press delivered or release dropped");
    }
    // Queue press/release in one interval, cancel it, then resume a fresh valid
    // sample before the old packet is read. Active=true alone must not revive it.
    for (const bool down : {true,false}) {
        const auto old=packets.Acquire(down,41);RAWINPUT input{};size=sizeof(input);
        Check(packets.Read(old,RID_INPUT,&input,&size,sizeof(header),true,42)==48 &&
            input.data.mouse.usButtonFlags==(down ? 0 : RI_MOUSE_LEFT_BUTTON_UP),"new interval revived queued press or lost release");
    }
    const auto fresh=packets.Acquire(true,42);RAWINPUT freshInput{};size=sizeof(freshInput);
    Check(packets.Read(fresh,RID_INPUT,&freshInput,&size,sizeof(header),true,42)==48 &&
        freshInput.data.mouse.usButtonFlags==RI_MOUSE_LEFT_BUTTON_DOWN,"current generation press suppressed");
}
void MouseOwnership() {
    using pointer::ButtonSources;
    using pointer::Event;
    // Replay both observed native overlap orders, and every release order.
    for (const bool virtualFirst : {false, true}) for (const bool virtualLast : {false, true}) {
        ButtonSources sources;
        Check(sources.Add(23) && sources.Add(27),"mouse registration");
        const uint32_t first=virtualFirst ? 27u : 23u, second=virtualFirst ? 23u : 27u;
        Event event{1,first,4,0,1,0x12345678};
        const Event original=event;
        Check(sources.Filter(event) && !std::memcmp(&event,&original,sizeof(event)),"first mouse press changed");
        event={2,second,4,0,1,0};Check(!sources.Filter(event),"overlap emitted second down");
        const uint32_t last=virtualLast ? 27u : 23u, released=virtualLast ? 23u : 27u;
        event={3,released,4,0,0,0};Check(!sources.Filter(event) && sources.Held(),"overlap prematurely released drag");
        event={4,last,4,0,0,0x87654321};
        Check(sources.Filter(event) && event.device==first && !sources.Held() &&
            event.time==4 && event.padding==0x87654321,"aggregate release lost first-down identity");
        event={5,23,4,0,1,0};Check(sources.Filter(event),"mouse couldn't start next drag");
        event.value=0;Check(sources.Filter(event),"mouse couldn't finish next drag");
    }
    ButtonSources sources;Check(sources.Add(23) && sources.Add(27),"source setup");
    const Event unaffected[]={{9,23,4,1,1,0xffffffff},{9,23,2,0,123,4},
        {9,99,4,0,1,4},{9,23,0,0,0,4},{9,23,4,0,2,4}};
    for (const auto& original : unaffected) {
        auto event=original;Check(sources.Filter(event) && !std::memcmp(&event,&original,sizeof(event)) &&
            !sources.Held(),"unrelated native input changed");
    }
    Event event{10,27,4,0,1,0};Check(sources.Filter(event),"disconnect setup");
    event.device=23;Check(!sources.Filter(event),"disconnect overlap setup");
    Event release{};
    Check(!sources.Remove(27,11,release) && sources.Held() && !sources.Known(27),"disconnect cancelled another hold");
    Check(sources.Remove(23,12,release) && !sources.Held() && release.device==27 && release.time==12 &&
        release.kind==4 && release.control==0 && release.value==0,"last disconnect didn't release original owner");
    Check(!sources.Remove(23,13,release),"duplicate disconnect emitted release");
    Check(sources.Add(23) && sources.Add(27),"reconnect failed");
    for (uint32_t i=0;i<ButtonSources::kCapacity-2;++i)Check(sources.Add(i+100),"registry capacity");
    Check(!sources.Add(1000) && sources.Add(23),"registry overflow or duplicate corrupts state");
    sources.Remove(23,14,release);Check(sources.Add(1000),"released registry slot not reusable");
}
bool Near(float a, float b) { return std::fabs(a-b) < 1e-5f; }
template<typename T> T Handle(uintptr_t n) { return reinterpret_cast<T>(n); }
std::map<std::string, XrPath> paths;
std::map<XrAction, std::string> names;
std::map<XrSpace,std::string> spaceNames;
unsigned anchorReads{};
bool anchorTracking=true,anchorFailure=false;
unsigned nextHandle = 10, reads = 0, destroyed = 0, spacesDestroyed = 0, profiles = 0;
bool failure = false, tracking = true, pressed = false;
XrTime locatedTime{};
unsigned attaches{}, syncs{}, setCount{};
XrResult syncResult=XR_SUCCESS, attachResult=XR_SUCCESS;
XrResult XRAPI_CALL Attach(XrSession, const XrSessionActionSetsAttachInfo* info) {
    ++attaches; setCount=info->countActionSets; return attachResult;
}
XrResult XRAPI_CALL Sync(XrSession, const XrActionsSyncInfo* info) {
    ++syncs; Check(info->countActiveActionSets==setCount,"gaze/controllers not synchronized together");return syncResult;
}
XrResult XRAPI_CALL Path(XrInstance, const char* name, XrPath* out) {
    auto& p = paths[name]; if (!p) p = paths.size(); *out = p; return XR_SUCCESS;
}
XrResult XRAPI_CALL Set(XrInstance, const XrActionSetCreateInfo*, XrActionSet* out) { *out=Handle<XrActionSet>(nextHandle++); return XR_SUCCESS; }
XrResult XRAPI_CALL Destroy(XrActionSet) { ++destroyed; return XR_SUCCESS; }
XrResult XRAPI_CALL Create(XrActionSet, const XrActionCreateInfo* ci, XrAction* out) {
    Check(ci->countSubactionPaths==2 && ci->subactionPaths[0]!=ci->subactionPaths[1], "hand subactions missing");
    if (failure && !strcmp(ci->actionName,"grip")) return XR_ERROR_RUNTIME_FAILURE;
    if(anchorFailure && !strcmp(ci->actionName,"controller_anchor"))return XR_ERROR_RUNTIME_FAILURE;
    *out=Handle<XrAction>(nextHandle++); names[*out]=ci->actionName; return XR_SUCCESS;
}
XrResult XRAPI_CALL Suggest(XrInstance, const XrInteractionProfileSuggestedBinding* info) {
    ++profiles; Check(info->countSuggestedBindings<=20 && info->countSuggestedBindings>=6, "binding bounds");
    for(unsigned i=0;i<info->countSuggestedBindings;++i) Check(info->suggestedBindings[i].action && info->suggestedBindings[i].binding,"invalid binding");
    return XR_SUCCESS;
}
XrResult XRAPI_CALL Space(XrSession, const XrActionSpaceCreateInfo* ci, XrSpace* out) {
    Check((names[ci->action]=="aim" || names[ci->action]=="controller_anchor") && ci->subactionPath && ci->poseInActionSpace.orientation.w==1,"invalid controller space");
    *out=Handle<XrSpace>(nextHandle++);spaceNames[*out]=names[ci->action];return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySpace(XrSpace) { ++spacesDestroyed; return XR_SUCCESS; }
XrResult XRAPI_CALL Pose(XrSession, const XrActionStateGetInfo* info, XrActionStatePose* out) { ++reads;if(names[info->action]=="controller_anchor")++anchorReads;out->isActive=XR_TRUE; return XR_SUCCESS; }
XrResult XRAPI_CALL Locate(XrSpace space, XrSpace, XrTime time, XrSpaceLocation* out) {
    ++reads; locatedTime=time; out->pose={{0,0,0,1},{.2f,1.3f,-.3f}};
    out->locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if(tracking) out->locationFlags|=XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if(spaceNames[space]=="controller_anchor"){
        out->pose.position={-.3f,1.1f,-.4f};
        if(!anchorTracking)out->locationFlags=0;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL Vector(XrSession,const XrActionStateGetInfo*,XrActionStateVector2f* out) { ++reads; out->isActive=XR_TRUE;out->currentState=pressed ? XrVector2f{.25f,-.5f} : XrVector2f{};return XR_SUCCESS; }
XrResult XRAPI_CALL Scalar(XrSession,const XrActionStateGetInfo*,XrActionStateFloat* out) { ++reads;out->isActive=XR_TRUE;out->currentState=pressed ? .75f : 0.f;return XR_SUCCESS; }
XrResult XRAPI_CALL Boolean(XrSession,const XrActionStateGetInfo*,XrActionStateBoolean* out) { ++reads;out->isActive=XR_TRUE;out->currentState=pressed;return XR_SUCCESS; }
}
int main() {
    MouseOwnership();
    PointerPackets();
    PointerMetadata();
    using namespace pc2vr;
    motion::Frame frame{}; frame.hand[0].active=frame.hand[1].active=true;
    frame.hand[0].stick={-1,1};frame.hand[1].stick={1,-1};
    frame.hand[0].trigger=.5f;frame.hand[1].trigger=1;
    frame.hand[0].grip=frame.hand[1].grip=.5f;
    frame.hand[0].primary=frame.hand[0].secondary=frame.hand[0].thumb=frame.hand[0].menu=true;
    frame.hand[1].primary=frame.hand[1].secondary=frame.hand[1].thumb=frame.hand[1].menu=true;
    auto pad=motion::Map(frame);
    Check(pad.sThumbLX==-32768 && pad.sThumbLY==32767 && pad.sThumbRX==32767 && pad.sThumbRY==-32768,"axis signs/range");
    Check(pad.bLeftTrigger==128 && pad.bRightTrigger==255 && pad.wButtons==0xf3f0,"Xbox mapping");
    frame.hand[0].active=false;pad=motion::Map(frame);
    Check(pad.sThumbLX==0 && pad.bLeftTrigger==0 && !(pad.wButtons&XINPUT_GAMEPAD_X),"lost hand retained inputs");
    Check(motion::Stick(std::numeric_limits<float>::quiet_NaN())==0 && motion::Trigger(std::numeric_limits<float>::infinity())==0,"nonfinite input");
    XINPUT_GAMEPAD physical{};physical.wButtons=XINPUT_GAMEPAD_A;physical.sThumbLX=-32768;physical.bLeftTrigger=255;
    auto merged=motion::Merge(physical,pad);Check(merged.sThumbLX==-32768 && merged.bLeftTrigger==255 && (merged.wButtons&XINPUT_GAMEPAD_A),"physical input lost");
    motion::Pad bridge;motion::Frame simple{};simple.hand[1].active=true;simple.hand[1].primary=true;
    bridge.Publish(simple,1000);XINPUT_GAMEPAD current{};Check(bridge.Read(1001,current),"pad not active");
    auto packet=bridge.Packet(current);Check(packet==bridge.Packet(current),"unchanged packet advanced");
    XINPUT_KEYSTROKE key{};Check(bridge.Keystroke(key,false) && key.VirtualKey==VK_PAD_A && key.Flags==XINPUT_KEYSTROKE_KEYDOWN,"A down missing");
    Check(!bridge.Keystroke(key,false),"repeat poll duplicated event");
    Check(!bridge.Read(1251,current) && !current.wButtons,"stale held input retained");
    Check(packet!=bridge.Packet(current) && bridge.Keystroke(key,false) && key.Flags==XINPUT_KEYSTROKE_KEYUP,"stale release missing");
    simple.hand[1].stick={1,0};bridge.Publish(simple,1300);
    Check(bridge.Keystroke(key,true) && key.VirtualKey==VK_PAD_A,"right consumption ate face button");
    Check(!bridge.Keystroke(key,true),"right-stick leaked into native keystrokes");
    bridge.Publish({},1301);while(bridge.Keystroke(key,true)) {}
    simple.hand[1].stick={};bridge.Physical(physical);bridge.Publish(simple,1400);
    Check(!bridge.Keystroke(key,false),"physical A duplicated");bridge.Publish({},1401);
    Check(!bridge.Keystroke(key,false),"VR release clobbered physical A");bridge.Physical({});
    // A consumer that stops reading cannot overflow storage or strand a held key.
    for(int i=0;i<300;++i) {simple.hand[1].primary=(i%2)==0;bridge.Publish(simple,1500+i);}
    bridge.Publish({},1900);while(bridge.Keystroke(key,false)) {}
    Check(!bridge.Held(VK_PAD_A),"overflow stranded held A");
    Check((motion::Pad::Mask(motion::Map(frame))&(1u<<23))!=0,"positive X not right");

    // Check tap/hold timing, exclusivity and interrupted gestures through Pad.
    motion::Pad chords; motion::Frame chord{}; chord.hand[0].active=true;
    auto readChord = [&](ULONGLONG time) { chords.Publish(chord,time);
        Check(chords.Read(time,current),"chord bridge inactive"); return current; };
    chord.hand[0].menu=true;
    Check(!readChord(2000).wButtons,"menu press prematurely sent Start");
    Check(!readChord(2200).wButtons && !readChord(2400).wButtons &&
        !readChord(2599).wButtons,"short menu hold sent button");
    Check(readChord(2600).wButtons==XINPUT_GAMEPAD_BACK,"menu hold missing Back");
    chord.hand[0].menu=false;Check(!readChord(2601).wButtons,"hold release also sent Start");
    chord.hand[0].menu=true;readChord(2700);chord.hand[0].menu=false;
    Check(readChord(2800).wButtons==XINPUT_GAMEPAD_START,"menu tap missing Start");
    Check(readChord(2859).wButtons==XINPUT_GAMEPAD_START && !readChord(2860).wButtons,"Start pulse duration");
    chord.hand[0].thumb=true;chord.hand[0].stick={.8f,.9f};
    auto cp=readChord(2900);Check(!cp.wButtons && cp.sThumbLX>0 && cp.sThumbLY>0,
        "clicked park stick generated D-pad or blocked movement");
    chord.hand[0].thumb=false;Check(readChord(2930).wButtons==XINPUT_GAMEPAD_LEFT_THUMB,"tilted tap missing L3");
    chord.hand[0].stick={};
    chord.hand[0].thumb=true;Check(!readChord(2940).wButtons,"stick press prematurely sent L3");
    chord.hand[0].thumb=false;Check(readChord(2950).wButtons==XINPUT_GAMEPAD_START,"stick double click missing pause");
    chord.hand[0].active=false;chords.Publish(chord,2951);
    Check(!chords.Read(2951,current) && !current.wButtons,"lost hand retained tap");
    chord.hand[0].active=true;chord.hand[0].menu=true;readChord(3000);
    Check(!chords.Read(3251,current) && !current.wButtons,"stale chord retained input");
    chord.hand[0].menu=false;Check(!readChord(3252).wButtons,"stale chord release sent Start");
    chord.hand[0].thumb=true;chord.hand[0].stick={-.8f,.9f};
    Check(!readChord(3300).wButtons && !readChord(3500).wButtons && !readChord(3700).wButtons &&
        !readChord(3899).wButtons,"stick hold generated a premature button");
    Check(readChord(3900).wButtons==XINPUT_GAMEPAD_START,"stick hold missing pause at threshold");
    chord.menuNavigation=true;
    cp=readChord(3959);Check(cp.wButtons==XINPUT_GAMEPAD_START && !cp.sThumbLX && !cp.sThumbLY,
        "tilted pause gesture also navigated the new menu");
    Check(!readChord(3960).wButtons && !readChord(4160).wButtons,"held stick repeated pause");
    chord.hand[0].thumb=false;chord.hand[0].stick={};
    Check(!readChord(4170).wButtons,"hold release also sent L3");
    chord.hand[0].thumb=true;readChord(4200);readChord(4400);readChord(4600);
    chord.hand[0].thumb=false;Check(readChord(4800).wButtons==XINPUT_GAMEPAD_START,
        "release sampled at hold threshold was misclassified as a tap");

    // Double click uses real elapsed time and the same Start event path as a
    // hold. Single-click L3 stays immediate; polling cannot duplicate a gesture.
    motion::Pad doubleClick;motion::Frame clicks{};clicks.hand[0].active=true;
    auto clickState = [&](ULONGLONG time, bool down) {
        clicks.hand[0].thumb=down;doubleClick.Publish(clicks,time);
        Check(doubleClick.Read(time,current),"double-click pad inactive");return current.wButtons;
    };
    Check(!clickState(5000,true) && clickState(5040,false)==XINPUT_GAMEPAD_LEFT_THUMB,
        "first click delayed or lost L3");
    Check(clickState(5099,false)==XINPUT_GAMEPAD_LEFT_THUMB && !clickState(5100,false),"L3 pulse duration changed");
    Check(!clickState(5140,true) && !clickState(5150,true),"second press prematurely sent pause");
    Check(clickState(5180,false)==XINPUT_GAMEPAD_START,"second release did not consume L3 for pause");
    Check(clickState(5239,false)==XINPUT_GAMEPAD_START && !clickState(5240,false),"double-click Start pulse duration");
    unsigned startDowns=0,startUps=0;
    while(doubleClick.Keystroke(key,false)) {
        if(key.VirtualKey==VK_PAD_START) {
            startDowns+=(key.Flags==XINPUT_KEYSTROKE_KEYDOWN);
            startUps+=(key.Flags==XINPUT_KEYSTROKE_KEYUP);
        }
    }
    Check(startDowns==1 && startUps==1,"double-click duplicated or stranded native Start event");
    Check(!clickState(5260,true) && clickState(5300,false)==XINPUT_GAMEPAD_LEFT_THUMB,
        "third click reused the consumed pair");
    clickState(5500,false);clickState(5630,true);
    Check(clickState(5650,false)==XINPUT_GAMEPAD_START,"double-click rejected inclusive 350 ms boundary");
    clickState(5800,true);clickState(5840,false);clickState(6000,false);clickState(6170,true);
    Check(clickState(6191,false)==XINPUT_GAMEPAD_LEFT_THUMB,"slow clicks incorrectly opened pause");
    clicks.hand[0].active=false;doubleClick.Publish(clicks,6200);clicks.hand[0].active=true;
    clickState(6210,true);Check(clickState(6250,false)==XINPUT_GAMEPAD_LEFT_THUMB,"disconnect retained first click");
    Check(!doubleClick.Read(6501,current),"stale double-click input remained active");
    clickState(6510,true);Check(clickState(6550,false)==XINPUT_GAMEPAD_LEFT_THUMB,"stale input retained first click");
    clickState(6600,true);clickState(6800,true);clickState(7000,true);
    Check(clickState(7200,true)==XINPUT_GAMEPAD_START && !clickState(7260,true) && !clickState(7300,false),
        "second-click hold repeated pause or emitted L3 on release");
    clickState(7320,true);Check(clickState(7360,false)==XINPUT_GAMEPAD_LEFT_THUMB,"hold retained pending click pair");
    clicks.hand[0].menu=true;clickState(7370,false);clicks.hand[0].menu=false;clickState(7380,false);
    clickState(7500,true);Check(clickState(7540,false)==XINPUT_GAMEPAD_LEFT_THUMB,"menu gesture retained click pair");
    for(const ULONGLONG step : {8ull,11ull,14ull,21ull,42ull}) {
        motion::Mapper sampled;motion::Frame input{};input.hand[0].active=true;
        unsigned pauses=0;bool wasStart=false;
        for(ULONGLONG t=0;t<600;t+=step) {
            input.hand[0].thumb=(t>=42 && t<126)||(t>=210 && t<294);
            const auto sample=sampled.Read(input,10000+t);
            const bool isStart=(sample.wButtons&XINPUT_GAMEPAD_START)!=0;
            pauses+=isStart && !wasStart;wasStart=isStart;
        }
        Check(pauses==1,"double-click depends on rendering refresh rate");
    }

    // The image uses an asymmetric native FOV. Rays from each eye and an
    // offset controller must agree on the same menu UV and surface point.
    const pointer::menu::Screen screen{{{0,0,0,1},{0,1.6f,0}},-.6f,.8f,.4f,-.5f,2.5f};
    const XrVector3f target{.25f,1.7f,-2.5f};
    for (const auto origin : {XrVector3f{-.0315f,1.6f,0},XrVector3f{.0315f,1.6f,0},XrVector3f{.2f,1.3f,-.35f}}) {
        auto direction=pointer::Sub(target,origin);
        const float length=std::sqrt(pointer::Dot(direction,direction));
        direction={direction.x/length,direction.y/length,direction.z/length};
        const auto hit=screen.Intersect({origin,direction});
        Check(hit.valid && std::fabs(hit.u-.5f)<1e-5f && std::fabs(hit.v-.4f)<1e-5f &&
            std::fabs(hit.distance-length)<1e-5f && std::fabs(hit.point.z-target.z)<1e-5f,
            "menu screen UV or endpoint diverged by ray origin");
    }
    Check(!screen.Intersect({{3,1.6f,0},{0,0,-1}}).valid &&
        !screen.Intersect({{0,1.6f,-3},{0,0,1}}).valid,"off-screen or back-facing menu ray hit");

    xr::AlternateEyes cadence;
    Check(cadence.Select(100)==-1,"AER used an uninitialized eye");
    cadence.Seed(100);
    for (uint64_t serial=101; serial<165; ++serial) {
        const int eye=static_cast<int>((serial-101)&1);
        Check(cadence.Select(serial)==eye && cadence.Capture(serial,eye) &&
            cadence.ReusedEye(serial)==(eye^1),"AER eye identity drifted");
    }
    Check(cadence.Select(166)==-1,"AER reused a frame across a render gap");
    cadence.Reset();
    cadence.Seed(180);
    Check(!cadence.Capture(181,1) && cadence.Select(182)==-1,
        "AER accepted a capture for the wrong eye");

    // Main menu uses the already native-verified D-pad focus path. Park analog
    // motion and physical input remain independent.
    motion::Pad navigation;motion::Frame nav{};nav.hand[0].active=true;
    nav.hand[0].stick={.8f,.9f};nav.menuNavigation=true;
    navigation.Publish(nav,3500);navigation.Read(3500,current);
    Check(current.wButtons==(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_RIGHT) &&
        !current.sThumbLX && !current.sThumbLY,"main menu stick leaked analog or missed focus directions");
    nav.hand[0].stick={.4f,.4f};navigation.Publish(nav,3510);navigation.Read(3510,current);
    Check(current.wButtons==(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_RIGHT),"main menu direction hysteresis lost");
    nav.hand[0].stick={};navigation.Publish(nav,3520);navigation.Read(3520,current);
    Check(!current.wButtons,"neutral main menu stick stranded D-pad");
    nav.menuNavigation=false;nav.hand[0].stick={.8f,.9f};navigation.Publish(nav,3530);navigation.Read(3530,current);
    Check(!current.wButtons && current.sThumbLX>0 && current.sThumbLY>0,"entering park lost analog movement");

    motion::LaserControls laser; motion::Frame pointing{};
    pointing.hand[1].active=pointing.hand[1].aimValid=true;
    pointing.hand[1].trigger=.8f;pointing.hand[1].primary=true;pointing.hand[1].thumb=true;
    auto li=laser.Read(pointing,4000,false);
    Check(!li.enabled && motion::Map(li.gamepad).bRightTrigger==204 &&
        (motion::Map(li.gamepad).wButtons&XINPUT_GAMEPAD_A) &&
        (motion::Map(li.gamepad).wButtons&XINPUT_GAMEPAD_RIGHT_THUMB),"absent laser consumed native A/RT/R3");
    li=laser.Read(pointing,4010,true);
    Check(li.enabled && !li.down && motion::Map(li.gamepad).bRightTrigger==204 &&
        !motion::Map(li.gamepad).wButtons,"held A activated newly available laser or zoom was consumed");
    pointing.hand[1].primary=false;pointing.hand[1].thumb=false;laser.Read(pointing,4020,true);
    pointing.hand[1].primary=true;li=laser.Read(pointing,4030,true);
    Check(li.enabled && li.down && motion::Map(li.gamepad).bRightTrigger==204 &&
        (motion::Map(li.gamepad).wButtons&XINPUT_GAMEPAD_A),"laser A did not use native selection or stole RT zoom");
    pointing.hand[1].primary=false;Check(!laser.Read(pointing,4040,true).down,"A release retained laser click");
    pointing.hand[1].trigger=1;li=laser.Read(pointing,4050,true);
    Check(!li.down && motion::Map(li.gamepad).bRightTrigger==255,"RT alone clicked laser or lost zoom");
    pointing.hand[1].thumb=true;laser.Read(pointing,4100,true);
    laser.Read(pointing,4300,true);laser.Read(pointing,4500,true);
    Check(laser.Read(pointing,4699,true).enabled,"laser toggled before hold threshold");
    li=laser.Read(pointing,4700,true);
    Check(!li.enabled && !motion::Map(li.gamepad).wButtons && motion::Map(li.gamepad).bRightTrigger==255,
        "laser hold leaked R3, failed to toggle or consumed zoom");
    pointing.hand[1].primary=true;li=laser.Read(pointing,4710,true);
    Check(!(motion::Map(li.gamepad).wButtons&XINPUT_GAMEPAD_A),"mode switch transferred held laser A to native A");
    pointing.hand[1].thumb=false;pointing.hand[1].primary=false;laser.Read(pointing,4720,true);
    pointing.hand[1].primary=true;li=laser.Read(pointing,4730,true);
    Check(!li.enabled && !li.down && (motion::Map(li.gamepad).wButtons&XINPUT_GAMEPAD_A),"laser-off native A missing");
    pointing.hand[1].primary=false;pointing.hand[1].thumb=true;laser.Read(pointing,4800,true);
    pointing.hand[1].thumb=false;li=laser.Read(pointing,4900,true);
    Check((motion::Map(li.gamepad).wButtons&XINPUT_GAMEPAD_RIGHT_THUMB) &&
        !(motion::Map(laser.Read(pointing,4960,true).gamepad).wButtons&XINPUT_GAMEPAD_RIGHT_THUMB),"short R3 tap missing or stranded");
    pointing.hand[1].thumb=true;laser.Read(pointing,5000,true);
    laser.Read(pointing,5200,true);laser.Read(pointing,5400,true);
    Check(laser.Read(pointing,5600,true).enabled,"second hold didn't restore laser");
    pointing.hand[1].thumb=false;laser.Read(pointing,5610,true);
    pointing.hand[1].primary=true;Check(laser.Read(pointing,5620,true).down,"restored laser A missing");
    pointing.hand[1].active=false;li=laser.Read(pointing,5630,true);
    Check(!li.enabled && !li.down,"tracking loss retained laser press");
    pointing.hand[1].active=true;Check(!laser.Read(pointing,5640,true).down,"tracking recovery transferred held A");
    pointing.hand[1].primary=false;laser.Read(pointing,5650,true);
    pointing.hand[1].primary=true;Check(laser.Read(pointing,5660,true).down,"neutral recovery didn't rearm laser");
    li=laser.Read(pointing,5670,false);
    Check(!li.down && !(motion::Map(li.gamepad).wButtons&XINPUT_GAMEPAD_A) && motion::Map(li.gamepad).bRightTrigger==255,
        "service loss transferred laser A or consumed zoom");
    pointing.hand[1].primary=false;laser.Read(pointing,5680,false);
    pointing.hand[1].primary=true;Check((motion::Map(laser.Read(pointing,5690,false).gamepad).wButtons&XINPUT_GAMEPAD_A),
        "service-loss neutral didn't restore native A");
    pointing.hand[1].primary=false;laser.Read(pointing,5700,true);
    pointing.hand[1].primary=true;laser.Read(pointing,5710,true);
    Check(!laser.Read(pointing,6000,true).down,"stale laser snapshot retained A");
    pointing.hand[1].primary=false;laser.Read(pointing,6010,true);
    pointing.hand[1].thumb=true;laser.Read(pointing,6020,true);
    laser.Read(pointing,6220,true);laser.Read(pointing,6420,true);
    Check(!laser.Read(pointing,6620,true).enabled,"continuous R3 hold failed to toggle");
    laser.Read(pointing,6820,true);laser.Read(pointing,7020,true);
    Check(!laser.Read(pointing,7220,true).enabled,"continuous R3 hold toggled repeatedly");
    pointing.hand[1].thumb=false;
    Check(!motion::Map(laser.Read(pointing,7230,true).gamepad).wButtons,"consumed R3 hold also emitted tap");
    pointing.hand[1].thumb=true;laser.Read(pointing,7240,true);
    pointing.hand[1].aimValid=false;laser.Read(pointing,7250,true);
    pointing.hand[1].thumb=false;pointing.hand[1].aimValid=true;
    Check(!motion::Map(laser.Read(pointing,7260,true).gamepad).wButtons,"tracking interruption emitted pending R3 tap");
    pointing.hand[1].thumb=true;laser.Read(pointing,7300,true);
    laser.Read(pointing,7500,true);laser.Read(pointing,7700,true);
    Check(laser.Read(pointing,7900,true).enabled,"tracking recovery lost preferred laser mode");
    pointing.hand[1].thumb=false;laser.Read(pointing,7910,true);
    pointing.hand[1].primary=true;li=laser.Read(pointing,7920,true);
    XINPUT_GAMEPAD physicalSelect{};physicalSelect.wButtons=XINPUT_GAMEPAD_A;physicalSelect.bRightTrigger=255;
    const auto mergedSelect=motion::Merge(physicalSelect,motion::Map(li.gamepad));
    Check(li.down && (mergedSelect.wButtons&XINPUT_GAMEPAD_A) && mergedSelect.bRightTrigger==255,
        "laser stole physical Xbox A or RT");

    {
        menu::detail::SelectionPanelState mirror;
        Check(!mirror.Read().known,"unknown panel must not permit automatic Back");
        const auto owner=mirror.NewOwner();
        const auto prefix="owner="+std::to_string(owner)+"&seq=";
        Check(mirror.Update(prefix+"1&visible=1&safe=1") && mirror.Read().canClose,"native panel state");
        Check(!mirror.Update(prefix+"2&visible=0&safe=1") && mirror.Read().visible,"invalid panel state accepted");
        Check(!mirror.Update(prefix+"2&visible=1&safe=1&safe=0"),"duplicate panel field accepted");
        Check(mirror.Update(prefix+"3&visible=0&safe=0") && !mirror.Read().visible,"panel close acknowledgement");
        Check(mirror.Update(prefix+"2&visible=1&safe=1") && !mirror.Read().visible,"late update reopened panel");
        mirror.NewOwner();Check(!mirror.Read().known,"new component inherited old state");
        mirror.Update(prefix+"4&visible=1&safe=1");Check(!mirror.Read().known,"old component changed new state");

        motion::Frame f{};f.hand[1].active=f.hand[1].aimValid=true;
        motion::SelectionPanel panel{7,true,true,true};
        motion::LaserControls switching;
        switching.Read(f,10000,true,panel);f.hand[1].primary=true;
        auto mapped=motion::Map(switching.Read(f,10010,true,panel).gamepad);
        Check(mapped.wButtons==XINPUT_GAMEPAD_B,"panel consumed A before Back handoff");
        f.hand[1].primary=false;
        Check(motion::Map(switching.Read(f,10020,true,panel).gamepad).wButtons==XINPUT_GAMEPAD_B,"short A lost pending selection");
        panel.visible=panel.canClose=false;
        Check(!motion::Map(switching.Read(f,10030,true,panel).gamepad).wButtons,"Back release missing before select");
        Check(motion::Map(switching.Read(f,10040,true,panel).gamepad).wButtons==XINPUT_GAMEPAD_A,"acknowledged select missing");
        panel.visible=panel.canClose=true;
        Check(!motion::Map(switching.Read(f,10100,true,panel).gamepad).wButtons,"forwarded A remained held in new panel");
        switching.Read(f,10110,true,panel);f.hand[1].primary=true;
        Check(motion::Map(switching.Read(f,10120,true,panel).gamepad).wButtons==XINPUT_GAMEPAD_B,"second panel switch didn't rearm");
        panel.canClose=false;
        Check(!motion::Map(switching.Read(f,10130,true,panel).gamepad).wButtons,"new modal received pending Back/A");
        f.hand[1].primary=false;switching.Read(f,10140,true,panel);f.hand[1].primary=true;
        Check(motion::Map(switching.Read(f,10150,true,panel).gamepad).wButtons==XINPUT_GAMEPAD_A,"modal lost ordinary confirm");

        for(unsigned loss=0;loss<4;++loss){
            motion::LaserControls c;motion::Frame input{};input.hand[1].active=input.hand[1].aimValid=true;
            panel={7,true,true,true};c.Read(input,20000,true,panel);input.hand[1].primary=true;c.Read(input,20010,true,panel);
            if(loss==0)input.hand[1].aimValid=false;
            if(loss==1)panel.owner=8;
            if(loss==2)panel.known=false;
            const auto result=c.Read(input,20020,loss!=3,panel);
            Check(!(motion::Map(result.gamepad).wButtons&(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B)),"ownership/tracking/pause loss forwarded pending click");
        }
        motion::LaserControls bounded;f={};f.hand[1].active=f.hand[1].aimValid=true;panel={7,true,true,true};
        bounded.Read(f,30000,true,panel);f.hand[1].primary=true;bounded.Read(f,30010,true,panel);
        for(ULONGLONG t=30110;t<=31010;t+=100)bounded.Read(f,t,true,panel);
        Check(!motion::Map(bounded.Read(f,31020,true,panel).gamepad).wButtons,"unacknowledged Back stayed held");

        // A tap may end before the panel-close RPC. The bridge must still carry
        // exactly one ordered Back down/up and Select down/up, without a held
        // input being transferred to the newly opened panel.
        motion::LaserControls routed;motion::Pad transport;f.hand[1].primary=false;
        auto publish=[&](ULONGLONG t){transport.Publish(routed.Read(f,t,true,panel).gamepad,t);};
        publish(40000);f.hand[1].primary=true;publish(40010);f.hand[1].primary=false;publish(40020);
        panel.visible=panel.canClose=false;publish(40030);publish(40040);publish(40100);publish(40110);
        const WORD keys[]={VK_PAD_B,VK_PAD_B,VK_PAD_A,VK_PAD_A};
        for(unsigned i=0;i<4;++i){
            XINPUT_KEYSTROKE e{};Check(transport.Keystroke(e,false) && e.VirtualKey==keys[i] &&
                e.Flags==(i%2?XINPUT_KEYSTROKE_KEYUP:XINPUT_KEYSTROKE_KEYDOWN),"queued panel handoff reordered or lost an edge");
        }
        XINPUT_KEYSTROKE extra{};Check(!transport.Keystroke(extra,false),"panel handoff duplicated an edge");

        motion::LaserControls modal;panel={7,true,true,true};f.hand[1].primary=false;
        modal.Read(f,50000,true,panel);f.hand[1].primary=true;modal.Read(f,50010,true,panel);
        panel.visible=panel.canClose=false;modal.Read(f,50020,true,panel);
        panel.blocked=true;
        Check(!motion::Map(modal.Read(f,50030,true,panel).gamepad).wButtons,"global dialog received queued selection after panel close");
        f.hand[1].primary=false;modal.Read(f,50040,true,panel);f.hand[1].primary=true;
        Check(motion::Map(modal.Read(f,50050,true,panel).gamepad).wButtons==XINPUT_GAMEPAD_A,"global dialog lost normal confirm");
    }

    for (unsigned mode=0;mode<4;++mode) {
        motion::ActionSets sets;attaches=syncs=0;
        const auto gaze=(mode&1) ? Handle<XrActionSet>(4) : XR_NULL_HANDLE;
        const auto controllers=(mode&2) ? Handle<XrActionSet>(5) : XR_NULL_HANDLE;
        Check(sets.Attach(Attach,Handle<XrSession>(2),gaze,controllers),"action attachment failed");
        Check(attaches==(mode ? 1u : 0u),"attachment count");
        Check(!sets.Attach(Attach,Handle<XrSession>(2),gaze,controllers),"duplicate attachment allowed");
        Check(!sets.Sync(Sync,Handle<XrSession>(2),false) && !syncs,"unfocused sync");
        Check(sets.Sync(Sync,Handle<XrSession>(2),true)==(mode!=0),"focused sync");
        Check(syncs==(mode ? 1u : 0u),"sync duplicated");
        syncResult=XR_SESSION_NOT_FOCUSED;Check(!sets.Sync(Sync,Handle<XrSession>(2),true),"NOT_FOCUSED accepted as active");syncResult=XR_SUCCESS;
    }
    motion::ActionSets rejected;attachResult=XR_ERROR_RUNTIME_FAILURE;syncs=0;
    Check(!rejected.Attach(Attach,Handle<XrSession>(2),Handle<XrActionSet>(5),XR_NULL_HANDLE) &&
        !rejected.Sync(Sync,Handle<XrSession>(2),true) && !syncs,"failed attach synchronized");attachResult=XR_SUCCESS;

    motion::Actions actions;
    const motion::Api api{Path,Set,Destroy,Create,Suggest,Space,DestroySpace,Pose,Boolean,Scalar,Vector,Locate};
    Check(actions.Create(api,Handle<XrInstance>(1),Handle<XrSession>(2)) && profiles==5,"profile setup");
    auto xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123456,true);
    Check(xr.hand[0].active && xr.hand[1].active,"neutral controllers didn't arm");pressed=true;
    Check(anchorReads==0,"unused grip pose must not be polled");
    xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123456,true);
    Check(xr.hand[0].active && xr.hand[1].aimValid && locatedTime==123456 && Near(xr.hand[0].stick.y,-.5f),"XR frame/time mapping");
    xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123456,true,true);
    Check(anchorReads==1 && xr.hand[0].controllerPoseValid && !xr.hand[1].controllerPoseValid && Near(xr.hand[0].controllerPose.position.x,-.3f) && locatedTime==123456,"left anchor uses runtime grip pose at the shared predicted time");
    anchorTracking=false;xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123456,true,true);
    Check(!xr.hand[0].controllerPoseValid && xr.hand[0].active && xr.hand[1].aimValid,"lost grip pose cannot disable Xbox/laser aim or retain an anchor");anchorTracking=true;
    const auto before=reads;xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123457,false);
    Check(reads==before && !xr.hand[1].active,"unfocused actions read or held");
    tracking=false;xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123458,true);
    Check(!xr.hand[1].active && !motion::Map(xr).wButtons,"tracking loss retained button");
    tracking=true;xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123459,true);
    Check(!xr.hand[1].active,"reconnect with held buttons activated inputs");pressed=false;
    xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123460,true);
    Check(xr.hand[1].active,"neutral reconnect didn't rearm");
    actions.Destroy();Check(destroyed==1 && spacesDestroyed==3 && !actions.Set(),"action cleanup");
    failure=true;Check(!actions.Create(api,Handle<XrInstance>(1),Handle<XrSession>(2)) && !actions.Set() && destroyed==2,"partial setup failure");
    failure=false;anchorFailure=true;Check(actions.Create(api,Handle<XrInstance>(1),Handle<XrSession>(2)),"optional anchor failure disabled controllers");
    xr=actions.Read(Handle<XrSession>(2),Handle<XrSpace>(3),123461,true,true);Check(xr.hand[0].active && !xr.hand[0].controllerPoseValid,"missing optional anchor retains neutral Xbox input");actions.Destroy();anchorFailure=false;

    const XrPosef plane{{0,0,0,1},{0,0,-2}};
    auto hit=pointer::Quad({{0,0,0},{0,0,-1}},plane,2,1);
    Check(hit.valid && Near(hit.u,.5f) && Near(hit.v,.5f) && Near(hit.distance,2),"centre panel hit");
    hit=pointer::Quad({{.8f,.3f,0},{0,0,-1}},plane,2,1);
    Check(hit.valid && Near(hit.u,.9f) && Near(hit.v,.2f) && Near(hit.point.x,.8f),"panel parallax or vertical flip");
    Check(!pointer::Quad({{0,0,0},{1,0,0}},plane,2,1).valid,"parallel ray accepted");
    Check(!pointer::Quad({{0,0,-3},{0,0,-1}},plane,2,1).valid,"behind hand accepted");
    Check(!pointer::Quad({{2,0,0},{0,0,-1}},plane,2,1).valid,"outside panel accepted");
    ui::PanelSettings settings{};settings.followHmd=true;settings.horizontalDeg=23;settings.heightDeg=-10;
    const XrPosef anchor{{0,0,0,1},{1,1.6f,2}}, head{{0,.38268343f,0,.92387953f},{2,1.7f,1}};
    const auto moved=ui::PanelPose(settings,anchor,head,true);
    const auto d=ui::RotatePanelVector(moved.orientation,{0,0,-1});
    hit=pointer::Quad({head.position,d},moved,1,.6f);
    Check(hit.valid && Near(hit.u,.5f) && Near(hit.v,.5f) && Near(hit.distance,settings.distanceM),"offset/follow/recenter geometry");
    std::puts("PASS mapping, physical merge, gestures, laser policy, mouse ownership/removal, raw packets/metadata/stale presses, releases/overflow, XR tracking and panel geometry");
}
