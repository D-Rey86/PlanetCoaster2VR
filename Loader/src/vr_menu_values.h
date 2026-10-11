#pragma once
#include <cstring>

namespace pc2vr::menu {
struct Setting {
    const char* name;
    const wchar_t* section;
    const wchar_t* key;
    int minimum, maximum, fallback;
    const char* cheeky; // optional dependency name; values are whole percentages if fractional
    bool fractional;
};
inline constexpr Setting kSettings[] = {
    {"Resolution", L"Resolution", L"ScalePercent", 25, 200, 100, nullptr, false},
    {"ExpandedCoverage", L"Resolution", L"ExpandedCoverage", 0, 1, 0, nullptr, false},
    {"Foveated", L"Rendering", L"Foveated", 0, 1, 1, nullptr, false},
    {"UIWidth", L"UI", L"PanelWidthDeg", 20, 120, 55, nullptr, false},
    {"UIDistance", L"UI", L"PanelDistanceCm", 30, 1000, 150, nullptr, false},
    {"UIHeight", L"UI", L"PanelHeightDeg", -60, 60, -5, nullptr, false},
    {"UIHorizontal", L"UI", L"PanelHorizontalDeg", -60, 60, 0, nullptr, false},
    {"UIFollowHmd", L"UI", L"FollowHmd", 0, 1, 0, nullptr, false},
    {"CheekyEnabled", nullptr, nullptr, 0, 1, 0, "Enabled", false},
    {"CheekyWidth", nullptr, nullptr, 20, 100, 55, "Width", true},
    {"CheekyHeight", nullptr, nullptr, 20, 100, 45, "Height", true},
    {"CheekyPeripheralDlaa", nullptr, nullptr, 0, 1, 1, "PeripheralDlaa", false},
    {"CheekyPeripheralScale", nullptr, nullptr, 20, 100, 75, "PeripheralDlaaScale", true},
    {"CheekyCenterSupersampling", nullptr, nullptr, 100, 200, 100, "CenterSupersampling", true},
    {"CheekyCenterMode", nullptr, nullptr, 0, 1, 0, "CenterMode", false},
    {"CheekyRoundness", nullptr, nullptr, 0, 100, 0, "Roundness", true},
    {"CheekyTransition", nullptr, nullptr, 0, 30, 4, "TransitionWidth", true},
    {"CheekyAlignedHeight", nullptr, nullptr, -100, 100, 0, "AlignedHeightOffset", true},
    {"CheekyManualHeight", nullptr, nullptr, -100, 100, 0, "HeightOffset", true},
    {"CheekyBorder", nullptr, nullptr, 0, 1, 0, "AlignmentBorder", false},
    {"CheekyGazeSmoothing", nullptr, nullptr, 0, 100, 20, "GazeSmoothingMs", false},
    {"CheekyGazeHold", nullptr, nullptr, 0, 1000, 400, "GazeHoldMs", false},
};
inline const Setting* FindSetting(const char* name) {
    for (const auto& setting : kSettings) if (!std::strcmp(name, setting.name)) return &setting;
    return nullptr;
}
inline bool ValidValue(const Setting& setting, int value) {
    return value >= setting.minimum && value <= setting.maximum;
}
}
