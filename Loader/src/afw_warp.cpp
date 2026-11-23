#include "afw_warp.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <utility>

#include "afw_shader.h"
#include "log.h"

namespace pc2vr::afw {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT kSlots = 6;  // the three eye-capture allocator pairs
constexpr UINT kSrvs = 7, kUavs = 4, kBlock = kSrvs+kUavs;
constexpr DXGI_FORMAT kGrid = DXGI_FORMAT_R32_UINT;
constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

struct View {
    float posNear[4], rightK[4], upJx[4], forwardJy[4], tanFov[4];
};
struct Constants { View source, target, previous; UINT history; };
static_assert(sizeof(Constants)/4 == 61);

struct Images {
    ComPtr<ID3D12Resource> depth[2], fresh, old, output;
    UINT w{}, h{}, colorW{}, colorH{};
};
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> clear, build, resolve;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    Images images;
    UINT stride{};
    bool failed{};
    int previousEye = -1;
    uint64_t serial{};
    View previous{};
} g;

View Pack(const cbmath::ParsedView& view) {
    const auto& b = view.basis;
    return {{view.pos.x, view.pos.y, view.pos.z, view.nearZ},
            {b.right.x, b.right.y, b.right.z, view.depthK},
            {b.up.x, b.up.y, b.up.z, view.jitterX},
            {b.forward.x, b.forward.y, b.forward.z, view.jitterY},
            {view.fov.tanLeft, view.fov.tanRight, view.fov.tanUp, view.fov.tanDown}};
}

bool Valid(const View& view) {
    for (const auto& v : {view.posNear,view.rightK,view.upJx,view.forwardJy,view.tanFov})
        for (int i=0; i<4; ++i) if (!std::isfinite(v[i])) return false;
    return view.posNear[3] > 0 && view.tanFov[1] > view.tanFov[0] && view.tanFov[2] > view.tanFov[3];
}

bool Initialize(ID3D12Device* device) {
    const D3D12_DESCRIPTOR_RANGE ranges[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,kSrvs,0,0,0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,kUavs,0,0,kSrvs}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2,ranges};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0,0,sizeof(Constants)/4};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_SIGNATURE_DESC signature{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)) ||
        FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&g.root))))
        return false;
    const auto compile = [&](const char* entry, ComPtr<ID3D12PipelineState>& pso) {
        blob.Reset();
        error.Reset();
        if (FAILED(D3DCompile(kShader,sizeof(kShader)-1,"PC2VR AFW",nullptr,nullptr,entry,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error))) {
            LOG("AFW: %s shader failed: %s",entry,error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown error");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g.root.Get();
        desc.CS = {blob->GetBufferPointer(),blob->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pso)));
    };
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kSlots*2*kBlock,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if (!compile("Clear",g.clear) || !compile("Build",g.build) || !compile("Resolve",g.resolve) ||
        FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&g.descriptors)))) return false;
    g.device = device;
    g.stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool Texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_STATES initial, ComPtr<ID3D12Resource>& resource) {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(g.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,IID_PPV_ARGS(&resource)));
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    list->ResourceBarrier(1,&b);
}

void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* a, ID3D12Resource* b) {
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = a;
    barriers[1].UAV.pResource = b;
    list->ResourceBarrier(2,barriers);
}

D3D12_GPU_DESCRIPTOR_HANDLE Bind(ID3D12GraphicsCommandList* list, UINT index, bool resolve, int eye,
                                const Guides& guides, ID3D12Resource* color, ID3D12Resource* priorColor, DXGI_FORMAT format) {
    const auto& v = g.images;
    auto cpu = g.descriptors->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g.descriptors->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index)*g.stride;
    gpu.ptr += static_cast<UINT64>(index)*g.stride;
    const auto srv = [&](ID3D12Resource* resource, DXGI_FORMAT type) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = type;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        g.device->CreateShaderResourceView(resource,&d,cpu);
        cpu.ptr += g.stride;
    };
    const auto uav = [&](ID3D12Resource* resource, DXGI_FORMAT type) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = type;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.device->CreateUnorderedAccessView(resource,nullptr,&d,cpu);
        cpu.ptr += g.stride;
    };
    srv(guides.depth,guides.depthView);
    srv(resolve ? nullptr : guides.motion,DXGI_FORMAT_R16G16_FLOAT);
    srv(resolve ? color : nullptr,format);
    srv(resolve ? priorColor : nullptr,format);
    srv(v.depth[eye^1].Get(),kGrid);
    srv(resolve ? v.fresh.Get() : nullptr,kGrid);
    srv(resolve ? v.old.Get() : nullptr,kGrid);
    uav(resolve ? nullptr : v.depth[eye].Get(),kGrid);
    uav(resolve ? nullptr : v.fresh.Get(),kGrid);
    uav(resolve ? nullptr : v.old.Get(),kGrid);
    uav(resolve ? v.output.Get() : nullptr,kColor);
    list->SetComputeRootDescriptorTable(0,gpu);
    return gpu;
}
}

void InvalidateHistory() { g.previousEye = -1; }

bool Prepare(ID3D12Device* device, const D3D12_RESOURCE_DESC& color, const Frame& frame, bool idle) {
    if (g.failed || !frame.guides.depth || !frame.guides.motion || !frame.width || !frame.height ||
        !Valid(Pack(frame.source)) || !Valid(Pack(frame.target))) return false;
    if (color.Format != DXGI_FORMAT_R10G10B10A2_UNORM && color.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    if (!g.device && !Initialize(device)) {
        g.failed = true;
        LOG("AFW: GPU pipeline unavailable; using AER");
        return false;
    }
    if (g.device.Get() != device) return false;
    // Limit the depth splat grid at high headset resolutions. Resolve samples
    // the native depth at the projected colour location for edge agreement.
    const UINT scale = std::max(frame.width,frame.height)>2048 ? 2 : 1;
    const UINT w = (frame.width+scale-1)/scale, h = (frame.height+scale-1)/scale;
    if (g.images.output && g.images.w == w && g.images.h == h &&
        g.images.colorW == color.Width && g.images.colorH == color.Height) return true;
    if (g.images.output && !idle) return false;
    Images next;
    next.w=w; next.h=h; next.colorW=static_cast<UINT>(color.Width); next.colorH=color.Height;
    if (!Texture(w,h,kGrid,kRead,next.depth[0]) || !Texture(w,h,kGrid,kRead,next.depth[1]) ||
        !Texture(w,h,kGrid,kRead,next.fresh) || !Texture(w,h,kGrid,kRead,next.old) ||
        !Texture(next.colorW,next.colorH,kColor,D3D12_RESOURCE_STATE_COMMON,next.output)) {
        g.failed = true;
        LOG("AFW: reconstruction surfaces unavailable; using AER");
        return false;
    }
    g.images = std::move(next);
    InvalidateHistory();
    LOG("AFW: reconstruction %ux%u, depth grid %ux%u",g.images.colorW,g.images.colorH,w,h);
    return true;
}

ID3D12Resource* Record(ID3D12GraphicsCommandList* list, unsigned slot, int eye, uint64_t serial,
                       const Frame& frame, ID3D12Resource* color, ID3D12Resource* priorColor, DXGI_FORMAT format) {
    if (slot >= kSlots) return nullptr;
    auto& v = g.images;
    const bool history = g.previousEye == (eye^1) && g.serial+1 == serial && priorColor;
    const Constants constants{Pack(frame.source),Pack(frame.target),g.previous,history ? 1u : 0u};
    ID3D12DescriptorHeap* heaps[]{g.descriptors.Get()};
    list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(g.root.Get());
    list->SetComputeRoot32BitConstants(1,sizeof(Constants)/4,&constants,0);
    Transition(list,v.fresh.Get(),kRead,kWrite);
    Transition(list,v.old.Get(),kRead,kWrite);
    Transition(list,v.depth[eye].Get(),kRead,kWrite);
    const UINT base = slot*2*kBlock;
    Bind(list,base,false,eye,frame.guides,color,priorColor,format);
    list->SetPipelineState(g.clear.Get());
    list->Dispatch((v.w+7)/8,(v.h+7)/8,1);
    UavBarrier(list,v.fresh.Get(),v.old.Get());
    list->SetPipelineState(g.build.Get());
    list->Dispatch((v.w+7)/8,(v.h+7)/8,1);
    Transition(list,v.depth[eye].Get(),kWrite,kRead);
    Transition(list,v.fresh.Get(),kWrite,kRead);
    Transition(list,v.old.Get(),kWrite,kRead);
    Transition(list,color,D3D12_RESOURCE_STATE_COPY_DEST,kRead);
    if (history) Transition(list,priorColor,D3D12_RESOURCE_STATE_COPY_DEST,kRead);
    Transition(list,v.output.Get(),D3D12_RESOURCE_STATE_COMMON,kWrite);
    Bind(list,base+kBlock,true,eye,frame.guides,color,history ? priorColor : nullptr,format);
    list->SetPipelineState(g.resolve.Get());
    list->Dispatch((v.colorW+7)/8,(v.colorH+7)/8,1);
    Transition(list,v.output.Get(),kWrite,D3D12_RESOURCE_STATE_COMMON);
    if (history) Transition(list,priorColor,kRead,D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list,color,kRead,D3D12_RESOURCE_STATE_COPY_DEST);
    g.previous = constants.source;
    g.previousEye = eye;
    g.serial = serial;
    return v.output.Get();
}
}
