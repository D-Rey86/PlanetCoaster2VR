#pragma once
#include "motion_controls.h"
#include <array>
#include <cstring>

namespace pc2vr::motion {
// Fixed storage; used under the input bridge mutex. No OS input, allocations or
// device enumeration. The timeout releases held inputs if rendering/polling stops.
class Pad {
public:
    static constexpr ULONGLONG kTimeoutMs = 250;
    static constexpr WORD kKeys[] = {
        VK_PAD_A, VK_PAD_B, VK_PAD_X, VK_PAD_Y, VK_PAD_LSHOULDER, VK_PAD_RSHOULDER,
        VK_PAD_START, VK_PAD_BACK, VK_PAD_LTHUMB_PRESS, VK_PAD_RTHUMB_PRESS,
        VK_PAD_DPAD_UP, VK_PAD_DPAD_DOWN, VK_PAD_DPAD_LEFT, VK_PAD_DPAD_RIGHT,
        VK_PAD_LTRIGGER, VK_PAD_RTRIGGER, VK_PAD_LTHUMB_UP, VK_PAD_LTHUMB_DOWN,
        VK_PAD_LTHUMB_LEFT, VK_PAD_LTHUMB_RIGHT, VK_PAD_RTHUMB_UP, VK_PAD_RTHUMB_DOWN,
        VK_PAD_RTHUMB_LEFT, VK_PAD_RTHUMB_RIGHT
    };
    static uint32_t Mask(const XINPUT_GAMEPAD& p) {
        constexpr WORD buttons[] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
            XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
            XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
            XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
        uint32_t mask = 0;
        for (unsigned i = 0; i < 14; ++i) if (p.wButtons & buttons[i]) mask |= 1u << i;
        if (p.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) mask |= 1u << 14;
        if (p.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) mask |= 1u << 15;
        const SHORT axes[] = {p.sThumbLY, p.sThumbLX, p.sThumbRY, p.sThumbRX};
        for (unsigned i = 0; i < 4; ++i) {
            const int threshold = i < 2 ? XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE : XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
            const unsigned positive = 16 + 2*i + (i % 2);
            const unsigned negative = 17 + 2*i - (i % 2);
            if (axes[i] > threshold) mask |= 1u << positive;
            if (axes[i] < -threshold) mask |= 1u << negative;
        }
        return mask;
    }
    void Publish(const Frame& f, ULONGLONG now) {
        if (active_ && now > deadline_) mapper_.Reset();
        active_ = f.hand[0].active || f.hand[1].active;
        deadline_ = now + kTimeoutMs;
        pad_ = mapper_.Read(f, now);
        Queue(Mask(pad_));
    }
    bool Read(ULONGLONG now, XINPUT_GAMEPAD& out) {
        if (active_ && now > deadline_) { active_ = false; pad_ = {}; mapper_.Reset(); Queue(0); }
        out = pad_; return active_;
    }
    void Physical(const XINPUT_GAMEPAD& p) { physical_ = Mask(p); }
    // The packet identifies the MERGED state, including physical-only changes.
    DWORD Packet(const XINPUT_GAMEPAD& p) {
        if (!packetValid_ || std::memcmp(&last_, &p, sizeof(p))) { ++packet_; last_ = p; packetValid_ = true; }
        return packet_;
    }
    bool Held(WORD key) const {
        for (unsigned i = 0; i < std::size(kKeys); ++i) if (kKeys[i] == key) return (level_ & (1u << i)) != 0;
        return false;
    }
    bool Keystroke(XINPUT_KEYSTROKE& out, bool consumeRight) {
        while (count_) {
            const auto e = events_[read_]; read_ = (read_ + 1) % events_.size(); --count_;
            const uint32_t bit = 1u << e.key;
            if (e.down) delivered_ |= bit; else delivered_ &= ~bit;
            if ((physical_ & bit) || (consumeRight && e.key >= 20)) continue;
            out = {}; out.VirtualKey = kKeys[e.key]; out.UserIndex = 0;
            out.Flags = e.down ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
            return true;
        }
        return false;
    }
private:
    struct Event { unsigned key; bool down; };
    void Queue(uint32_t next) {
        uint32_t changed = level_ ^ next;
        unsigned needed = 0; for (unsigned i = 0; i < std::size(kKeys); ++i) if (changed & (1u << i)) ++needed;
        // If a consumer stopped draining events, reconcile its last delivered level
        // instead of leaving an old press/release queued indefinitely.
        if (count_ + needed > events_.size()) { read_ = count_ = 0; changed = delivered_ ^ next; }
        for (unsigned i = 0; i < std::size(kKeys); ++i) if (changed & (1u << i)) {
            events_[(read_ + count_) % events_.size()] = {i, (next & (1u << i)) != 0}; ++count_;
        }
        level_ = next;
    }
    XINPUT_GAMEPAD pad_{}, last_{};
    Mapper mapper_{};
    ULONGLONG deadline_{};
    bool active_{}, packetValid_{};
    DWORD packet_{};
    uint32_t level_{}, delivered_{}, physical_{};
    std::array<Event, 96> events_{};
    size_t read_{}, count_{};
};
}  // namespace pc2vr::motion
