#include "game_state.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "game_props.h"
#include "log.h"

namespace pc2vr::gamestate {
namespace {

constexpr ULONGLONG kScanIntervalMs = 2000;
constexpr ULONGLONG kStaleScanIntervalMs = 10000;
constexpr int32_t kMaxLocks = 64;  // a plausible lock count; anything else is not this record
constexpr uint32_t kTable = 0x45;  // a record holding a table
constexpr int kMaxNodeBits = 8;    // ride settings: 16 fields (2^4) measured

const char* const kNames[] = {"nDisableChangeCameraModeCount", "tSelection"};
enum { kCountName, kSelectionName, kNameCount };

struct Found {
    const uint8_t* value;
    uintptr_t key;
    uintptr_t modeKey = 0, interestKey = 0;  // keys belonging to this selection table
};

std::atomic<bool> g_scanBusy{false}, g_scanDone{false};
std::mutex g_scanMutex;
std::vector<Found> g_scanCounts, g_scanSelections;  // guarded by g_scanMutex

// Render thread only.
std::vector<Found> g_counts;  // the camera-mode record and any stale copy of it
Found g_selection{};
int32_t g_locks = 0;
bool g_inPark = false;
Ride g_ride = Ride::None;
ULONGLONG g_lastScan = 0;
int g_ambiguousLogs = 0;

Ride ModeOf(const void* value) {
    char t[16];
    if (!props::StringValue(value, t, sizeof(t))) return Ride::None;
    if (strcmp(t, "guest") == 0) return Ride::Guest;
    if (strcmp(t, "cinematic") == 0) return Ride::Cinematic;
    if (strcmp(t, "interest") == 0) return Ride::Interest;
    return Ride::None;
}

bool CountLive(const Found& f, int32_t& locks) {
    if (!props::RecordIs(f.value, f.key, props::kInt) || !props::SafeRead(f.value, &locks, 4)) return false;
    return locks >= 0 && locks <= kMaxLocks;
}

// The ride camera's settings (measured 2026-10-03): the game copies the settings table for
// its UI every few seconds and the copies linger until collected (up to 11 seen, all
// alike); the table it keeps, and changes when the mode is switched, is the one held by
// "tSelection". A table object holds log2 of its field count at +0xB and a pointer to its
// fields (records 0x30 apart) at +0x18. It counts as the ride settings if it has
// sSelectedInterestCam; the mode is its sMode.
Ride ModeInSelection(const void* selectionValue, uintptr_t modeKey, uintptr_t interestKey) {
    uintptr_t table;
    uint8_t header[0x20];
    if (!props::SafeRead(selectionValue, &table, 8) || table < 0x10000 ||
        !props::SafeRead(reinterpret_cast<const void*>(table), header, sizeof(header)))
        return Ride::None;
    const int bits = header[0xB];
    uintptr_t nodes;
    memcpy(&nodes, header + 0x18, 8);
    if (bits > kMaxNodeBits || nodes < 0x10000) return Ride::None;
    Ride mode = Ride::None;
    bool interest = false;
    for (int i = 0; i < (1 << bits); ++i) {
        const uint8_t* rec = reinterpret_cast<const uint8_t*>(nodes) + i * 0x30;
        if (props::RecordIs(rec, modeKey, props::kString)) mode = ModeOf(rec);
        else interest = interest || props::RecordIs(rec, interestKey, props::kString);
    }
    return interest ? mode : Ride::None;
}

// Park changes can leave multiple string objects with identical names (measured
// 2026-10-12). Their heap-wide uniqueness says nothing about which camera is active.
// Resolve the keys from the selection's own fields on the scan thread, then retain
// the existing cheap identity checks on the render thread.
bool ResolveSelection(Found& found) {
    uintptr_t table, nodes;
    uint8_t header[0x20];
    if (!props::SafeRead(found.value, &table, sizeof(table)) || table < 0x10000 ||
        !props::SafeRead(reinterpret_cast<const void*>(table), header, sizeof(header)))
        return false;
    const int bits = header[0xB];
    memcpy(&nodes, header + 0x18, sizeof(nodes));
    if (bits > kMaxNodeBits || nodes < 0x10000) return false;
    found.modeKey = found.interestKey = 0;
    for (int i = 0; i < (1 << bits); ++i) {
        uint8_t rec[0x20];
        if (!props::SafeRead(reinterpret_cast<const uint8_t*>(nodes) + i * 0x30, rec, sizeof(rec))) return false;
        uint32_t kind;
        uintptr_t key;
        memcpy(&kind, rec + 0x10, sizeof(kind));
        memcpy(&key, rec + 0x18, sizeof(key));
        if (kind != props::kString) continue;
        char name[32];
        if (!props::StringValue(&key, name, sizeof(name))) continue;
        if (strcmp(name, "sMode") == 0) found.modeKey = key;
        else if (strcmp(name, "sSelectedInterestCam") == 0) found.interestKey = key;
    }
    return found.modeKey && found.interestKey &&
           ModeInSelection(found.value, found.modeKey, found.interestKey) != Ride::None;
}

struct ScanCtx {
    std::vector<uintptr_t> keys[kNameCount];
    std::vector<Found> counts, selections;
};

bool Has(const std::vector<uintptr_t>& v, uintptr_t q) {
    for (uintptr_t k : v)
        if (k == q) return true;
    return false;
}

void ScanRegion(uint8_t* p, size_t bytes, void* ctx) {
    ScanCtx& c = *static_cast<ScanCtx*>(ctx);
    __try {
        for (size_t i = 0x18; i + 8 <= bytes; i += 8) {
            uintptr_t q;
            memcpy(&q, p + i, 8);
            if (q < 0x10000) continue;
            const uint8_t* v = p + i - 0x18;
            uint32_t kind;
            memcpy(&kind, v + 0x10, 4);
            if (kind == props::kInt && Has(c.keys[kCountName], q)) {
                int32_t n;
                memcpy(&n, v, 4);
                if (n >= 0 && n <= kMaxLocks && c.counts.size() < 8) c.counts.push_back({v, q});
            } else if (kind == kTable && Has(c.keys[kSelectionName], q) && c.selections.size() < 16) {
                c.selections.push_back({v, q});
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::atomic<int> g_scanLogs{0};

DWORD WINAPI ScanThread(LPVOID) {
    const ULONGLONG t0 = GetTickCount64();
    ScanCtx c;
    props::FindNames(kNames, kNameCount, c.keys);
    if (!c.keys[kCountName].empty() || !c.keys[kSelectionName].empty()) props::ForEachHeapRegion(ScanRegion, &c);
    // Only the selections holding the ride camera settings.
    std::vector<Found> rides;
    std::vector<uintptr_t> tables;  // two fields holding the same table count once
    for (Found f : c.selections) {
        uintptr_t table;
        if (!ResolveSelection(f) || !props::SafeRead(f.value, &table, 8) ||
            Has(tables, table))
            continue;
        tables.push_back(table);
        rides.push_back(f);
    }
    if (g_scanLogs.fetch_add(1) < 5)
        LOG("gamestate: search took %llu ms: %zu camera-mode record(s), %zu ride camera selection(s)", GetTickCount64() - t0,
            c.counts.size(), rides.size());
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        g_scanCounts = std::move(c.counts);
        g_scanSelections = std::move(rides);
    }
    g_scanDone.store(true);
    g_scanBusy.store(false);
    return 0;
}

void StartScan() {
    if (g_scanBusy.exchange(true)) return;
    g_lastScan = GetTickCount64();
    g_scanDone.store(false);
    HANDLE h = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else g_scanBusy.store(false);
}

const char* RideName(Ride r) {
    switch (r) {
        case Ride::Guest: return "guest";
        case Ride::Cinematic: return "cinematic";
        case Ride::Interest: return "interest";
        default: return "none";
    }
}

}  // namespace

void Update(const float cameraPos[3]) {
    if (g_scanDone.exchange(false)) {
        std::vector<Found> counts, selections;
        {
            std::lock_guard<std::mutex> lock(g_scanMutex);
            counts.swap(g_scanCounts);
            selections.swap(g_scanSelections);
        }
        g_counts = std::move(counts);
        if (selections.size() == 1 && !g_selection.value) {
            g_selection = selections[0];
        }
        if ((g_counts.size() > 1 || selections.size() > 1) && ++g_ambiguousLogs <= 10)
            LOG("gamestate: %zu camera-mode record(s) and %zu ride camera selection(s)", g_counts.size(), selections.size());
    }

    // Measured after a park load: two camera-mode records, the live one (which changed
    // when the ride camera started) and a stale copy left behind when the game rebuilt its
    // table (overwritten later). A stale copy keeps the value it had while loading (0), so
    // the park counts as locked when any record is.
    int32_t locks = 0;
    for (size_t i = 0; i < g_counts.size();) {
        int32_t n;
        if (CountLive(g_counts[i], n)) {
            locks = n > locks ? n : locks;
            ++i;
        } else {
            g_counts.erase(g_counts.begin() + i);
        }
    }
    const bool inPark = !g_counts.empty();
    if (!inPark) g_selection = {};
    if (inPark != g_inPark) LOG(inPark ? "gamestate: in a park (camera-mode record at %p)" : "gamestate: left the park", inPark ? g_counts[0].value : nullptr);
    g_inPark = inPark;
    if (inPark && locks > 0 && g_locks == 0) g_lastScan = 0;  // the ride camera may have started: look now
    g_locks = inPark ? locks : 0;

    // The selection field is kept between ride cameras (it may hold something else, or
    // nothing, in between); its table is read every frame while a camera mode is locked.
    Ride ride = Ride::None;
    if (g_locks > 0 && g_selection.value) {
        if (props::RecordIs(g_selection.value, g_selection.key, kTable))
            ride = ModeInSelection(g_selection.value, g_selection.modeKey, g_selection.interestKey);
        if (ride == Ride::None) g_selection = {};
    }
    if (ride != g_ride) LOG("gamestate: ride camera %s", RideName(ride));
    g_ride = ride;

    // A camera cut (the drawn camera jumps more than 2 m: entering or leaving the ride
    // camera, loading) is when records are made or rebuilt: look again.
    static float last[3] = {};
    const bool cut = std::fabs(cameraPos[0] - last[0]) + std::fabs(cameraPos[1] - last[1]) + std::fabs(cameraPos[2] - last[2]) > 2.f;
    memcpy(last, cameraPos, sizeof(last));
    if (cut) g_lastScan = 0;

    // Search while the park's record is unknown (menus, loading), while the ride camera's
    // settings are expected but not found, and now and then while a stale copy remains.
    const ULONGLONG since = GetTickCount64() - g_lastScan;
    const bool want = cut || ((!g_inPark || (g_locks > 0 && g_ride == Ride::None)) && since >= kScanIntervalMs) ||
                      (g_counts.size() > 1 && since >= kStaleScanIntervalMs);
    if (want) StartScan();
}

bool InPark() { return g_inPark; }
int CameraModeLocks() { return g_locks; }
Ride RideCamera() { return g_ride; }

}  // namespace pc2vr::gamestate
