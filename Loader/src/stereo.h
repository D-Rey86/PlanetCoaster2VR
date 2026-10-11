#pragma once

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12CommandList;

namespace pc2vr::stereo {

// True stereo: the game's main render batch (one ExecuteCommandLists with all the 3D
// work, the UI and the final composite) is executed once per eye. Before each run
// the main view's camera buffers get that eye's position; the GPU waits on a fence
// between the runs while a helper thread moves the camera, so the game's submit
// thread never blocks. After each run the finished back buffer is copied to that
// eye's image (xr::CaptureEye). On with [Loader] Stereo=1 (needs EngineView).
//
using ExecuteFn = void(__stdcall*)(ID3D12CommandQueue*, uint32_t, ID3D12CommandList* const*);

bool Active();
// Returns true when it submitted the lists itself (the caller must not).
bool OnExecute(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists, ExecuteFn execute);

}  // namespace pc2vr::stereo
