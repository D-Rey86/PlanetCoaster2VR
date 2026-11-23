// Actual native adapter with a fake original; no game, hook patch, XR session,
// window or desktop input is created. Compile the real composition shader too.
#include "../../Loader/src/world_pointer.cpp"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

namespace pc2vr::pointer::native {
uintptr_t Address(Landmark){return 0;}
bool VerifyRetail() { return false; }
bool Install(Site,void*,Hook&) { return false; }
}
namespace {
using namespace pc2vr;
void Check(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
void Near(float a,float b,const char* why) {Check(std::fabs(a-b)<2e-3f,why);}
void Near(const cbmath::Vec3& a,const cbmath::Vec3& b,const char* why) {Near(a.x,b.x,why);Near(a.y,b.y,why);Near(a.z,b.z,why);}
cbmath::Vec3 Reference(const XrVector3f& local,float yaw,float body,float tilt) {
    // Independent accepted EngineView mapping: reflection -> yaw -> body tilt.
    const float cy=std::cos(yaw),sy=std::sin(yaw),ct=std::cos(tilt),st=std::sin(tilt);
    const cbmath::Vec3 v{local.x*cy-local.z*sy,local.y,-local.x*sy-local.z*cy};
    const cbmath::Vec3 k{std::cos(body),0,-std::sin(body)};
    const auto cross=cbmath::Cross(k,v);const float dot=cbmath::Dot(k,v);
    return {ct*v.x+st*cross.x+(1-ct)*dot*k.x,
            ct*v.y+st*cross.y+(1-ct)*dot*k.y,
            ct*v.z+st*cross.z+(1-ct)*dot*k.z};
}
void Geometry() {
    for(int i=0;i<120;++i) {
        const float a=i*.047f,b=i*.019f,c=i*.031f;
        const auto q=ui::Multiply(ui::Multiply({0,std::sin(a/2),0,std::cos(a/2)},
            {std::sin(b/2),0,0,std::cos(b/2)}),{0,0,std::sin(c/2),std::cos(c/2)});
        const float yaw=i*.03f-1.5f,body=i*.05f-2.8f,tilt=(i%13)*.2f-1.2f;
        const auto r=ui::RotatePanelVector(q,{1,0,0}),u=ui::RotatePanelVector(q,{0,1,0}),f=ui::RotatePanelVector(q,{0,0,-1});
        cbmath::ParsedView game{};game.pos={120,75,-60};
        game.basis={Reference(r,yaw,body,tilt),Reference(u,yaw,body,tilt),Reference(f,yaw,body,tilt)};
        const XrVector3f head{4,1.7f,-3},offset{.31f,-.24f,-.42f};
        const pointer::Ray ray{{head.x+offset.x,head.y+offset.y,head.z+offset.z},
            ui::RotatePanelVector(q,{0,0,-1})};
        const float scale=1.f+(i%5)*40.f;
        const auto actual=pointer::ToWorld(ray,head,q,game,scale);
        auto expected=Reference(offset,yaw,body,tilt);
        expected={game.pos.x+scale*expected.x,game.pos.y+scale*expected.y,game.pos.z+scale*expected.z};
        Near(actual.origin,expected,"hand origin differs from accepted eye-offset mapping");
        Near(actual.direction,Reference(ray.direction,yaw,body,tilt),"aim differs under lagged camera, roll, recenter, turn or tilt");
        const auto space=pointer::Space(head,q,game,scale);
        const auto restored=space.Point(actual.origin,scale),restoredDirection=space.Vector(actual.direction);
        Near({restored.x,restored.y,restored.z},{ray.origin.x,ray.origin.y,ray.origin.z},"world point inverse differs under camera/head/scale movement");
        Near({restoredDirection.x,restoredDirection.y,restoredDirection.z},{ray.direction.x,ray.direction.y,ray.direction.z},"world direction inverse differs under camera/head movement");
        Near(cbmath::Dot(actual.direction,actual.direction),1,"direction not unit length");
        auto centred=ray;centred.origin=head;
        Near(pointer::ToWorld(centred,head,q,game,scale).origin,game.pos,"head-centred ray not at native camera origin");
    }
}
bool nativeSuccess=true;uint32_t nativeCalls{};
void* expectedManager=reinterpret_cast<void*>(uintptr_t{7});
bool FakeRay(void* manager,uint64_t* camera,float* origin,float* direction,const float* uv) {
    Check(manager==expectedManager && uv[0]==.3f && uv[1]==.7f,"native arguments changed");
    ++nativeCalls;*camera=81;origin[0]=12;direction[0]=13;return nativeSuccess;
}
void Adapter() {
    using namespace pointer::world;
    Check(!Prepare() && !Ready(),"foreign process accepted native hook installation");
    ready.store(true);mouseCaller=0x35c1c0;
    const HWND hwnd=reinterpret_cast<HWND>(uintptr_t{23});
    Frame frame{{{.2f,1.4f,-.5f},{0,0,-1}},{{20,30,40},{0,0,1}},hwnd,1000,true};
    Check(Publish(frame),"valid world frame rejected");
    float origin[4]{1,2,3,41},direction[4]{4,5,6,42};
    Check(!Apply(mouseCaller+1,1000,hwnd,origin,direction) && origin[0]==1,"other ray caller modified");
    Check(!Apply(mouseCaller,1000,nullptr,origin,direction),"unfocused override accepted");
    Check(!Apply(mouseCaller,999,hwnd,origin,direction),"future snapshot accepted");
    Check(!Apply(mouseCaller,1251,hwnd,origin,direction),"stale snapshot accepted");
    Check(Apply(mouseCaller,1250,hwnd,origin,direction),"fresh scoped snapshot rejected");
    Check(origin[0]==20 && origin[1]==30 && origin[2]==40 && origin[3]==41 &&
        direction[0]==0 && direction[2]==1 && direction[3]==42,"xyz replacement or native fourth component wrong");
    Clear();Check(!Apply(mouseCaller,1000,hwnd,origin,direction),"clear retained native override");
    frame.world.direction.x=std::numeric_limits<float>::quiet_NaN();
    Check(!Publish(frame),"nonfinite direction published");
    frame.world.direction={0,0,8};Check(!Publish(frame),"nonunit ray published");
    frame.world.direction={0,0,1};Check(Publish(frame),"frame recovery failed");
    rayHook.original.store(reinterpret_cast<void*>(FakeRay));
    uint64_t camera{};const float uv[4]{.3f,.7f,0,0};
    Check(MouseRay(expectedManager,&camera,origin,direction,uv) && camera==81 && nativeCalls==1 &&
        origin[0]==12 && direction[0]==13,"native success, identity or foreign-caller forwarding changed");
    nativeSuccess=false;Check(!MouseRay(expectedManager,&camera,origin,direction,uv) && nativeCalls==2,"native failure changed");
    frame.world.origin={0,1,2};Check(Publish(frame),"concurrency baseline rejected");
    std::thread writer([&]{for(int i=0;i<20000;++i){auto value=frame;const float x=static_cast<float>(i);
        value.world.origin={x,x+1,x+2};Publish(value);}});
    for(int i=0;i<20000;++i) if(Apply(mouseCaller,1000,hwnd,origin,direction))
        Check(origin[1]==origin[0]+1 && origin[2]==origin[0]+2,"torn cross-thread ray frame");
    writer.join();Clear();
}
uint32_t FakeQuery(void* manager,uint8_t* request){
    Check(manager==expectedManager && request,"query arguments changed");SetLastError(567);return 3;
}
pointer::WorldRay queried{};
uint32_t RoutingQuery(void*,uint8_t* request){
    queried={pointer::world::Load<cbmath::Vec3>(request+0x50),pointer::world::Load<cbmath::Vec3>(request+0x60)};
    if(queried.origin.x==10){
        const auto inv=pointer::world::Load<cbmath::Vec3>(request+0x90),offset=pointer::world::Load<cbmath::Vec3>(request+0x80);
        Near(inv,{1000000,1000000,.01f},"redirected native inverse-segment cache differs");
        Near(offset,{-10000000,-20000000,-.3f},"redirected native origin cache differs");
    }
    SetLastError(567);return 0;
}
void QueryRouting(){
    using namespace pointer::world;
    ready.store(true);Clear();Frame frame{};frame.valid=true;frame.window=reinterpret_cast<HWND>(uintptr_t{23});
    frame.time=1000;frame.world={{10,20,30},{0,0,1}};frame.camera={{67,127,346},{-.6f,0,-.8f}};
    Check(Publish(frame),"controller-query frame rejected");
    uint8_t request[0xa0]{},saved[0xa0]{};float w=41;
    const float length=100;std::memcpy(request+0x70,&length,4);
    auto requestRay=[&](pointer::WorldRay ray){std::memcpy(request+0x50,&ray.origin,12);std::memcpy(request+0x60,&ray.direction,12);
        std::memcpy(request+0x5c,&w,4);std::memcpy(request+0x6c,&w,4);std::memcpy(saved,request,sizeof(saved));};
    requestRay(frame.camera);
    Check(RunQuery(RoutingQuery,nullptr,request,1000,frame.window)==0 && GetLastError()==567,"redirect changed native return/error");
    Near(queried.origin,frame.world.origin,"controller query did not use hand origin");
    Near(queried.direction,frame.world.direction,"controller query did not use hand direction");
    Check(!std::memcmp(saved,request,sizeof(saved)),"controller query changed request or float4 w after return");
    float o[4]{},d[4]{};Apply(mouseCaller,1000,frame.window,o,d);requestRay(frame.world);
    RunQuery(RoutingQuery,nullptr,request,1000,frame.window);Near(queried.origin,frame.world.origin,"mouse query changed");
    auto foreign=frame.camera;foreign.direction={0,-1,0};requestRay(foreign);
    RunQuery(RoutingQuery,nullptr,request,1000,frame.window);Near(queried.direction,foreign.direction,"unrelated ray redirected");
    // Real moving-head regression: the Lua camera transform is newer than the
    // rendered camera. Use its measured pose without broadening ray tolerances.
    const CameraSource source{expectedManager,81};
    Check(Apply(mouseCaller,1000,frame.window,o,d,source),"native camera source not bound");
    float pose[8]{.0021122391f,-.99616390f,.083800986f,.025108730f,67,127,346,1};
    const auto f=ui::RotatePanelVector({pose[0],pose[1],pose[2],pose[3]},{0,0,1});
    const pointer::WorldRay live{{67,127,346},{f.x,f.y,f.z}};
    ObserveCamera(source,pose,1001);requestRay(live);
    RunQuery(RoutingQuery,nullptr,request,1001,frame.window);
    Near(queried.origin,frame.world.origin,"newer native camera rejected during head motion");
    Near(queried.direction,frame.world.direction,"newer native camera did not use hand aim");
    Check(!std::memcmp(saved,request,sizeof(saved)),"moving-head query not restored");
    // Neither a different camera, a foreign ray, another thread, nor a stale
    // lifecycle can borrow that camera observation.
    auto other=live;other.direction.x+=.001f;requestRay(other);
    RunQuery(RoutingQuery,nullptr,request,1001,frame.window);
    Near(queried.direction,other.direction,"native camera loosened exact direction matching");
    pose[0]=pose[1]=pose[2]=0;pose[3]=1;
    ObserveCamera({source.manager,82},pose,1002);requestRay({live.origin,{0,0,1}});
    RunQuery(RoutingQuery,nullptr,request,1002,frame.window);
    Near(queried.origin,live.origin,"foreign camera observation reused");
    requestRay(live);
    std::thread otherThread([&]{RunQuery(RoutingQuery,nullptr,request,1002,frame.window);});otherThread.join();
    Near(queried.origin,live.origin,"camera observation leaked across game threads");
    frame.time=1252;Check(Publish(frame),"fresh render frame rejected");
    RunQuery(RoutingQuery,nullptr,request,1252,frame.window);
    Near(queried.origin,live.origin,"stale native camera observation reused");
    frame.time=1000;Check(Publish(frame),"query frame reset failed");
    requestRay(frame.camera);RunQuery(RoutingQuery,nullptr,request,1251,frame.window);
    Near(queried.origin,frame.camera.origin,"stale query redirected");
    RunQuery(RoutingQuery,nullptr,request,999,frame.window);Near(queried.origin,frame.camera.origin,"future query redirected");
    RunQuery(RoutingQuery,nullptr,request,1000,nullptr);Near(queried.origin,frame.camera.origin,"background query redirected");
    Clear();RunQuery(RoutingQuery,nullptr,request,1000,frame.window);Near(queried.origin,frame.camera.origin,"cleared query redirected");
}
uint32_t transformCalls{};
void* FakeTransform(void* manager,float* pose,uint64_t entity){
    Check(manager==expectedManager && entity==81,"native camera transform arguments changed");
    ++transformCalls;pose[0]=pose[1]=pose[2]=0;pose[3]=1;
    pose[4]=67;pose[5]=127;pose[6]=346;pose[7]=29;SetLastError(891);return pose;
}
void TransformAdapter(){
    using namespace pointer::world;
    transformHook.original.store(reinterpret_cast<void*>(FakeTransform));
    float pose[8]{};Clear();cameraSample={};
    Check(WorldTransform(expectedManager,pose,81)==pose && transformCalls==1 && GetLastError()==891 &&
        pose[7]==29 && !cameraSample.valid,"unrelated native transform forwarding changed");
    Frame frame{};frame.valid=true;frame.window=reinterpret_cast<HWND>(uintptr_t{23});
    frame.time=GetTickCount64();frame.world={{10,20,30},{0,0,1}};Check(Publish(frame),"camera adapter frame rejected");
    float o[4]{},d[4]{};Check(Apply(mouseCaller,frame.time,frame.window,o,d,{expectedManager,81}),"adapter source rejected");
    Check(WorldTransform(expectedManager,pose,81)==pose && transformCalls==2 && GetLastError()==891 &&
        cameraSample.valid && cameraSample.epoch==currentEpoch,"active native camera result not retained");
    Near(cameraSample.ray.origin,{67,127,346},"native camera position changed");
    Near(cameraSample.ray.direction,{0,0,1},"native camera orientation changed");
    const auto epoch=cameraSample.epoch;Clear();
    WorldTransform(expectedManager,pose,81);
    Check(cameraSample.epoch==epoch && currentEpoch!=epoch,"clear rebound native camera");
}
void Hits(){
    using namespace pointer::world;
    ready.store(true);Frame frame{};frame.valid=true;frame.window=reinterpret_cast<HWND>(uintptr_t{23});
    frame.time=1000;frame.world={{10,20,30},{0,0,1}};frame.worldUnitsPerMetre=10;
    Check(Publish(frame),"hit baseline invalid");
    uint8_t request[0xa0]{},records[5*0xa0]{};const uint8_t* data=records;uint32_t capacity=5;
    std::memcpy(request+0x40,&data,sizeof(data));std::memcpy(request+0x48,&capacity,4);
    std::memcpy(request+0x50,&frame.world.origin,12);std::memcpy(request+0x60,&frame.world.direction,12);
    cbmath::Vec3 points[]{{10,20,130},{10,20,50},{10,20,80}};
    for(size_t i=0;i<3;++i)std::memcpy(records+i*0xa0,&points[i],12);
    ObserveQuery(frame,request,3,1000,currentEpoch);auto beam=ReadBeam(frame,1000);
    Check(beam.hit,"matching native hit absent");Near(beam.length,2,"nearest unsorted native hit or world scale wrong");
    Check(!ReadBeam(frame,1251).hit && !ReadBeam(frame,999).hit,"stale/future native hit displayed");
    frame.worldUnitsPerMetre=20;Near(ReadBeam(frame,1000).length,1,"zoom scale not applied to native endpoint");
    auto different=frame;different.world.origin.x+=1;Publish(different);
    auto paired=ReadBeam(different,1000);Check(paired.hit,"moving hand discarded native collision sample");
    Near({paired.local.origin.x,paired.local.origin.y,paired.local.origin.z},{.5f,1.f,1.5f},"queried world ray was moved onto a different hand");
    different.space.origin={1,2,3};different.space.axes={{0,0,1},{0,1,0},{1,0,0}};
    paired=ReadBeam(different,1000);const auto expectedLocal=different.space.Point(frame.world.origin,different.worldUnitsPerMetre);
    Near({paired.local.origin.x,paired.local.origin.y,paired.local.origin.z},{expectedLocal.x,expectedLocal.y,expectedLocal.z},"cached ray did not follow exact new camera transform");
    ObserveQuery(frame,request,0,1000,currentEpoch);Check(!ReadBeam(frame,1000).hit,"native miss retained stale dot");
    ObserveQuery(frame,request,3,1000,currentEpoch);Clear();Check(!ReadBeam(frame,1000).hit,"clear retained hit");
    Publish(frame);const auto oldEpoch=currentEpoch;Clear();Publish(frame);ObserveQuery(frame,request,3,1000,oldEpoch);
    Check(!ReadBeam(frame,1000).hit,"in-flight hit survived reset and same-pose recovery");
    ObserveQuery(frame,request,3,1000,currentEpoch,10);ObserveQuery(frame,request,0,1000,currentEpoch,9);
    Check(ReadBeam(frame,1000).hit,"older query overwrote a newer hit");
    Clear();Publish(frame);
    Publish(frame);ObserveQuery(frame,request,6,1000,currentEpoch);Check(!ReadBeam(frame,1000).hit,"oversized native result buffer accepted");
    points[1]={30,20,50};std::memcpy(records+0xa0,&points[1],12);ObserveQuery(frame,request,3,1000,currentEpoch);
    Near(ReadBeam(frame,1000).length,2.5f,"off-ray record accepted as hit");
    queryHook.original.store(reinterpret_cast<void*>(FakeQuery));queryCaller=0;SetLastError(123);
    Check(PhysicsQuery(expectedManager,request)==3 && GetLastError()==567,"foreign query result/LastError changed");Clear();
}
void Shader(const char* path) {
    std::ifstream file(path);
    const std::string source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    const auto begin=source.find("static const char* kShader = R\"(",source.find("bool CreateComposePipeline("));
    Check(begin!=std::string::npos,"composition shader source missing");
    const auto textBegin=source.find('\n',begin)+1,end=source.find(")\";",textBegin);
    Check(end!=std::string::npos,"shader end missing");
    const auto shader=source.substr(textBegin,end-textBegin);
    for(const char* entry:{"VS","PS"}) {
        Microsoft::WRL::ComPtr<ID3DBlob> code,error;
        const HRESULT hr=D3DCompile(shader.data(),shader.size(),"actual_compose",nullptr,nullptr,entry,
            entry[0]=='V'?"vs_5_0":"ps_5_0",0,0,&code,&error); // same flags as production
        if(FAILED(hr) && error) std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));
        Check(SUCCEEDED(hr),"actual composition shader did not compile");
        if(entry[0]=='P') {
            Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
            Check(SUCCEEDED(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&reflection))),"shader reflection failed");
            auto* constants=reflection->GetConstantBufferByName("C");D3D11_SHADER_BUFFER_DESC buffer{};
            Check(SUCCEEDED(constants->GetDesc(&buffer)) && buffer.Size==256,"composition constant-buffer layout changed");
            D3D11_SHADER_VARIABLE_DESC variable{};
            Check(SUCCEEDED(constants->GetVariableByName("laserOrigin")->GetDesc(&variable)) && variable.StartOffset==208,
                "beam origin ABI differs from CPU constants");
            Check(SUCCEEDED(constants->GetVariableByName("laserDirection")->GetDesc(&variable)) && variable.StartOffset==224,
                "beam direction ABI differs from CPU constants");
            Check(SUCCEEDED(constants->GetVariableByName("markerTan")->GetDesc(&variable)) &&
                variable.StartOffset==240 && variable.Size==12,"marker reprojection ABI differs from CPU constants");
        }
    }
}
}
int main(int argc,char** argv) {
    Geometry();Adapter();Hits();QueryRouting();TransformAdapter();Shader(argc>1 ? argv[1] : "Loader/src/xr_runtime.cpp");
    std::puts("PASS world hand-origin/stereo mapping, native forwarding/scope/focus/staleness, coherent publication and actual eye shader compilation");
}
