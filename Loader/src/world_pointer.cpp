#include "world_pointer.h"
#include "motion_controls.h"
#include "motion_trace.h"
#include "native_input_hook.h"
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace pc2vr::pointer::world {
namespace {
using RayFn=bool (*)(void*,uint64_t*,float*,float*,const float*);
native::Hook rayHook;
native::Hook queryHook;
native::Hook transformHook;
SRWLOCK frameLock=SRWLOCK_INIT;
Frame current{};
Frame emitted{};
uint64_t currentEpoch{};
uint64_t emittedSerial{};
struct CameraSource { void* manager{};uint64_t entity{}; };
std::atomic<void*> cameraManager{};
std::atomic<uint64_t> cameraEntity{};
// The Lua picker reads a newer native camera than the GPU frame. Keep that
// existing accessor's result on its calling thread, paired with the lifecycle.
struct CameraSample {WorldRay ray{};CameraSource source{};ULONGLONG time{};uint64_t epoch{};bool valid{};};
thread_local CameraSample cameraSample;
struct NativeHit {WorldRay ray{};cbmath::Vec3 point{};HWND window{};ULONGLONG time{};uint64_t serial{};bool observed{},valid{};} hit;
std::once_flag setup;
std::atomic<bool> ready{};
uintptr_t mouseCaller{};
uintptr_t queryCaller{};
constexpr ULONGLONG kStaleMs=250;

bool Valid(const Frame& f) {
    const float lengthSquared=cbmath::Dot(f.world.direction,f.world.direction);
    return f.valid && f.window && Finite(f.world.origin) && Finite(f.world.direction) &&
        std::isfinite(f.local.origin.x) && std::isfinite(f.local.origin.y) && std::isfinite(f.local.origin.z) &&
        std::isfinite(f.local.direction.x) && std::isfinite(f.local.direction.y) && std::isfinite(f.local.direction.z) &&
        lengthSquared>.99f && lengthSquared<1.01f && std::isfinite(f.worldUnitsPerMetre) && f.worldUnitsPerMetre>0;
}
bool Matches(const WorldRay& a,const WorldRay& b) {
    const auto close=[](float x,float y,float tolerance){return std::fabs(x-y)<=tolerance;};
    return close(a.origin.x,b.origin.x,.001f) && close(a.origin.y,b.origin.y,.001f) && close(a.origin.z,b.origin.z,.001f) &&
        close(a.direction.x,b.direction.x,.00001f) && close(a.direction.y,b.direction.y,.00001f) && close(a.direction.z,b.direction.z,.00001f);
}
template<class T> T Load(const uint8_t* p){T value;std::memcpy(&value,p,sizeof(value));return value;}
void ObserveQuery(const Frame& before,const uint8_t* request,uint32_t count,ULONGLONG now,uint64_t observedEpoch,uint64_t serial=0) {
    const WorldRay ray{Load<cbmath::Vec3>(request+0x50),Load<cbmath::Vec3>(request+0x60)};
    if(!before.valid || !Matches(before.world,ray))return;
    NativeHit next{};next.ray=ray;next.window=before.window;next.time=now;next.serial=serial;next.observed=true;
    const auto capacity=Load<uint32_t>(request+0x48);
    const auto* records=Load<const uint8_t*>(request+0x40);
    float nearest=INFINITY;
    // Retail Lua RayCast caps its fixed native result buffer at five records.
    // Each record's leading float4 is the measured world collision position.
    if(records && count<=capacity && capacity<=5)for(uint32_t i=0;i<count;++i){
        const auto point=Load<cbmath::Vec3>(records+i*0xa0);
        if(!Finite(point))continue;
        const cbmath::Vec3 delta{point.x-ray.origin.x,point.y-ray.origin.y,point.z-ray.origin.z};
        const float distance=cbmath::Dot(delta,ray.direction);
        const cbmath::Vec3 error{delta.x-distance*ray.direction.x,delta.y-distance*ray.direction.y,delta.z-distance*ray.direction.z};
        if(distance>0 && distance<nearest && cbmath::Dot(error,error)<=.0001f){nearest=distance;next.point=point;next.valid=true;}
    }
    AcquireSRWLockExclusive(&frameLock);
    if(current.valid && current.window==before.window && currentEpoch==observedEpoch && serial>=hit.serial)hit=next;
    ReleaseSRWLockExclusive(&frameLock);
}
using QueryFn=uint32_t(*)(void*,uint8_t*);
bool QueryFrame(const WorldRay& ray,ULONGLONG now,HWND foreground,Frame& before,uint64_t& epoch,uint64_t& serial) {
    AcquireSRWLockExclusive(&frameLock);
    // Controller picking uses the exact native camera ray; mouse picking uses
    // the ray emitted by GetMouseRay. Both belong to the same scoped Lua query.
    // Other rays, camera modes, stale poses and background windows pass through.
    const bool nativeCamera=cameraSample.valid && cameraSample.epoch==currentEpoch &&
        cameraSample.source.manager==cameraManager.load(std::memory_order_relaxed) &&
        cameraSample.source.entity==cameraEntity.load(std::memory_order_relaxed) &&
        now>=cameraSample.time && now-cameraSample.time<=kStaleMs && Matches(cameraSample.ray,ray);
    const bool camera=current.valid && (nativeCamera ||
        (cbmath::Dot(current.camera.direction,current.camera.direction)>.99f && Matches(current.camera,ray)));
    before=camera ? current : emitted;
    const bool valid=before.valid && before.window==foreground && now>=before.time && now-before.time<=kStaleMs &&
        (camera || Matches(before.world,ray));
    epoch=currentEpoch;serial=valid ? ++emittedSerial : 0;
    ReleaseSRWLockExclusive(&frameLock);
    return valid;
}
uint32_t RunQuery(QueryFn original,void* manager,uint8_t* request,ULONGLONG now,HWND foreground) {
    const WorldRay saved{Load<cbmath::Vec3>(request+0x50),Load<cbmath::Vec3>(request+0x60)};
    Frame before;uint64_t epoch,serial;
    if(!QueryFrame(saved,now,foreground,before,epoch,serial)){
#if defined(PC2VR_MOTION_INPUT_TRACE)
        const uint32_t count=original(manager,request);const DWORD error=GetLastError();
        motiontrace::Query(false,count,now,before.time,before.valid,saved.origin,saved.direction,before.world.origin,before.world.direction);
        SetLastError(error);return count;
#else
        return original(manager,request);
#endif
    }
    const float length=Load<float>(request+0x70);
    if(!std::isfinite(length) || length<=0)return original(manager,request);
    const cbmath::Vec3 savedOffset=Load<cbmath::Vec3>(request+0x80),savedInverse=Load<cbmath::Vec3>(request+0x90);
    // Retail RayCast builds its broad-phase slab cache from the full segment
    // delta, with signed 1e-6 clamping (1aac8f7..1aac91d). Keep that cache paired
    // with the redirected ray while preserving the caller's segment length.
    const auto inverse=[length](float direction){const float delta=direction*length;
        return 1.f/std::copysign(std::max(std::fabs(delta),1.e-6f),delta);};
    const cbmath::Vec3 inv{inverse(before.world.direction.x),inverse(before.world.direction.y),inverse(before.world.direction.z)};
    const cbmath::Vec3 offset{-before.world.origin.x*inv.x,-before.world.origin.y*inv.y,-before.world.origin.z*inv.z};
    // Preserve native float4 w components and restore the request before its
    // Lua caller consumes it. Only this existing query's xyz are redirected.
    std::memcpy(request+0x50,&before.world.origin,12);
    std::memcpy(request+0x60,&before.world.direction,12);
    std::memcpy(request+0x80,&offset,12);
    std::memcpy(request+0x90,&inv,12);
    const uint32_t count=original(manager,request);const DWORD error=GetLastError();
#if defined(PC2VR_MOTION_INPUT_TRACE)
    motiontrace::Query(true,count,now,before.time,before.valid,before.world.origin,before.world.direction,before.world.origin,before.world.direction);
#endif
    ObserveQuery(before,request,count,GetTickCount64(),epoch,serial);
    std::memcpy(request+0x50,&saved.origin,12);
    std::memcpy(request+0x60,&saved.direction,12);
    std::memcpy(request+0x80,&savedOffset,12);
    std::memcpy(request+0x90,&savedInverse,12);
    SetLastError(error);return count;
}
uint32_t PhysicsQuery(void* manager,uint8_t* request) {
    const auto original=reinterpret_cast<QueryFn>(queryHook.original.load(std::memory_order_acquire));
    if(reinterpret_cast<uintptr_t>(_ReturnAddress())!=queryCaller)return original(manager,request);
    return RunQuery(original,manager,request,GetTickCount64(),GetForegroundWindow());
}
bool Apply(uintptr_t caller, ULONGLONG now, HWND foreground, float* origin, float* direction,CameraSource source={}) {
    if (caller!=mouseCaller) return false;
    Frame frame;
    AcquireSRWLockExclusive(&frameLock);frame=current;
    const bool valid=frame.valid && foreground==frame.window && now>=frame.time && now-frame.time<=kStaleMs;
    if(valid){
        emitted=frame;++emittedSerial;
        if(source.manager && source.entity){
            cameraEntity.store(source.entity,std::memory_order_relaxed);
            cameraManager.store(source.manager,std::memory_order_release);
        }
    }
    ReleaseSRWLockExclusive(&frameLock);
    if (!valid) return false;
    // Native output slots are float4. Preserve their fourth component and the
    // native camera identity/success result; only the measured xyz are replaced.
    std::memcpy(origin,&frame.world.origin,sizeof(frame.world.origin));
    std::memcpy(direction,&frame.world.direction,sizeof(frame.world.direction));
    return true;
}
bool MouseRay(void* manager,uint64_t* camera,float* origin,float* direction,const float* uv) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto original=reinterpret_cast<RayFn>(rayHook.original.load(std::memory_order_acquire));
    const bool ok=original(manager,camera,origin,direction,uv);
    if (ok && caller==mouseCaller) {
        // Retail 19d403 loads this transform-system receiver before resolving
        // the camera entity returned by the same successful native getter.
        const CameraSource source{Load<void*>(static_cast<const uint8_t*>(manager)+0x2c8),*camera};
        Apply(caller,GetTickCount64(),GetForegroundWindow(),origin,direction,source);
    }
    return ok;
}
void ObserveCamera(CameraSource source,const float* pose,ULONGLONG now) {
    if(source.manager!=cameraManager.load(std::memory_order_acquire) ||
        source.entity!=cameraEntity.load(std::memory_order_relaxed))return;
    // Retail 315620 returns quaternion xyzw followed by position float4.
    // Its +Z axis is exactly the camera-forward vector consumed by Lua RayCast.
    const XrQuaternionf q{pose[0],pose[1],pose[2],pose[3]};
    const auto forward=ui::RotatePanelVector(q,{0,0,1});
    const WorldRay ray{{pose[4],pose[5],pose[6]},{forward.x,forward.y,forward.z}};
    const float norm=cbmath::Dot(ray.direction,ray.direction);
    if(!Finite(ray.origin) || !Finite(ray.direction) || norm<.99f || norm>1.01f)return;
    AcquireSRWLockShared(&frameLock);
    if(current.valid && source.manager==cameraManager.load(std::memory_order_relaxed) &&
        source.entity==cameraEntity.load(std::memory_order_relaxed))
        cameraSample={ray,source,now,currentEpoch,true};
    ReleaseSRWLockShared(&frameLock);
}
using TransformFn=void*(*)(void*,float*,uint64_t);
void* WorldTransform(void* manager,float* pose,uint64_t entity) {
    const auto original=reinterpret_cast<TransformFn>(transformHook.original.load(std::memory_order_acquire));
    // Most transforms are unrelated: two cached identity checks, no lock/search.
    if(manager!=cameraManager.load(std::memory_order_acquire) || entity!=cameraEntity.load(std::memory_order_relaxed))
        return original(manager,pose,entity);
    void* result=original(manager,pose,entity);const DWORD error=GetLastError();
    ObserveCamera({manager,entity},pose,GetTickCount64());
    SetLastError(error);return result;
}
} // namespace
bool Prepare() {
    std::call_once(setup,[]{
        if (!native::VerifyRetail()) return;
        mouseCaller=native::Address(native::Landmark::MouseRayCaller);
        queryCaller=native::Address(native::Landmark::QueryCaller);
        ready.store(native::Install(native::Site::WorldTransform,reinterpret_cast<void*>(WorldTransform),transformHook) &&
            native::Install(native::Site::MouseRay,reinterpret_cast<void*>(MouseRay),rayHook) &&
            native::Install(native::Site::PhysicsQuery,reinterpret_cast<void*>(PhysicsQuery),queryHook),std::memory_order_release);
    });
    return Ready();
}
bool Ready() { return ready.load(std::memory_order_acquire); }
bool Publish(const Frame& frame) {
    const bool valid=Ready() && Valid(frame);
    AcquireSRWLockExclusive(&frameLock);
    if(!valid || !current.valid || current.window!=frame.window){
        ++currentEpoch;hit={};emitted={};cameraManager.store(nullptr,std::memory_order_release);
    }
    current=valid ? frame : Frame{};
    ReleaseSRWLockExclusive(&frameLock);
    return valid;
}
void Clear() {
    AcquireSRWLockExclusive(&frameLock);++currentEpoch;current={};emitted={};hit={};
    cameraManager.store(nullptr,std::memory_order_release);ReleaseSRWLockExclusive(&frameLock);
}
Beam ReadBeam(const Frame& frame,ULONGLONG now) {
    NativeHit sample;AcquireSRWLockShared(&frameLock);sample=hit;ReleaseSRWLockShared(&frameLock);
    Beam beam{};beam.local=frame.local;
    if(!frame.valid || now<frame.time || now-frame.time>kStaleMs || !sample.observed || sample.window!=frame.window ||
        now<sample.time || now-sample.time>kStaleMs || !std::isfinite(frame.worldUnitsPerMetre) || frame.worldUnitsPerMetre<=0)return beam;
    beam.local={frame.space.Point(sample.ray.origin,frame.worldUnitsPerMetre),frame.space.Vector(sample.ray.direction)};
    if(sample.valid){
        const cbmath::Vec3 delta{sample.point.x-sample.ray.origin.x,sample.point.y-sample.ray.origin.y,sample.point.z-sample.ray.origin.z};
        const float length=cbmath::Dot(delta,sample.ray.direction)/frame.worldUnitsPerMetre;
        if(std::isfinite(length) && length>0){beam.length=length;beam.hit=true;}
    }
    return beam;
}
} // namespace pc2vr::pointer::world
