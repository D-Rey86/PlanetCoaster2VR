#pragma once
#include <cstdint>

namespace pc2vr::motion {
// Event-driven snapshot of the native information-panel component. Unknown,
// frozen, editing or higher-priority UI contexts must retain ordinary input.
struct SelectionPanel {
    uint64_t owner{};
    bool known{},visible{},canClose{};
    bool blocked{}; // A modal may belong to a separate Gameface document.
};
}
