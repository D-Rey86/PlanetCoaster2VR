#include "ride_camera.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "game_props.h"
#include "log.h"

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
    g_fov = nullptr;
    g_look = nullptr;
    g_phase = Phase::None;
}

}  // namespace

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
        Release();
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
            // One look record, or this is not a ride camera (menus, cutscenes, park).
            if (g_looks.size() != 1 || g_fovs.empty()) {
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
                float look[2];
                if (SafeRead(g_looks[0], look, sizeof(look)) && SafeRead(reinterpret_cast<uint8_t*>(c) + 0x18, &g_fovKey, 8) &&
                    SafeRead(reinterpret_cast<uint8_t*>(g_looks[0]) + 0x18, &g_lookKey, 8)) {
                    g_fov = c;
                    g_look = g_looks[0];
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
            // Still the camera drawn: the game holds what was last written. Anything else
            // (the camera was replaced or freed) and nothing more is written there.
            float fov, look[2];
            if (!props::RecordIs(g_fov, g_fovKey, kFloatKind) || !props::RecordIs(g_look, g_lookKey, kLookKind)) {
                LOG("ridecam: the ride camera's records are gone");
                Forget();
                g_attempts = 0;
                break;
            }
            if (!SafeRead(g_fov, &fov, 4) || !SafeRead(g_look, look, sizeof(look)) || fov != g_lastFov || look[0] != g_lastLook[0] ||
                look[1] != g_lastLook[1]) {
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
    if (!restore) {
        // Leaving the park: its records may already be freed; write nothing.
        if (g_phase == Phase::Located) LOG("ridecam: left the ride camera (park closed)");
        Forget();
        g_attempts = 0;
        return;
    }
    if (g_phase == Phase::Verifying && g_candidate < g_fovs.size()) SafeWrite(g_fovs[g_candidate], &g_orig, 4);
    if (g_phase == Phase::Located && props::RecordIs(g_fov, g_fovKey, kFloatKind) && props::RecordIs(g_look, g_lookKey, kLookKind)) {
        float fov, look[2];
        if (SafeRead(g_fov, &fov, 4) && fov == g_lastFov) SafeWrite(g_fov, &g_gameFov, 4);
        if (SafeRead(g_look, look, sizeof(look)) && look[0] == g_lastLook[0] && look[1] == g_lastLook[1])
            SafeWrite(g_look, g_gameLook, sizeof(g_gameLook));
        LOG("ridecam: left the ride camera");
    }
    Forget();
    g_attempts = 0;
}

bool Located() { return g_phase == Phase::Located; }

bool Write(float yaw, float pitch, float verticalFov) {
    if (g_phase != Phase::Located) return false;
    const float look[2] = {yaw, pitch};
    if (!SafeWrite(g_look, look, sizeof(look)) || !SafeWrite(g_fov, &verticalFov, 4)) {
        Forget();
        return false;
    }
    memcpy(g_lastLook, look, sizeof(look));
    g_lastFov = verticalFov;
    return true;
}

}  // namespace pc2vr::ridecam
