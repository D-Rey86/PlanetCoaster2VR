#include "ride_visibility.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "native_input_hook.h"
#include "log.h"

namespace pc2vr::ridecam {
namespace {
using LuaMethod = int (*)(void*);
using SetViewFlags = void (*)(void*, uint64_t, uint32_t, bool, uint8_t, uint32_t);
pointer::native::Hook g_dummyHook, g_headHook;
SetViewFlags g_flags{};
std::atomic<bool> g_ready{};

struct DummyResult { void* lua{}; uint64_t entity{}; };
struct HeadRequest {
    void* api{};
    void* scene{};
    uint64_t entity{};
    bool hidden{}, local{};
};
// The query and its immediate head-hide call share a script worker. Retain
// only that result, not all seats/guests. No allocations/scans/logging per query.
thread_local DummyResult g_lastDummy;
// The SAME Lua state migrates between workers across frames/seat changes.
// Visibility ownership must survive that migration. Lock only on head-hide
// transitions, never in the frequent dummy query or render paths.
std::mutex g_hiddenMutex;
HeadRequest g_hidden;

uint64_t DummyResultOf(void* lua) {
    __try {
        auto* top=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(lua)+0x10);
        return *reinterpret_cast<uint32_t*>(top-8)==0x13 ? *reinterpret_cast<uint64_t*>(top-24) : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool ReadHeadRequest(void* lua, HeadRequest& out) {
    __try {
        auto* state=static_cast<uint8_t*>(lua);
        auto* top=*reinterpret_cast<uint8_t**>(state+0x10);
        auto* bottom=**reinterpret_cast<uint8_t***>(state+0x20);
        // Function, native API userdata, entity integer, hide bool, local bool.
        if (top-bottom!=120 || *reinterpret_cast<uint32_t*>(bottom+40)!=0x47 ||
            *reinterpret_cast<uint32_t*>(bottom+64)!=0x13 ||
            *reinterpret_cast<uint32_t*>(bottom+88)!=1 ||
            *reinterpret_cast<uint32_t*>(bottom+112)!=1) return false;
        auto* userdata=*reinterpret_cast<uint8_t**>(bottom+24);
        auto* api=*reinterpret_cast<uint8_t**>(userdata+0x30);
        auto* meshes=*reinterpret_cast<uint8_t**>(api+0x160);
        out.api=api;
        out.scene=*reinterpret_cast<void**>(meshes+0x3e8);
        out.entity=*reinterpret_cast<uint64_t*>(bottom+48);
        out.hidden=*reinterpret_cast<uint64_t*>(bottom+72)!=0;
        out.local=*reinterpret_cast<uint64_t*>(bottom+96)!=0;
        return out.scene && out.entity;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int DummyInSeat(void* lua) {
    const auto original=reinterpret_cast<LuaMethod>(g_dummyHook.original.load(std::memory_order_acquire));
    const int results=original(lua);
    if (g_ready.load(std::memory_order_relaxed))
        g_lastDummy={lua, results==1 ? DummyResultOf(lua) : 0};
    return results;
}

int PeepHeadHidden(void* lua) {
    const auto original=reinterpret_cast<LuaMethod>(g_headHook.original.load(std::memory_order_acquire));
    HeadRequest request{};
    const bool valid=g_ready.load(std::memory_order_acquire) && ReadHeadRequest(lua,request);
    const bool selected=valid && request.local && request.hidden &&
        g_lastDummy.lua==lua && g_lastDummy.entity==request.entity;
    g_lastDummy={};
    const int result=original(lua);
    if (!valid) return result;
    const std::lock_guard<std::mutex> lock(g_hiddenMutex);

    // Reacquire the scene from this live API call. Never dereference a cached
    // scene pointer after a park unload. A new scene invalidates our old token.
    if (g_hidden.entity && (request.api!=g_hidden.api || request.scene!=g_hidden.scene)) g_hidden={};
    if (!request.hidden && request.entity==g_hidden.entity) {
        g_flags(request.scene,request.entity,0x200,false,2,2);
        g_hidden={};
    } else if (selected && !g_hidden.entity) {
        // The game's peep-head helper cannot resolve crash-test dummies. Use
        // its own first-person view-category flag on this confirmed dummy.
        // Other riders, normal guests and all other flag bits are untouched.
        g_flags(request.scene,request.entity,0x200,true,2,2);
        g_hidden=request;
    }
    return result;
}
}

void PrepareVisibility() {
    using namespace pointer::native;
    const auto flags=Address(Landmark::SceneViewFlags);
    constexpr uint8_t expected[]={0x40,0x53,0x48,0x83,0xec,0x50,0x48,0x89,0x6c,0x24,0x68,0x48,0x8d,0x99,0x30,0x03};
    if (!flags || std::memcmp(reinterpret_cast<const void*>(flags),expected,sizeof(expected))) return;
    g_flags=reinterpret_cast<SetViewFlags>(flags);
    const bool ready=Install(Site::DummyInSeat,reinterpret_cast<void*>(&DummyInSeat),g_dummyHook) &&
        Install(Site::PeepHeadHidden,reinterpret_cast<void*>(&PeepHeadHidden),g_headHook);
    g_ready.store(ready,std::memory_order_release);
    LOG("ridecam: selected test-dummy visibility %s",ready ? "ready" : "unavailable");
}
}
