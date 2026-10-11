#include "highlight_mask.h"
#include "highlight_signature.h"
#include "rtv_registry.h"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace pc2vr::highlight {
namespace {
// The marker belongs to the PSO itself: destroyed/reused COM pointers cannot
// inherit a cached classification from an older object.
constexpr GUID kMaskPipeline = {0x57c06719, 0xc311, 0x4d3f, {0x99, 0x3a, 0x4d, 0xf3, 0xd7, 0x6c, 0x91, 0x67}};
std::atomic<bool> g_enabled{false};
struct List {
    bool maskPipeline = false, targetValid = false;
    ID3D12Resource* target = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    std::array<ID3D12Resource*, 8> cleared{};
    unsigned count = 0;
};
std::mutex g_mutex;
std::unordered_map<ID3D12GraphicsCommandList*, List> g_lists;

bool MaskPipeline(ID3D12PipelineState* pipeline) {
    UINT size = sizeof(BOOL); BOOL mask = FALSE;
    return pipeline && SUCCEEDED(pipeline->GetPrivateData(kMaskPipeline, &size, &mask)) && size == sizeof(mask) && mask;
}

void Clear(ID3D12GraphicsCommandList* list, List& r) {
    if (!r.maskPipeline || !r.targetValid || r.count == r.cleared.size()) return;
    for (unsigned i = 0; i < r.count; ++i) if (r.cleared[i] == r.target) return;
    // Measured: the native highlight target starts each frame at RGBA=0, but its
    // draw list doesn't clear it. Replaying that list accumulated both eyes' masks.
    // At this draw it is already in RENDER_TARGET state. Recording the clear here
    // makes every replay start empty, including each eye, without extra resources.
    constexpr float zero[4]{};
    list->ClearRenderTargetView(r.handle, zero, 0, nullptr);
    r.cleared[r.count++] = r.target;
}

template<typename T> bool StreamPart(const uint8_t* p, size_t size, size_t& offset, const T*& value) {
    const size_t start = (offset + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + alignof(T) - 1) & ~(alignof(T) - 1);
    if (start > size || sizeof(T) > size - start) return false;
    value = reinterpret_cast<const T*>(p + start);
    offset = (start + sizeof(T) + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    return offset <= size;
}
template<typename T> bool Skip(const uint8_t* p, size_t size, size_t& offset) {
    const T* value = nullptr; return StreamPart(p, size, offset, value);
}
}

void SetEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }

void OnPipelineCreated(ID3D12PipelineState* pipeline, const D3D12_SHADER_BYTECODE& shader) {
    if (!pipeline || !IsMaskShader(shader.pShaderBytecode, shader.BytecodeLength)) return;
    constexpr BOOL mask = TRUE;
    pipeline->SetPrivateData(kMaskPipeline, sizeof(mask), &mask);
}

void OnPipelineCreated(ID3D12PipelineState* pipeline, const D3D12_PIPELINE_STATE_STREAM_DESC& stream) {
    const auto* p = static_cast<const uint8_t*>(stream.pPipelineStateSubobjectStream);
    if (!p) return;
    for (size_t offset = 0; offset < stream.SizeInBytes;) {
        if (sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > stream.SizeInBytes - offset) return;
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        std::memcpy(&type, p + offset, sizeof(type));
        const D3D12_SHADER_BYTECODE* shader = nullptr;
        bool valid = false;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            if (StreamPart(p, stream.SizeInBytes, offset, shader)) OnPipelineCreated(pipeline, *shader);
            return;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: valid = Skip<D3D12_SHADER_BYTECODE>(p, stream.SizeInBytes, offset); break;
#define PART(tag, T) case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##tag: valid = Skip<T>(p, stream.SizeInBytes, offset); break
        PART(ROOT_SIGNATURE, ID3D12RootSignature*);
        PART(STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC);
        PART(BLEND, D3D12_BLEND_DESC);
        PART(SAMPLE_MASK, UINT);
        PART(RASTERIZER, D3D12_RASTERIZER_DESC);
        PART(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC);
        PART(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC);
        PART(IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
        PART(PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE);
        PART(RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY);
        PART(DEPTH_STENCIL_FORMAT, DXGI_FORMAT);
        PART(SAMPLE_DESC, DXGI_SAMPLE_DESC);
        PART(NODE_MASK, UINT);
        PART(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE);
        PART(FLAGS, D3D12_PIPELINE_STATE_FLAGS);
        PART(DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1);
        PART(VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC);
        PART(DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2);
        PART(RASTERIZER1, D3D12_RASTERIZER_DESC1);
        PART(RASTERIZER2, D3D12_RASTERIZER_DESC2);
#undef PART
        default: return; // A future stream layout is left untouched.
        }
        if (!valid) return;
    }
}

void OnListReset(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& r = g_lists[list]; r = {}; r.maskPipeline = MaskPipeline(pipeline);
}
void OnPipelineBound(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    const bool mask = MaskPipeline(pipeline);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& r = g_lists[list]; r.maskPipeline = mask;
}
void OnTargetsBound(ID3D12GraphicsCommandList* list, unsigned count, bool hasDepth,
                    D3D12_CPU_DESCRIPTOR_HANDLE handle, const rtvreg::Info* target) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& r = g_lists[list];
    r.targetValid = count == 1 && !hasDepth && target && target->format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                    target->flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET && target->mipLevels == 1 &&
                    target->arraySize == 1 && target->samples == 1;
    r.target = target ? target->resource : nullptr; r.handle = handle;
}
void BeforeIndirectDraw(ID3D12GraphicsCommandList* list) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_lists.find(list);
    if (it != g_lists.end()) Clear(list, it->second);
}
}
