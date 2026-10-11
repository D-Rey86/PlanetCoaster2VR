#pragma once

#include <cstdint>

struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;

namespace pc2vr::patcher {

// Rewrites every copy of the main view's constant buffer just before the GPU
// consumes it, so the game renders with the headset's orientation and FOV.
//
// Root CBV binds are recorded per command list while the game records; when a
// list is submitted, its view CBs are rebuilt in place (upload memory). All
// copies from one game frame get the same rebuilt rows, built from one head
// pose that the XR module pairs with that frame.

void OnCommandListReset(ID3D12GraphicsCommandList* list);
void OnRootCbvBound(ID3D12GraphicsCommandList* list, uint64_t gpuVa);
void OnExecute(uint32_t count, ID3D12CommandList* const* lists);

void SetEnabled(bool on);
// Experiment: compute everything (and drive the camera) but leave the buffers as the game wrote them.
void SetWriteEnabled(bool on);
// Experiment: keep the game's own FOV in the rebuilt rows (orientation only).
void SetUseHeadsetFov(bool on);

// CPU pointers (upload memory) of the distinct main-view camera CB copies bound
// by these lists; returns how many (at most max).
int MainViewCbs(uint32_t count, ID3D12CommandList* const* lists, uint8_t** out, int max);
// The same, from the copies OnExecute read for this submission (no new reads of slow
// upload memory), with their rows as read; -1 if OnExecute did not just see these lists.
int MainViewCopies(uint32_t count, ID3D12CommandList* const* lists, uint8_t** out, const float (**rows)[4], int max);
void OnPresent(uint64_t frame);

}  // namespace pc2vr::patcher
