#include "rtv_registry.h"

#include <windows.h>
#include <d3d12.h>

#include <mutex>
#include <unordered_map>

namespace pc2vr::rtvreg {
namespace {
std::mutex g_mutex;
std::unordered_map<SIZE_T, Info> g_rtvs;
}  // namespace

void Record(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (!resource) return;
    const D3D12_RESOURCE_DESC d = resource->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    Info i;
    i.resource = resource;
    i.width = static_cast<uint32_t>(d.Width);
    i.height = d.Height;
    i.format = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : d.Format;
    i.flags = d.Flags;
    i.mipLevels = d.MipLevels;
    i.arraySize = d.DepthOrArraySize;
    i.samples = d.SampleDesc.Count;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rtvs[handle.ptr] = i;
}

bool Lookup(D3D12_CPU_DESCRIPTOR_HANDLE handle, Info& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rtvs.find(handle.ptr);
    if (it == g_rtvs.end()) return false;
    out = it->second;
    return true;
}

}  // namespace pc2vr::rtvreg
