#include "depth_probe.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace pc2vr::depth {
namespace {

constexpr D3D12_RESOURCE_STATES kDepthStates = D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ;
constexpr int kTiles = 16;  // 16x16 screen tiles
constexpr int kSlots = 3;   // readback ring

struct Transition {
    ID3D12Resource* resource;
    UINT subresource;
    D3D12_RESOURCE_STATES after;
};
std::mutex g_mutex;
std::unordered_map<ID3D12GraphicsCommandList*, std::vector<Transition>> g_lists;

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;  // one SRV per slot
    UINT srvStride = 0;
    ComPtr<ID3D12Resource> out;         // 256 uints, UAV
    ComPtr<ID3D12Resource> readback;    // kSlots x 256 uints
    const uint32_t* mapped = nullptr;
    ComPtr<ID3D12CommandAllocator> alloc[kSlots];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    uint64_t slotValue[kSlots] = {};
    float slotNearZ[kSlots] = {};
    bool slotPending[kSlots] = {};
    int slot = 0;
    bool failed = false;
} g;

float g_nearest = 0.f;
float g_center = 0.f;  // 0 = mostly sky
ULONGLONG g_nearestMs = 0;
int g_logs = 0;

const char kShader[] = R"(
Texture2D<float> Depth : register(t0);
RWByteAddressBuffer Out : register(u0);
cbuffer C : register(b0) { uint W; uint H; };
groupshared uint g_max;
[numthreads(8, 8, 1)]
void CS(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) g_max = 0;
    GroupMemoryBarrierWithGroupSync();
    uint m = 0;
    [unroll] for (uint j = 0; j < 4; ++j)
        [unroll] for (uint i = 0; i < 4; ++i) {
            uint x = (g.x * 32 + t.x * 4 + i) * W / 512;
            uint y = (g.y * 32 + t.y * 4 + j) * H / 512;
            float d = Depth.Load(int3(x, y, 0));
            if (d > 0 && d <= 1) m = max(m, asuint(d));  // reversed-Z: nearest = largest
        }
    InterlockedMax(g_max, m);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) Out.Store((g.y * 16 + g.x) * 4, g_max);
}
)";

bool Init(ID3D12CommandQueue* queue) {
    if (g.pso) return true;
    if (g.failed) return false;
    g.failed = true;  // until everything below succeeds
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&g.device)))) return false;
    ID3D12Device* dev = g.device.Get();
    ComPtr<ID3DBlob> cs, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "pc2vr_depth", nullptr, nullptr, "CS", "cs_5_0", 0, 0, &cs, &err))) {
        LOG("depth: shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0};
    D3D12_ROOT_SIGNATURE_DESC rs{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSig))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g.rootSig.Get();
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g.pso)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) return false;
    g.srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kTiles * kTiles * 4;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc = {1, 0};
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                            IID_PPV_ARGS(&g.out))))
        return false;
    bd.Width = kSlots * kTiles * kTiles * 4;
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    if (FAILED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g.readback))))
        return false;
    void* p = nullptr;
    if (FAILED(g.readback->Map(0, nullptr, &p))) return false;  // stays mapped (readback heaps allow it)
    g.mapped = static_cast<const uint32_t*>(p);
    for (auto& a : g.alloc)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0].Get(), nullptr, IID_PPV_ARGS(&g.list)))) return false;
    g.list->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    g.failed = false;
    LOG("depth: nearest-distance probe ready");
    return true;
}

// Finished measurements: the nearest tile distance, ignoring the 3 nearest tiles (a
// stray particle or edge pixel).
void Collect() {
    for (int s = 0; s < kSlots; ++s) {
        if (!g.slotPending[s] || g.fence->GetCompletedValue() < g.slotValue[s]) continue;
        g.slotPending[s] = false;
        float dist[kTiles * kTiles], center[kTiles * kTiles];
        int n = 0, nc = 0;
        for (int i = 0; i < kTiles * kTiles; ++i) {
            float d;
            memcpy(&d, &g.mapped[s * kTiles * kTiles + i], 4);
            if (!(d > 0.f && std::isfinite(d))) continue;
            dist[n++] = g.slotNearZ[s] / d;
            const int tx = i % kTiles, ty = i / kTiles;
            if (tx >= 4 && tx < 12 && ty >= 4 && ty < 12) center[nc++] = dist[n - 1];
        }
        // What you look at: the median of the central 8x8 tiles' nearest distances
        // (none when at least half of them are sky).
        if (nc >= 32) {
            std::nth_element(center, center + nc / 2, center + nc);
            g_center = center[nc / 2];
        } else {
            g_center = 0.f;
        }
        if (n == 0) {  // only sky
            g_nearest = 1e9f;
        } else {
            const int k = std::min(3, n - 1);
            std::nth_element(dist, dist + k, dist + n);
            g_nearest = dist[k];
        }
        g_nearestMs = GetTickCount64();
    }
}

}  // namespace

void OnBarrier(ID3D12GraphicsCommandList* list, unsigned count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (!barriers) return;
    for (unsigned i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
        if (!((b.Transition.StateBefore | b.Transition.StateAfter) & kDepthStates)) continue;
        if (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) continue;  // split barrier: the end half sets the state
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& v = g_lists[list];
        if (v.size() < 256) v.push_back({b.Transition.pResource, b.Transition.Subresource, b.Transition.StateAfter});
    }
}

void OnListReset(ID3D12GraphicsCommandList* list) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_lists.find(list);
    if (it != g_lists.end()) it->second.clear();
}

void Measure(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists, uint32_t renderW,
             uint32_t renderH, float nearZ) {
    if (!queue || !(nearZ > 0.f) || !Init(queue)) return;
    Collect();
    // The batch's depth buffer: a depth-stencil resource of the render size written in this
    // batch, and the state its depth plane (subresource 0) ends the batch in.
    struct Seen {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool known = false, written = false;
    };
    std::vector<std::pair<ID3D12Resource*, Seen>> seen;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t l = 0; l < count; ++l) {
            auto it = g_lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[l]));
            if (it == g_lists.end()) continue;
            for (const Transition& t : it->second) {
                if (t.subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && t.subresource != 0) continue;
                auto s = std::find_if(seen.begin(), seen.end(), [&](const auto& e) { return e.first == t.resource; });
                if (s == seen.end()) s = seen.insert(seen.end(), {t.resource, Seen{}});
                s->second.state = t.after;
                s->second.known = true;
                if (t.after & D3D12_RESOURCE_STATE_DEPTH_WRITE) s->second.written = true;
            }
        }
    }
    ID3D12Resource* depth = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
    for (const auto& [res, s] : seen) {
        if (!s.known || !s.written) continue;
        const D3D12_RESOURCE_DESC d = res->GetDesc();  // alive: used by the batch just executed
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != renderW || d.Height != renderH ||
            d.SampleDesc.Count != 1 || (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            continue;
        DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
        if (d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || d.Format == DXGI_FORMAT_R32G8X24_TYPELESS) f = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        if (d.Format == DXGI_FORMAT_D32_FLOAT || d.Format == DXGI_FORMAT_R32_TYPELESS) f = DXGI_FORMAT_R32_FLOAT;
        if (f == DXGI_FORMAT_UNKNOWN) continue;
        depth = res;
        state = s.state;
        srvFormat = f;
    }
    if (!depth) {
        if (++g_logs <= 3) LOG("depth: no depth buffer of %ux%u written in this batch; not measured", renderW, renderH);
        return;
    }
    const int slot = g.slot;
    if (g.slotPending[slot]) return;  // the GPU has not finished this slot yet: skip a frame
    g.slot = (g.slot + 1) % kSlots;
    if (g_logs < 3) {
        ++g_logs;
        LOG("depth: measuring %p (%ux%u, SRV format %d), batch-end state 0x%x", depth, renderW, renderH, srvFormat, state);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srvFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * g.srvStride;
    g.device->CreateShaderResourceView(depth, &sd, cpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot) * g.srvStride;

    g.alloc[slot]->Reset();
    ID3D12GraphicsCommandList* cl = g.list.Get();
    cl->Reset(g.alloc[slot].Get(), g.pso.Get());
    const bool needBarrier = !(state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = depth;
    b.Transition.Subresource = 0;  // the depth plane
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (needBarrier) cl->ResourceBarrier(1, &b);
    ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g.rootSig.Get());
    const UINT wh[2] = {renderW, renderH};
    cl->SetComputeRoot32BitConstants(0, 2, wh, 0);
    cl->SetComputeRootDescriptorTable(1, gpu);
    cl->SetComputeRootUnorderedAccessView(2, g.out->GetGPUVirtualAddress());
    cl->Dispatch(kTiles, kTiles, 1);
    D3D12_RESOURCE_BARRIER ob{};
    ob.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ob.Transition.pResource = g.out.Get();
    ob.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ob.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ob.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_BARRIER back[2] = {ob, b};
    std::swap(back[1].Transition.StateBefore, back[1].Transition.StateAfter);
    cl->ResourceBarrier(needBarrier ? 2 : 1, back);  // depth back to its batch-end state
    cl->CopyBufferRegion(g.readback.Get(), static_cast<UINT64>(slot) * kTiles * kTiles * 4, g.out.Get(), 0, kTiles * kTiles * 4);
    std::swap(ob.Transition.StateBefore, ob.Transition.StateAfter);
    cl->ResourceBarrier(1, &ob);
    cl->Close();
    ID3D12CommandList* run[] = {cl};
    queue->ExecuteCommandLists(1, run);
    g.slotValue[slot] = ++g.fenceValue;
    g.slotNearZ[slot] = nearZ;
    g.slotPending[slot] = true;
    queue->Signal(g.fence.Get(), g.fenceValue);
}

bool NearestDistance(float& metres, float& center) {
    if (!g_nearestMs || GetTickCount64() - g_nearestMs > 500) return false;
    metres = g_nearest;
    center = g_center;
    return true;
}

}  // namespace pc2vr::depth
