#include "inline_hook.h"

#include <windows.h>

#include <cstdint>
#include <initializer_list>
#include <cstring>

#include "log.h"

namespace pc2vr::inlinehook {
namespace {

void WriteAbsJump(unsigned char* at, const void* target) {
    at[0] = 0xFF;  // jmp qword ptr [rip+0]
    at[1] = 0x25;
    at[2] = at[3] = at[4] = at[5] = 0;
    memcpy(at + 6, &target, 8);
}

unsigned char* AllocNear(unsigned char* target) {
    for (intptr_t delta = 0x10000; delta < 0x7FF00000; delta += 0x10000)
        for (int sign : {-1, 1}) {
            auto* want = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(target) & ~uintptr_t(0xFFFF)) + sign * delta);
            if (void* p = VirtualAlloc(want, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return static_cast<unsigned char*>(p);
        }
    return nullptr;
}

}  // namespace

bool Install(void* targetPtr, const unsigned char* expected, size_t copyLen, void* hook, void** original, const char* name) {
    auto* target = static_cast<unsigned char*>(targetPtr);
    if (!target) return false;

    if (target[0] == 0xE9 && memcmp(target + 5, expected + 5, copyLen - 5) == 0 &&
        (reinterpret_cast<uintptr_t>(target) & 7) == 0) {
        int32_t rel;
        memcpy(&rel, target + 1, 4);
        unsigned char* oldDest = target + 5 + rel;
        unsigned char* relay = AllocNear(target);
        if (!relay) {
            LOG("hook %s: no memory near the target for a relay", name);
            return false;
        }
        WriteAbsJump(relay, hook);
        FlushInstructionCache(GetCurrentProcess(), relay, 16);
        *original = oldDest;
        long long before, after;
        memcpy(&before, target, 8);
        unsigned char bytes[8];
        memcpy(bytes, target, 8);
        const int32_t newRel = static_cast<int32_t>(relay - (target + 5));
        memcpy(bytes + 1, &newRel, 4);
        memcpy(&after, bytes, 8);
        DWORD old = 0;
        if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        const bool ok = InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(target), after, before) == before;
        VirtualProtect(target, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, 8);
        LOG("hook %s: chained after an existing hook (%p): %s", name, oldDest, ok ? "OK" : "FAILED");
        return ok;
    }

    if (copyLen < 16 || copyLen > 48 || memcmp(target, expected, copyLen) != 0 ||
        (reinterpret_cast<uintptr_t>(target) & 15) != 0) {
        char hex[3 * 24 + 1] = {};
        for (int i = 0; i < 24; ++i) wsprintfA(hex + i * 3, "%02x ", target[i]);
        LOG("hook %s: unexpected bytes at %p (%s); not installed", name, target, hex);
        return false;
    }
    auto* tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, target, copyLen);
    WriteAbsJump(tramp + copyLen, target + copyLen);
    FlushInstructionCache(GetCurrentProcess(), tramp, 128);
    *original = tramp;

    alignas(16) unsigned char patch[16];
    WriteAbsJump(patch, hook);
    patch[14] = 0x90;
    patch[15] = 0x90;
    alignas(16) long long cmp[2];
    memcpy(cmp, target, 16);
    long long desired[2];
    memcpy(desired, patch, 16);
    DWORD old = 0;
    if (!VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
    const bool ok =
        InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(target), desired[1], desired[0], cmp) != 0;
    VirtualProtect(target, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    LOG("hook %s: %s", name, ok ? "installed" : "FAILED (bytes changed)");
    return ok;
}

}  // namespace pc2vr::inlinehook

