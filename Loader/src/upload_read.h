#pragma once

#include <cstddef>

namespace pc2vr::uploads {

// Copies a fresh CPU snapshot, retaining the safe-read failure contract. Only
// confirmed write-combined mappings can use the CPU's streaming-load hint.
bool ReadSnapshot(void* dst, const void* src, size_t bytes, bool writeCombined);

}  // namespace pc2vr::uploads
