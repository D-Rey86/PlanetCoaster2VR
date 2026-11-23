#pragma once
#include <windows.h>
#include "world_pointer_geometry.h"

namespace pc2vr::pointer::world {
// First world-laser stage: native aiming and the visible beam. Button transport
// is deliberately not enabled by this component. Existing Xbox input is retained.
// Prepare runs once on the setup worker; unsupported retail builds fail closed.
bool Prepare();
bool Ready();
// One cached pose/ray, shared by the native pick override and both rendered eyes.
// The caller clears on camera mode, tracking, session or window invalidation.
struct Frame {
    Ray local{};
    WorldRay world{};
    HWND window{};
    ULONGLONG time{};
    bool valid{};
    float worldUnitsPerMetre=1.f;
    WorldSpace space{};
    WorldRay camera{}; // Exact native camera ray used by controller picking.
};
struct Beam {float length=25.f;bool hit{};Ray local{};};
// One matching native collision result, shared by both eyes. No extra raycast.
Beam ReadBeam(const Frame& frame,ULONGLONG now);
bool Publish(const Frame& frame);
void Clear();
} // namespace pc2vr::pointer::world
