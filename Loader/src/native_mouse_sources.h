#pragma once
#include <windows.h>
#include <cstdint>

namespace pc2vr::pointer::sources {
// Set up before announcing this process-local handle to the native window.
// The native object is retained once, never recreated on tracking/mode changes.
bool Install(HANDLE ownDevice);
// Observe native input unchanged until all cached sources are neutral. The
// caller explicitly enables aggregation only after that initialization barrier.
bool Enable();
bool Available();
bool OwnNeutral();
struct Snapshot {
    uint32_t ownId{}, known{};
    bool captured{}, enabled{}, failed{}, removed{}, ownHeld{};
    int32_t nativeLevel{};
};
Snapshot Read(); // demand-only status, no logging/capture in input callbacks
} // namespace pc2vr::pointer::sources
