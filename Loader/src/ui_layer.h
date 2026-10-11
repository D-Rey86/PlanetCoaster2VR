#pragma once

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct D3D12_RESOURCE_BARRIER;

namespace pc2vr::rtvreg {
struct Info;
}

namespace pc2vr::ui {

// The game's UI on its own panel in front of the player.
//
// Measured (RenderDoc, menu and park): the UI library (Coherent) renders its views into
// their own textures; one list of the render batch composites them, premultiplied,
// into the UI layer (window-size RGBA8, drawn through an sRGB view); the next list (the
// last one that writes the frame image) is the display mapper:
//   frame = tonemap(scene) * (1 - ui.a) + ui.rgb
// So a UI layer cleared to zero before that list leaves the 3D scene alone. Between the
// two lists of each eye's run, the UI layer is downsampled (first eye only) into the
// panel image and then cleared; the headset shows the panel as a quad layer.
//
// In VR the game's window is the shape of the headset view, ~110 degrees across, and
// the UI is laid out on its edges: HUD at ~50 degrees out, menus as large as the view.

// Recording-time hooks.
void OnSetRenderTargets(ID3D12GraphicsCommandList* list, const rtvreg::Info* rt0);
void OnBarrier(ID3D12GraphicsCommandList* list, unsigned count, const D3D12_RESOURCE_BARRIER* barriers);
void OnListReset(ID3D12GraphicsCommandList* list);

// Settings: [UI] in pc2vr.ini, re-read while the game runs (at most once a second).
void WatchIni(const wchar_t* iniPath);
void PollSettings();
struct PanelSettings {
    bool on = true;
    float widthDeg = 55.f;     // angular width as seen from the head
    float distanceM = 1.5f;
    float heightDeg = -5.f;    // centre above (+) / below (-) eye level
    float horizontalDeg = 0.f;
    bool followHmd = false;
    bool markers3d = true;     // the game's world markers stay in the 3D view (off: on the panel with the HUD)
};
PanelSettings Settings();
// Menu changes publish one complete snapshot; disk persistence is off the render thread.
void ApplySettings(const PanelSettings& settings);

// Stereo path, per render batch. compositeIndex: the list that writes the frame image.
// Returns where to split the batch (the UI work runs before lists[split]), or -1 to
// run the batch whole (UI stays in the image).
int PlanBatch(uint32_t count, ID3D12CommandList* const* lists, ID3D12Resource* frameImage, int compositeIndex);
using ExecuteFn = void(__stdcall*)(ID3D12CommandQueue*, uint32_t, ID3D12CommandList* const*);
// The list that composites the HUD over the game's world markers (path previews, guide
// arrows, selection brushes: drawn in 3D, then written into the UI layer before the
// HUD), or -1. Before it, SaveMarkers takes the markers out of the UI layer; Separate
// puts them back after the HUD is captured, so they stay in each eye's 3D image.
int MarkerSplit();
void SaveMarkers(ID3D12CommandQueue* queue, int eye, ExecuteFn execute);
// Between the two parts of eye `eye`'s run.
void Separate(ID3D12CommandQueue* queue, int eye, ExecuteFn execute);
// After both runs: remembers the UI layer's state for the next batch.
void FinishBatch(uint32_t count, ID3D12CommandList* const* lists);

}  // namespace pc2vr::ui
