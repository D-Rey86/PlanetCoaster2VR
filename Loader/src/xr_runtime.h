#pragma once

#include <cstdint>

#include "cb_math.h"

struct ID3D12CommandQueue;
struct IDXGISwapChain;
struct ID3D12Resource;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct D3D12_RESOURCE_BARRIER;

namespace pc2vr::xr {

// Native OpenXR session on the game's own D3D12 device and present queue.
// All entry points run on the game's render thread (the thread that calls
// ExecuteCommandLists and Present), so pose selection and submission happen in
// command order.

// Called from the Present hook, before the game's Present. Creates the session
// lazily, submits the frame that was just rendered, then waits for and begins
// the next one.
void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue);

// Head orientation (game world space, rotation only) and FOV to render the
// current frame with. The first call per frame locates the views at the
// predicted display time and records that pose for submission with this frame;
// later calls in the same frame return the same pose.
struct RenderView {
    cbmath::Basis basis;  // final camera basis in game space
    cbmath::Fov fov;
};
bool AcquireRenderView(const cbmath::ParsedView& gameCamera, RenderView& out);

// When on (default), the game's own camera is turned to follow the head; see camctl.
void SetCameraControl(bool on);

// Render size: ScalePercent of the headset's recommended density; FOV margin (total,
// degrees) around both eyes' FOV for head motion during the frames of lag.
void SetResolution(float scale, float fovMarginDeg);
// Process-local menu requests cross to the existing XR render thread atomically.
void RequestResolutionScale(int percent);
struct ResolutionInfo {uint32_t width{},height{};};
ResolutionInfo ResolutionStatus();
// Off restores pre-compatibility sizing, cropping and native camera FOV policy.
// Changes are consumed between completed XR frames, never midway through an eye pair.
void RequestExpandedCoverage(bool on);
// Send each eye only the part of the image its own FOV covers (see CropToEye).
void SetSubmitEyeFov(bool on);
void SetEngineFov(bool on, float maxRadians);
void SetPitchMode(int mode);
// Default on: the engine renders where the head looks and its image is shown with its
// own camera pose; the view buffers are not rewritten.
void SetEngineView(bool on);
// Right-stick turning: snapDeg > 0 turns in steps, otherwise smoothly at smoothDegPerSec.
void SetTurning(float snapDeg, float smoothDegPerSec);
void SetTiltSpeed(float degPerSec);
bool EngineView();

void RequestRecenter();
bool IsRunning();

// ---- true stereo (see stereo.cpp) ----
// worldScale multiplies the runtime's eye separation in the game world.
void SetStereo(bool on, float worldScale);
// Zoom-linked depth ([Stereo] ZoomDepth, ModelDistanceCm): eye separation follows the
// camera's distance to its orbit pivot so the park looks like a 3D model at any zoom.
void SetZoomDepth(bool on, float modelDistanceM, float nearestM);
// Each eye's centre of view in head space, as tan(x), tan(y) (foveated rendering);
// fullRate: this frame is a fixed screen into the game and is not foveated.
bool EyeCentersTan(float out[2][2], bool& fullRate);
// The game's UI goes on the panel this frame: the head-driven park and ride views, and
// the paused park. False on a fixed screen showing the game's own shot.
bool UiOnPanel();
bool StereoEnabled();
// This frame's eye offsets from the head on the head's axes (right, up, forward), in
// metres; false until the frame's head pose has been acquired.
// tanFov: each eye's render frustum (tan of left, right, up, down; head space) when
// perEye is set (each eye renders only its own FOV).
// local: each eye's offset from the head in game space (metres; multiply by worldScale).
bool StereoEyesForFrame(float local[2][3], float& worldScale, float tanFov[2][4], bool& perEye);
void SetPerEyeProjection(bool on);
// The texture the game's render batch (these lists) composites its finished frame
// into, or null if not identified (see OnGameBarrier). index: the list that does.
ID3D12Resource* BatchFrameImage(uint32_t count, ID3D12CommandList* const* lists, int* index = nullptr);
// ---- the UI panel (see ui_layer) ----
// Panel image size for a panel widthDeg wide at the headset's pixel density, the UI
// layer's shape (srcW x srcH), never larger than it. 0 until the headset is known.
void PanelPixels(float widthDeg, uint32_t srcW, uint32_t srcH, uint32_t& w, uint32_t& h);
// On the game's queue, before the panel image is rewritten: waits for the headset's
// last read of it.
void WaitPanelFree(ID3D12CommandQueue* queue);
// The panel image for this frame is written (queued on `queue`): Present shows it.
void PanelCaptured(ID3D12CommandQueue* queue, ID3D12Resource* image, uint32_t w, uint32_t h);
// Copies `image` (the batch's frame image) into eye image `eye` on the game's render
// queue (call right after that eye's run of the render batch).
bool CaptureEye(int eye, ID3D12CommandQueue* queue, ID3D12Resource* image);
// Both eyes of this frame are captured: Present shows them side by side.
void EyesCaptured(ID3D12CommandQueue* queue);
// From the command-list hooks: the game's copy of its finished frame into the back
// buffer reveals the texture the render batch ends in (the eye capture source).
void OnGameCopy(ID3D12Resource* dst, ID3D12Resource* src);
// Command-list hooks: which candidate frame image each list renders to (stereo only).
void OnGameBarrier(ID3D12GraphicsCommandList* list, unsigned count, const D3D12_RESOURCE_BARRIER* barriers);
void OnListReset(ID3D12GraphicsCommandList* list);
void OnResizeBuffers();

}  // namespace pc2vr::xr
