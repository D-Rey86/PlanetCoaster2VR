// Executes the real XR camera/crop/sizing code with deterministic tracking and
// camera-property consumers. No game, GPU, desktop input or user files touched.
// Build against a preserved xr_runtime.cpp with PC2VR_POLICY_REFERENCE to produce
// an independent binary oracle; compare its records with candidate Off/On.
#ifdef PC2VR_POLICY_EXPANDED_REFERENCE
#define PC2VR_POLICY_REFERENCE
#endif
#include <windows.h>
namespace fixture {
bool manualClock{},pauseMenu{};
ULONGLONG clockTime{};
ULONGLONG Tick(){return manualClock ? clockTime : ::GetTickCount64();}
}
#define GetTickCount64 fixture::Tick
#include "xr_runtime.cpp"
#undef GetTickCount64
#if __has_include("motion_controls.h")
#include "motion_controls.h"
#define PC2VR_POLICY_MOTION
#endif
#if __has_include("world_pointer.h")
#include "world_pointer.h"
#define PC2VR_POLICY_WORLD_POINTER
#endif
#if __has_include("world_click.h")
#include "world_click.h"
#define PC2VR_POLICY_WORLD_CLICK
#endif
#include <cstdio>
#include <cstdlib>
namespace cbmath=pc2vr::cbmath;

namespace fixture {
#ifdef PC2VR_POLICY_MOTION
pc2vr::motion::Frame injected{},published{};
#endif
bool clickReady{};
bool clickUi{};
int camera{};
float yaw{},pitch{},nativeFov{},rideFov{};
XrPosef head{};
XrView eyes[2]{};
void Require(bool ok,const char* message) {if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
class QueryFailure final : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override {*out=nullptr;return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
} queryFailure;
XrResult XRAPI_PTR LocateSpace(XrSpace,XrSpace,XrTime,XrSpaceLocation* out) {
    out->pose=head;out->locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;return XR_SUCCESS;
}
XrResult XRAPI_PTR LocateViews(XrSession,const XrViewLocateInfo*,XrViewState* state,uint32_t capacity,uint32_t* count,XrView* out) {
    Require(capacity>=2,"two-eye location capacity");*count=2;
    state->viewStateFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
    out[0]=eyes[0];out[1]=eyes[1];return XR_SUCCESS;
}
XrResult XRAPI_PTR Poll(XrInstance,XrEventDataBuffer*) {return XR_EVENT_UNAVAILABLE;}
bool endedOriginal{},endObserved{};
XrResult XRAPI_PTR End(XrSession,const XrFrameEndInfo*) {
#ifndef PC2VR_POLICY_REFERENCE
    endedOriginal=!pc2vr::xr::s.expandedCoverage;endObserved=true;
#endif
    return XR_SUCCESS;
}
constexpr float rad=3.14159265f/180.f;
XrQuaternionf Multiply(XrQuaternionf a,XrQuaternionf b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
XrQuaternionf Euler(float y,float p,float r) {
    y*=rad*.5f;p*=rad*.5f;r*=rad*.5f;
    return Multiply(Multiply({0,std::sin(y),0,std::cos(y)},{std::sin(p),0,0,std::cos(p)}),{0,0,std::sin(r),std::cos(r)});
}
cbmath::Basis CameraBasis(float y,float p) {
    const cbmath::Vec3 f{std::sin(y)*std::cos(p),-std::sin(p),std::cos(y)*std::cos(p)},r{std::cos(y),0,-std::sin(y)};
    return {r,cbmath::Cross(f,r),f};
}
template<class T> void Record(FILE* output,const T& value) {Require(std::fwrite(&value,sizeof(value),1,output)==1,"write oracle record");}
}
namespace pc2vr::camctl {
void Update(const cbmath::Vec3&,const cbmath::Vec3&,float) {}
bool IsLocated() {return fixture::camera==0;}
bool Read(float& y,float& p) {y=fixture::yaw;p=fixture::pitch;return IsLocated();}
bool Write(float y,float p) {fixture::yaw=y;fixture::pitch=p;return true;}
bool OrbitDistance(float&) {return false;}
bool SetEngineFov(float f) {fixture::nativeFov=f;return true;}
void RestoreEngineFov() {}
}
namespace pc2vr::ridecam {
void Update(float,const float*) {}
void Release(bool) {}
bool Located() {return fixture::camera==1;}
bool Write(float,float,float f) {fixture::rideFov=f;return true;}
}
namespace pc2vr::gamestate {
void Update(const float*) {}
bool InPark() {return fixture::camera!=2;}
Ride RideCamera() {return fixture::camera==1?Ride::Guest:Ride::None;}
int CameraModeLocks() {return fixture::camera==1?1:0;}
}
namespace pc2vr::depth {bool NearestDistance(float&,float&) {return false;}}
namespace pc2vr::cfgredirect {void LogStatus() {}}
namespace pc2vr::controls {Settings Current() {return {};}}
namespace pc2vr::input {
#ifdef PC2VR_POLICY_MOTION
void PublishMotion(const motion::Frame& frame) {fixture::published=frame;}
bool MotionAvailable() {return false;}
#endif
void SetConsumeRightStick(bool) {}
float TurnAxis() {return 0;}
float TiltAxis() {return 0;}
}
#ifdef PC2VR_POLICY_MOTION
namespace pc2vr::motion {
bool Actions::Create(const Api&,XrInstance,XrSession) {return false;}
#ifdef PC2VR_POLICY_REFERENCE
Frame Actions::Read(XrSession,XrSpace,XrTime,bool focused) {return focused ? fixture::injected : Frame{};}
#else
Frame Actions::Read(XrSession,XrSpace,XrTime,bool focused,bool) {return focused ? fixture::injected : Frame{};}
#endif
void Actions::Destroy() {}
}
#endif
#ifdef PC2VR_POLICY_WORLD_POINTER
namespace pc2vr::pointer::world {
bool Publish(const Frame&) {return true;}
void Clear() {}
#ifndef PC2VR_SKIP_LASER_INTEGRATION
Beam ReadBeam(const Frame&,ULONGLONG) {return {};}
#endif
}
#endif
#ifdef PC2VR_POLICY_WORLD_CLICK
namespace pc2vr::pointer::click {
bool BindWindow(HWND) {return false;}
bool Available() {return fixture::clickReady;}
bool UiOwnsInput() {return fixture::clickUi;}
bool Publish(bool,ULONGLONG) {return true;}
void Clear() {}
}
#endif
namespace pc2vr::ui {PanelSettings Settings() {return {};}}
namespace pc2vr::menu {
void DeviceAvailable(ID3D12Device*) {} bool PauseVisible(){return fixture::pauseMenu;}
#ifndef PC2VR_POLICY_REFERENCE
motion::SelectionPanel SelectionPanelState(){return {};}
ui::RideLayout RidePanelLayout(){return {};}
ui::RideLayout CameraPanelLayout(){return {};}
ui::RideLayout BuildPanelLayout(){return {};}
ui::RideLayout DashboardLayout(){return {};}
bool PostRideClick(const ui::RideClick&){return false;}
bool PostCameraClick(const ui::RideClick&){return false;}
bool PostBuildClick(const ui::RideClick&){return false;}
bool PostDashboardClick(const ui::RideClick&){return false;}
void BuildAvailability(bool){}
void RideInputSnapshot(bool,uint64_t){}
#endif
}
#ifndef PC2VR_POLICY_REFERENCE
namespace pc2vr::pointer::menu {void Publish(HWND,POINT,bool,int,ULONGLONG){} void Clear(){}}
// This fixture exercises CPU camera policy. AFW GPU execution is independently
// covered by afw_warp_test; fail if the CPU fixture unexpectedly reaches it.
namespace pc2vr::afw {
void SetEnabled(bool){} void InvalidateHistory(){}
bool Prepare(ID3D12Device*,const D3D12_RESOURCE_DESC&,const Frame&,bool){
    fixture::Require(false,"CPU policy test reached AFW GPU allocation");return false;
}
ID3D12Resource* Record(ID3D12GraphicsCommandList*,unsigned,int,uint64_t,const Frame&,ID3D12Resource*,ID3D12Resource*,DXGI_FORMAT){
    fixture::Require(false,"CPU policy test reached AFW GPU recording");return nullptr;
}
}
#endif
namespace pc2vr::log {void Write(const char*,...) {}}

#if defined(PC2VR_POLICY_WORLD_CLICK) && !defined(PC2VR_SKIP_LASER_INTEGRATION)
namespace fixture {
XrResult XRAPI_PTR Attach(XrSession,const XrSessionActionSetsAttachInfo*){return XR_SUCCESS;}
XrResult XRAPI_PTR Sync(XrSession,const XrActionsSyncInfo*){return XR_SUCCESS;}
void LaserIntegration(){
    using namespace pc2vr::xr;
    camera=0;s=State{};s.worldLaser=s.worldEligible=true;s.sessionState=XR_SESSION_STATE_FOCUSED;
    manualClock=clickReady=true;clockTime=1000;injected={};
    injected.hand[0].active=injected.hand[1].active=injected.hand[1].aimValid=true;
    injected.hand[0].trigger=.7f;
    g.xrSyncActions=Sync;
    Require(s.actionSets.Attach(Attach,XR_NULL_HANDLE,XR_NULL_HANDLE,reinterpret_cast<XrActionSet>(uintptr_t{1})),"action fixture attach failed");
    SyncActions();Require(s.laserInput.enabled && published.hand[0].trigger==.7f,"laser changed left-controller input");
    clockTime=1010;injected.hand[1].primary=true;injected.hand[1].trigger=.9f;SyncActions();
    Require(s.laserInput.down && published.hand[1].primary && published.hand[1].trigger==.9f,"native A selection or RT zoom consumed");
    clockTime=1020;injected.hand[1].primary=false;injected.hand[1].trigger=0;injected.hand[1].thumb=true;SyncActions();
    clockTime=1220;SyncActions();clockTime=1420;SyncActions();clockTime=1620;SyncActions();
    Require(!s.laserInput.enabled && !published.hand[1].thumb,"held R3 did not toggle laser or leaked R3");
    clockTime=1630;injected.hand[1].thumb=false;SyncActions();
    clockTime=1640;injected.hand[1].trigger=.8f;SyncActions();
    Require(!s.laserInput.enabled && published.hand[1].trigger==.8f,"laser-off did not restore Xbox RT");
    clockTime=1650;injected.hand[1].trigger=0;SyncActions();
    clockTime=1660;injected.hand[1].thumb=true;SyncActions();clockTime=1860;SyncActions();clockTime=2060;SyncActions();clockTime=2260;SyncActions();
    clockTime=2270;injected.hand[1].thumb=false;SyncActions();
    Require(s.laserInput.enabled,"held R3 failed to restore laser");
    clockTime=2280;injected.hand[1].primary=true;injected.hand[1].trigger=.9f;SyncActions();
    s.worldEligible=false;clickReady=false;clockTime=2290;SyncActions();
    Require(!s.laserInput.enabled && !published.hand[1].primary && published.hand[1].trigger==.9f,"view loss transferred held A or consumed RT zoom");
    clockTime=2300;injected.hand[1].primary=false;injected.hand[1].trigger=0;SyncActions();
    camera=2;clockTime=2310;injected.hand[0].stick={.8f,.9f};SyncActions();
    Require(published.menuNavigation,"main menu not routed to directional navigation");
    pc2vr::motion::Mapper mapper;auto menuPad=mapper.Read(published,clockTime);
    Require(menuPad.wButtons==(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_RIGHT) && !menuPad.sThumbLX && !menuPad.sThumbLY,"main menu published analog or missed D-pad");
    camera=0;clockTime=2320;SyncActions();auto parkPad=mapper.Read(published,clockTime);
    Require(!published.menuNavigation && !parkPad.wButtons && parkPad.sThumbLX>0 && parkPad.sThumbLY>0,"park transition lost analog motion");
    clockTime=2330;s.worldEligible=true;clickReady=false;clickUi=true;injected.hand[0].stick={};SyncActions();
    clockTime=2340;injected.hand[1].primary=true;SyncActions();
    Require(s.laserInput.enabled && published.hand[1].primary,"desktop hover hid the laser or native A");
    clockTime=2350;clickUi=false;SyncActions();
    Require(s.laserInput.down && published.hand[1].primary,"desktop hover interrupted native A hold");
    pauseMenu=true;clockTime=2360;injected.hand[1].primary=false;injected.hand[0].stick={0,-.9f};SyncActions();
    const auto pausePad=mapper.Read(published,clockTime);
    Require(published.menuNavigation && (pausePad.wButtons&XINPUT_GAMEPAD_DPAD_DOWN) && !pausePad.sThumbLY,"pause visibility did not route thumbstick to D-pad");
    clockTime=2370;injected.hand[1].primary=true;SyncActions();
    Require(published.hand[1].primary && !s.laserInput.enabled && !s.worldEligible,"pause menu lost A or retained world pointer ownership");
    pauseMenu=false;s.worldEligible=true;clockTime=2380;SyncActions();
    Require(!s.laserInput.down,"leaving pause transferred held A to a world click");
    clickUi=false;manualClock=clickReady=false;injected={};
}
}
#endif

int main(int argc,char** argv) {
    using namespace pc2vr::xr;
    using fixture::Require;
    Require(argc==4,"usage: expanded0/1 ceilingDeg output.bin");
    const bool expanded=std::atoi(argv[1])==1;const float ceiling=static_cast<float>(std::atoi(argv[2]))*3.14159265f/180.f;
    (void)expanded;
    FILE* output{};Require(fopen_s(&output,argv[3],"wb")==0,"open test-owned output");
    g.xrLocateSpace=fixture::LocateSpace;g.xrLocateViews=fixture::LocateViews;g.xrPollEvent=fixture::Poll;g.xrEndFrame=fixture::End;
    const float pose[][3]{{0,0,0},{-25,-10,25},{30,15,-30},{0,0,45}};
    unsigned samples{};
    for(int profile=0;profile<4;++profile)for(int camera=0;camera<3;++camera)for(const auto& rotation:pose) {
        s=State{};s.phase=Phase::Ready;s.recW=profile==0?3072:720;s.recH=profile==0?3264:778;
        SetStereo(true,1.f);SetResolution(1.f,1.f);
#if !defined(PC2VR_POLICY_REFERENCE) || defined(PC2VR_POLICY_EXPANDED_REFERENCE)
        s.recEyeW[0]=s.recW;s.recEyeW[1]=profile==2?800:s.recW;
        s.recEyeH[0]=s.recH;s.recEyeH[1]=profile==2?864:s.recH;
#endif
#ifndef PC2VR_POLICY_REFERENCE
        SetEngineFov(true,ceiling);RequestExpandedCoverage(expanded);
        OnPresent(nullptr,nullptr);Require(s.expandedCoverage==expanded,"request applied before next XR frame");
        Require(!s.eyeFovValid,"mode transition invalidates geometry until next locate");
#else
        SetEngineFov(true,ceiling);
#endif
        fixture::camera=camera;fixture::yaw=0;fixture::pitch=.3f;fixture::nativeFov=fixture::rideFov=0;
        fixture::head={fixture::Euler(rotation[0],rotation[1],rotation[2]),{.02f,1.7f,-.03f}};
        for(int eye=0;eye<2;++eye) {
            const XrQuaternionf cant=profile==2?fixture::Euler(eye==0?10.f:-10.f,5.f,eye==0?-8.f:8.f):
                profile==3?fixture::Euler(eye==0?10.f:-10.f,0,0):XrQuaternionf{0,0,0,1};
            fixture::eyes[eye]={XR_TYPE_VIEW};fixture::eyes[eye].pose=fixture::head;
            fixture::eyes[eye].pose.orientation=fixture::Multiply(fixture::head.orientation,cant);
            fixture::eyes[eye].pose.position.x+=eye==0?-.032f:.032f;
            fixture::eyes[eye].fov=profile==0?XrFovf{(eye==0?-54.5f:-39.9f)*fixture::rad,(eye==0?39.9f:54.5f)*fixture::rad,52.6f*fixture::rad,-52.6f*fixture::rad}:
                profile==3?XrFovf{(eye==0?-75.f:-55.f)*fixture::rad,(eye==0?55.f:75.f)*fixture::rad,50.f*fixture::rad,-50.f*fixture::rad}:
                XrFovf{(eye==0?-54.f:-40.f)*fixture::rad,(eye==0?40.f:54.f)*fixture::rad,44.f*fixture::rad,-55.f*fixture::rad};
        }
        for(int frame=0;frame<6;++frame) {
            s.frameBegun=true;s.shouldRender=true;s.poseAcquired=false;s.displayTime=1000000000LL+frame*11111111LL;
            cbmath::ParsedView game{};game.basis=fixture::CameraBasis(fixture::yaw,fixture::pitch);game.pos={10,20,30};
            const float fov=fixture::nativeFov>0?fixture::nativeFov:fixture::rideFov>0?fixture::rideFov:1.8f;
            const float up=std::tan(fov*.5f),aspect=s.targetH?static_cast<float>(s.targetW)/s.targetH:3920.f/3656.f;
            game.fov={-up*aspect,up*aspect,up,-up};RenderView view{};
            Require(AcquireRenderView(game,view),"real camera path locates both eyes");
            fixture::Record(output,s.eyeFovHead);fixture::Record(output,s.fovUnion);fixture::Record(output,view);
            fixture::Record(output,s.submitPose);fixture::Record(output,s.submitFov);fixture::Record(output,s.eyeWorld);
            fixture::Record(output,fixture::nativeFov);fixture::Record(output,fixture::rideFov);
            s.lastSizeCheck=0;UpdateRenderSize(reinterpret_cast<IDXGISwapChain*>(&fixture::queryFailure));
            fixture::Record(output,s.targetW);fixture::Record(output,s.targetH);
            s.srcW=s.targetW;s.srcH=s.targetH;
            for(int eye=0;eye<2;++eye) {
                const auto eyeOutput=EyeOutputFor(eye,true);XrCompositionLayerProjectionView projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                projection.pose=eyeOutput.pose;projection.fov=eyeOutput.fov;
                projection.subImage.imageRect.extent={static_cast<int32_t>(s.srcW),static_cast<int32_t>(s.srcH)};
                if(eyeOutput.crop)CropToEye(eye,projection,eyeOutput.fov);
                fixture::Record(output,projection.pose);fixture::Record(output,projection.fov);fixture::Record(output,projection.subImage.imageRect);
            }
            s.copyFence.Attach(reinterpret_cast<ID3D12Fence*>(&fixture::queryFailure));uint32_t pw{},ph{};
            PanelPixels(55,s.srcW,s.srcH,pw,ph);s.copyFence.Detach();fixture::Record(output,pw);fixture::Record(output,ph);++samples;
        }
    }
#ifndef PC2VR_POLICY_REFERENCE
    // Requests coalesce, remain pending until the completed-frame boundary, and
    // invalidate sizing only on a real policy change. No user INI/GPU used.
    s.frameBegun=false;s.running=false;s.poseAcquired=true;s.eyeFovValid=true;s.lastSizeCheck=123;
    s.expandedCoverage=false;RequestExpandedCoverage(true);RequestExpandedCoverage(false);
    Require(!s.expandedCoverage && s.eyeFovValid,"requests do not mutate an acquired eye pair");
    OnPresent(nullptr,nullptr);Require(!s.expandedCoverage && s.eyeFovValid && s.lastSizeCheck==123,"coalesced same mode avoids invalidation");
    RequestExpandedCoverage(true);Require(!s.expandedCoverage,"pending mode preserves current frame");
    OnPresent(nullptr,nullptr);Require(s.expandedCoverage && !s.eyeFovValid && s.lastSizeCheck==0,"real transition invalidates old bounds and sizing");
    RequestExpandedCoverage(false);OnPresent(nullptr,nullptr);Require(!s.expandedCoverage,"live return to original policy");
    s.frameBegun=true;s.shouldRender=false;fixture::endObserved=false;RequestExpandedCoverage(true);
    OnPresent(nullptr,nullptr);Require(fixture::endObserved && fixture::endedOriginal && s.expandedCoverage,"old XR frame ends before new policy applies");
#endif
#ifdef PC2VR_POLICY_WORLD_POINTER
    // Exercise the actual eligibility path, independently of the byte oracle:
    // tracked park only, never the accepted Guest view or a fixed camera.
    for(int camera=0;camera<3;++camera) {
        s=State{};s.worldLaser=true;s.stereo=true;s.sessionState=XR_SESSION_STATE_FOCUSED;
#ifdef PC2VR_POLICY_WORLD_CLICK
        s.laserInput.enabled=true;
#endif
        s.gameWindow=reinterpret_cast<HWND>(uintptr_t{23});
        s.motionFrame.hand[1].active=s.motionFrame.hand[1].aimValid=true;
        s.motionFrame.hand[1].aim={{0,0,0,1},{.3f,1.4f,-.4f}};
        fixture::camera=camera;fixture::yaw=0;fixture::pitch=.3f;
        fixture::head={{0,0,0,1},{0,1.7f,0}};
        for(int frame=0;frame<6;++frame) {
            s.frameBegun=s.shouldRender=true;s.poseAcquired=false;s.displayTime=1000000000LL+frame*11111111LL;
            cbmath::ParsedView game{};game.basis=fixture::CameraBasis(fixture::yaw,fixture::pitch);
            game.pos={10,20,30};game.fov={-1,1,1,-1};RenderView out{};
            Require(AcquireRenderView(game,out),"world eligibility camera update failed");
        }
        Require(s.worldPointer.valid==(camera==0),"world laser escaped tracked park camera ownership");
        s.sessionState=XR_SESSION_STATE_VISIBLE;s.poseAcquired=false;
        cbmath::ParsedView game{};game.basis=fixture::CameraBasis(fixture::yaw,fixture::pitch);
        game.fov={-1,1,1,-1};RenderView out{};
        Require(AcquireRenderView(game,out) && !s.worldPointer.valid,"world laser survived XR focus loss");
    }
#endif
#if defined(PC2VR_POLICY_WORLD_CLICK) && !defined(PC2VR_SKIP_LASER_INTEGRATION)
    fixture::LaserIntegration();
#endif
    Require(std::fclose(output)==0,"close oracle output");std::printf("PASS: %u actual XR CPU-path samples; frame-boundary policy requests\n",samples);
}
