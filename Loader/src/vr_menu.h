#pragma once
#include "ride_panel.h"
#include "selection_panel.h"

struct ID3D12Device;

namespace pc2vr::menu {
// Startup-only Gameface resource integration. Does not create a UI view or GPU surface.
void Install(const wchar_t* modDirectory);
// Called once when the existing XR device is discovered, never from a menu poll.
void DeviceAvailable(ID3D12Device* device);
// Actual pause-screen component visibility, independent of cursor/camera state.
bool PauseVisible();
motion::SelectionPanel SelectionPanelState();
ui::RideLayout RidePanelLayout();
ui::RideLayout CameraPanelLayout();
ui::RideLayout BuildPanelLayout();
ui::RideLayout DashboardLayout();
bool PostRideClick(const ui::RideClick& click);
bool PostCameraClick(const ui::RideClick& click);
bool PostBuildClick(const ui::RideClick& click);
bool PostDashboardClick(const ui::RideClick& click);
void BuildAvailability(bool ready);
void BuildConfigurationChanged();
void RideInputSnapshot(bool active,uint64_t time);
}
