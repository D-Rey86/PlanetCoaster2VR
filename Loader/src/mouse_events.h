#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace pc2vr::pointer {
// Native InputProvider records, verified against the mouse producer and queue.
struct Event {
    uint64_t time{};
    uint32_t device{};
    uint16_t kind{}, control{};
    int32_t value{};
    uint32_t padding{};
};
static_assert(sizeof(Event) == 24 && offsetof(Event, value) == 16);

// Serialized by the native input bridge. Device identities are supplied at
// registration; no object lookup, allocation, OS input or native state writes.
// Keep the first-down identity through the final release, even if it comes
// from another mouse. Otherwise native UI can lose the press owner's release.
class ButtonSources {
public:
    static constexpr size_t kCapacity = 64;
    bool Add(uint32_t id) {
        if (Find(id) != kCapacity) return true;
        for (auto& source : sources_) if (!source.used) {
            source = {id, true}; return true;
        }
        return false;
    }
    bool Known(uint32_t id) const { return Find(id) != kCapacity; }
    bool Level(uint32_t id) const {
        const auto index=Find(id);
        return index!=kCapacity && (held_&(uint64_t{1}<<index))!=0;
    }
    bool Held() const { return held_ != 0; }
    // False means suppress a duplicate logical edge. Other devices/controls
    // pass through byte-for-byte; the caller validates button values first.
    bool Filter(Event& event) {
        const size_t index = Find(event.device);
        if (index == kCapacity || event.kind != 4 || event.control != 0 ||
            (event.value != 0 && event.value != 1)) return true;
        const bool before = Held();
        const uint64_t bit = uint64_t{1} << index;
        held_ = event.value ? held_ | bit : held_ & ~bit;
        if (before == Held()) return false;
        if (Held()) owner_ = event.device;
        else event.device = owner_;
        return true;
    }
    // If disconnecting the final held source, emit this release BEFORE the
    // native device-removed record. Do not reuse a slot until its hold clears.
    bool Remove(uint32_t id, uint64_t time, Event& release) {
        const size_t index = Find(id);
        if (index == kCapacity) return false;
        const bool before = Held();
        held_ &= ~(uint64_t{1} << index);
        sources_[index] = {};
        if (!before || Held()) return false;
        release = {time, owner_, 4, 0, 0, 0};
        return true;
    }
private:
    struct Source { uint32_t id{}; bool used{}; };
    size_t Find(uint32_t id) const {
        for (size_t i = 0; i < kCapacity; ++i)
            if (sources_[i].used && sources_[i].id == id) return i;
        return kCapacity;
    }
    std::array<Source, kCapacity> sources_{};
    uint64_t held_{};
    uint32_t owner_{};
};
} // namespace pc2vr::pointer
