#pragma once

#include <cstdint>

#include "game_state.h"

namespace pc2vr::xr {

// Park/Guest camera ownership changes independently of when the new view is
// ready. Keep the native acquisition running while the compositor hides the cut.
class CameraHandoff {
public:
    struct Sample { float fade{}; bool hold{}; };

    Sample Update(uint64_t now, bool inPark, bool paused, bool locked, gamestate::Ride mode,
                  bool parkReady, bool guestReady, bool canHold) {
        if (!inPark || paused) {
            *this = {};
            return {};
        }

        const bool other = mode == gamestate::Ride::Cinematic || mode == gamestate::Ride::Interest;
        const Camera ready = parkReady ? Camera::Park : guestReady ? Camera::Guest : Camera::Unknown;
        if (phase_ == Phase::Idle) {
            const bool changing = (stable_ == Camera::Park && locked && !other) ||
                (stable_ == Camera::Guest && !locked);
            if (!changing) {
                if (other) stable_ = Camera::Unknown;
                else if (ready != Camera::Unknown) stable_ = ready;
                return {};
            }
            phase_ = canHold ? Phase::Out : Phase::Black;
            started_ = phaseAt_ = now;
        }
        if (phase_ == Phase::Out) {
            if (now - phaseAt_ < kOutMs) return {Ease(now - phaseAt_, kOutMs), true};
            phase_ = Phase::Black;
            phaseAt_ = now;
        }
        if (phase_ == Phase::Black) {
            const Camera target = locked ? Camera::Guest : Camera::Park;
            if (now - started_ < kTimeoutMs && (now - phaseAt_ < kBlackMs || (!other && ready != target)))
                return {1.f, false};
            phase_ = Phase::In;
            phaseAt_ = now;
        }
        if (now - phaseAt_ < kInMs) return {1.f - Ease(now - phaseAt_, kInMs), false};
        phase_ = Phase::Idle;
        stable_ = ready;
        return {};
    }

private:
    enum class Camera { Unknown, Park, Guest };
    enum class Phase { Idle, Out, Black, In };
    static constexpr uint64_t kOutMs = 90, kBlackMs = 45, kInMs = 190, kTimeoutMs = 2600;

    static float Ease(uint64_t elapsed, uint64_t duration) {
        const float t = static_cast<float>(elapsed) / static_cast<float>(duration);
        return t * t * (3.f - 2.f * t);
    }

    Camera stable_{};
    Phase phase_{};
    uint64_t started_{}, phaseAt_{};
};

} // namespace pc2vr::xr
