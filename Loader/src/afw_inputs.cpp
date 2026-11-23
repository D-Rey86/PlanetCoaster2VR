#include "afw_inputs.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "rtv_registry.h"

namespace pc2vr::afw {
namespace {
constexpr GUID kVelocityPipeline = {0xe49bcb40,0x7409,0x4c03,{0xb6,0xb0,0x6f,0x90,0x22,0xcf,0xe1,0x2e}};
// InitialiseVelocityData_Main_ps, verified in the archived TAA capture and
// in the current VR render batch with AA Off. A different shader is unknown.
constexpr uint8_t kVelocityHash[] = {0x3d,0x67,0x0b,0x35,0xda,0x53,0x7a,0xfb,0x9d,0x1c,0xfe,0x99,0x1e,0x0a,0x9b,0xd0};
constexpr size_t kBarrierLimit = 2048;
std::atomic<bool> g_enabled{};
std::mutex g_mutex;
struct Draw {
    rtvreg::Info motion{}, depth{};
};
struct List {
    bool velocity{}, overflow{}, ambiguous{};
    Draw bound{}, drawn{};
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
};
std::unordered_map<ID3D12GraphicsCommandList*, List> g_lists;

uint32_t U32(const uint8_t* bytes) { uint32_t value; std::memcpy(&value, bytes, 4); return value; }

bool VelocityPipeline(ID3D12PipelineState* pipeline) {
    BOOL yes = FALSE;
    UINT size = sizeof(yes);
    return pipeline && SUCCEEDED(pipeline->GetPrivateData(kVelocityPipeline, &size, &yes)) && yes;
}

bool Same(const Draw& a, const Draw& b) {
    return a.motion.resource == b.motion.resource && a.depth.resource == b.depth.resource;
}

DXGI_FORMAT DepthView(DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_R32G8X24_TYPELESS) return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    if (format == DXGI_FORMAT_R32_TYPELESS) return DXGI_FORMAT_R32_FLOAT;
    return DXGI_FORMAT_UNKNOWN;
}

bool Usable(const rtvreg::Info& info, UINT w, UINT h) {
    return info.resource && info.width == w && info.height == h && info.arraySize == 1 &&
        info.mipLevels == 1 && info.samples == 1 && !(info.flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}

struct State {
    D3D12_RESOURCE_STATES value{};
    bool seen{}, split{};
    void Transition(const D3D12_RESOURCE_BARRIER& barrier) {
        split = barrier.Flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
        if (!split) { value = barrier.Transition.StateAfter; seen = true; }
    }
    bool Readable() const { return seen && !split && (value & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }
};
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

void SetEnabled(bool enabled) {
    const bool before = g_enabled.exchange(enabled, std::memory_order_relaxed);
    if (before && !enabled) {
        const std::lock_guard lock(g_mutex);
        g_lists.clear();
    }
}

void MarkVelocityPipeline(ID3D12PipelineState* pipeline, const D3D12_SHADER_BYTECODE& shader) {
    const auto* bytes = static_cast<const uint8_t*>(shader.pShaderBytecode);
    const size_t size = shader.BytecodeLength;
    if (!pipeline || !bytes || size < 32 || std::memcmp(bytes,"DXBC",4)) return;
    const uint32_t count = U32(bytes+28);
    if (count > (size-32)/4) return;
    for (uint32_t i=0; i<count; ++i) {
        const uint32_t offset = U32(bytes+32+i*4);
        if (offset > size-28 || std::memcmp(bytes+offset,"HASH",4) || U32(bytes+offset+4)!=20) continue;
        if (!std::memcmp(bytes+offset+12,kVelocityHash,sizeof(kVelocityHash))) {
            constexpr BOOL yes = TRUE;
            pipeline->SetPrivateData(kVelocityPipeline,sizeof(yes),&yes);
        }
        return;
    }
}

void OnReset(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    if (!Enabled()) return;
    const bool velocity = VelocityPipeline(pipeline);
    const std::lock_guard lock(g_mutex);
    auto& value = g_lists[list];
    value.velocity = velocity;
    value.overflow = value.ambiguous = false;
    value.bound = value.drawn = {};
    value.barriers.clear();
}

void OnPipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    if (!Enabled()) return;
    const bool velocity = VelocityPipeline(pipeline);
    const std::lock_guard lock(g_mutex);
    g_lists[list].velocity = velocity;
}

void OnTargets(ID3D12GraphicsCommandList* list, const rtvreg::Info* target, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    if (!Enabled()) return;
    Draw bound;
    if (depth && target && target->format == DXGI_FORMAT_R16G16_FLOAT && rtvreg::Lookup(*depth,bound.depth))
        bound.motion = *target;
    const std::lock_guard lock(g_mutex);
    g_lists[list].bound = bound;
}

void OnDraw(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances) {
    if (!Enabled() || vertices != 3 || instances != 1) return;
    const std::lock_guard lock(g_mutex);
    const auto it = g_lists.find(list);
    if (it == g_lists.end()) return;
    auto& value = it->second;
    if (!value.velocity || !value.bound.motion.resource) return;
    if (value.drawn.motion.resource && !Same(value.drawn,value.bound)) value.ambiguous = true;
    value.drawn = value.bound;
}

void OnBarriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (!Enabled() || !count || !barriers) return;
    const std::lock_guard lock(g_mutex);
    auto& value = g_lists[list];
    if (count > kBarrierLimit-value.barriers.size()) { value.overflow = true; return; }
    value.barriers.insert(value.barriers.end(),barriers,barriers+count);
}

Guides FindGuides(UINT count, ID3D12CommandList* const* lists, UINT width, UINT height) {
    if (!Enabled()) return {};
    const std::lock_guard lock(g_mutex);
    Draw selected;
    for (UINT i=0; i<count; ++i) {
        const auto it = g_lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (it == g_lists.end()) continue;
        const auto& list = it->second;
        if (list.overflow || list.ambiguous) return {};
        const auto& draw = list.drawn;
        if (!Usable(draw.motion,width,height) || !Usable(draw.depth,width,height) || !DepthView(draw.depth.format)) continue;
        if (selected.motion.resource && !Same(selected,draw)) return {};
        selected = draw;
    }
    if (!selected.motion.resource) return {};
    State motion, depth;
    for (UINT i=0; i<count; ++i) {
        const auto it = g_lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (it == g_lists.end()) continue;
        for (const auto& barrier : it->second.barriers) {
            if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                const auto a = barrier.Aliasing.pResourceBefore, b = barrier.Aliasing.pResourceAfter;
                if (!a || !b || a==selected.motion.resource || b==selected.motion.resource ||
                    a==selected.depth.resource || b==selected.depth.resource) return {};
            } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                       (barrier.Transition.Subresource == 0 || barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) {
                if (barrier.Transition.pResource == selected.motion.resource) motion.Transition(barrier);
                if (barrier.Transition.pResource == selected.depth.resource) depth.Transition(barrier);
            }
        }
    }
    if (!motion.Readable() || !depth.Readable()) return {};
    return {selected.depth.resource, selected.motion.resource, DepthView(selected.depth.format)};
}
}
