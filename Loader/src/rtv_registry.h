#pragma once

#include <cstdint>

#include <dxgiformat.h>

struct ID3D12Resource;
struct D3D12_RENDER_TARGET_VIEW_DESC;
struct D3D12_CPU_DESCRIPTOR_HANDLE;

namespace pc2vr::rtvreg {

// Which texture and view format each render-target view (CPU descriptor handle) points
// at, from the device's CreateRenderTargetView. Used at command-list recording time by
// foveated rendering (vrs: scene-size targets) and the UI layer (ui_layer: the game's
// UI targets). The resource pointer is for identification only (no reference held).
struct Info {
    ID3D12Resource* resource = nullptr;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the view's format (the resource's when the view has none)
    uint32_t flags = 0;
    uint16_t mipLevels = 0, arraySize = 0;
    uint32_t samples = 0;
};

void Record(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle);
bool Lookup(D3D12_CPU_DESCRIPTOR_HANDLE handle, Info& out);

}  // namespace pc2vr::rtvreg
