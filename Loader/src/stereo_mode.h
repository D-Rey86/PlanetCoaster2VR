#pragma once

namespace pc2vr::xr {
enum class StereoMode : int { TrueStereo, AER, AFW };

constexpr StereoMode ReadStereoMode(int saved, int legacy) {
    if (saved == -1) return legacy == 1 ? StereoMode::AER : StereoMode::TrueStereo;
    if (saved == 1) return StereoMode::AER;
    if (saved == 2) return StereoMode::AFW;
    return StereoMode::TrueStereo;
}
}
