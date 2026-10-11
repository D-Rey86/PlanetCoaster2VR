#include "game_props.h"

#include <windows.h>

#include <cstring>

namespace pc2vr::props {

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

void ForEachHeapRegion(void (*fn)(uint8_t* base, size_t size, void* ctx), void* ctx) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* addr = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    const uint8_t* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi))) {
        uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
        const bool rw = (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE));
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && mbi.RegionSize <= (64ull << 20)) fn(base, mbi.RegionSize, ctx);
        addr = base + mbi.RegionSize;
    }
}

namespace {

struct FindCtx {
    const char* const* names;
    size_t count;
    std::vector<uintptr_t>* keys;
};

// A name string object: type 4 at +8 and the length at +0xB, then the text and its zero
// at +0x18 (8-aligned). Matching the header too keeps a longer name ending in the same
// text from counting.
void FindInRegion(uint8_t* p, size_t bytes, void* ctx) {
    const FindCtx& c = *static_cast<const FindCtx*>(ctx);
    __try {
        for (size_t i = 0x18; i + 0x40 <= bytes; i += 8) {
            if (p[i - 0x10] != 4) continue;
            const uint8_t len = p[i - 0xD];
            for (size_t k = 0; k < c.count; ++k) {
                const char* n = c.names[k];
                if (static_cast<uint8_t>(n[0]) != p[i] || strlen(n) != len || memcmp(p + i, n, len + 1) != 0) continue;
                if (c.keys[k].size() < 8) c.keys[k].push_back(reinterpret_cast<uintptr_t>(p + i - 0x18));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

void FindNames(const char* const* names, size_t count, std::vector<uintptr_t>* keys) {
    for (size_t k = 0; k < count; ++k) keys[k].clear();
    FindCtx c{names, count, keys};
    ForEachHeapRegion(FindInRegion, &c);
}

bool RecordIs(const void* value, uintptr_t key, uint32_t kind) {
    uint8_t r[0x20];
    if (!value || !SafeRead(value, r, sizeof(r))) return false;
    uint32_t k;
    uintptr_t q;
    memcpy(&k, r + 0x10, 4);
    memcpy(&q, r + 0x18, 8);
    return k == kind && q == key;
}

bool StringValue(const void* value, char* out, size_t outSize) {
    uintptr_t s;
    uint8_t h[0x18 + 40];
    if (!SafeRead(value, &s, 8) || s < 0x10000 || !SafeRead(reinterpret_cast<const void*>(s), h, sizeof(h))) return false;
    const uint8_t len = h[0xB];
    if (h[8] != 4 || len == 0 || len >= outSize || len > 40) return false;
    memcpy(out, h + 0x18, len);
    out[len] = 0;
    return true;
}

}  // namespace pc2vr::props
