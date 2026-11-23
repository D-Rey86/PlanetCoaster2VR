#include "ride_camera.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "game_props.h"
#include "log.h"
#include "native_input_hook.h"
#include "ride_visibility.h"

namespace pc2vr::ridecam {
namespace {

// Property records (game_props.h), found by name:
//  - "vCameraYPR" (kind 0xa: yaw, pitch, roll): the ride camera's look direction.
//  - "nDefaultFOV" (kind 3, a float, radians): the FOV the ride camera renders with (the
//    rendered FOV follows a nudge of it; the camera's own nCameraFOV does not).
constexpr uint32_t kLookKind = 0xa;
constexpr uint32_t kFloatKind = 0x3;
constexpr char kLookName[] = "vCameraYPR";
constexpr char kFovName[] = "nDefaultFOV";
constexpr float kNudge = 0.002f;           // FOV verification nudge (radians, invisible)
constexpr float kTolerance = 0.0005f;
constexpr int kNudgeFrames = 10;
constexpr int kFastAttempts = 10;          // scans 2 s apart after a camera cut, then 10 s apart
constexpr size_t kMaxFovs = 16;

using props::SafeRead;
using props::SafeWrite;

enum class Phase { None, Scanning, Verifying, Located };
Phase g_phase = Phase::None;  // render thread only
std::atomic<bool> g_scanDone{false};
std::mutex g_scanMutex;
std::vector<float*> g_scanFovs, g_scanLooks;  // guarded by g_scanMutex
ULONGLONG g_lastScan = 0;
ULONGLONG g_lookLostAt = 0;
int g_attempts = 0;

std::vector<float*> g_fovs, g_looks;  // this search's FOV records and look candidates
size_t g_candidate = 0;
int g_frames = 0, g_match = 0;
bool g_second = false;
float g_orig = 0.f;

float* g_fov = nullptr;    // the render camera's FOV (located)
float* g_look = nullptr;   // the look record's (yaw, pitch)
float g_gameFov = 0.f;     // values before the mod changed them
float g_gameLook[2] = {};
float g_lastFov = 0.f;     // what they should hold now
float g_lastLook[2] = {};

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// The names' string objects, looked up on every scan: one found while the main menu was
// up matched no record on the ride later (measured), so they are not kept.
std::vector<uintptr_t> g_lookDescs, g_fovDescs;  // scan thread only
uintptr_t g_fovKey = 0, g_lookKey = 0;  // the located records' names (their +0x18)

// The Lua interpreter looks up a property immediately before copying its
// value to the ride script. Several script operands can name the same
// property. Learn their shared string identity from the verified live record.
using Lookup = float* (*)(void*, const void*);
pointer::native::Hook g_lookupHook;
uintptr_t g_lookupCaller{};
bool g_lookupReady{};
SRWLOCK g_lookupLock = SRWLOCK_INIT;
std::atomic<float*> g_liveLook{};
std::atomic<uintptr_t> g_lookupKey{};
struct LookWatch {
    uintptr_t key{};
    void* table{};
    void* nodes{};
    float native[2]{}, applied[2]{}, desired[2]{};
    ULONGLONG lastWrite{};
    const void* operand{};
} g_watch;

// After a cut within Guest mode, the heap can still contain the previous
// table nodes. Resolve an ambiguous scan using the already verified Lua read.
struct ReadIdentity {
    uintptr_t key{};
    void* table{};
    const void* operand{};
};
ReadIdentity g_beforeCut;  // render thread only, cleared when leaving Guest mode
ReadIdentity g_recovery;   // guarded by g_lookupLock
float* g_recoveredLook{};  // guarded by g_lookupLock
void* g_recoveredNodes{};  // guarded by g_lookupLock
std::atomic<const void*> g_recoveryOperand{};

void* TableNodes(void* table) {
    void* nodes{};
    SafeRead(static_cast<const uint8_t*>(table) + 0x18, &nodes, sizeof(nodes));
    return nodes;
}

void StopRecovery() {
    g_recoveryOperand.store(nullptr, std::memory_order_release);
}

bool StartRecovery() {
    if (!g_beforeCut.operand) return false;
    AcquireSRWLockExclusive(&g_lookupLock);
    g_recovery = g_beforeCut;
    g_recoveredLook = nullptr;
    g_recoveredNodes = nullptr;
    g_recoveryOperand.store(g_recovery.operand, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lookupLock);
    return true;
}

float* FinishRecovery() {
    AcquireSRWLockExclusive(&g_lookupLock);
    StopRecovery();
    float* value = g_recoveredNodes && g_recoveredNodes == TableNodes(g_recovery.table) ? g_recoveredLook : nullptr;
    ReleaseSRWLockExclusive(&g_lookupLock);
    if (!props::RecordIs(value, g_beforeCut.key, kLookKind)) return nullptr;
    for (float* candidate : g_looks)
        if (candidate == value) return value;
    return nullptr;
}

float* LookupLook(void* table, const void* operand) {
    const auto original = reinterpret_cast<Lookup>(g_lookupHook.original.load(std::memory_order_acquire));
    if (!g_liveLook.load(std::memory_order_acquire)) {
        const void* expected = g_recoveryOperand.load(std::memory_order_acquire);
        if (!expected || operand != expected || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_lookupCaller)
            return original(table, operand);
        float* value = original(table, operand);
        AcquireSRWLockExclusive(&g_lookupLock);
        uintptr_t key{};
        if (operand == g_recoveryOperand.load(std::memory_order_relaxed) && table == g_recovery.table &&
            SafeRead(operand, &key, sizeof(key)) && key == g_recovery.key &&
            props::RecordIs(value, key, kLookKind)) {
            g_recoveredLook = value;
            g_recoveredNodes = TableNodes(table);
        }
        ReleaseSRWLockExclusive(&g_lookupLock);
        return value;
    }
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_lookupCaller)
        return original(table, operand);
    uintptr_t operandKey;
    memcpy(&operandKey, operand, sizeof(operandKey));
    auto learned = g_lookupKey.load(std::memory_order_relaxed);
    if (learned && operandKey != learned) return original(table, operand);
    float* value = original(table, operand);
    if (learned && value == g_liveLook.load(std::memory_order_acquire)) return value;

    AcquireSRWLockExclusive(&g_lookupLock);
    float* live = g_liveLook.load(std::memory_order_relaxed);
    learned = g_lookupKey.load(std::memory_order_relaxed);
    if (live && !learned && value == live && operandKey == g_watch.key) {
        g_watch.table = table;
        g_watch.nodes = TableNodes(table);
        g_watch.operand = operand;
        if (g_watch.nodes) g_lookupKey.store(operandKey, std::memory_order_relaxed);
    } else if (live && value != live && operandKey == learned &&
               props::RecordIs(value, g_watch.key, kLookKind)) {
        void* nodes = TableNodes(table);
        const bool resized = table == g_watch.table && nodes != g_watch.nodes;
        const bool replaced = !resized && operand == g_watch.operand && GetTickCount64() - g_watch.lastWrite <= 250 &&
            !props::RecordIs(live, g_watch.key, kLookKind);
        float native[2];
        // Carry the last look onto the replacement before this VM instruction
        // copies it. A confirmed resize of the same table can also occur
        // during a long frame. A different table needs a fresh tracked pose.
        if (nodes && g_watch.lastWrite && (resized || replaced) &&
            SafeRead(value, native, sizeof(native)) && SafeWrite(value, g_watch.desired, sizeof(g_watch.desired))) {
            g_watch.table = table;
            g_watch.nodes = nodes;
            if (!resized || native[0] != g_watch.desired[0] || native[1] != g_watch.desired[1])
                memcpy(g_watch.native, native, sizeof(native));
            memcpy(g_watch.applied, g_watch.desired, sizeof(g_watch.applied));
            g_liveLook.store(value, std::memory_order_release);
        }
    }
    ReleaseSRWLockExclusive(&g_lookupLock);
    return value;
}

void WatchLook(float* value, uintptr_t key) {
    if (!g_lookupReady) return;
    AcquireSRWLockExclusive(&g_lookupLock);
    g_watch = {};
    g_watch.key = key;
    if (value) memcpy(g_watch.native, g_gameLook, sizeof(g_watch.native));
    g_lookupKey.store(0, std::memory_order_relaxed);
    g_liveLook.store(value, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lookupLock);
}

void AdoptLookLocked() {
    const auto latest = g_liveLook.load(std::memory_order_relaxed);
    if (latest != g_look && props::RecordIs(latest, g_lookKey, kLookKind)) {
        g_look = latest;
        memcpy(g_gameLook, g_watch.native, sizeof(g_gameLook));
        memcpy(g_lastLook, g_watch.applied, sizeof(g_lastLook));
    }
}

void AdoptLook() {
    const auto live = g_liveLook.load(std::memory_order_acquire);
    if (!live || live == g_look) return;
    AcquireSRWLockShared(&g_lookupLock);
    AdoptLookLocked();
    ReleaseSRWLockShared(&g_lookupLock);
}

bool Contains(const std::vector<uintptr_t>& v, uintptr_t q) {
    for (uintptr_t d : v)
        if (d == q) return true;
    return false;
}

// Records pointing at one of the descriptors, with the expected kind and a plausible value.
void ScanRegion(uint8_t* p, size_t bytes, std::vector<float*>& fovs, std::vector<float*>& looks) {
    __try {
        for (size_t i = 0x18; i + 8 <= bytes; i += 8) {
            uintptr_t q;
            memcpy(&q, p + i, 8);
            if (q < 0x10000) continue;
            uint8_t* v = p + i - 0x18;
            float f[2];
            memcpy(f, v, 8);
            if (Contains(g_lookDescs, q) && U32(v + 0x10) == kLookKind && std::isfinite(f[0]) && std::isfinite(f[1]) &&
                std::fabs(f[0]) < 10.f && std::fabs(f[1]) < 2.f)
                looks.push_back(reinterpret_cast<float*>(v));
            else if (Contains(g_fovDescs, q) && U32(v + 0x10) == kFloatKind && f[0] > 0.1f && f[0] < 3.f && fovs.size() < kMaxFovs)
                fovs.push_back(reinterpret_cast<float*>(v));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
struct ScanOut {
    std::vector<float*> fovs, looks;
};

DWORD WINAPI ScanThread(LPVOID) {
    const char* const names[] = {kLookName, kFovName};
    std::vector<uintptr_t> keys[2];
    props::FindNames(names, 2, keys);
    g_lookDescs = keys[0];
    g_fovDescs = keys[1];
    ScanOut out;
    props::ForEachHeapRegion([](uint8_t* base, size_t size, void* ctx) {
        ScanOut& o = *static_cast<ScanOut*>(ctx);
        ScanRegion(base, size, o.fovs, o.looks);
    }, &out);
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_scanFovs = std::move(out.fovs);
        g_scanLooks = std::move(out.looks);
    }
    g_scanDone.store(true);
    return 0;
}

void Forget() {
    StopRecovery();
    if (g_liveLook.load(std::memory_order_acquire)) WatchLook(nullptr, 0);
    g_fov = nullptr;
    g_look = nullptr;
    g_lookLostAt = 0;
    g_phase = Phase::None;
}

}  // namespace

void Prepare() {
    PrepareVisibility();
    using namespace pointer::native;
    g_lookupCaller = Address(Landmark::RideLookupCaller);
    g_lookupReady = g_lookupCaller && Install(Site::RideLookup, reinterpret_cast<void*>(&LookupLook), g_lookupHook);
}

void Update(float renderedFov, const float pos[3]) {
    if (!std::isfinite(renderedFov)) return;
    // A camera cut (the drawn camera jumps more than 2 m in a frame, e.g. into the ride
    // camera) is a new camera to look for; otherwise a few scans per period are enough.
    static float last[3] = {};
    static bool lastValid = false;
    const float jump = std::fabs(pos[0] - last[0]) + std::fabs(pos[1] - last[1]) + std::fabs(pos[2] - last[2]);
    const bool cut = lastValid && jump > 2.f;
    memcpy(last, pos, sizeof(last));
    lastValid = true;
    if (cut && g_phase == Phase::Located) {
        // Measured: leaving the ride camera (B) draws the park camera right away while the
        // ride camera's values still hold ours; the park camera then rendered with the
        // widened FOV. A cut ends the ride camera: put the game's values back.
        LOG("ridecam: camera cut (%.1f m)", jump);
        ReadIdentity previous;
        AcquireSRWLockShared(&g_lookupLock);
        if (g_lookupKey.load(std::memory_order_relaxed))
            previous = {g_watch.key, g_watch.table, g_watch.operand};
        ReleaseSRWLockShared(&g_lookupLock);
        Release();
        g_beforeCut = previous;
        return;
    }
    if (cut && g_phase == Phase::None) g_attempts = 0;
    switch (g_phase) {
        case Phase::None: {
            const ULONGLONG now = GetTickCount64();
            // The look record can appear a few seconds after the ride camera starts.
            if (now - g_lastScan < (g_attempts < kFastAttempts ? 2000u : 10000u)) break;
            g_lastScan = now;
            ++g_attempts;
            g_scanDone.store(false);
            HANDLE h = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
            if (!h) break;
            CloseHandle(h);
            g_phase = Phase::Scanning;
            break;
        }
        case Phase::Scanning:
            if (!g_scanDone.load()) break;
            {
                std::lock_guard<std::mutex> lock(g_scanMutex);
                g_fovs = g_scanFovs;
                g_looks = g_scanLooks;
            }
            LOG("ridecam: %zu nDefaultFOV and %zu vCameraYPR record(s)", g_fovs.size(), g_looks.size());
            // An initial acquisition still requires one look record. On a cut
            // within Guest mode, allow the verified native reader to resolve duplicates.
            if (g_looks.empty() || g_fovs.empty() || (g_looks.size() > 1 && !StartRecovery())) {
                g_phase = Phase::None;
                break;
            }
            g_candidate = 0;
            g_frames = g_match = 0;
            g_second = false;
            g_phase = Phase::Verifying;
            break;

        case Phase::Verifying: {
            // Which FOV record the render camera uses: the rendered FOV follows a nudge up,
            // then down, two frames each (works while the car moves).
            if (g_candidate >= g_fovs.size()) {
                StopRecovery();
                g_phase = Phase::None;
                break;
            }
            float* c = g_fovs[g_candidate];
            if (g_frames == 0 && !g_second && !SafeRead(c, &g_orig, 4)) {
                ++g_candidate;
                break;
            }
            const float want = g_orig + (g_second ? -kNudge : kNudge);
            SafeWrite(c, &want, 4);
            g_match = std::fabs(renderedFov - want) < kTolerance ? g_match + 1 : 0;
            if (g_match >= 2 && !g_second) {
                g_second = true;
                g_match = 0;
                g_frames = 1;
                break;
            }
            if (g_match >= 2) {
                SafeWrite(c, &g_orig, 4);
                float* selected = g_looks.size() == 1 ? g_looks.front() : FinishRecovery();
                float look[2];
                if (selected && SafeRead(selected, look, sizeof(look)) && SafeRead(reinterpret_cast<uint8_t*>(c) + 0x18, &g_fovKey, 8) &&
                    SafeRead(reinterpret_cast<uint8_t*>(selected) + 0x18, &g_lookKey, 8)) {
                    g_fov = c;
                    g_look = selected;
                    // The game's own value: remembered from the first time (a value this
                    // mod left behind is never taken for it).
                    static float* knownFov = nullptr;
                    static float knownGameFov = 0.f;
                    if (c != knownFov) {
                        knownFov = c;
                        knownGameFov = g_orig;
                    }
                    g_gameFov = knownGameFov;
                    g_lastFov = g_orig;
                    memcpy(g_gameLook, look, sizeof(look));
                    memcpy(g_lastLook, look, sizeof(look));
                    g_phase = Phase::Located;
                    WatchLook(g_look, g_lookKey);
                    LOG("ridecam: ride camera found: FOV %p (%.4f), look %p (%.4f %.4f)", c, g_orig, g_look, look[0], look[1]);
                } else {
                    g_phase = Phase::None;
                }
                break;
            }
            if (++g_frames >= kNudgeFrames) {
                SafeWrite(c, &g_orig, 4);
                g_frames = g_match = 0;
                g_second = false;
                ++g_candidate;
            }
            break;
        }

        case Phase::Located: {
            AdoptLook();
            // Still the camera drawn: the game holds what was last written. Anything else
            // (the camera was replaced or freed) and nothing more is written there.
            float fov = 0, look[2]{};
            const bool validFov = props::RecordIs(g_fov, g_fovKey, kFloatKind) && SafeRead(g_fov, &fov, 4);
            const bool validLook = props::RecordIs(g_look, g_lookKey, kLookKind) && SafeRead(g_look, look, sizeof(look));
            if (validFov && fov == g_lastFov && !validLook && g_lookupKey.load(std::memory_order_relaxed)) {
                // The table can move between the script's camera calculation
                // and the render callback. Give the authenticated lookup a
                // short opportunity to supply the replacement.
                const auto now = GetTickCount64();
                if (!g_lookLostAt) g_lookLostAt = now;
                if (now - g_lookLostAt <= 250) break;
            }
            if (!validFov || !validLook) {
                LOG("ridecam: the ride camera's records are gone");
                if (validFov && fov == g_lastFov) SafeWrite(g_fov, &g_gameFov, 4);
                Forget();
                g_attempts = 0;
                break;
            }
            g_lookLostAt = 0;
            if (fov != g_lastFov || look[0] != g_lastLook[0] || look[1] != g_lastLook[1]) {
                LOG("ridecam: the ride camera changed (FOV %.4f, look %.4f %.4f); searching again", fov, look[0], look[1]);
                // Measured: leaving the ride camera resets the look first. nDefaultFOV is
                // the game's default for other cameras too: put it back if it holds ours.
                if (fov == g_lastFov) SafeWrite(g_fov, &g_gameFov, 4);
                Forget();
                g_attempts = 0;
            }
            break;
        }
    }
}

void Release(bool restore) {
    g_beforeCut = {};
    if (!restore) {
        // Leaving the park: its records may already be freed; write nothing.
        if (g_phase == Phase::Located) LOG("ridecam: left the ride camera (park closed)");
        Forget();
        g_attempts = 0;
        return;
    }
    AdoptLook();
    if (g_liveLook.load(std::memory_order_acquire)) WatchLook(nullptr, 0);
    if (g_phase == Phase::Verifying && g_candidate < g_fovs.size()) SafeWrite(g_fovs[g_candidate], &g_orig, 4);
    if (g_phase == Phase::Located) {
        float fov, look[2];
        if (props::RecordIs(g_fov, g_fovKey, kFloatKind) && SafeRead(g_fov, &fov, 4) && fov == g_lastFov)
            SafeWrite(g_fov, &g_gameFov, 4);
        if (props::RecordIs(g_look, g_lookKey, kLookKind) && SafeRead(g_look, look, sizeof(look)) &&
            look[0] == g_lastLook[0] && look[1] == g_lastLook[1])
            SafeWrite(g_look, g_gameLook, sizeof(g_gameLook));
        LOG("ridecam: left the ride camera");
    }
    Forget();
    g_attempts = 0;
}

bool Located() { return g_phase == Phase::Located; }

bool Write(float yaw, float pitch, float verticalFov) {
    if (g_phase != Phase::Located || g_lookLostAt) return false;
    const float look[2] = {yaw, pitch};
    const bool watching = g_liveLook.load(std::memory_order_acquire) != nullptr;
    if (watching) {
        AcquireSRWLockExclusive(&g_lookupLock);
        AdoptLookLocked();
    }
    const bool wrote = props::RecordIs(g_look, g_lookKey, kLookKind) &&
        SafeWrite(g_look, look, sizeof(look)) && SafeWrite(g_fov, &verticalFov, 4);
    if (watching) {
        if (wrote) {
            memcpy(g_watch.desired, look, sizeof(look));
            g_watch.lastWrite = GetTickCount64();
        }
        ReleaseSRWLockExclusive(&g_lookupLock);
    }
    if (!wrote) {
        Forget();
        return false;
    }
    memcpy(g_lastLook, look, sizeof(look));
    g_lastFov = verticalFov;
    return true;
}

}  // namespace pc2vr::ridecam
