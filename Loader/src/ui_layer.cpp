#include "ui_layer.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "rtv_registry.h"
#include "xr_runtime.h"

using Microsoft::WRL::ComPtr;

namespace pc2vr::ui {
namespace {

// ---- recording time: which list composites into an sRGB RGBA8 target, and the UI
// layer's transitions ----
struct ListRec {
    ID3D12Resource* srgbTarget = nullptr;  // last R8G8B8A8 target bound through an sRGB view
    bool transitioned = false;             // the known UI layer changed state in this list
    bool ambiguous = false;                // ... through a split or per-subresource barrier
    D3D12_RESOURCE_STATES after = D3D12_RESOURCE_STATE_COMMON;
};
std::mutex g_listMutex;
std::unordered_map<ID3D12GraphicsCommandList*, ListRec> g_lists;
std::atomic<ID3D12Resource*> g_known{nullptr};  // the identified UI layer (transitions recorded for it)

// ---- settings ----
std::mutex g_settingsMutex;
PanelSettings g_settings;
wchar_t g_ini[MAX_PATH] = {};
FILETIME g_iniTime{};
ULONGLONG g_lastPoll = 0;

void ReadSettings() {
    PanelSettings p;
    p.on = GetPrivateProfileIntW(L"UI", L"Panel", 1, g_ini) != 0;
    p.widthDeg = std::clamp(static_cast<float>(GetPrivateProfileIntW(L"UI", L"PanelWidthDeg", 55, g_ini)), 20.f, 120.f);
    p.distanceM = std::clamp(static_cast<float>(GetPrivateProfileIntW(L"UI", L"PanelDistanceCm", 150, g_ini)) / 100.f, 0.3f, 10.f);
    p.heightDeg = std::clamp(static_cast<float>(static_cast<int>(GetPrivateProfileIntW(L"UI", L"PanelHeightDeg", -5, g_ini))), -60.f, 60.f);
    p.horizontalDeg = std::clamp(static_cast<float>(static_cast<int>(GetPrivateProfileIntW(L"UI", L"PanelHorizontalDeg", 0, g_ini))), -60.f, 60.f);
    p.followHmd = GetPrivateProfileIntW(L"UI", L"FollowHmd", 0, g_ini) != 0;
    p.markers3d = GetPrivateProfileIntW(L"UI", L"MarkersIn3D", 1, g_ini) != 0;
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings = p;
    }
    LOG("ui: panel %s, %.0f deg wide, %.2f m away, centre %+.0f deg; world markers %s", p.on ? "on" : "off (UI drawn into the 3D image)",
        p.widthDeg, p.distanceM, p.heightDeg, p.markers3d ? "in the 3D view" : "on the panel");
}

// ---- GPU ----
constexpr int kRing = 3;
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;  // downsample into an sRGB target
    ComPtr<ID3D12DescriptorHeap> srvHeap;  // shader visible: kRing slots (the game's UI layer)
    ComPtr<ID3D12DescriptorHeap> rtvHeap;  // [0] panel image, [1] the game's UI layer (for the clear)
    UINT srvStride = 0, rtvStride = 0;
    ComPtr<ID3D12CommandAllocator> alloc[kRing][2];
    ComPtr<ID3D12GraphicsCommandList> list[2];
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t fenceValue = 0, slotValue[kRing][2] = {};
    int slot = 0;
    bool failed = false;

    ComPtr<ID3D12Resource> ui;     // the game's UI layer (reference held while identified)
    ID3D12Resource* rtvFor = nullptr;  // resource the UI-layer RTV was made for
    ComPtr<ID3D12Resource> panel;  // downsampled UI, PIXEL_SHADER_RESOURCE between uses
    uint32_t panelW = 0, panelH = 0;
    // The game's world markers, saved from the UI layer before the HUD goes on (per eye:
    // each eye's run saves and restores its own). COPY_DEST between uses.
    ComPtr<ID3D12Resource> markers[2];
    ComPtr<ID3D12CommandAllocator> markerAlloc[kRing][2];
    ComPtr<ID3D12GraphicsCommandList> markerList[2];
    uint64_t markerSlotValue[kRing][2] = {};
    int markerSlot = 0;
    bool markersSaved[2] = {};
} g;

// This batch's plan.
struct Plan {
    bool valid = false;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;  // the UI layer's state where the batch is split
    uint32_t srcW = 0, srcH = 0;
    // The list that composites the HUD into the UI layer, and the UI layer's state before
    // it (the world markers are in the layer by then); -1 when not known.
    int hudIndex = -1;
    D3D12_RESOURCE_STATES markerState = D3D12_RESOURCE_STATE_COMMON;
} g_plan;
bool g_lastStateKnown = false;
D3D12_RESOURCE_STATES g_lastState = D3D12_RESOURCE_STATE_COMMON;

bool InitGpu(ID3D12CommandQueue* queue) {
    if (g.pso) return true;
    if (g.failed) return false;
    g.failed = true;  // until everything below succeeded
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&g.device)))) return false;
    // Box filter: each panel pixel averages the UI texels it covers (premultiplied alpha,
    // linear light: the source is read through an sRGB view, the target written through
    // one). Bilinear taps spread evenly over the footprint, up to 4x4.
    static const char* kShader = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { float2 texel; float2 footprint; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 PS(V i) : SV_Target {
    const int nx = clamp((int)ceil(footprint.x * 0.5), 1, 4);
    const int ny = clamp((int)ceil(footprint.y * 0.5), 1, 4);
    float4 acc = 0;
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            const float2 o = ((float2(x, y) + 0.5) / float2(nx, ny) - 0.5) * footprint * texel;
            acc += src.SampleLevel(smp, i.uv + o, 0);
        }
    return acc / (nx * ny);
}
)";
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "pc2vr_ui", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "pc2vr_ui", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err))) {
        LOG("ui: shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp{};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSig))))
        return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g.rootSig.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    pd.SampleDesc.Count = 1;
    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.pso)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(g.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g.srvHeap))) ||
        FAILED(g.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g.rtvHeap))))
        return false;
    g.srvStride = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.rtvStride = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (auto& slot : g.alloc)
        for (auto& a : slot)
            if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    for (int e = 0; e < 2; ++e) {
        if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0][e].Get(), nullptr,
                                               IID_PPV_ARGS(&g.list[e]))))
            return false;
        g.list[e]->Close();
    }
    for (auto& slot : g.markerAlloc)
        for (auto& a : slot)
            if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    for (int e = 0; e < 2; ++e) {
        if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.markerAlloc[0][e].Get(), nullptr,
                                               IID_PPV_ARGS(&g.markerList[e]))))
            return false;
        g.markerList[e]->Close();
    }
    if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.failed = false;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Rtv(int i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * g.rtvStride;
    return h;
}

// The panel image at the size the headset can show (recreated when it changes). Only
// called on the first eye, after waiting for the headset's last read of it.
bool EnsurePanel(uint32_t w, uint32_t h) {
    if (g.panel && g.panelW == w && g.panelH == h) return true;
    // The headset's copy of an earlier frame may still read the old image on the GPU:
    // kept alive (sizes change only with the settings or the window).
    static std::vector<ComPtr<ID3D12Resource>> retired;
    if (g.panel) retired.push_back(g.panel);
    g.panel.Reset();
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc = {1, 0};
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 nullptr, IID_PPV_ARGS(&g.panel))))
        return false;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.device->CreateRenderTargetView(g.panel.Get(), &rv, Rtv(0));
    g.panelW = w;
    g.panelH = h;
    LOG("ui: panel image %ux%u", w, h);
    return true;
}

void Transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}

bool IsRgba8(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

}  // namespace

// ------------------------------------------------------------ recording --
void OnSetRenderTargets(ID3D12GraphicsCommandList* list, const rtvreg::Info* rt0) {
    if (!list || !rt0 || rt0->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return;
    std::lock_guard<std::mutex> lock(g_listMutex);
    g_lists[list].srgbTarget = rt0->resource;
}

void OnBarrier(ID3D12GraphicsCommandList* list, unsigned count, const D3D12_RESOURCE_BARRIER* barriers) {
    ID3D12Resource* known = g_known.load(std::memory_order_relaxed);
    if (!known || !barriers) return;
    for (unsigned i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Transition.pResource != known) continue;
        std::lock_guard<std::mutex> lock(g_listMutex);
        ListRec& r = g_lists[list];
        r.transitioned = true;
        r.after = b.Transition.StateAfter;
        r.ambiguous = b.Flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY ||
                      b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
}

void OnListReset(ID3D12GraphicsCommandList* list) {
    std::lock_guard<std::mutex> lock(g_listMutex);
    g_lists.erase(list);
}

// ------------------------------------------------------------- settings --
void WatchIni(const wchar_t* iniPath) {
    wcsncpy_s(g_ini, iniPath, _TRUNCATE);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
    ReadSettings();
}

void PollSettings() {
    if (!g_ini[0]) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastPoll < 1000) return;
    g_lastPoll = now;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa) || CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) == 0) return;
    g_iniTime = fa.ftLastWriteTime;
    ReadSettings();
}

PanelSettings Settings() {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

void ApplySettings(const PanelSettings& settings) {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings=settings;
}

// ---------------------------------------------------------------- batch --
int PlanBatch(uint32_t count, ID3D12CommandList* const* lists, ID3D12Resource* frameImage, int compositeIndex) {
    g_plan.valid = false;
    if (!frameImage || compositeIndex <= 0 || static_cast<uint32_t>(compositeIndex) >= count || !Settings().on) return -1;
    // A fixed screen showing the game's own shot (menus, cutscenes, Cinematic and Interest
    // ride cameras): the UI stays in the picture, where the game lines it up with its 3D
    // background. On the panel, the main menu's UI came apart from its globe once the
    // shot was placed where you faced (reported in the headset 2026-10-04).
    if (!xr::UiOnPanel()) return -1;
    ID3D12Resource* ui = nullptr;
    bool known = g_lastStateKnown;
    D3D12_RESOURCE_STATES state = g_lastState;
    int hudIndex = -1;
    bool markerKnown = false;
    D3D12_RESOURCE_STATES markerState = D3D12_RESOURCE_STATE_COMMON;
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        // The UI layer: the sRGB target the lists before the composite render last. That
        // list (the HUD composite) is where the world markers are taken out.
        for (int i = 0; i < compositeIndex; ++i) {
            auto it = g_lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (it != g_lists.end() && it->second.srgbTarget) {
                ui = it->second.srgbTarget;
                hudIndex = i;
            }
        }
        if (ui && ui == g_known.load()) {
            for (int i = 0; i < compositeIndex; ++i) {
                if (i == hudIndex) {
                    markerState = state;
                    markerKnown = known;
                }
                auto it = g_lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
                if (it == g_lists.end() || !it->second.transitioned) continue;
                state = it->second.after;
                known = !it->second.ambiguous;
            }
        }
    }
    if (!ui) return -1;
    const D3D12_RESOURCE_DESC fd = frameImage->GetDesc(), ud = ui->GetDesc();
    if (ud.Width != fd.Width || ud.Height != fd.Height || !IsRgba8(ud.Format) || ud.MipLevels != 1 || ud.DepthOrArraySize != 1)
        return -1;
    if (ui != g_known.load()) {
        // A new UI layer (first sight, or recreated with the window): its transitions are
        // recorded from now on; separated from the next batch whose state is certain.
        g_known.store(ui);
        static std::vector<ComPtr<ID3D12Resource>> retired;  // our last clear of the old one may still be queued
        if (g.ui) retired.push_back(g.ui);
        g.ui = ui;
        g_lastStateKnown = false;
        LOG("ui: UI layer %p (%llux%u format %d), composited by list %d of %u", ui, static_cast<unsigned long long>(ud.Width),
            ud.Height, ud.Format, compositeIndex, count);
        return -1;
    }
    if (!known) return -1;
    g_plan.valid = true;
    g_plan.state = state;
    g_plan.srcW = static_cast<uint32_t>(ud.Width);
    g_plan.srcH = ud.Height;
    g_plan.hudIndex = markerKnown && hudIndex > 0 ? hudIndex : -1;
    g_plan.markerState = markerState;
    g.markersSaved[0] = g.markersSaved[1] = false;
    static int unknownLogs = 0;
    if (g_plan.hudIndex < 0 && ++unknownLogs <= 5)
        LOG("ui: the UI layer's state before the HUD list is not known; this frame's world markers go on the panel");
    return compositeIndex;
}

int MarkerSplit() { return g_plan.valid && Settings().markers3d ? g_plan.hudIndex : -1; }

namespace {
// Per eye, the markers copy: the UI layer's size and format.
bool EnsureMarkers(int eye, ID3D12Resource* ui) {
    const D3D12_RESOURCE_DESC ud = ui->GetDesc();
    if (g.markers[eye]) {
        const D3D12_RESOURCE_DESC md = g.markers[eye]->GetDesc();
        if (md.Width == ud.Width && md.Height == ud.Height && md.Format == ud.Format) return true;
        static std::vector<ComPtr<ID3D12Resource>> retired;  // an earlier copy may still be queued
        retired.push_back(g.markers[eye]);
        g.markers[eye].Reset();
    }
    D3D12_RESOURCE_DESC d = ud;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    return SUCCEEDED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&g.markers[eye])));
}

void EnsureUiRtv(ID3D12Resource* ui) {
    if (g.rtvFor == ui) return;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.device->CreateRenderTargetView(ui, &rv, Rtv(1));
    g.rtvFor = ui;
}
}  // namespace

// Measured (RenderDoc, the unmodded game in the park, 2026-10-04): the path builder's
// preview and guide arrow and the multi-select brush are drawn in 3D into a layer of
// their own, which a compute pass then writes into the UI layer, before the list that
// composites the HUD over it. With the whole UI layer on the panel, they floated with
// it, away from the ground they mark (reported in the headset 2026-10-04).
void SaveMarkers(ID3D12CommandQueue* queue, int eye, ExecuteFn execute) {
    if (!g_plan.valid || g_plan.hudIndex < 0 || eye < 0 || eye > 1 || !InitGpu(queue)) return;
    ID3D12Resource* ui = g.ui.Get();
    if (!ui || !EnsureMarkers(eye, ui)) return;
    EnsureUiRtv(ui);
    if (eye == 0) g.markerSlot = (g.markerSlot + 1) % kRing;
    const int slot = g.markerSlot;
    if (g.fence->GetCompletedValue() < g.markerSlotValue[slot][eye]) return;  // not saved: Separate clears as before
    // Only when Separate will put them back: its slot is free and (first eye) the panel
    // has a size. Otherwise nothing is taken out and the frame goes as before.
    const int sepSlot = eye == 0 ? (g.slot + 1) % kRing : g.slot;
    uint32_t pw = 0, ph = 0;
    if (eye == 0) xr::PanelPixels(Settings().widthDeg, g_plan.srcW, g_plan.srcH, pw, ph);
    if (g.fence->GetCompletedValue() < g.slotValue[sepSlot][eye] || (eye == 0 && (!pw || !ph))) return;
    ID3D12GraphicsCommandList* l = g.markerList[eye].Get();
    g.markerAlloc[slot][eye]->Reset();
    l->Reset(g.markerAlloc[slot][eye].Get(), nullptr);
    // The markers drawn so far are kept aside, and the layer cleared so the HUD list
    // composites the HUD alone.
    const D3D12_RESOURCE_STATES s = g_plan.markerState;
    ID3D12Resource* m = g.markers[eye].Get();
    Transition(l, ui, s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyResource(m, ui);
    Transition(l, m, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(l, ui, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    l->ClearRenderTargetView(Rtv(1), zero, 0, nullptr);
    Transition(l, ui, D3D12_RESOURCE_STATE_RENDER_TARGET, s);
    l->Close();
    ID3D12CommandList* run[] = {l};
    execute(queue, 1, run);
    g.markerSlotValue[slot][eye] = ++g.fenceValue;
    queue->Signal(g.fence.Get(), g.fenceValue);
    g.markersSaved[eye] = true;
}

void Separate(ID3D12CommandQueue* queue, int eye, ExecuteFn execute) {
    if (!g_plan.valid || eye < 0 || eye > 1 || !InitGpu(queue)) return;
    ID3D12Resource* ui = g.ui.Get();
    EnsureUiRtv(ui);
    if (eye == 0) g.slot = (g.slot + 1) % kRing;
    // Recycle this slot's allocator (used kRing frames ago; normally long finished).
    const uint64_t slotValue = g.slotValue[g.slot][eye];
    for (int tries = 0; g.fence->GetCompletedValue() < slotValue && tries < 20; ++tries) {
        g.fence->SetEventOnCompletion(slotValue, g.event);
        WaitForSingleObject(g.event, 50);
    }
    if (g.fence->GetCompletedValue() < slotValue) {
        // Never clear without capturing: leave this frame's UI in the image.
        g_plan.valid = false;
        return;
    }
    const D3D12_RESOURCE_STATES s = g_plan.state;
    ID3D12GraphicsCommandList* l = g.list[eye].Get();
    g.alloc[g.slot][eye]->Reset();
    l->Reset(g.alloc[g.slot][eye].Get(), nullptr);
    D3D12_RESOURCE_STATES cur = s;
    bool panelDone = false;
    if (eye == 0) {
        // Panel image size: the panel's angular width at the headset's pixel density.
        uint32_t pw = 0, ph = 0;
        xr::PanelPixels(Settings().widthDeg, g_plan.srcW, g_plan.srcH, pw, ph);
        xr::WaitPanelFree(queue);  // the headset may still be reading last frame's panel image
        if (pw && ph && EnsurePanel(pw, ph)) {
            const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (!(cur & read)) {
                Transition(l, ui, cur, read);
                cur = read;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
            srvCpu.ptr += static_cast<SIZE_T>(g.slot) * g.srvStride;
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
            srvGpu.ptr += static_cast<UINT64>(g.slot) * g.srvStride;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            g.device->CreateShaderResourceView(ui, &sv, srvCpu);
            Transition(l, g.panel.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12DescriptorHeap* heaps[] = {g.srvHeap.Get()};
            l->SetDescriptorHeaps(1, heaps);
            l->SetGraphicsRootSignature(g.rootSig.Get());
            l->SetPipelineState(g.pso.Get());
            l->SetGraphicsRootDescriptorTable(0, srvGpu);
            const float c[4] = {1.f / g_plan.srcW, 1.f / g_plan.srcH, static_cast<float>(g_plan.srcW) / pw,
                                static_cast<float>(g_plan.srcH) / ph};
            l->SetGraphicsRoot32BitConstants(1, 4, c, 0);
            const D3D12_VIEWPORT vp{0, 0, static_cast<float>(pw), static_cast<float>(ph), 0, 1};
            const D3D12_RECT sr{0, 0, static_cast<LONG>(pw), static_cast<LONG>(ph)};
            l->RSSetViewports(1, &vp);
            l->RSSetScissorRects(1, &sr);
            const D3D12_CPU_DESCRIPTOR_HANDLE prtv = Rtv(0);
            l->OMSetRenderTargets(1, &prtv, FALSE, nullptr);
            l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            l->DrawInstanced(3, 1, 0, 0);
            Transition(l, g.panel.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            panelDone = true;
        }
        if (!panelDone) {
            // No panel this frame: keep the UI in the image (both eyes).
            l->Close();
            g_plan.valid = false;
            return;
        }
    }
    if (g.markersSaved[eye]) {
        // The HUD is on the panel; the UI layer gets the world markers back, so the
        // display mapper draws them into this eye's 3D image (each eye's own markers,
        // rendered with that eye's camera).
        ID3D12Resource* m = g.markers[eye].Get();
        Transition(l, ui, cur, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(ui, m);
        Transition(l, ui, D3D12_RESOURCE_STATE_COPY_DEST, s);
        Transition(l, m, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g.markersSaved[eye] = false;
    } else {
        // Clear the UI layer: the display mapper then outputs the 3D scene alone.
        Transition(l, ui, cur, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float zero[4] = {0.f, 0.f, 0.f, 0.f};
        l->ClearRenderTargetView(Rtv(1), zero, 0, nullptr);
        Transition(l, ui, D3D12_RESOURCE_STATE_RENDER_TARGET, s);
    }
    l->Close();
    ID3D12CommandList* run[] = {l};
    execute(queue, 1, run);
    g.slotValue[g.slot][eye] = ++g.fenceValue;
    queue->Signal(g.fence.Get(), g.fenceValue);
    if (eye == 0) xr::PanelCaptured(queue, g.panel.Get(), g.panelW, g.panelH);
}

void FinishBatch(uint32_t count, ID3D12CommandList* const* lists) {
    if (!g_known.load()) return;
    std::lock_guard<std::mutex> lock(g_listMutex);
    for (uint32_t i = 0; i < count; ++i) {
        auto it = g_lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (it == g_lists.end() || !it->second.transitioned) continue;
        g_lastState = it->second.after;
        g_lastStateKnown = !it->second.ambiguous;
    }
}

}  // namespace pc2vr::ui
