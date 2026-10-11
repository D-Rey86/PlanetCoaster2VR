#include "vrs.h"

#include <windows.h>
#include <d3d12.h>
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

using Microsoft::WRL::ComPtr;

namespace pc2vr::vrs {
namespace {

std::atomic<bool> g_on{false};
float g_innerTan = 0.f, g_middleTan = 0.f;  // radii as tan of the angle from the eye's centre
float g_innerDeg = 25.f, g_middleDeg = 40.f;

std::mutex g_listMutex;
std::unordered_map<ID3D12GraphicsCommandList*, ComPtr<ID3D12GraphicsCommandList5>> g_lists5;
std::unordered_map<ID3D12GraphicsCommandList*, bool> g_applied;  // VRS set since the list's last reset

std::atomic<ID3D12Resource*> g_image{nullptr};  // bound by the hooks once filled
std::atomic<uint32_t> g_sceneW{0}, g_sceneH{0};

constexpr int kRing = 3;
struct Gpu {
    bool checked = false, supported = false, rate4x4 = false;
    UINT tile = 16;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> image;
    uint32_t tw = 0, th = 0, pitch = 0, sceneW = 0, sceneH = 0;
    ComPtr<ID3D12Resource> upload[kRing][2];   // per ring slot and eye
    float uploadKey[kRing][2][8] = {};         // inputs each pattern was built from
    bool uploadValid[kRing][2] = {};
    ComPtr<ID3D12CommandAllocator> alloc[kRing][2];
    ComPtr<ID3D12GraphicsCommandList> list[2];
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0, slotValue[kRing][2] = {};
    int slot = 0;
    bool imageInSrs = false;
} g;

bool CheckSupport(ID3D12Device* dev) {
    if (g.checked) return g.supported;
    g.checked = true;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) ||
        o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2) {
        LOG("vrs: variable rate shading tier 2 not supported by this GPU/driver; foveated rendering off");
        return false;
    }
    g.tile = o6.ShadingRateImageTileSize;
    g.rate4x4 = o6.AdditionalShadingRatesSupported != FALSE;
    g.supported = true;
    LOG("vrs: tier 2 supported, shading-rate tile %u px, outer rate %s", g.tile, g.rate4x4 ? "4x4" : "2x2");
    return true;
}

// The shading-rate image for this scene size (recreated when it changes).
bool EnsureImage(ID3D12Device* dev, uint32_t w, uint32_t h) {
    if (g.image && g.sceneW == w && g.sceneH == h) return true;
    g_image.store(nullptr);  // lists recorded from now on do not bind the old one
    // Lists already recorded (or still on the GPU) may use the old image: kept alive.
    static std::vector<ComPtr<ID3D12Resource>> retired;
    if (g.image) retired.push_back(g.image);
    g.image.Reset();
    g.tw = (w + g.tile - 1) / g.tile;
    g.th = (h + g.tile - 1) / g.tile;
    g.pitch = (g.tw + 255) & ~255u;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g.tw;
    d.Height = g.th;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8_UINT;
    d.SampleDesc = {1, 0};
    const D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g.image))))
        return false;
    g.imageInSrs = false;
    D3D12_RESOURCE_DESC b{};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = static_cast<UINT64>(g.pitch) * g.th;
    b.Height = 1;
    b.DepthOrArraySize = 1;
    b.MipLevels = 1;
    b.SampleDesc = {1, 0};
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    for (auto& slot : g.upload)
        for (auto& u : slot) {
            u.Reset();
            if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&u))))
                return false;
        }
    memset(g.uploadValid, 0, sizeof(g.uploadValid));
    g.sceneW = w;
    g.sceneH = h;
    LOG("vrs: shading-rate image %ux%u tiles for a %ux%u scene", g.tw, g.th, w, h);
    return true;
}

// Fills eye `eye`'s pattern: full rate within inner of the eye's centre, 2x2 out to
// middle, 4x4 beyond where supported (otherwise2x2). Distances in tan space from the centre (cheap; close to the
// angle near the centre, conservative further out).
void FillPattern(int slot, int eye, const float frustum[4], const float center[2], bool fullRate) {
    uint8_t* p = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(g.upload[slot][eye]->Map(0, &none, reinterpret_cast<void**>(&p)))) return;
    const float l = frustum[0], r = frustum[1], u = frustum[2], dn = frustum[3];
    const float in2 = g_innerTan * g_innerTan, mid2 = g_middleTan * g_middleTan;
    for (uint32_t y = 0; y < g.th; ++y) {
        const float ty = u + (dn - u) * ((y + 0.5f) * g.tile / g.sceneH);
        for (uint32_t x = 0; x < g.tw; ++x) {
            const float tx = l + (r - l) * ((x + 0.5f) * g.tile / g.sceneW);
            const float dx = tx - center[0], dy = ty - center[1];
            const float d2 = dx * dx + dy * dy;
            p[y * g.pitch + x] = static_cast<uint8_t>(fullRate || d2 < in2 ? D3D12_SHADING_RATE_1X1
                                                     : d2 < mid2 ? D3D12_SHADING_RATE_2X2
                                                                 : (g.rate4x4 ? D3D12_SHADING_RATE_4X4 : D3D12_SHADING_RATE_2X2));
        }
    }
    const D3D12_RANGE written{0, static_cast<SIZE_T>(g.pitch) * g.th};
    g.upload[slot][eye]->Unmap(0, &written);
}

bool InitLists(ID3D12Device* dev) {
    if (g.fence) return true;
    for (auto& slot : g.alloc)
        for (auto& a : slot)
            if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    for (int e = 0; e < 2; ++e) {
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0][e].Get(), nullptr, IID_PPV_ARGS(&g.list[e]))))
            return false;
        g.list[e]->Close();
    }
    return SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
}

}  // namespace

void Configure(bool on, float innerDeg, float middleDeg) {
    g_innerDeg = std::max(5.f, innerDeg);
    g_middleDeg = std::max(g_innerDeg + 1.f, middleDeg);
    g_innerTan = std::tan(g_innerDeg * 0.0174533f);
    g_middleTan = std::tan(g_middleDeg * 0.0174533f);
    g_on.store(on);
    LOG("vrs: foveated rendering %s (full rate within %.0f deg, 2x2 to %.0f deg, outer rate selected by GPU support)", on ? "on" : "off", g_innerDeg,
        g_middleDeg);
}

void SetEnabled(bool on) { g_on.store(on); }

namespace {
wchar_t g_ini[MAX_PATH] = {};
FILETIME g_iniTime{};
ULONGLONG g_lastPoll = 0;
}  // namespace

void WatchIni(const wchar_t* iniPath) {
    wcsncpy_s(g_ini, iniPath, _TRUNCATE);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
}

void PollSettings() {
    if (!g_ini[0]) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastPoll < 1000) return;
    g_lastPoll = now;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa) || CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) == 0) return;
    g_iniTime = fa.ftLastWriteTime;
    Configure(GetPrivateProfileIntW(L"Rendering", L"Foveated", 1, g_ini) == 1,
              static_cast<float>(GetPrivateProfileIntW(L"Rendering", L"FoveatedInnerDeg", 25, g_ini)),
              static_cast<float>(GetPrivateProfileIntW(L"Rendering", L"FoveatedMiddleDeg", 40, g_ini)));
}
bool Enabled() { return g_on.load(); }

void OnListReset(ID3D12GraphicsCommandList* list) {
    std::lock_guard<std::mutex> lock(g_listMutex);
    auto it = g_applied.find(list);
    if (it != g_applied.end()) it->second = false;
}

namespace {
// The game's UI passes, which must stay at full rate (in windowed VR mode the UI is the
// scene's size; measured 2026-10-03: HUD text at the image edges was shaded 4x4).
// From RenderDoc captures of the menu and the park: the UI library (Coherent) draws
// into R8G8B8A8_UNORM targets with a D32S8 depth-stencil bound (stencil clipping), and
// its surfaces are composited into the UI layer through an sRGB view. No scene pass
// binds an R8G8B8A8_UNORM target together with depth; the one scene-size pass through
// an sRGB view is a single full-screen quad.
bool IsUiPass(const rtvreg::Info& rt0, bool hasDepth) {
    switch (rt0.format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return hasDepth;
        default:
            return false;
    }
}
}  // namespace

void OnSetRenderTargets(ID3D12GraphicsCommandList* list, const rtvreg::Info* rt0, bool hasDepth) {
    if (!list) return;
    ID3D12Resource* image = g_on.load(std::memory_order_relaxed) ? g_image.load(std::memory_order_relaxed) : nullptr;
    bool scene = false;
    if (image && rt0)
        scene = rt0->width == g_sceneW.load(std::memory_order_relaxed) && rt0->height == g_sceneH.load(std::memory_order_relaxed) &&
                !IsUiPass(*rt0, hasDepth);
    ID3D12GraphicsCommandList5* l5 = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        bool& applied = g_applied[list];
        if (!scene && !applied) return;  // nothing set on this list: leave it untouched
        auto it = g_lists5.find(list);
        if (it == g_lists5.end()) {
            ComPtr<ID3D12GraphicsCommandList5> q;
            list->QueryInterface(IID_PPV_ARGS(&q));
            it = g_lists5.emplace(list, q).first;
        }
        l5 = it->second.Get();
        if (!l5) return;
        applied = scene;
    }
    if (scene) {
        const D3D12_SHADING_RATE_COMBINER combiners[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                          D3D12_SHADING_RATE_COMBINER_OVERRIDE};
        l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
        l5->RSSetShadingRateImage(image);
    } else {  // a pass of another size (UI, post, shadows): back to full rate
        l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
        l5->RSSetShadingRateImage(nullptr);
    }
}

void PrepareEye(ID3D12CommandQueue* queue, int eye, uint32_t sceneW, uint32_t sceneH, const float frustum[4],
                const float center[2], bool fullRate) {
    if (!queue || eye < 0 || eye > 1 || !sceneW || !sceneH) return;
    if (!g.device && FAILED(queue->GetDevice(IID_PPV_ARGS(&g.device)))) return;
    if (!CheckSupport(g.device.Get())) {
        g_on.store(false);
        return;
    }
    if (!g_on.load()) return;
    if (!InitLists(g.device.Get()) || !EnsureImage(g.device.Get(), sceneW, sceneH)) return;
    g_sceneW.store(sceneW);
    g_sceneH.store(sceneH);
    const float key[8] = {frustum[0], frustum[1], frustum[2], frustum[3], center[0], center[1], g_innerTan + 10.f * g_middleTan,
                          fullRate ? 1.f : 0.f};
    if (eye == 0) g.slot = (g.slot + 1) % kRing;
    const int s = g.slot;
    if (g.fence->GetCompletedValue() < g.slotValue[s][eye]) return;  // still in use: this eye keeps the last pattern
    // This slot's earlier copy has finished (checked above): its upload buffer can be
    // refilled when the inputs changed (every frame with eye tracking).
    bool changed = !g.uploadValid[s][eye];
    for (int i = 0; i < 8 && !changed; ++i) changed = std::fabs(key[i] - g.uploadKey[s][eye][i]) > 1e-3f;
    if (changed) {
        FillPattern(s, eye, frustum, center, fullRate);
        memcpy(g.uploadKey[s][eye], key, sizeof(key));
        g.uploadValid[s][eye] = true;
    }
    g.alloc[s][eye]->Reset();
    ID3D12GraphicsCommandList* cl = g.list[eye].Get();
    cl->Reset(g.alloc[s][eye].Get(), nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g.image.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    if (g.imageInSrs) cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{g.image.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{g.upload[s][eye].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UINT, g.tw, g.th, 1, g.pitch};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &b);
    cl->Close();
    ID3D12CommandList* run[] = {cl};
    queue->ExecuteCommandLists(1, run);
    g.slotValue[s][eye] = ++g.fenceValue;
    queue->Signal(g.fence.Get(), g.fenceValue);
    g.imageInSrs = true;
    g_image.store(g.image.Get());
}

}  // namespace pc2vr::vrs
