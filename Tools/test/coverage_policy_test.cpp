// Executes the real XR camera/crop/sizing code with deterministic tracking and
// camera-property consumers. No game, GPU, desktop input or user files touched.
// Build against a preserved xr_runtime.cpp with PC2VR_POLICY_REFERENCE to produce
// an independent binary oracle; compare its records with candidate Off/On.
#ifdef PC2VR_POLICY_EXPANDED_REFERENCE
#define PC2VR_POLICY_REFERENCE
#endif
#include "xr_runtime.cpp"
#include <cstdio>
#include <cstdlib>
namespace cbmath=pc2vr::cbmath;

namespace fixture {
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
}
namespace pc2vr::depth {bool NearestDistance(float&,float&) {return false;}}
namespace pc2vr::cfgredirect {void LogStatus() {}}
namespace pc2vr::controls {Settings Current() {return {};}}
namespace pc2vr::input {
void SetConsumeRightStick(bool) {}
float TurnAxis() {return 0;}
float TiltAxis() {return 0;}
}
namespace pc2vr::ui {PanelSettings Settings() {return {};}}
namespace pc2vr::menu {void DeviceAvailable(ID3D12Device*) {}}
namespace pc2vr::log {void Write(const char*,...) {}}

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
    Require(std::fclose(output)==0,"close oracle output");std::printf("PASS: %u actual XR CPU-path samples; frame-boundary policy requests\n",samples);
}
