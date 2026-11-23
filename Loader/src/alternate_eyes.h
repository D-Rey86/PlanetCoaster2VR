#pragma once
#include <cstdint>

namespace pc2vr::xr {
// Eye identity comes from the capture after the main render batch. Frame is
// the Present/EndFrame serial, independent of refresh rate and elapsed time.
class AlternateEyes {
public:
    int Select(uint64_t frame) const {
        if (latestEye_ < 0 || newest_ != frame - 1) return -1;
        return latestEye_ ^ 1;
    }
    void Seed(uint64_t frame) {
        newest_ = frame;
        latestEye_ = 1;
        reusedEye_ = -1;
    }
    bool Capture(uint64_t frame, int eye) {
        if (Select(frame) != eye) {
            Reset();
            return false;
        }
        newest_ = frame;
        latestEye_ = eye;
        reusedEye_ = eye ^ 1;
        return true;
    }
    int ReusedEye(uint64_t frame) const { return newest_ == frame ? reusedEye_ : -1; }
    void Reset() { latestEye_ = reusedEye_ = -1; }
private:
    uint64_t newest_{};
    int latestEye_ = -1, reusedEye_ = -1;
};
}
