#pragma once

#include <cstdint>

namespace pc2vr::hooks {

// Hooks are vtable patches inside dxgi.dll / D3D12Core.dll only; game code is
// never modified. Vtables are found with a throwaway device created from the
// system DLLs.
//
// InstallEarly: device resource-creation hooks, so upload buffers the game
//   creates during start-up are tracked. Creates a device but no DXGI objects.
// InstallLate:  swap chain, command queue and command list hooks, installed once
//   the game's window exists (creating DXGI objects before the game initialises
//   crashed it on the first test).
bool InstallEarly();
bool InstallLate(bool enableXr);
void Uninstall();

struct Stats {
    uint64_t presents;
    uint64_t executes;
};
Stats GetStats();

}  // namespace pc2vr::hooks

