#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace pc2vr::uploads::detail
{

struct Mapping
{
    uint64_t id = 0, begin = 0, end = 0;
    const uint8_t* cpu = nullptr;
    bool writeCombined = false;
};

inline bool Contains(const Mapping& e, uint64_t va, uint64_t bytes)
{
    return va >= e.begin && va <= e.end && bytes <= e.end - va;
}

// Cache translation metadata only. Buffer contents are still read afresh by the
// camera path. Any registration/removal invalidates positive and negative hits.
struct TranslationCache
{
    uint64_t owner = 0, generation = 0;
    bool exact = false;
    uint64_t va = 0, bytes = 0;
    const uint8_t* result = nullptr;
    bool property = false;
    bool positive = false;
    Mapping mapping;
    uint64_t startLow = 0, startHigh = 0;
    bool gap = false;
    uint64_t gapLow = 0, gapHigh = 0;
};

class UploadTable
{
    inline static std::atomic<uint64_t> nextOwner{0};
    const uint64_t owner = nextOwner.fetch_add(1, std::memory_order_relaxed) + 1;
    mutable std::shared_mutex mutex;
    std::vector<Mapping> entries;
    uint64_t nextId = 0;
    std::atomic<uint64_t> generation{0};

  public:
    uint64_t Add(uint64_t begin, uint64_t size, const uint8_t* cpu, bool writeCombined)
    {
        if (!cpu || size > std::numeric_limits<uint64_t>::max() - begin)
            return 0;
        std::unique_lock<std::shared_mutex> lock(mutex);
        const uint64_t id = nextId + 1;
        entries.push_back({id, begin, begin + size, cpu, writeCombined});
        nextId = id;
        // Publish after the mutation. Fast readers use a value copy from an
        // earlier, complete snapshot; slow readers hold the shared lock.
        generation.fetch_add(1, std::memory_order_release);
        return id;
    }

    void Remove(uint64_t id) noexcept
    {
        if (!id)
            return;
        std::unique_lock<std::shared_mutex> lock(mutex);
        const auto it = std::find_if(entries.begin(), entries.end(), [id](const Mapping& e) { return e.id == id; });
        if (it == entries.end())
            return;
        entries.erase(it); // Mapping is trivial; removal needs no allocation.
        generation.fetch_add(1, std::memory_order_release);
    }

    uint32_t Count() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return static_cast<uint32_t>(entries.size());
    }

    uint64_t Generation() const
    {
        return generation.load(std::memory_order_acquire);
    }

    const uint8_t* Translate(uint64_t va, uint64_t bytes, bool* property, TranslationCache& cache) const
    {
        if (property)
            *property = false;
        if (bytes > std::numeric_limits<uint64_t>::max() - va)
            return nullptr;
        const uint64_t current = generation.load(std::memory_order_acquire);
        if (cache.owner == owner && cache.generation == current)
        {
            if (cache.exact && cache.va == va && cache.bytes == bytes)
            {
                if (property)
                    *property = cache.property;
                return cache.result;
            }
            // An older live mapping may overlap a newer one. Its cached start
            // interval excludes every newer mapping, preserving newest-wins.
            // Zero-byte endpoint queries use the exact cache or locked lookup.
            if (cache.positive && bytes && va >= cache.startLow && va < cache.startHigh &&
                Contains(cache.mapping, va, bytes))
            {
                if (property)
                    *property = cache.mapping.writeCombined;
                return cache.mapping.cpu + (va - cache.mapping.begin);
            }
            if (cache.gap && va >= cache.gapLow && va < cache.gapHigh)
                return nullptr;
        }

        std::shared_lock<std::shared_mutex> lock(mutex);
        const uint64_t snapshot = generation.load(std::memory_order_relaxed);
        if (cache.owner != owner || cache.generation != snapshot)
        {
            cache = {};
            cache.owner = owner;
            cache.generation = snapshot;
        }
        cache.exact = true;
        cache.va = va;
        cache.bytes = bytes;
        cache.result = nullptr;
        cache.property = false;
        for (size_t i = entries.size(); i-- > 0;)
        {
            const Mapping& e = entries[i];
            if (!Contains(e, va, bytes))
                continue;
            cache.result = e.cpu + (va - e.begin);
            cache.property = e.writeCombined;
            cache.mapping = e;
            cache.startLow = e.begin;
            cache.startHigh = e.end;
            cache.positive = true;
            for (size_t j = i + 1; j < entries.size(); ++j)
            {
                const Mapping& newer = entries[j];
                if (newer.end <= va)
                    cache.startLow = std::max(cache.startLow, newer.end);
                else if (newer.begin > va)
                    cache.startHigh = std::min(cache.startHigh, newer.begin);
                else
                {
                    cache.positive = false;
                    break;
                }
            }
            if (property)
                *property = cache.property;
            return cache.result;
        }

        // Missing default-heap/small-buffer addresses often lie in the same
        // empty GPU-VA interval. Cache that interval, not an assumed resource.
        bool outside = true;
        uint64_t low = 0, high = std::numeric_limits<uint64_t>::max();
        for (const Mapping& e : entries)
        {
            if (va >= e.begin && va <= e.end)
            {
                outside = false;
                break;
            }
            if (e.end < va)
                low = std::max(low, e.end + 1);
            if (e.begin > va)
                high = std::min(high, e.begin);
        }
        if (outside)
        {
            cache.gap = true;
            cache.gapLow = low;
            cache.gapHigh = high;
        }
        return nullptr;
    }
};
} // namespace pc2vr::uploads::detail
