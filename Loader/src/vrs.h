#pragma once

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
namespace pc2vr::rtvreg {
struct Info;
}

namespace pc2vr::vrs {

// Foveated rendering with D3D12 variable rate shading (tier 2, image based): while the
// game draws into render targets of the scene's size, a shading-rate image makes the
// pixel shaders run at full rate around each eye's centre of view and at 2x2 / 4x4
// rates further out (2x2 fallback when4x4 is unsupported). Only pixel shading is affected (the game's compute passes run as
// before). Per eye: before each eye's run the image is filled for that eye's centre.

// Command-list hooks (recording time). rt0: the first bound render target (null if none
// or unknown); hasDepth: a depth-stencil view is bound with it. The game's UI passes are
// never foveated.
void OnSetRenderTargets(ID3D12GraphicsCommandList* list, const rtvreg::Info* rt0, bool hasDepth);
void OnListReset(ID3D12GraphicsCommandList* list);

// Settings: on/off and the inner (full rate) / middle (2x2) radii in degrees.
void Configure(bool on, float innerDeg, float middleDeg);
void SetEnabled(bool on);
// Live settings: pc2vr.ini's [Rendering] section is re-read when the file changes
// (checked at most once a second from Present), so foveation can be switched without
// restarting the game.
void WatchIni(const wchar_t* iniPath);
void PollSettings();
bool Enabled();

// From the stereo path, before each eye's run: the scene's render size, the engine
// frustum (tan left/right/up/down) and this eye's centre of view (tan x, tan y). Fills
// the shading-rate image for that eye on the game's queue.
// fullRate: the whole image at full rate (a fixed screen into the game).
void PrepareEye(ID3D12CommandQueue* queue, int eye, uint32_t sceneW, uint32_t sceneH, const float frustum[4],
                const float center[2], bool fullRate);

}  // namespace pc2vr::vrs
