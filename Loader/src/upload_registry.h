#pragma once

#include <cstdint>

struct ID3D12Resource;

namespace pc2vr::uploads {

// Tracks live CPU-visible (UPLOAD heap) buffers the game creates, so a GPU virtual
// address bound as a constant buffer can be translated to the CPU pointer the
// game wrote it through. Only buffers >= kMinSize are tracked (rings, not
// one-off staging copies).
constexpr uint64_t kMinSize = 1ull << 20;

void OnBufferCreated(ID3D12Resource* resource, uint64_t size);

// Returns the CPU address for a GPU VA inside a tracked buffer, or nullptr.
// 'bytes' must fit inside the same buffer.
// Optional mapping property is established once at registration, not per read.
// The caller must keep the bound resource alive while using the returned address;
// translation does not acquire a resource lease.
const uint8_t* Translate(uint64_t gpuVa, uint64_t bytes, bool* writeCombined = nullptr);

// Current registrations (retained if lifetime attachment is unavailable).
// No fixed registration limit.
uint32_t Count();

}  // namespace pc2vr::uploads
