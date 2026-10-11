#include "cb_patcher.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "cb_math.h"
#include "log.h"
#include "upload_registry.h"
#include "upload_read.h"
#include "view_cb.h"
#include "xr_runtime.h"

namespace pc2vr::patcher {
namespace {

using cbmath::Rows;

std::atomic<bool> g_enabled{true};

// Per command list: root CBV addresses bound since its last Reset.
std::mutex g_listMutex;
std::unordered_map<ID3D12GraphicsCommandList*, std::vector<uint64_t>> g_lists;

// Borrow capacity into a call-owned vector. A nested submission on this thread
// borrows an empty cache instead of clearing its caller's live address list.
struct AddressScratch {
    std::vector<uint64_t> values;
    static std::vector<uint64_t>& Cache() {
        static thread_local std::vector<uint64_t> cache;
        return cache;
    }
    AddressScratch() { values.swap(Cache()); values.clear(); }
    ~AddressScratch() { values.swap(Cache()); }
    AddressScratch(const AddressScratch&) = delete;
    AddressScratch& operator=(const AddressScratch&) = delete;
};

// Render-thread state (OnExecute is only called on the game's submit thread).
//
// Measured in the menu and park (ViewDiag): one game frame binds 6-11 copies of
// the main view CB. They share the camera rows but differ in per-pass rows
// (e.g. 97.xy tile counts), and one same-sized variant is not a camera buffer
// at all (rows 0-3 hold other data). So each copy is validated on its own, and
// only the camera rows are rewritten, from that copy's own values.
struct FrameState {
    bool valid = false;
    float key[22] = {};  // rows 4-7, 84.xyz and 97.zw (jitter) of the game's buffer
    cbmath::Basis basis{};
    cbmath::Fov fov{};
    float rows0to7[8][4] = {};  // what we wrote this frame (all copies get the same)
};
FrameState g_frame;
float g_prevRows[8][4];
bool g_havePrev = false;
float g_mainArea = 0.f;
uint64_t g_nonFinite = 0;
bool g_writeEnabled = true;
bool g_useHeadsetFov = true;
bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FrameKey(const float (*rows)[4], float* key) {
    memcpy(key, rows[4], 16 * 4);
    memcpy(key + 16, rows[viewcb::kRowCameraPos], 12);
    key[19] = rows[97][2];
    key[20] = rows[97][3];
    key[21] = rows[viewcb::kRowRenderSize][0];
}

bool NearlyEqual(const float* a, const float* b, int n) {
    for (int i = 0; i < n; ++i)
        // Written so that NaN or infinity fails the comparison (a NaN-filled copy once
        // passed validation, was rebuilt into NaN rows, and hung the GPU).
        if (!(std::fabs(a[i] - b[i]) <= 1e-4f + 1e-4f * std::fabs(b[i]))) return false;
    return true;
}

// True when rows 0-7 and the basis follow the standard camera layout, i.e. the
// rebuild with the buffer's own camera reproduces them (see cb_math_test).
bool IsStandardCamera(const Rows& rows, const cbmath::ParsedView& v) {
    Rows check;
    memcpy(check, rows, sizeof(check));
    cbmath::Rebuild(v, v.basis, v.fov, nullptr, check);
    return NearlyEqual(check[0], rows[0], 32);
}

// Rows written by cbmath::Rebuild; everything else keeps the copy's own values.
void WriteCameraRows(uint8_t* cpu, const Rows& rows) {
    SafeCopy(cpu, rows[0], 16 * 16);                       // 0-15
    SafeCopy(cpu + 79 * 16, rows[79], 5 * 16);             // 79-83
    SafeCopy(cpu + 86 * 16, rows[86], 7 * 16);             // 86-92
    SafeCopy(cpu + 95 * 16 + 8, &rows[95][2], 8);          // 95.zw
    SafeCopy(cpu + 99 * 16 + 12, &rows[99][3], 4);         // 99.w
}
// Upload heaps are write-combined: CPU reads are uncached and slow. Every root CBV of
// a submission was being read in full (1.7 KB each, hundreds per frame), measured as
// ~20 ms per frame with VR on. So row 17 (size and its reciprocals, distinctive) is
// read first, and the whole buffer only for a plausible view of at least minArea.
bool IsViewCb(const float (*rows)[4]);
bool ReadView(const uint8_t* cpu, Rows& rows, float minArea, bool writeCombined) {
    float size[4];
    if (!cpu || !uploads::ReadSnapshot(size, cpu + viewcb::kRowRenderSize * 16, sizeof(size), writeCombined)) return false;
    if (!(size[0] >= 16.f && size[1] >= 16.f && size[0] <= 16384.f && size[1] <= 16384.f)) return false;
    if (std::fabs(size[2] * size[0] - 1.f) > 1e-4f || std::fabs(size[3] * size[1] - 1.f) > 1e-4f) return false;
    if (size[0] * size[1] < minArea) return false;
    return uploads::ReadSnapshot(rows, cpu, sizeof(rows), writeCombined) && IsViewCb(rows);
}

bool IsViewCb(const float (*rows)[4]) {
    const float w = rows[viewcb::kRowRenderSize][0], h = rows[viewcb::kRowRenderSize][1];
    if (!(w >= 16.f && h >= 16.f && w <= 16384.f && h <= 16384.f)) return false;
    if (std::fabs(rows[viewcb::kRowRenderSize][2] * w - 1.f) > 1e-4f) return false;
    if (std::fabs(rows[viewcb::kRowRenderSize][3] * h - 1.f) > 1e-4f) return false;
    const float fov = rows[viewcb::kRowFov][3];
    return fov > 0.1f && fov < 3.0f;
}

// The main view's camera copies of the current submission, read once (OnExecute) and
// shared with the stereo module (MainViewCopies), since every read of upload memory
// is slow.
struct CachedCopy {
    uint8_t* cpu;
    Rows rows;
};
std::vector<CachedCopy> g_cache;
ID3D12CommandList* const* g_cacheLists = nullptr;
uint32_t g_cacheCount = 0;

void PatchRows(uint8_t* cpu, const Rows& rows) {
    const float area = rows[viewcb::kRowRenderSize][0] * rows[viewcb::kRowRenderSize][1];
    // g_mainArea is the largest view of this submission (set in OnExecute). It used to
    // be the largest ever seen, which locked out the main view once its size dropped
    // (DLSS renders below the window size: stereo fell back to mono after a second).
    if (area < g_mainArea) return;  // probe / shadow / secondary views stay as the game made them
    if (area < 512.f * 288.f) return;  // only a probe or shadow view seen so far, not the main view

    // A copy we already rewrote this frame (bound again by a later submission).
    if (g_frame.valid && NearlyEqual(rows[4], g_frame.rows0to7[4], 16)) return;

    const cbmath::ParsedView view = cbmath::Parse(rows);
    if (!IsStandardCamera(rows, view)) {
        return;
    }

    float key[22];
    FrameKey(rows, key);
    if (!g_frame.valid || memcmp(key, g_frame.key, sizeof(key)) != 0) {
        // First copy of a new game frame: one head pose for the whole frame.
        xr::RenderView rv;
        if (!xr::AcquireRenderView(view, rv)) {
            g_frame.valid = false;
            g_havePrev = false;
            return;
        }
        if (g_frame.valid) {
            memcpy(g_prevRows, g_frame.rows0to7, sizeof(g_prevRows));
            g_havePrev = true;
        }
        memcpy(g_frame.key, key, sizeof(key));
        g_frame.basis = rv.basis;
        g_frame.fov = rv.fov;
        g_frame.valid = true;
    }

    Rows out;
    memcpy(out, rows, sizeof(out));
    cbmath::Rebuild(view, g_frame.basis, g_useHeadsetFov ? g_frame.fov : view.fov, g_havePrev ? g_prevRows : nullptr, out);
    memcpy(g_frame.rows0to7, out[0], sizeof(g_frame.rows0to7));
    for (int r = 0; r < cbmath::kRows; ++r)
        for (int k = 0; k < 4; ++k)
            if (!std::isfinite(out[r][k])) {
                if (++g_nonFinite < 20) LOG("patcher: non-finite value in rebuilt row %d.%d; copy left unpatched", r, k);
                return;
            }
    if (g_writeEnabled && !xr::EngineView()) WriteCameraRows(cpu, out);
}
}  // namespace

void OnCommandListReset(ID3D12GraphicsCommandList* list) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_listMutex);
    auto it = g_lists.find(list);
    if (it != g_lists.end()) it->second.clear();
}

void OnRootCbvBound(ID3D12GraphicsCommandList* list, uint64_t gpuVa) {
    if (!g_enabled.load(std::memory_order_relaxed) || (gpuVa & 0xFF) != 0) return;
    if (!uploads::Translate(gpuVa, viewcb::kSize)) return;
    std::lock_guard<std::mutex> lock(g_listMutex);
    auto& v = g_lists[list];
    if (v.empty() || v.back() != gpuVa) v.push_back(gpuVa);
}

void OnPresent(uint64_t) {
    // Engine view never rewrites the buffers, so "a copy we already rewrote" cannot
    // happen; but a camera that does not move (and, with DLSS, unchanged unjittered
    // rows) looked like it, so no head pose was taken and stereo fell back to mono.
    // Every frame starts fresh.
    if (xr::EngineView()) g_frame.valid = false;
}

namespace {
}  // namespace

void OnExecute(uint32_t count, ID3D12CommandList* const* lists) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (!xr::IsRunning()) return;
    // Cache capacity, never addresses or camera contents. Every submission is
    // rebuilt from the current Reset-scoped records and validated as before.
    AddressScratch scratch;
    auto& vas = scratch.values;
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        for (uint32_t i = 0; i < count; ++i) {
            auto it = g_lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (it == g_lists.end()) continue;
            vas.insert(vas.end(), it->second.begin(), it->second.end());
        }
    }
    std::sort(vas.begin(), vas.end());
    vas.erase(std::unique(vas.begin(), vas.end()), vas.end());
    if (!xr::IsRunning()) return;
    g_mainArea = 0.f;
    g_cache.clear();
    g_cacheLists = lists;
    g_cacheCount = count;
    for (uint64_t va : vas) {
        // Camera buffers only: with DLSS a same-layout buffer at the output size (not a
        // camera) is larger than the real main view, which renders below it.
        bool writeCombined = false;
        uint8_t* cpu = const_cast<uint8_t*>(uploads::Translate(va, viewcb::kSize, &writeCombined));
        CachedCopy cc{cpu, {}};
        if (!ReadView(cpu, cc.rows, 512.f * 288.f, writeCombined)) continue;
        if (!IsStandardCamera(cc.rows, cbmath::Parse(cc.rows))) continue;
        g_mainArea = std::max(g_mainArea, cc.rows[viewcb::kRowRenderSize][0] * cc.rows[viewcb::kRowRenderSize][1]);
        if (g_cache.size() < 32) g_cache.push_back(cc);
    }
    for (const CachedCopy& cc : g_cache)
        if (cc.rows[viewcb::kRowRenderSize][0] * cc.rows[viewcb::kRowRenderSize][1] >= g_mainArea) PatchRows(cc.cpu, cc.rows);
}

int MainViewCopies(uint32_t count, ID3D12CommandList* const* lists, uint8_t** out, const float (**rows)[4], int max) {
    if (lists != g_cacheLists || count != g_cacheCount) return -1;  // not the submission OnExecute just read
    // Every copy of the main camera, whatever its size. Measured 2026-10-03 with DLSS
    // (xr-sim): the scene renders through 14 copies at the DLSS input size (2132x1988)
    // and one copy at the output size (3672x3424, unjittered) serves the passes after the
    // upscale. Taking only the largest size gave both eyes the same image (mean eye
    // difference 0.0002 against 0.024 with TAA). Same camera = same position, forward
    // and vertical FOV as the largest copy; the 128x128 probe views differ in all three.
    const CachedCopy* ref = nullptr;
    for (const CachedCopy& cc : g_cache)
        if (cc.rows[viewcb::kRowRenderSize][0] * cc.rows[viewcb::kRowRenderSize][1] >= g_mainArea) {
            ref = &cc;
            break;
        }
    if (!ref) return 0;
    auto sameCamera = [&](const Rows& r) {
        return NearlyEqual(r[viewcb::kRowCameraPos], ref->rows[viewcb::kRowCameraPos], 3) &&
               NearlyEqual(r[88], ref->rows[88], 3) && NearlyEqual(&r[viewcb::kRowFov][3], &ref->rows[viewcb::kRowFov][3], 1);
    };
    int n = 0;
    for (const CachedCopy& cc : g_cache)
        if (sameCamera(cc.rows) && n < max) {
            out[n] = cc.cpu;
            rows[n] = cc.rows;
            ++n;
        }
    return n;
}

int MainViewCbs(uint32_t count, ID3D12CommandList* const* lists, uint8_t** out, int max) {
    AddressScratch scratch;
    auto& vas = scratch.values;
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        for (uint32_t i = 0; i < count; ++i) {
            auto it = g_lists.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (it != g_lists.end()) vas.insert(vas.end(), it->second.begin(), it->second.end());
        }
    }
    std::sort(vas.begin(), vas.end());
    vas.erase(std::unique(vas.begin(), vas.end()), vas.end());
    int n = 0;
    for (uint64_t va : vas) {
        bool writeCombined = false;
        uint8_t* cpu = const_cast<uint8_t*>(uploads::Translate(va, viewcb::kSize, &writeCombined));
        Rows rows;
        if (!ReadView(cpu, rows, 512.f * 288.f, writeCombined)) continue;
        if (!IsStandardCamera(rows, cbmath::Parse(rows))) continue;  // same-sized buffer that is not a camera
        if (n < max) out[n++] = cpu;
    }
    return n;
}

void SetEnabled(bool on) { g_enabled.store(on); }
void SetWriteEnabled(bool on) { g_writeEnabled = on; }
void SetUseHeadsetFov(bool on) { g_useHeadsetFov = on; }

}  // namespace pc2vr::patcher
