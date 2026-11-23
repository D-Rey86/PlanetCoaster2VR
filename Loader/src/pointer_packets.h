#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pc2vr::pointer {
// Process-local WM_INPUT payloads, serialized by the pointer bridge. A token is
// never passed to Windows GetRawInputData: only the executable's import wrapper
// recognizes it. Ordinary raw packets continue through the existing HID path.
class RawPackets {
public:
    static constexpr size_t kCapacity = 16;
    RawPackets() = default;
    RawPackets(const RawPackets&) = delete;
    RawPackets& operator=(const RawPackets&) = delete;
    HANDLE Device() { return &deviceTag_; }
    HRAWINPUT Acquire(bool down, uint64_t generation = 0) {
        for (size_t n = 0; n < kCapacity; ++n) {
            const size_t i = (next_ + n) % kCapacity;
            auto& slot = slots_[i];
            if (slot.busy) continue;
            slot.raw = {}; slot.raw.header.dwType = RIM_TYPEMOUSE;
            slot.raw.header.dwSize = sizeof(MousePacket);
            slot.raw.header.hDevice = Device();
            slot.raw.header.wParam = RIM_INPUT;
            slot.raw.mouse.usButtonFlags = down ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_UP;
            slot.generation = generation;
            slot.busy = true; next_ = (i + 1) % kCapacity;
            return reinterpret_cast<HRAWINPUT>(&slot);
        }
        return nullptr; // caller retries its desired level; never overwrite a queued release
    }
    bool Owns(HRAWINPUT token) const { return Index(token) < kCapacity; }
    bool Pending(HRAWINPUT token) const { const auto i=Index(token); return i<kCapacity && slots_[i].busy; }
    void Cancel(HRAWINPUT token) { const auto i=Index(token); if (i<kCapacity) slots_[i].busy=false; }
    UINT Read(HRAWINPUT token, UINT command, void* data, UINT* size, UINT headerSize,
              bool allowPress = true, uint64_t generation = 0) {
        const size_t i = Index(token);
        if (i == kCapacity || !slots_[i].busy || !size || headerSize != sizeof(RAWINPUTHEADER))
            return Error(ERROR_INVALID_PARAMETER);
        const UINT needed = command == RID_INPUT ? sizeof(MousePacket) : command == RID_HEADER ? sizeof(RAWINPUTHEADER) : 0;
        if (!needed) return Error(ERROR_INVALID_PARAMETER);
        if (!data) { *size=needed; return 0; }
        if (*size < needed) { *size=needed; return Error(ERROR_INSUFFICIENT_BUFFER); }
        auto packet=slots_[i].raw;
        // A queued press may outlive focus/tracking. The native bridge supplies
        // current validity AND the publication generation. Focus recovery cannot
        // revive a queued press from a previous ownership interval. Releases
        // always reach the game, including across a generation change.
        if (!allowPress || slots_[i].generation != generation)
            packet.mouse.usButtonFlags &= ~RI_MOUSE_LEFT_BUTTON_DOWN;
        std::memcpy(data, &packet, needed); *size=needed;
        // The verified native consumer makes one complete read, then works on
        // its own stack copy. Query/header/error paths must not consume a slot.
        if (command == RID_INPUT) slots_[i].busy=false;
        return needed;
    }
private:
    struct MousePacket { RAWINPUTHEADER header; RAWMOUSE mouse; };
    static_assert(sizeof(MousePacket) == 48 && offsetof(MousePacket, mouse) == 24);
    struct Slot { MousePacket raw{}; uint64_t generation{}; bool busy{}; };
    size_t Index(HRAWINPUT token) const {
        const auto address=reinterpret_cast<uintptr_t>(token), first=reinterpret_cast<uintptr_t>(slots_.data());
        if (address<first || address-first>=sizeof(slots_) || (address-first)%sizeof(Slot)) return kCapacity;
        return (address-first)/sizeof(Slot);
    }
    static UINT Error(DWORD value) { SetLastError(value); return UINT(-1); }
    std::array<Slot,kCapacity> slots_{};
    size_t next_{};
    uint64_t deviceTag_{};
};
} // namespace pc2vr::pointer
