#include "d3d12_hooks.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <string>

#include "cb_patcher.h"
#include "depth_probe.h"
#include "rtv_registry.h"
#include "highlight_mask.h"
#include "ui_layer.h"
#include "vrs.h"
#include "stereo.h"
#include "log.h"
#include "upload_registry.h"
#include "view_cb.h"
#include "xr_runtime.h"

using Microsoft::WRL::ComPtr;

namespace pc2vr::hooks {
namespace {

// ---- vtable slots (counted from the d3d12.h / dxgi.h interface order) ----
// IDXGISwapChain / IDXGISwapChain1
constexpr size_t kSlotPresent = 8;
constexpr size_t kSlotResizeBuffers = 13;
constexpr size_t kSlotPresent1 = 22;
// ID3D12CommandQueue
constexpr size_t kSlotExecuteCommandLists = 10;
// ID3D12Device .. ID3D12Device10
constexpr size_t kSlotCreateCommittedResource = 27;
constexpr size_t kSlotCreatePlacedResource = 29;
constexpr size_t kSlotCreateRenderTargetView = 20;
constexpr size_t kSlotCreateGraphicsPipelineState = 10;
constexpr size_t kSlotCreatePipelineState = 47; // ID3D12Device2
constexpr size_t kSlotCreateCommittedResource1 = 53;  // ID3D12Device4
constexpr size_t kSlotCreateCommittedResource2 = 69;  // ID3D12Device8
constexpr size_t kSlotCreatePlacedResource1 = 70;     // ID3D12Device8
constexpr size_t kSlotCreateCommittedResource3 = 76;  // ID3D12Device10
constexpr size_t kSlotCreatePlacedResource2 = 77;     // ID3D12Device10
// ID3D12GraphicsCommandList
constexpr size_t kSlotListReset = 10;
constexpr size_t kSlotCopyTextureRegion = 16;
constexpr size_t kSlotCopyResource = 17;
constexpr size_t kSlotResourceBarrier = 26;
constexpr size_t kSlotOMSetRenderTargets = 46;
constexpr size_t kSlotSetPipelineState = 25;
constexpr size_t kSlotExecuteIndirect = 59;
constexpr size_t kSlotSetComputeRootCbv = 37;
constexpr size_t kSlotSetGraphicsRootCbv = 38;

struct Patch {
    void** slot = nullptr;
    void* original = nullptr;
};

Patch g_present, g_present1, g_resize, g_execute;
Patch g_ccr, g_cpr, g_ccr1, g_ccr2, g_cpr1, g_ccr3, g_cpr2, g_crtv, g_omrt;
Patch g_setComputeCbv, g_setGraphicsCbv, g_listReset, g_copyTex, g_copyRes, g_barrier;
Patch g_graphicsPipeline, g_pipelineStream, g_setPipeline, g_indirect;

void** g_cmdListVtable = nullptr;

std::atomic<uint64_t> g_presents{0};
std::atomic<IDXGISwapChain*> g_loggedSwapChain{nullptr};
std::atomic<int> g_selfTestHits{0};
bool g_xrEnabled = true;
thread_local ID3D12CommandQueue* t_lastDirectQueue = nullptr;

bool PatchSlot(void** vtable, size_t index, void* hook, Patch& patch) {
    void** slot = &vtable[index];
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    patch.slot = slot;
    patch.original = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

void RestoreSlot(Patch& patch) {
    if (!patch.slot) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(patch.slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *patch.slot = patch.original;
        VirtualProtect(patch.slot, sizeof(void*), oldProtect, &oldProtect);
    }
    patch.slot = nullptr;
}

template <typename Fn>
Fn Orig(const Patch& p) {
    return reinterpret_cast<Fn>(p.original);
}

// ---- resource creation: feed the upload-buffer registry ----

void TrackIfUploadBuffer(const D3D12_HEAP_PROPERTIES* heapProps, D3D12_HEAP_TYPE placedHeapType,
                         const D3D12_RESOURCE_DESC* desc, REFIID riid, void* const* out) {
    if (!desc || !out || !*out) return;
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    const D3D12_HEAP_TYPE type = heapProps ? heapProps->Type : placedHeapType;
    if (type != D3D12_HEAP_TYPE_UPLOAD) return;
    g_selfTestHits.fetch_add(1);
    ComPtr<ID3D12Resource> resource;
    if (FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&resource)))) return;
    (void)riid;
    uploads::OnBufferCreated(resource.Get(), desc->Width);
}

// Render-target views and render-target binds: foveated rendering (vrs) and the UI
// panel (ui_layer).
void STDMETHODCALLTYPE HookCreateRenderTargetView(ID3D12Device* self, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    Orig<decltype(&HookCreateRenderTargetView)>(g_crtv)(self, res, desc, handle);
    rtvreg::Record(res, desc, handle);
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D12GraphicsCommandList* self, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                              BOOL singleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    Orig<decltype(&HookOMSetRenderTargets)>(g_omrt)(self, count, rtvs, singleRange, dsv);
    rtvreg::Info rt0;
    const bool known = count > 0 && rtvs && rtvreg::Lookup(rtvs[0], rt0);
    highlight::OnTargetsBound(self, count, dsv != nullptr, count && rtvs ? rtvs[0] : D3D12_CPU_DESCRIPTOR_HANDLE{}, known ? &rt0 : nullptr);
    vrs::OnSetRenderTargets(self, known ? &rt0 : nullptr, dsv != nullptr);
    ui::OnSetRenderTargets(self, known ? &rt0 : nullptr);
}

D3D12_HEAP_TYPE HeapType(ID3D12Heap* heap) { return heap ? heap->GetDesc().Properties.Type : D3D12_HEAP_TYPE_DEFAULT; }

HRESULT STDMETHODCALLTYPE HookCreateGraphicsPipelineState(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                          REFIID riid, void** out) {
    const HRESULT hr = Orig<decltype(&HookCreateGraphicsPipelineState)>(g_graphicsPipeline)(self, desc, riid, out);
    if (SUCCEEDED(hr) && desc && out && *out) {
        ComPtr<ID3D12PipelineState> pipeline;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&pipeline))))
            highlight::OnPipelineCreated(pipeline.Get(), desc->PS);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE HookCreatePipelineState(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                                  REFIID riid, void** out) {
    const HRESULT hr = Orig<decltype(&HookCreatePipelineState)>(g_pipelineStream)(self, desc, riid, out);
    if (SUCCEEDED(hr) && desc && out && *out) {
        ComPtr<ID3D12PipelineState> pipeline;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&pipeline))))
            highlight::OnPipelineCreated(pipeline.Get(), *desc);
    }
    return hr;
}
void STDMETHODCALLTYPE HookSetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pipeline) {
    Orig<decltype(&HookSetPipelineState)>(g_setPipeline)(self, pipeline);
    highlight::OnPipelineBound(self, pipeline);
}
void STDMETHODCALLTYPE HookExecuteIndirect(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* signature,
    UINT maxCount, ID3D12Resource* arguments, UINT64 argumentOffset, ID3D12Resource* count, UINT64 countOffset) {
    // Wait until the observed indirect mesh draw: a stale previous PSO while
    // binding targets must never clear an unrelated surface.
    highlight::BeforeIndirectDraw(self);
    Orig<decltype(&HookExecuteIndirect)>(g_indirect)(self, signature, maxCount, arguments, argumentOffset, count, countOffset);
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* hp,
                                                      D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* desc,
                                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                                      REFIID riid, void** out) {
    using Fn = decltype(&HookCreateCommittedResource);
    const HRESULT hr = Orig<Fn>(g_ccr)(self, hp, hf, desc, state, clear, riid, out);
    if (SUCCEEDED(hr)) TrackIfUploadBuffer(hp, D3D12_HEAP_TYPE_DEFAULT, desc, riid, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreatePlacedResource(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset,
                                                   const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    using Fn = decltype(&HookCreatePlacedResource);
    const HRESULT hr = Orig<Fn>(g_cpr)(self, heap, offset, desc, state, clear, riid, out);
    if (SUCCEEDED(hr)) TrackIfUploadBuffer(nullptr, HeapType(heap), desc, riid, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource1(ID3D12Device4* self, const D3D12_HEAP_PROPERTIES* hp,
                                                       D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* desc,
                                                       D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                                       ID3D12ProtectedResourceSession* session, REFIID riid,
                                                       void** out) {
    using Fn = decltype(&HookCreateCommittedResource1);
    const HRESULT hr = Orig<Fn>(g_ccr1)(self, hp, hf, desc, state, clear, session, riid, out);
    if (SUCCEEDED(hr)) TrackIfUploadBuffer(hp, D3D12_HEAP_TYPE_DEFAULT, desc, riid, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource2(ID3D12Device8* self, const D3D12_HEAP_PROPERTIES* hp,
                                                       D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC1* desc,
                                                       D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                                       ID3D12ProtectedResourceSession* session, REFIID riid,
                                                       void** out) {
    using Fn = decltype(&HookCreateCommittedResource2);
    const HRESULT hr = Orig<Fn>(g_ccr2)(self, hp, hf, desc, state, clear, session, riid, out);
    if (SUCCEEDED(hr)) TrackIfUploadBuffer(hp, D3D12_HEAP_TYPE_DEFAULT, reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc), riid, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreatePlacedResource1(ID3D12Device8* self, ID3D12Heap* heap, UINT64 offset,
                                                    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state,
                                                    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    using Fn = decltype(&HookCreatePlacedResource1);
    const HRESULT hr = Orig<Fn>(g_cpr1)(self, heap, offset, desc, state, clear, riid, out);
    if (SUCCEEDED(hr)) TrackIfUploadBuffer(nullptr, HeapType(heap), reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc), riid, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource3(ID3D12Device10* self, const D3D12_HEAP_PROPERTIES* hp,
                                                       D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC1* desc,
                                                       D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear,
                                                       ID3D12ProtectedResourceSession* session, UINT32 numFormats,
                                                       const DXGI_FORMAT* formats, REFIID riid, void** out) {
    using Fn = decltype(&HookCreateCommittedResource3);
    const HRESULT hr = Orig<Fn>(g_ccr3)(self, hp, hf, desc, layout, clear, session, numFormats, formats, riid, out);
    if (SUCCEEDED(hr)) TrackIfUploadBuffer(hp, D3D12_HEAP_TYPE_DEFAULT, reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc), riid, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreatePlacedResource2(ID3D12Device10* self, ID3D12Heap* heap, UINT64 offset,
                                                    const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                                    const D3D12_CLEAR_VALUE* clear, UINT32 numFormats,
                                                    const DXGI_FORMAT* formats, REFIID riid, void** out) {
    using Fn = decltype(&HookCreatePlacedResource2);
    const HRESULT hr = Orig<Fn>(g_cpr2)(self, heap, offset, desc, layout, clear, numFormats, formats, riid, out);
    if (SUCCEEDED(hr)) TrackIfUploadBuffer(nullptr, HeapType(heap), reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc), riid, out);
    return hr;
}

// ---- command list: constant buffer binds ----

HRESULT STDMETHODCALLTYPE HookListReset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* alloc,
                                        ID3D12PipelineState* pso) {
    highlight::OnListReset(self, pso);
    patcher::OnCommandListReset(self);
    xr::OnListReset(self);
    depth::OnListReset(self);
    vrs::OnListReset(self);
    ui::OnListReset(self);
    return Orig<decltype(&HookListReset)>(g_listReset)(self, alloc, pso);
}

// Copies: the game copies its finished frame into the back buffer on a second queue;
// the stereo eye capture needs that source (xr::OnGameCopy). Barriers: which list renders
// into it (xr::OnGameBarrier).
void STDMETHODCALLTYPE HookCopyTextureRegion(ID3D12GraphicsCommandList* self, const D3D12_TEXTURE_COPY_LOCATION* dst,
                                             UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src,
                                             const D3D12_BOX* box) {
    if (dst && src) xr::OnGameCopy(dst->pResource, src->pResource);
    Orig<decltype(&HookCopyTextureRegion)>(g_copyTex)(self, dst, x, y, z, src, box);
}

void STDMETHODCALLTYPE HookCopyResource(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, ID3D12Resource* src) {
    xr::OnGameCopy(dst, src);
    Orig<decltype(&HookCopyResource)>(g_copyRes)(self, dst, src);
}

void STDMETHODCALLTYPE HookResourceBarrier(ID3D12GraphicsCommandList* self, UINT count,
                                           const D3D12_RESOURCE_BARRIER* barriers) {
    xr::OnGameBarrier(self, count, barriers);
    depth::OnBarrier(self, count, barriers);
    ui::OnBarrier(self, count, barriers);
    Orig<decltype(&HookResourceBarrier)>(g_barrier)(self, count, barriers);
}

void STDMETHODCALLTYPE HookSetComputeRootCbv(ID3D12GraphicsCommandList* self, UINT index,
                                             D3D12_GPU_VIRTUAL_ADDRESS va) {
    patcher::OnRootCbvBound(self, va);
    Orig<decltype(&HookSetComputeRootCbv)>(g_setComputeCbv)(self, index, va);
}

void STDMETHODCALLTYPE HookSetGraphicsRootCbv(ID3D12GraphicsCommandList* self, UINT index,
                                              D3D12_GPU_VIRTUAL_ADDRESS va) {
    patcher::OnRootCbvBound(self, va);
    Orig<decltype(&HookSetGraphicsRootCbv)>(g_setGraphicsCbv)(self, index, va);
}

// ---- swap chain / queue ----

void DescribeSwapChain(IDXGISwapChain* swapChain) {
    if (g_loggedSwapChain.exchange(swapChain) == swapChain) return;
    ComPtr<IDXGISwapChain1> sc1;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc1)))) sc1->GetDesc1(&desc);
    HWND hwnd = nullptr;
    if (sc1) sc1->GetHwnd(&hwnd);
    LOG("swapchain %p: %ux%u format=%d buffers=%u swapEffect=%d flags=0x%x hwnd=%p", swapChain, desc.Width,
        desc.Height, desc.Format, desc.BufferCount, desc.SwapEffect, desc.Flags, hwnd);
}

void OnPresent(IDXGISwapChain* self) {
    const uint64_t n = g_presents.fetch_add(1) + 1;
    vrs::PollSettings();
    ui::PollSettings();
    if (n == 1) {
        DescribeSwapChain(self);
    }
    patcher::OnPresent(n);
    if (g_xrEnabled) xr::OnPresent(self, t_lastDirectQueue);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags) {

    OnPresent(self);
    const HRESULT hr = Orig<decltype(&HookPresent)>(g_present)(self, syncInterval, flags);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* self, UINT syncInterval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    OnPresent(self);
    const HRESULT hr = Orig<decltype(&HookPresent1)>(g_present1)(self, syncInterval, flags, params);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count, UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags) {
    static std::atomic<uint64_t> calls{0};
    xr::OnResizeBuffers();  // drop cached back-buffer pointers and the game's frame image
    const HRESULT hr = Orig<decltype(&HookResizeBuffers)>(g_resize)(self, count, width, height, format, flags);
    const uint64_t n = calls.fetch_add(1) + 1;
    if (n <= 20 || (n % 10000) == 0)  // the game can retry this in a tight loop; keep the log readable
        LOG("ResizeBuffers #%llu %p count=%u %ux%u format=%d flags=0x%x -> 0x%08lx", n, self, count, width, height,
            format, flags, hr);
    g_loggedSwapChain.store(nullptr);
    return hr;
}
void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* self, UINT count,
                                               ID3D12CommandList* const* lists) {
    // The swap chain's queue is not reachable through the swap chain here (GetDevice
    // fails for the game's chain), so remember the last direct queue that submitted
    // on this thread: on the render thread that is the queue that drew the frame.
    if (self->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_lastDirectQueue = self;
    patcher::OnExecute(count, lists);
    if (stereo::Active() &&
        stereo::OnExecute(self, count, lists, Orig<stereo::ExecuteFn>(g_execute))) {
        return;
    }
    Orig<decltype(&HookExecuteCommandLists)>(g_execute)(self, count, lists);
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

HMODULE SystemModule(const wchar_t* name) {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    return LoadLibraryW((std::wstring(sysdir) + L"\\" + name).c_str());
}

ComPtr<ID3D12Device> CreateDummyDevice() {
    // The system DLL directly, so we see the real implementation vtables rather
    // than a wrapper (Streamline's interposer, overlays) loaded in-process.
    HMODULE d3d12 = SystemModule(L"d3d12.dll");
    auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    ComPtr<ID3D12Device> device;
    if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return nullptr;
    return device;
}

}  // namespace

bool InstallEarly() {
    ComPtr<ID3D12Device> device = CreateDummyDevice();
    if (!device) {
        LOG("InstallEarly: dummy device failed");
        return false;
    }
    void** dv = *reinterpret_cast<void***>(device.Get());
    bool ok = PatchSlot(dv, kSlotCreateCommittedResource, reinterpret_cast<void*>(&HookCreateCommittedResource), g_ccr) &&
              PatchSlot(dv, kSlotCreatePlacedResource, reinterpret_cast<void*>(&HookCreatePlacedResource), g_cpr) &&
              PatchSlot(dv, kSlotCreateRenderTargetView, reinterpret_cast<void*>(&HookCreateRenderTargetView), g_crtv);
    if (ok) ok = PatchSlot(dv, kSlotCreateGraphicsPipelineState, reinterpret_cast<void*>(&HookCreateGraphicsPipelineState), g_graphicsPipeline);
    ComPtr<ID3D12Device2> d2;
    if (ok && SUCCEEDED(device.As(&d2)))
        ok = PatchSlot(*reinterpret_cast<void***>(d2.Get()), kSlotCreatePipelineState,
                       reinterpret_cast<void*>(&HookCreatePipelineState), g_pipelineStream);
    ComPtr<ID3D12Device4> d4;
    ComPtr<ID3D12Device8> d8;
    ComPtr<ID3D12Device10> d10;
    if (ok && SUCCEEDED(device.As(&d4)))
        ok = PatchSlot(dv, kSlotCreateCommittedResource1, reinterpret_cast<void*>(&HookCreateCommittedResource1), g_ccr1);
    if (ok && SUCCEEDED(device.As(&d8)))
        ok = PatchSlot(dv, kSlotCreateCommittedResource2, reinterpret_cast<void*>(&HookCreateCommittedResource2), g_ccr2) &&
             PatchSlot(dv, kSlotCreatePlacedResource1, reinterpret_cast<void*>(&HookCreatePlacedResource1), g_cpr1);
    if (ok && SUCCEEDED(device.As(&d10)))
        ok = PatchSlot(dv, kSlotCreateCommittedResource3, reinterpret_cast<void*>(&HookCreateCommittedResource3), g_ccr3) &&
             PatchSlot(dv, kSlotCreatePlacedResource2, reinterpret_cast<void*>(&HookCreatePlacedResource2), g_cpr2);

    // Self-test: our own small upload buffers must reach the hooks. A wrong slot
    // index would show up here instead of corrupting the game later.
    g_selfTestHits.store(0);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    int expected = 0;
    {
        ComPtr<ID3D12Resource> r;
        if (SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&r))))
            ++expected;
    }
    if (d4) {
        ComPtr<ID3D12Resource> r;
        if (SUCCEEDED(d4->CreateCommittedResource1(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, nullptr, IID_PPV_ARGS(&r))))
            ++expected;
    }
    if (d8) {
        D3D12_RESOURCE_DESC1 rd1{};
        memcpy(&rd1, &rd, sizeof(rd));
        ComPtr<ID3D12Resource> r;
        if (SUCCEEDED(d8->CreateCommittedResource2(&hp, D3D12_HEAP_FLAG_NONE, &rd1, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, nullptr, IID_PPV_ARGS(&r))))
            ++expected;
    }
    {
        D3D12_HEAP_DESC heapDesc{};
        heapDesc.SizeInBytes = 65536;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        ComPtr<ID3D12Heap> heap;
        ComPtr<ID3D12Resource> r;
        if (SUCCEEDED(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap))) &&
            SUCCEEDED(device->CreatePlacedResource(heap.Get(), 0, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&r))))
            ++expected;
    }
    const int hits = g_selfTestHits.load();
    LOG("InstallEarly: device vtable %p, patched=%d, interfaces d4=%d d8=%d d10=%d, self-test %d/%d", dv, ok ? 1 : 0,
        d4 ? 1 : 0, d8 ? 1 : 0, d10 ? 1 : 0, hits, expected);
    if (!ok || hits != expected) {
        LOG("InstallEarly: self-test failed, removing device hooks");
        RestoreSlot(g_pipelineStream);
        RestoreSlot(g_graphicsPipeline);
        RestoreSlot(g_crtv);
        RestoreSlot(g_cpr2);
        RestoreSlot(g_ccr3);
        RestoreSlot(g_cpr1);
        RestoreSlot(g_ccr2);
        RestoreSlot(g_ccr1);
        RestoreSlot(g_cpr);
        RestoreSlot(g_ccr);
        return false;
    }

    // Command list vtable, patched later in InstallLate.
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                            IID_PPV_ARGS(&list)))) {
        g_cmdListVtable = *reinterpret_cast<void***>(list.Get());
        list->Close();
    }
    return true;
}

bool InstallLate(bool enableXr) {
    g_xrEnabled = enableXr;
    HMODULE dxgi = SystemModule(L"dxgi.dll");
    using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto createFactory = dxgi ? reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    ComPtr<ID3D12Device> device = CreateDummyDevice();
    if (!createFactory || !device) {
        LOG("InstallLate: missing dxgi or device");
        return false;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PC2VR_Dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
                                wc.hInstance, nullptr);

    bool ok = false;
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IDXGISwapChain1> swapChain;
        HRESULT hr = createFactory(0, IID_PPV_ARGS(&factory));
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 16;
        sd.Height = 16;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &swapChain);
        if (FAILED(hr)) {
            LOG("InstallLate: dummy swapchain failed hr=0x%08lx", hr);
        } else {
            void** sv = *reinterpret_cast<void***>(swapChain.Get());
            void** qv = *reinterpret_cast<void***>(queue.Get());
            ok = PatchSlot(sv, kSlotPresent, reinterpret_cast<void*>(&HookPresent), g_present) &&
                 PatchSlot(sv, kSlotPresent1, reinterpret_cast<void*>(&HookPresent1), g_present1) &&
                 PatchSlot(sv, kSlotResizeBuffers, reinterpret_cast<void*>(&HookResizeBuffers), g_resize) &&
                 PatchSlot(qv, kSlotExecuteCommandLists, reinterpret_cast<void*>(&HookExecuteCommandLists), g_execute);
            if (ok && g_cmdListVtable) {
                ok = PatchSlot(g_cmdListVtable, kSlotSetComputeRootCbv, reinterpret_cast<void*>(&HookSetComputeRootCbv),
                               g_setComputeCbv) &&
                     PatchSlot(g_cmdListVtable, kSlotSetGraphicsRootCbv,
                               reinterpret_cast<void*>(&HookSetGraphicsRootCbv), g_setGraphicsCbv) &&
                     PatchSlot(g_cmdListVtable, kSlotListReset, reinterpret_cast<void*>(&HookListReset), g_listReset);
            }
            if (ok && g_cmdListVtable)
                ok = PatchSlot(g_cmdListVtable, kSlotCopyTextureRegion, reinterpret_cast<void*>(&HookCopyTextureRegion), g_copyTex) &&
                     PatchSlot(g_cmdListVtable, kSlotCopyResource, reinterpret_cast<void*>(&HookCopyResource), g_copyRes) &&
                     PatchSlot(g_cmdListVtable, kSlotResourceBarrier, reinterpret_cast<void*>(&HookResourceBarrier), g_barrier) &&
                     PatchSlot(g_cmdListVtable, kSlotOMSetRenderTargets, reinterpret_cast<void*>(&HookOMSetRenderTargets), g_omrt);
            if (ok && g_cmdListVtable)
                ok = PatchSlot(g_cmdListVtable, kSlotSetPipelineState, reinterpret_cast<void*>(&HookSetPipelineState), g_setPipeline) &&
                     PatchSlot(g_cmdListVtable, kSlotExecuteIndirect, reinterpret_cast<void*>(&HookExecuteIndirect), g_indirect);
            LOG("InstallLate: swapchain vtable %p, queue vtable %p, cmdlist vtable %p, patched=%d", sv, qv,
                g_cmdListVtable, ok ? 1 : 0);
        }
    }
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

void Uninstall() {
    for (Patch* p : {&g_indirect, &g_setPipeline, &g_pipelineStream, &g_graphicsPipeline, &g_omrt, &g_crtv, &g_barrier, &g_copyRes, &g_copyTex, &g_listReset, &g_setGraphicsCbv, &g_setComputeCbv, &g_execute, &g_resize, &g_present1, &g_present, &g_cpr2,
                     &g_ccr3, &g_cpr1, &g_ccr2, &g_ccr1, &g_cpr, &g_ccr})
        RestoreSlot(*p);
}

}  // namespace pc2vr::hooks
