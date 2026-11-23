#pragma once
#include <atomic>
#include <cstdint>

namespace pc2vr::pointer::native {
// Separate from the system-DLL hook helper. Only these measured retail
// input entries are allowed; unknown binaries or existing patches fail closed.
enum class Site { Register, Append, MouseRay, UiMove, PhysicsQuery, WorldTransform, UiUpdate, RideLookup, DummyInSeat, PeepHeadHidden };
struct Hook {
    std::atomic<void*> original{};
    void* target{};
};
// Worker/setup thread only, once per process. Verifies on-disk SHA256 and loaded
// PE identity. Does not suspend threads or chain another native hook.
bool VerifyRetail();
enum class Landmark { MouseRayCaller, QueryCaller, RawDataImport, RawInfoImport, RawListImport, UiMove, UiDown, UiUp, UiWheel, RideLookupCaller, SceneViewFlags };
uintptr_t Address(Landmark landmark);
bool Install(Site site, void* replacement, Hook& hook);
} // namespace pc2vr::pointer::native
