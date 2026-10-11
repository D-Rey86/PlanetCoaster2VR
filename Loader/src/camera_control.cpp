#include "camera_control.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "game_props.h"
#include "game_state.h"
#include "log.h"

namespace pc2vr::camctl {
namespace {

constexpr float kTwoPi = 6.28318530718f;

bool SafeRead(const void* p, void* out, size_t n);
bool SafeWrite(void* p, const void* in, size_t n);
// Verification nudge: 0.002 rad (0.11 deg) is invisible but far above the
// precision of the yaw recovered from the rendered camera (~1e-6 rad).
constexpr float kNudge = 0.002f;
bool g_resetGuard = true;  // test: [Experiments] CamResetGuard=0 turns off the reload guard (Update)
constexpr float kNudgeTolerance = 0.0005f;
constexpr int kNudgeFrames = 10;      // frames to wait for the rendered camera to follow
constexpr int kSettleFrames = 4;      // frames after restoring before the next candidate
constexpr size_t kMaxCandidates = 6;  // never nudge more places than this

enum class Phase { Idle, Scanning, Verifying, Located };

struct Pose {
    float yaw = 0, pitch = 0;
    float pos[3] = {};
};

Phase g_phase = Phase::Idle;  // render thread only
std::atomic<bool> g_scanDone{false}, g_scanBusy{false};
std::mutex g_scanMutex;
std::vector<float*> g_scanHits;  // guarded by g_scanMutex
bool g_scanByName = false;       // guarded by g_scanMutex: the hits are named records
Pose g_scanPose;                 // pose the last scan looked for
ULONGLONG g_lastScanTick = 0;

std::vector<float*> g_candidates;
bool g_candidatesByName = false;
float g_prevYaw = 0.f, g_prevPitch = 0.f;
int g_stillCount = 0;
int g_matchCount = 0;
bool g_secondStep = false;
size_t g_candidate = 0;
int g_frameInStep = 0;
bool g_nudging = false;
float g_orig[2] = {};

std::atomic<float*> g_ctl{nullptr};
uintptr_t g_ctlKey = 0;  // the controller record's name ("vCameraYPR") when found by name
int g_mismatchFrames = 0;
// The last pair written, and the rendered orientation last frame (Located phase).
float g_lastWrite[2] = {};
bool g_lastWriteValid = false;
Pose g_renderedPrev{};
bool g_notDrawn = false;  // the controller holds our writes but the game is not drawing from it
bool g_foreign = false;   // the game draws another camera (not the frozen pause view): no writes
Pose g_lastDrawn{};       // the camera last drawn from the controller
bool g_lastDrawnValid = false;
int g_foreignLogs = 0;

// Engine field of view (vertical, radians): a tagged property near the controller.
enum class FovPhase { None, Scanning, Verifying, Located, Failed };
std::atomic<bool> g_fovScanDone{false};
std::vector<float*> g_fovScanHits;  // guarded by g_scanMutex
float g_fovScanValue = 0.f;
FovPhase g_fovPhase = FovPhase::None;
std::vector<float*> g_fovCandidates;
size_t g_fovCandidate = 0;
int g_fovFrames = 0;
float g_fovOrig = 0.f;
std::atomic<float*> g_fov{nullptr};
float g_fovGameValue = 0.f;
bool g_fovSecond = false;
int g_fovMatch = 0;  // the game's own FOV before we changed it

constexpr uint32_t kPropertyTag = 0x44;  // uint32 8 bytes before each camera property value (measured)

void ScanTagged(const uint8_t* p, size_t bytes, float value, std::vector<float*>& hits) {
    __try {
        for (size_t i = 8; i + 4 <= bytes; i += 4) {
            const float v = *reinterpret_cast<const float*>(p + i);
            if (!(v > value - 2e-4f && v < value + 2e-4f)) continue;
            if (*reinterpret_cast<const uint32_t*>(p + i - 8) != kPropertyTag) continue;
            hits.push_back(reinterpret_cast<float*>(const_cast<uint8_t*>(p + i)));
            if (hits.size() >= 16) return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

DWORD WINAPI FovScanThread(LPVOID) {
    const float value = g_fovScanValue;
    std::vector<float*> hits;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* addr = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    const uint8_t* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi)) && hits.size() < 16) {
        uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
        const bool rw = (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE));
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && mbi.RegionSize <= (64ull << 20))
            ScanTagged(base, mbi.RegionSize, value, hits);
        addr = base + mbi.RegionSize;
    }
    LOG("camctl: heap scan found %zu tagged FOV value(s)", hits.size());
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_fovScanHits = std::move(hits);
    }
    g_fovScanDone.store(true);
    return 0;
}

void FindFovCandidates(float* ctl, float gameFov) {
    g_fovCandidates.clear();
    uint8_t* base = reinterpret_cast<uint8_t*>(ctl);
    // Seen between -0x270 and +0x2D0 from the yaw pair, sometimes with the 0x44
    // property tag and sometimes without (measured), so only the value is matched
    // here; the verification nudge decides. Read in 64 KB pages, SEH per page.
    uint8_t* aligned = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(base) & ~uintptr_t(0xFFFF));
    for (intptr_t page = -0x100000; page <= 0x100000; page += 0x10000) {
        static float buf[0x10000 / 4];
        if (!SafeRead(aligned + page, buf, sizeof(buf))) continue;  // unmapped 64 KB block
        for (size_t i = 0; i < 0x10000 / 4; ++i)
            if (buf[i] > gameFov - 2e-4f && buf[i] < gameFov + 2e-4f)
                g_fovCandidates.push_back(reinterpret_cast<float*>(aligned + page) + i);
    }
    g_fovCandidate = 0;
    g_fovFrames = 0;
    g_fovOrig = gameFov;
    // Nearest first; verification nudges each by 0.002 rad (invisible) and keeps
    // the one the rendered FOV follows.
    std::sort(g_fovCandidates.begin(), g_fovCandidates.end(), [ctl](float* a, float* b) {
        return std::llabs(reinterpret_cast<intptr_t>(a) - reinterpret_cast<intptr_t>(ctl)) <
               std::llabs(reinterpret_cast<intptr_t>(b) - reinterpret_cast<intptr_t>(ctl));
    });
    if (g_fovCandidates.size() > kMaxCandidates) g_fovCandidates.resize(kMaxCandidates);
    LOG("camctl: %zu engine FOV candidate(s) near the controller (value %.5f)", g_fovCandidates.size(), gameFov);
    if (!g_fovCandidates.empty()) {
        g_fovPhase = FovPhase::Verifying;
        return;
    }
    // Not near the controller this session: tagged values anywhere in the heap.
    g_fovScanValue = gameFov;
    g_fovScanDone.store(false);
    g_fovPhase = FovPhase::Scanning;
    HANDLE h = CreateThread(nullptr, 0, FovScanThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else g_fovPhase = FovPhase::Failed;
}

void UpdateFov(float renderedFov) {
    if (g_fovPhase == FovPhase::Scanning && g_fovScanDone.load()) {
        {
            std::lock_guard<std::mutex> lock(g_scanMutex);
            g_fovCandidates = g_fovScanHits;
        }
        if (g_fovCandidates.size() > kMaxCandidates) g_fovCandidates.resize(kMaxCandidates);
        g_fovCandidate = 0;
        g_fovFrames = 0;
        g_fovPhase = g_fovCandidates.empty() ? FovPhase::Failed : FovPhase::Verifying;
        if (g_fovCandidates.empty()) LOG("camctl: engine FOV not found; the view edges may be culled");
    }
    if (g_fovPhase != FovPhase::Verifying) return;
    if (g_fovCandidate >= g_fovCandidates.size()) {
        g_fovPhase = FovPhase::Failed;
        LOG("camctl: engine FOV not found; the view edges may be culled");
        return;
    }
    float* c = g_fovCandidates[g_fovCandidate];
    if ((reinterpret_cast<uintptr_t>(c) & 15) != 0) {  // always 16-byte aligned in 6 sessions
        ++g_fovCandidate;
        return;
    }
    // Same two-step check as the yaw: follow +nudge, then -nudge, two frames each.
    const float target = g_fovOrig + (g_fovSecond ? -kNudge : kNudge);
    SafeWrite(c, &target, 4);
    g_fovMatch = std::fabs(renderedFov - target) < kNudgeTolerance ? g_fovMatch + 1 : 0;
    if (g_fovMatch >= 2 && !g_fovSecond) {
        g_fovSecond = true;
        g_fovMatch = 0;
        g_fovFrames = 0;
        return;
    }
    if (g_fovMatch >= 2) {
        g_fovSecond = false;
        g_fovMatch = 0;
        SafeWrite(c, &g_fovOrig, 4);
        g_fovGameValue = g_fovOrig;
        g_fov.store(c);
        g_fovPhase = FovPhase::Located;
        LOG("camctl: engine FOV at %p (game value %.4f rad)", c, g_fovOrig);
        return;
    }
    if (++g_fovFrames >= kNudgeFrames) {
        SafeWrite(c, &g_fovOrig, 4);
        g_fovSecond = false;
        g_fovMatch = 0;
        ++g_fovCandidate;
        g_fovFrames = 0;
    }
}

bool SafeRead(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWrite(void* p, const void* in, size_t n) {
    __try {
        memcpy(p, in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float AngleDiff(float a, float b) {
    float d = std::fmod(a - b, kTwoPi);
    if (d > kTwoPi * 0.5f) d -= kTwoPi;
    if (d < -kTwoPi * 0.5f) d += kTwoPi;
    return d;
}

// Scans one region in place (no copy). SEH: a region can be decommitted while we read.
void ScanRegion(const float* f, size_t count, const Pose& t, std::vector<float*>& hits) {
    __try {
        for (size_t i = 0; i + 1 < count; ++i) {
            const float pitch = f[i + 1];
            if (!(pitch > t.pitch - 1e-3f && pitch < t.pitch + 1e-3f)) continue;
            if (!(std::fabs(AngleDiff(f[i], t.yaw)) < 1e-3f)) continue;
            // The controller's pair has always been 16-byte aligned (6 sessions); an
            // unaligned match once passed a single nudge by coincidence and was written to.
            if ((reinterpret_cast<uintptr_t>(f + i) & 15) != 0) continue;
            hits.push_back(const_cast<float*>(f + i));
            if (hits.size() >= 32) return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The park camera's controller is the record named "vCameraYPR" (kind 0xa) whose pair is
// the rendered orientation (measured 2026-10-03: the same address camctl verified by value,
// in xr-sim and in the headset; the main menu and ride cameras have records of that name
// too, which game_state keeps camctl away from). Found by name, the candidates are few and
// specific; the value search over the whole heap is the fallback.
struct NamedScan {
    std::vector<uintptr_t> keys[1];
    Pose t;
    std::vector<float*> hits;
};

void ScanNamed(uint8_t* p, size_t bytes, void* ctx) {
    NamedScan& c = *static_cast<NamedScan*>(ctx);
    __try {
        for (size_t i = 0x18; i + 8 <= bytes && c.hits.size() < kMaxCandidates; i += 16) {  // pair 16-byte aligned
            uintptr_t q;
            memcpy(&q, p + i, 8);
            bool named = false;
            for (uintptr_t k : c.keys[0]) named = named || q == k;
            if (!named) continue;
            const float* f = reinterpret_cast<const float*>(p + i - 0x18);
            uint32_t kind;
            memcpy(&kind, p + i - 0x8, 4);
            if (kind != props::kVector || !(std::fabs(f[1] - c.t.pitch) < 1e-3f) || !(std::fabs(AngleDiff(f[0], c.t.yaw)) < 1e-3f)) continue;
            c.hits.push_back(const_cast<float*>(f));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

DWORD WINAPI ScanThread(LPVOID) {
    const Pose t = g_scanPose;
    const ULONGLONG t0 = GetTickCount64();
    NamedScan named;
    named.t = t;
    const char* const names[] = {"vCameraYPR"};
    props::FindNames(names, 1, named.keys);
    if (!named.keys[0].empty()) props::ForEachHeapRegion(ScanNamed, &named);
    std::vector<float*> hits = std::move(named.hits);
    const bool byName = !hits.empty();
    size_t scanned = 0;
    if (!byName) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uint8_t* addr = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
        const uint8_t* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
        MEMORY_BASIC_INFORMATION mbi;
        while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi)) && hits.size() < 32) {
            uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
            const bool rw = (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE));
            // Heap segments only: the controller has always been in a small private RW
            // region (a few MB); the large regions are GPU upload/readback memory.
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && mbi.RegionSize <= (64ull << 20)) {
                ScanRegion(reinterpret_cast<const float*>(base), mbi.RegionSize / 4, t, hits);
                scanned += mbi.RegionSize;
            }
            addr = base + mbi.RegionSize;
        }
    }
    // Lowest address first: in every session so far the driving copy was the lower one.
    std::sort(hits.begin(), hits.end());
    if (byName)
        LOG("camctl: %zu camera record(s) named vCameraYPR at yaw %.4f pitch %.4f (%llu ms)", hits.size(), t.yaw, t.pitch,
            GetTickCount64() - t0);
    else
        LOG("camctl: scanned %zu MB in %llu ms: %zu yaw/pitch pair(s) for yaw %.4f pitch %.4f", scanned >> 20,
            GetTickCount64() - t0, hits.size(), t.yaw, t.pitch);
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_scanHits = std::move(hits);
        g_scanByName = byName;
    }
    g_scanDone.store(true);
    g_scanBusy.store(false);
    return 0;
}
bool SamePose(const Pose& a, const Pose& b) {
    return std::fabs(AngleDiff(a.yaw, b.yaw)) < 1e-3f && std::fabs(a.pitch - b.pitch) < 1e-3f &&
           std::fabs(a.pos[0] - b.pos[0]) < 1e-2f && std::fabs(a.pos[2] - b.pos[2]) < 1e-2f;
}

void StartScan(const Pose& p) {
    // A pose with yaw and pitch near zero matches countless unrelated zero pairs in
    // memory; never scan (or later nudge) for those.
    if (std::fabs(p.yaw) < 0.02f && std::fabs(p.pitch) < 0.02f) return;
    const ULONGLONG now = GetTickCount64();
    if (g_lastScanTick != 0 && now - g_lastScanTick < 1000) return;
    // Same pose: the same answer, except that a park camera can exist before the game
    // draws from it (just after loading), so it is looked for again every few seconds.
    if (g_lastScanTick != 0 && SamePose(p, g_scanPose) && now - g_lastScanTick < 5000) return;
    if (g_scanBusy.exchange(true)) return;  // a dropped search still running
    g_lastScanTick = now;
    g_scanPose = p;
    g_scanDone.store(false);
    HANDLE h = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (!h) {
        g_scanBusy.store(false);
        return;
    }
    CloseHandle(h);
    g_phase = Phase::Scanning;
}

void NextCandidate() {
    ++g_candidate;
    g_frameInStep = 0;
    g_nudging = false;
}

}  // namespace

// ---- the gamepad camera's orbit pivot ----
// Measured 2026-10-02 (Tools/pivottest.py, virtual Xbox pad): near the pair, the
// controller keeps its position and a pivot point on its view ray. While the pad
// zooms (and orbits), the game re-aims the camera at that pivot. Turning the camera
// by writing the pair alone left the pivot behind, and a zoom then snapped the view
// back toward it (turn 0.6 rad then zoom out: snapped back after 0.7 s); moving the
// pivot onto the new view ray at the same distance removed the snap. So every write
// moves the pivot too. Offsets differ between sessions, so both fields are found by
// value at rest: when the controller is verified (the camera has been at rest and
// nothing has turned it yet), or later after three matches at rest.
namespace {
constexpr int kMaxPivots = 3;
int g_pivotOffs[kMaxPivots] = {}, g_pivotCount = 0, g_camOff = 0;  // byte offsets from the pair
bool g_pivotLocated = false;
int g_pivotConfirms = 0;
int g_pivotTries = 0;
int g_pivotRestFrames = 0;
Pose g_pivotPrev{};
constexpr int kPivotWindow = 0x800;

bool OnRay(const float* p, const float* cam, const float* fwd, float& dist) {
    const float v[3] = {p[0] - cam[0], p[1] - cam[1], p[2] - cam[2]};
    dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(dist > 0.5f && dist < 20000.f)) return false;
    return (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]) / dist > 0.99995f;
}

void FindPivot(float* ctl, const Pose& now, int confirmsNeeded) {
    const float fwd[3] = {std::sin(now.yaw) * std::cos(now.pitch), -std::sin(now.pitch), std::cos(now.yaw) * std::cos(now.pitch)};
    static float win[2 * kPivotWindow / 4];
    uint8_t* base = reinterpret_cast<uint8_t*>(ctl) - kPivotWindow;
    if (!SafeRead(base, win, sizeof(win))) return;
    int cams = 0, pivots = 0, camOff = 0, pivotOffs[kMaxPivots] = {};
    for (int i = 0; i + 2 < static_cast<int>(sizeof(win) / 4); i += 4) {  // 16-byte aligned triples
        const float* t = win + i;
        if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2]) || std::fabs(t[0]) > 1e6f) continue;
        const int off = i * 4 - kPivotWindow;
        if (std::fabs(t[0] - now.pos[0]) < 0.02f && std::fabs(t[1] - now.pos[1]) < 0.02f && std::fabs(t[2] - now.pos[2]) < 0.02f) {
            if (!cams++) camOff = off;
            continue;
        }
        float d;
        if (OnRay(t, now.pos, fwd, d)) {
            if (pivots < kMaxPivots) pivotOffs[pivots] = off;
            ++pivots;
        }
    }
    // Two points on the ray were seen (the pivot and, it appears, its target or the
    // ground hit); both lie on the view ray at rest, so both are kept on it.
    if (cams < 1 || pivots < 1 || pivots > kMaxPivots) {
        if (++g_pivotTries <= 5)
            LOG("camctl: orbit pivot not found yet (%d position field(s), %d point(s) on the view ray)", cams, pivots);
        g_pivotConfirms = 0;
        return;
    }
    if (g_pivotConfirms > 0 && (camOff != g_camOff || pivots != g_pivotCount || memcmp(pivotOffs, g_pivotOffs, sizeof(pivotOffs)) != 0))
        g_pivotConfirms = 0;
    g_camOff = camOff;
    g_pivotCount = pivots;
    memcpy(g_pivotOffs, pivotOffs, sizeof(pivotOffs));
    if (++g_pivotConfirms >= confirmsNeeded) {
        g_pivotLocated = true;
        LOG("camctl: %d orbit point(s) on the view ray at pair%+d%s, camera position at pair%+d", g_pivotCount, g_pivotOffs[0],
            g_pivotCount > 1 ? " and more" : "", g_camOff);
        for (int k = 1; k < g_pivotCount; ++k) LOG("camctl:   orbit point %d at pair%+d", k, g_pivotOffs[k]);
    }
}

// Puts the pivot on the view ray of (yaw, pitch), same distance from the camera.
void MovePivot(float* ctl, float yaw, float pitch) {
    if (!g_pivotLocated) return;
    float cam[3];
    uint8_t* b = reinterpret_cast<uint8_t*>(ctl);
    if (!SafeRead(b + g_camOff, cam, sizeof(cam)) || !std::isfinite(cam[0] + cam[1] + cam[2])) return;
    const float f[3] = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    for (int k = 0; k < g_pivotCount; ++k) {
        float piv[3];
        if (!SafeRead(b + g_pivotOffs[k], piv, sizeof(piv))) continue;
        const float v[3] = {piv[0] - cam[0], piv[1] - cam[1], piv[2] - cam[2]};
        const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(d > 0.5f && d < 20000.f)) continue;
        const float out[3] = {cam[0] + f[0] * d, cam[1] + f[1] * d, cam[2] + f[2] * d};
        if (std::isfinite(out[0] + out[1] + out[2])) SafeWrite(b + g_pivotOffs[k], out, sizeof(out));
    }
}

int g_zoomKept[kMaxPivots] = {}, g_zoomBroke[kMaxPivots] = {};
float g_prevCam[3] = {}, g_prevDist[kMaxPivots] = {};
bool g_zoomPrevValid = false;
int g_zoomPoint = -1;

void ResetPivot() {
    memset(g_zoomKept, 0, sizeof(g_zoomKept));
    memset(g_zoomBroke, 0, sizeof(g_zoomBroke));
    g_zoomPrevValid = false;
    g_zoomPoint = -1;
    g_pivotLocated = false;
    g_pivotConfirms = 0;
    g_pivotTries = 0;
    g_pivotRestFrames = 0;
}
}  // namespace

void SetResetGuard(bool on) { g_resetGuard = on; }

namespace {
// Stops using the controller; write = false when its memory may be freed (the park closed).
void Drop(const char* why, bool write) {
    if (g_phase == Phase::Idle && !g_ctl.load()) return;
    if (write && g_phase == Phase::Verifying && g_nudging && g_candidate < g_candidates.size())
        SafeWrite(g_candidates[g_candidate], g_orig, sizeof(float));  // undo a verification nudge
    if (g_ctl.load() || g_phase == Phase::Verifying) LOG("camctl: %s; camera control stopped", why);
    g_ctl.store(nullptr);
    g_ctlKey = 0;
    ResetPivot();
    g_fov.store(nullptr);
    g_fovPhase = FovPhase::None;
    g_phase = Phase::Idle;
    g_candidates.clear();
    g_nudging = g_secondStep = false;
    g_matchCount = g_frameInStep = 0;
    g_lastScanTick = 0;
    g_notDrawn = false;
    g_foreign = false;
    g_lastDrawnValid = false;
    g_mismatchFrames = 0;
}
}  // namespace

void Update(const cbmath::Vec3& pos, const cbmath::Vec3& f, float renderedFov) {
    Pose now;
    now.yaw = std::atan2(f.x, f.z);
    now.pitch = -std::asin(std::fmax(-1.f, std::fmin(1.f, f.y)));
    now.pos[0] = pos.x;
    now.pos[1] = pos.y;
    now.pos[2] = pos.z;

    // The park camera only (game_state.h). Outside a park (main menu, loading) nothing is
    // searched or written: in the headset (2026-10-03) the controller found in the park was
    // still written after exiting to the main menu, and the game crashed there. In the
    // ride camera the look record is the same record as the park controller (measured:
    // same address), and it belongs to ridecam.
    if (!gamestate::InPark()) {
        Drop("left the park", false);
        return;
    }
    if (gamestate::RideCamera() != gamestate::Ride::None) {
        Drop("the ride camera is up", true);
        return;
    }
    // A camera-mode lock without ride camera settings (yet): keep a located controller,
    // but do not search or nudge until it is known which camera this is.
    const bool locked = gamestate::CameraModeLocks() > 0;
    if (locked && g_phase != Phase::Located) {
        if (g_phase == Phase::Verifying) Drop("the camera mode changed while verifying", true);
        g_phase = Phase::Idle;
        return;
    }
    if (g_ctlKey && g_phase == Phase::Located && !props::RecordIs(g_ctl.load(), g_ctlKey, props::kVector)) {
        Drop("the controller record is gone", false);
        return;
    }

    switch (g_phase) {
        case Phase::Idle:
            StartScan(now);
            break;

        case Phase::Scanning:
            if (!g_scanDone.load()) break;
            {
                std::lock_guard<std::mutex> lock(g_scanMutex);
                g_candidates = g_scanHits;
                g_candidatesByName = g_scanByName;
            }
            g_candidate = 0;
            g_frameInStep = 0;
            g_nudging = false;
            // Verification writes to each candidate briefly; only do that for a short,
            // specific list. A long list means the value is too common to be safe.
            if (g_candidates.size() > kMaxCandidates) {
                LOG("camctl: %zu candidates is too ambiguous to test; waiting for the camera to move", g_candidates.size());
                g_candidates.clear();
            }
            g_phase = g_candidates.empty() ? Phase::Idle : Phase::Verifying;
            break;

        case Phase::Verifying: {
            if (g_candidate >= g_candidates.size()) {
                LOG("camctl: no candidate drives the camera; will rescan when the camera moves");
                g_phase = Phase::Idle;
                break;
            }
            float* c = g_candidates[g_candidate];
            // Only test while the camera is at rest: a camera turning on its own can
            // pass through the nudged value and fake a match (seen once in testing).
            const bool camStill = std::fabs(AngleDiff(now.yaw, g_prevYaw)) < 1e-5f && std::fabs(now.pitch - g_prevPitch) < 1e-5f;
            g_prevYaw = now.yaw;
            g_prevPitch = now.pitch;
            g_stillCount = camStill ? g_stillCount + 1 : 0;
            if (!g_nudging) {
                if (g_frameInStep++ < kSettleFrames || g_stillCount < kSettleFrames) break;
                float v[2];
                if (!SafeRead(c, v, sizeof(v)) || std::fabs(AngleDiff(v[0], now.yaw)) > 2e-3f) {
                    NextCandidate();  // gone, or no longer the current orientation
                    break;
                }
                g_orig[0] = v[0];
                g_orig[1] = v[1];
                g_nudging = true;
                g_frameInStep = 0;
            }
            const float target = g_orig[0] + kNudge;
            SafeWrite(c, &target, sizeof(float));
            // Two-step check: the rendered camera must follow a nudge up, then a nudge
            // down, each for two consecutive frames, before this copy is trusted.
            const float want = g_secondStep ? g_orig[0] - kNudge : target;
            if (g_secondStep) SafeWrite(c, &want, sizeof(float));
            g_matchCount = std::fabs(AngleDiff(now.yaw, want)) < kNudgeTolerance ? g_matchCount + 1 : 0;
            if (g_matchCount >= 2 && !g_secondStep) {
                g_secondStep = true;
                g_matchCount = 0;
                g_frameInStep = 0;
                break;
            }
            if (g_matchCount >= 2) {
                g_matchCount = 0;
                g_secondStep = false;
                LOG("camctl: candidate %zu at %p drives the camera", g_candidate, c);
                SafeWrite(c, g_orig, sizeof(float));
                g_ctlKey = 0;
                if (g_candidatesByName) SafeRead(reinterpret_cast<uint8_t*>(c) + 0x18, &g_ctlKey, 8);
                g_ctl.store(c);
                g_mismatchFrames = 0;
                g_phase = Phase::Located;
                // The camera is at rest here and nothing has turned it yet, so the pivot
                // still lies on the view ray (the nudges were 0.002 rad).
                ResetPivot();
                FindPivot(c, now, 1);
                FindFovCandidates(c, renderedFov);
                break;
            }
            if (++g_frameInStep >= kNudgeFrames) {
                SafeWrite(c, g_orig, sizeof(float));
                g_secondStep = false;
                g_matchCount = 0;
                NextCandidate();
            }
            break;
        }

        case Phase::Located: {
            // The rendered camera should track the stored orientation (one or two
            // frames behind our writes). A long disagreement means the controller
            // was replaced (park reload, mode change): find it again.
            float v[2];
            float* c = g_ctl.load();
            const bool ok = c && SafeRead(c, v, sizeof(v)) && std::isfinite(v[0]);
            // Measured in the headset (2026-10-02 log): after Start and menu navigation the
            // rendered camera stood still at the pose it had when the menu opened while the
            // controller kept every head write, and after 180 frames the controller was
            // dropped; with the game drawing from it again later, nothing turned the camera
            // until the stick moved it (black edges when looking around). A game that is
            // not drawing from the controller (rendered camera still, controller holding
            // what was written) has not replaced it, so it is kept; only a rendered camera
            // that moves on its own away from the controller, or an unreadable one, is.
            const bool renderedStill = std::fabs(AngleDiff(now.yaw, g_renderedPrev.yaw)) < 1e-4f &&
                                       std::fabs(now.pitch - g_renderedPrev.pitch) < 1e-4f;
            g_renderedPrev = now;
            const bool holdsWrite = ok && g_lastWriteValid && std::fabs(AngleDiff(v[0], g_lastWrite[0])) < 1e-4f &&
                                    std::fabs(v[1] - g_lastWrite[1]) < 1e-4f;
            const bool mismatch = !ok || std::fabs(AngleDiff(v[0], now.yaw)) > 0.35f;
            // Measured in the headset (2026-10-03, Restart Scenario): while the park
            // reloads, every copy of the pair reads exactly (0, 0) and the game draws a
            // camera at the world origin; the game crashed (access violation in its own
            // code) a second later, while this mod kept writing the controller's
            // orientation, FOV and orbit points into the object being rebuilt. A pair of
            // exactly zero is never one the mod writes or the park camera holds, and the
            // origin camera is the loading view: drop the controller at once, write
            // nothing more, and find it again once a park camera is drawn.
            const bool reset = g_resetGuard && ((ok && v[0] == 0.f && v[1] == 0.f) || (now.pos[0] == 0.f && now.pos[1] == 0.f && now.pos[2] == 0.f));
            if (reset) {
                LOG("camctl: the game is resetting its camera (pair %.4f %.4f, camera at %.2f %.2f %.2f); controller dropped, searching again",
                    ok ? v[0] : 0.f, ok ? v[1] : 0.f, now.pos[0], now.pos[1], now.pos[2]);
                g_ctl.store(nullptr);
                ResetPivot();
                g_fov.store(nullptr);
                g_fovPhase = FovPhase::None;
                g_phase = Phase::Idle;
                g_lastScanTick = 0;
                g_notDrawn = false;
                g_mismatchFrames = 0;
                break;
            }
            // A paused screen shows the park frozen exactly where it was last drawn from the
            // controller. Any other camera drawn (the main menu after Exit to Career Menu,
            // cutscenes) is not this one: nothing is written while it is drawn (in the
            // headset the old controller was written for 15 s in the main menu, then the
            // game crashed).
            const bool frozen = g_lastDrawnValid && SamePose(now, g_lastDrawn) && std::fabs(now.pos[1] - g_lastDrawn.pos[1]) < 1e-2f;
            g_foreign = mismatch && !frozen;
            if (!mismatch) {
                g_lastDrawn = now;
                g_lastDrawnValid = true;
            }
            if (mismatch && renderedStill && holdsWrite && frozen) {
                if (!g_notDrawn) LOG("camctl: the game is not drawing from the controller (paused screen?); keeping it");
                g_notDrawn = true;
                g_mismatchFrames = 0;
            } else if (mismatch) {
                if (g_mismatchFrames == 0 && ++g_foreignLogs <= 20) LOG("camctl: the game draws another camera; not writing");
                if (++g_mismatchFrames > 180) {
                    LOG("camctl: controller no longer matches the camera; searching again");
                    g_ctl.store(nullptr);
                    ResetPivot();
                    g_fov.store(nullptr);
                    g_fovPhase = FovPhase::None;
                    g_phase = Phase::Idle;
                    g_lastScanTick = 0;
                }
            } else {
                if (g_notDrawn) LOG("camctl: the game draws from the controller again");
                g_notDrawn = false;
                g_mismatchFrames = 0;
            }
            // Look for the pivot only with the camera at rest and drawn where the pair
            // says (the rendered camera lags writes by a frame or two).
            if (!g_pivotLocated && ok) {
                const bool still = SamePose(now, g_pivotPrev) && std::fabs(AngleDiff(v[0], now.yaw)) < 1e-4f &&
                                   std::fabs(v[1] - now.pitch) < 1e-4f;
                g_pivotPrev = now;
                g_pivotRestFrames = still ? g_pivotRestFrames + 1 : 0;
                if (g_pivotRestFrames >= 30) {
                    g_pivotRestFrames = 0;
                    FindPivot(c, now, 3);
                }
            }
            UpdateFov(renderedFov);
            break;
        }
    }
}

bool IsLocated() { return g_ctl.load() != nullptr; }

bool Read(float& yaw, float& pitch) {
    float* p = g_ctl.load();
    float v[2];
    if (!p || !SafeRead(p, v, sizeof(v)) || !std::isfinite(v[0]) || !std::isfinite(v[1])) return false;
    yaw = v[0];
    pitch = v[1];
    return true;
}

// Which orbit point is the zoom pivot. Measured 2026-10-03 (virtual pad, read-only
// sampling of the controller): of the two points on the view ray, one keeps its
// distance to the camera while the game pans, turns or tilts and changes only when
// zooming (2 to 307 m); the other follows the ground while panning (4 to 134 m). The
// order of the two differs between sessions, so the zoom point is the one whose
// distance holds while the camera moves; until panning has shown that, the shorter.

bool OrbitDistance(float& metres) {
    float* p = g_ctl.load();
    if (!p || !g_pivotLocated || g_pivotCount < 1) {
        g_zoomPrevValid = false;
        return false;
    }
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    float cam[3];
    if (!SafeRead(b + g_camOff, cam, sizeof(cam)) || !std::isfinite(cam[0] + cam[1] + cam[2])) return false;
    float dist[kMaxPivots] = {};
    for (int k = 0; k < g_pivotCount; ++k) {
        float piv[3];
        if (!SafeRead(b + g_pivotOffs[k], piv, sizeof(piv))) return false;
        dist[k] = std::sqrt((piv[0] - cam[0]) * (piv[0] - cam[0]) + (piv[1] - cam[1]) * (piv[1] - cam[1]) +
                            (piv[2] - cam[2]) * (piv[2] - cam[2]));
        if (!(dist[k] > 0.5f && dist[k] < 20000.f)) return false;
    }
    if (g_zoomPrevValid) {
        const float moved = std::fabs(cam[0] - g_prevCam[0]) + std::fabs(cam[1] - g_prevCam[1]) + std::fabs(cam[2] - g_prevCam[2]);
        if (moved > 0.05f)
            for (int k = 0; k < g_pivotCount; ++k)
                (std::fabs(dist[k] - g_prevDist[k]) < 0.002f * dist[k] ? g_zoomKept[k] : g_zoomBroke[k])++;
    }
    memcpy(g_prevCam, cam, sizeof(cam));
    memcpy(g_prevDist, dist, sizeof(dist));
    g_zoomPrevValid = true;
    int best = 0;
    for (int k = 1; k < g_pivotCount; ++k) {
        const int sk = g_zoomKept[k] - g_zoomBroke[k], sb = g_zoomKept[best] - g_zoomBroke[best];
        if (sk > sb || (sk == sb && dist[k] < dist[best])) best = k;
    }
    if (best != g_zoomPoint && (g_zoomKept[best] - g_zoomBroke[best]) >= 10) {
        LOG("camctl: zoom pivot is orbit point %d (pair%+d): distance held in %d of %d camera moves", best, g_pivotOffs[best],
            g_zoomKept[best], g_zoomKept[best] + g_zoomBroke[best]);
        g_zoomPoint = best;
    }
    metres = dist[g_zoomPoint >= 0 ? g_zoomPoint : best];
    return true;
}

bool Write(float yaw, float pitch) {
    float* p = g_ctl.load();
    const float v[2] = {yaw, pitch};
    g_lastWriteValid = false;
    if (!p || g_foreign || !SafeWrite(p, v, sizeof(v))) return false;
    g_lastWrite[0] = yaw;
    g_lastWrite[1] = pitch;
    g_lastWriteValid = true;
    MovePivot(p, yaw, pitch);
    return true;
}

}  // namespace pc2vr::camctl

namespace pc2vr::camctl {

bool SetEngineFov(float verticalRadians) {
    float* p = g_fov.load();
    return p && !g_foreign && SafeWrite(p, &verticalRadians, 4);
}

void RestoreEngineFov() {
    float* p = g_fov.load();
    if (p && g_fovGameValue > 0.f && SafeWrite(p, &g_fovGameValue, 4)) LOG("camctl: engine FOV restored to %.4f", g_fovGameValue);
}

}  // namespace pc2vr::camctl
