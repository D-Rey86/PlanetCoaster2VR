#pragma once

struct ID3D12Device;

namespace pc2vr::menu {
// Startup-only Gameface resource integration. Does not create a UI view or GPU surface.
void Install(const wchar_t* modDirectory);
// Called once when the existing XR device is discovered, never from a menu poll.
void DeviceAvailable(ID3D12Device* device);
}
