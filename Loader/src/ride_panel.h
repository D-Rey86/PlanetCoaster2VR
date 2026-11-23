#pragma once
#include <cstdint>

namespace pc2vr::ui {
// Actual native DOM bounds in CSS pixels, mirrored only on layout changes.
struct RideLayout {
    uint64_t owner{}, sequence{}, generation{};
    float x{}, y{}, width{}, height{}, viewWidth{}, viewHeight{};
    bool visible{};
    float paintLeft{},paintTop{},paintRight{},paintBottom{};
    float rootX{},rootY{},rootWidth{};
    float scrollX{},scrollY{},scrollWidth{},scrollHeight{};
    float rootHeight{};
};
struct RideClick {
    uint64_t owner{}, sequence{}, time{};
    float x{}, y{};
    uintptr_t window{};
    int scroll{}; // zero is a click; nonzero is one native wheel step
};
}
