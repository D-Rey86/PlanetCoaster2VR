#pragma once

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct D3D12_RESOURCE_BARRIER;

namespace pc2vr::depth {

// Nearest visible distance, from the game's own depth buffer. After the render batch,
// a small compute pass on the game's queue reduces the scene depth (reversed-Z) to the
// nearest value in each of 16x16 screen tiles; the CPU reads that back two frames later.
// The depth buffer and its state at the end of the batch come from the batch's own
// barriers; when either is not known, nothing is measured (no guessing of states).

// Command-list hooks: transitions of depth-stencil resources, per list.
void OnBarrier(ID3D12GraphicsCommandList* list, unsigned count, const D3D12_RESOURCE_BARRIER* barriers);
void OnListReset(ID3D12GraphicsCommandList* list);

// After the batch has been executed (both eye runs): measures its depth buffer.
// renderW/H: the main view's render size (depth matches it); nearZ: the projection's
// near distance (reversed-Z infinite: depth = nearZ / distance).
void Measure(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists, uint32_t renderW,
             uint32_t renderH, float nearZ);

// Latest completed measurement (game metres): the nearest visible distance (robust to a
// few stray pixels) and the typical distance at the centre of the view (0 when the centre
// is mostly sky). False when no measurement has completed recently.
bool NearestDistance(float& metres, float& center);

}  // namespace pc2vr::depth
