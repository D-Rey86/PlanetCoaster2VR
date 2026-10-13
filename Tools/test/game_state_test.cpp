// Exercise the production detector and ownership transitions against the measured
// script-record layout. Including the implementation exposes its scan handoff without
// starting a process-wide background heap scan in this deterministic test.
#include "../../Loader/src/game_state.cpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace pc2vr::log { void Write(const char*, ...) {} }

namespace {
using Bytes = std::array<uint8_t, 80>;
struct alignas(8) Record { uint8_t data[48]{}; };

void Check(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "%s\n", reason); std::exit(1); }
}
void Name(Bytes& storage, const char* text) {
    storage = {};
    storage[8] = 4;
    storage[11] = static_cast<uint8_t>(strlen(text));
    memcpy(storage.data() + 24, text, strlen(text) + 1);
}
void Set(Record& rec, uintptr_t value, uint32_t kind, const Bytes& name) {
    rec = {};
    const uintptr_t key = reinterpret_cast<uintptr_t>(name.data());
    memcpy(rec.data, &value, 8);
    memcpy(rec.data + 16, &kind, 4);
    memcpy(rec.data + 24, &key, 8);
}
struct Fixture {
    Bytes modeName{}, interestName{}, selectionName{}, countName{}, guest{}, interest{};
    std::array<Record, 16> fields{};
    Bytes table{};
    Record selection{}, count{};
    Fixture() {
        Name(modeName, "sMode"); Name(interestName, "sSelectedInterestCam");
        Name(selectionName, "tSelection"); Name(countName, "nDisableChangeCameraModeCount");
        Name(guest, "guest"); Name(interest, "LookAtGuest");
        Set(fields[11], reinterpret_cast<uintptr_t>(guest.data()), pc2vr::props::kString, modeName);
        Set(fields[14], reinterpret_cast<uintptr_t>(interest.data()), pc2vr::props::kString, interestName);
        table[11] = 4;
        const uintptr_t nodes = reinterpret_cast<uintptr_t>(fields.data());
        memcpy(table.data() + 24, &nodes, 8);
        Set(selection, reinterpret_cast<uintptr_t>(table.data()), 0x45, selectionName);
        Lock(1);
    }
    void Lock(uintptr_t locks) { Set(count, locks, pc2vr::props::kInt, countName); }
    pc2vr::gamestate::Found Selection() {
        return {selection.data, reinterpret_cast<uintptr_t>(selectionName.data())};
    }
};
}

int main() {
    using namespace pc2vr::gamestate;
    Fixture first, reloaded;
    auto found = first.Selection();
    Check(ResolveSelection(found), "single-park guest selection rejected");
    // Exact failure trigger: old and new park strings coexist. Identity from the old
    // table cannot validate the new one even though the text is identical.
    auto current = reloaded.Selection();
    Check(ModeInSelection(current.value, found.modeKey, found.interestKey) == Ride::None,
          "fixture failed to reproduce different string identities after reload");
    std::array<Bytes, 12> duplicateNames{};
    for (auto& name : duplicateNames) Name(name, "sSelectedInterestCam");
    Check(ResolveSelection(current) && current.interestKey != found.interestKey,
          "reloaded selection with duplicate names rejected");

    g_scanBusy = true;  // prevent asynchronous discovery; feed its real handoff below
    g_scanCounts = {{reloaded.count.data, reinterpret_cast<uintptr_t>(reloaded.countName.data())}};
    g_scanSelections = {current};
    g_scanDone = true;
    const float pos[3]{};
    Update(pos);
    Check(InPark() && CameraModeLocks() == 1 && RideCamera() == Ride::Guest, "guest ownership not acquired");
    Name(reloaded.guest, "cinematic"); Update(pos);
    Check(RideCamera() == Ride::Cinematic, "cinematic no longer distinguished from guest");
    Name(reloaded.guest, "interest"); Update(pos);
    Check(RideCamera() == Ride::Interest, "interest camera no longer distinguished");
    reloaded.Lock(0); Update(pos);
    Check(InPark() && RideCamera() == Ride::None, "stale ride table claimed unlocked park camera");
    reloaded.Lock(1); Name(reloaded.guest, "ellipse"); Update(pos);
    Check(RideCamera() == Ride::None && !g_selection.value, "non-ride settings claimed camera");
    Name(reloaded.guest, "guest");
    reloaded.fields[14] = {};
    auto missing = reloaded.Selection();
    Check(!ResolveSelection(missing), "table without ride marker accepted");
    first.table[11] = 9;
    auto malformed = first.Selection();
    Check(!ResolveSelection(malformed), "oversized table accepted");
    first.table[11] = 4;
    const uintptr_t bad = 1;
    memcpy(first.table.data() + 24, &bad, 8);
    Check(!ResolveSelection(malformed), "invalid node pointer accepted");
    reloaded.count = {}; Update(pos);
    Check(!InPark() && CameraModeLocks() == 0 && RideCamera() == Ride::None, "unload retained ownership");
    std::puts("duplicate-name reload, guest/other cameras, unlock, invalidation and malformed tables passed");
}
