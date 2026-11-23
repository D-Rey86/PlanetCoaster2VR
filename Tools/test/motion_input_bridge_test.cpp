// Exercise the actual hooks with a fake native XInput producer; never install
// hooks or create an XR/game session in this test process.
#include "../../Loader/src/input_hook.cpp"
#include <cstdio>
#include <cstdlib>

namespace pc2vr::log { void Write(const char*, ...) {} }
namespace {
bool connected=false, eventPending=false;
XINPUT_GAMEPAD physical{};
XINPUT_KEYSTROKE nativeEvent{};
unsigned nativeCalls{};
void Check(bool ok,const char* why) { if(!ok) {std::fprintf(stderr,"%s\n",why);std::exit(1);} }
DWORD WINAPI State(DWORD user, XINPUT_STATE* out) {
    ++nativeCalls;
    if(!out || user>=XUSER_MAX_COUNT)return ERROR_BAD_ARGUMENTS;
    if(!connected)return ERROR_DEVICE_NOT_CONNECTED;
    *out={77,physical};return ERROR_SUCCESS;
}
DWORD WINAPI Capabilities(DWORD user,DWORD flags,XINPUT_CAPABILITIES* out) {
    if(!out || user>=XUSER_MAX_COUNT || (flags!=0 && flags!=XINPUT_FLAG_GAMEPAD))return ERROR_BAD_ARGUMENTS;
    if(!connected)return ERROR_DEVICE_NOT_CONNECTED;
    *out={};out->SubType=99;return ERROR_SUCCESS;
}
DWORD WINAPI Keystroke(DWORD,DWORD,XINPUT_KEYSTROKE* out) {
    if(!out)return ERROR_BAD_ARGUMENTS;
    if(!eventPending)return connected ? ERROR_EMPTY : ERROR_DEVICE_NOT_CONNECTED;
    *out=nativeEvent;eventPending=false;return ERROR_SUCCESS;
}
}
int main() {
    using namespace pc2vr::input;
    g_xinputOriginal=State;g_capsOriginal=Capabilities;g_keystrokeOriginal=Keystroke;
    g_motionAvailable=false;
    XINPUT_STATE state{};XINPUT_CAPABILITIES caps{};
    Check(HookGetState(0,&state)==ERROR_DEVICE_NOT_CONNECTED,"disabled motion invented pad");
    connected=true;physical.sThumbRX=20000;physical.wButtons=XINPUT_GAMEPAD_B;
    Check(HookGetState(0,&state)==ERROR_SUCCESS && state.dwPacketNumber==77 && state.Gamepad.wButtons==physical.wButtons,"disabled physical pass-through");
    SetConsumeRightStick(true);HookGetState(1,&state);
    Check(!state.Gamepad.sThumbRX && TurnAxis()>0,"physical capture/consume changed");
    SetConsumeRightStick(false);connected=false;physical={};
    g_motionAvailable=true;
    pc2vr::motion::Frame frame{};frame.hand[1].active=true;frame.hand[1].primary=true;frame.hand[1].stick={1,0};
    PublishMotion(frame);
    Check(HookCapabilities(0,0,&caps)==ERROR_SUCCESS && caps.SubType==XINPUT_DEVSUBTYPE_GAMEPAD,"VR pad not discoverable");
    Check(HookCapabilities(1,0,&caps)==ERROR_DEVICE_NOT_CONNECTED,"VR duplicated across slots");
    Check(HookCapabilities(0,99,&caps)==ERROR_BAD_ARGUMENTS,"bad flags accepted");
    Check(HookGetState(0,&state)==ERROR_SUCCESS && state.Gamepad.wButtons==XINPUT_GAMEPAD_A && state.Gamepad.sThumbRX==32767,"motion-only state");
    const DWORD packet=state.dwPacketNumber;HookGetState(0,&state);
    Check(state.dwPacketNumber==packet,"same merged packet changed");
    Check(HookGetState(2,&state)==ERROR_DEVICE_NOT_CONNECTED,"motion state in second index");
    SetConsumeRightStick(true);XINPUT_KEYSTROKE key{};
    Check(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS && key.VirtualKey==VK_PAD_A,"motion-only A event");
    Check(HookGetKeystroke(0,0,&key)==ERROR_EMPTY,"motion-only idle keystroke falsely disconnected");
    Check(HookGetKeystroke(1,0,&key)==ERROR_DEVICE_NOT_CONNECTED,"other keystroke slot falsely connected");
    SetConsumeRightStick(false);PublishMotion({});while(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS) {}
    PublishMotion(frame);
    connected=true;physical.wButtons=XINPUT_GAMEPAD_B;physical.sThumbLX=-25000;physical.bLeftTrigger=255;
    HookGetState(0,&state);
    Check(state.dwPacketNumber!=packet && (state.Gamepad.wButtons&(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B))==(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B)
        && state.Gamepad.sThumbLX==-25000 && state.Gamepad.bLeftTrigger==255,"physical+VR merge");
    Check(HookCapabilities(0,0,&caps)==ERROR_SUCCESS && caps.SubType==99,"physical capabilities overwritten");
    SetConsumeRightStick(true);HookGetState(0,&state);
    Check(!state.Gamepad.sThumbRX && TurnAxis()==1,"motion turning bypassed camera ownership");
    Check(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS && key.VirtualKey==VK_PAD_A,"motion A event");
    Check(HookGetKeystroke(0,0,&key)==ERROR_EMPTY,"consumed VR right-stick event reached game");
    nativeEvent={VK_PAD_A,0,XINPUT_KEYSTROKE_KEYUP,0,0};eventPending=true;
    Check(HookGetKeystroke(0,0,&key)==ERROR_EMPTY,"physical release clobbered held motion A");
    nativeEvent={VK_PAD_B,0,XINPUT_KEYSTROKE_KEYDOWN,1,0};eventPending=true;
    Check(HookGetKeystroke(1,0,&key)==ERROR_SUCCESS && key.VirtualKey==VK_PAD_B,"other physical slot event lost");
    // Expiration must also clear camera turning, not remain in the physical cache.
    g_userTurn[1]=g_userTurn[2]=g_userTurn[3]=0; // earlier physical pad at slot 1
    g_motionPad.Publish(frame,GetTickCount64()-1000);
    Check(TurnAxis()==0,"expired VR axis stranded in physical cache");
    Check(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS && key.VirtualKey==VK_PAD_A && (key.Flags&XINPUT_KEYSTROKE_KEYUP),"expiration release");
    PublishMotion({});HookGetState(0,&state);
    Check(state.Gamepad.wButtons==XINPUT_GAMEPAD_B && state.Gamepad.bLeftTrigger==255,"inactive VR erased physical input");
    Check(nativeCalls>5,"fake native path not exercised");
    // Exercise main-menu navigation and pause through the real XInput hooks,
    // including keystrokes, with no physical controller connected.
    g_motionPad=pc2vr::motion::Pad{};connected=false;SetConsumeRightStick(true);
    frame={};frame.hand[0].active=true;frame.menuNavigation=true;frame.hand[0].stick={.9f,0};
    PublishMotion(frame);HookGetState(0,&state);
    Check(!state.Gamepad.sThumbLX && !state.Gamepad.sThumbLY && state.Gamepad.wButtons==XINPUT_GAMEPAD_DPAD_RIGHT,
        "main menu bridge leaked analog motion or missed D-pad");
    Check(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS && key.VirtualKey==VK_PAD_DPAD_RIGHT &&
        key.Flags==XINPUT_KEYSTROKE_KEYDOWN,"main menu D-pad keystroke missing");
    frame.hand[0].stick={};PublishMotion(frame);while(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS) {}
    frame.hand[0].menu=true;PublishMotion(frame);HookGetState(0,&state);
    Check(!state.Gamepad.wButtons,"Menu press prematurely emitted Start/Back");
    frame.hand[0].menu=false;PublishMotion(frame);HookGetState(0,&state);
    Check(state.Gamepad.wButtons==XINPUT_GAMEPAD_START,"short left Menu tap not mapped to pause/Start");
    Check(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS && key.VirtualKey==VK_PAD_START &&
        key.Flags==XINPUT_KEYSTROKE_KEYDOWN,"pause/Start keystroke missing");
    // SteamVR can own the left Menu button. A double stick click must reach
    // both actual XInput hook paths without depending on that button.
    g_motionPad=pc2vr::motion::Pad{};frame={};frame.hand[0].active=true;
    frame.hand[0].thumb=true;PublishMotion(frame);
    frame.hand[0].thumb=false;PublishMotion(frame);HookGetState(0,&state);
    Check(state.Gamepad.wButtons==XINPUT_GAMEPAD_LEFT_THUMB,"double-click first tap lost immediate L3");
    Check(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS && key.VirtualKey==VK_PAD_LTHUMB_PRESS &&
        key.Flags==XINPUT_KEYSTROKE_KEYDOWN,"first L3 keystroke missing");
    frame.hand[0].thumb=true;PublishMotion(frame);HookGetState(0,&state);
    Check(!state.Gamepad.wButtons,"double-click second press emitted Start early");
    frame.hand[0].thumb=false;PublishMotion(frame);HookGetState(0,&state);
    Check(state.Gamepad.wButtons==XINPUT_GAMEPAD_START,"double-click pause missing from XInput state");
    unsigned pauseDowns=0,pauseUps=0;
    while(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS) {
        if(key.VirtualKey==VK_PAD_START) {
            pauseDowns+=(key.Flags==XINPUT_KEYSTROKE_KEYDOWN);
            pauseUps+=(key.Flags==XINPUT_KEYSTROKE_KEYUP);
        }
    }
    Check(pauseDowns==1 && pauseUps==0,"double-click Start keystroke missing or duplicated");
    Sleep(static_cast<DWORD>(pc2vr::motion::Mapper::kTapMs+5));
    PublishMotion(frame);HookGetState(0,&state);
    Check(!state.Gamepad.wButtons,"double-click Start state remained held");
    Check(HookGetKeystroke(0,0,&key)==ERROR_SUCCESS && key.VirtualKey==VK_PAD_START &&
        key.Flags==XINPUT_KEYSTROKE_KEYUP,"double-click Start release missing");
    std::puts("PASS actual XInput hooks: discovery, slot identity, packets, physical coexistence, ownership and expiration");
}
