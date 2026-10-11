#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pc2vr::highlight {
// DXBC input signatures survive stripped debug information and GPU-specific PSO
// compilation. Inspect only signature semantic names, never shader hashes/RVAs.
inline bool IsMaskShader(const void* bytes, size_t size) {
    if (!bytes || size < 32) return false;
    const auto* p = static_cast<const uint8_t*>(bytes);
    auto u32 = [](const uint8_t* at) { uint32_t v; std::memcpy(&v, at, 4); return v; };
    if (std::memcmp(p, "DXBC", 4) || u32(p + 24) > size) return false;
    const uint32_t chunks = u32(p + 28);
    if (chunks > (size - 32) / 4) return false;
    constexpr char semantic[] = "__USER_HIGHLIGHTMASKWRITE_MASK";
    bool found = false;
    for (uint32_t i = 0; i < chunks; ++i) {
        const size_t offset = u32(p + 32 + i * 4);
        if (offset > size - 8) return false;
        const size_t length = u32(p + offset + 4);
        if (length > size - offset - 8) return false;
        const bool newer = std::memcmp(p + offset, "ISG1", 4) == 0;
        if (!newer && std::memcmp(p + offset, "ISGN", 4)) continue;
        if (length < 8) return false;
        const uint8_t* data = p + offset + 8;
        const size_t count = u32(data), first = u32(data + 4), stride = newer ? 32 : 24;
        if (first > length || count > (length - first) / stride) return false;
        for (size_t j = 0; j < count; ++j) {
            const size_t name = u32(data + first + j * stride + (newer ? 4 : 0));
            if (name <= length && sizeof(semantic) <= length - name &&
                std::memcmp(data + name, semantic, sizeof(semantic)) == 0) found = true;
        }
    }
    return found;
}
}
