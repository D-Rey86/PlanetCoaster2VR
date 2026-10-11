#pragma once

namespace pc2vr::input {

// Hooks xinput1_4!XInputGetState and hid!HidP_GetData (system DLLs, not game
// code). While VR camera control is active the right stick is consumed: the game
// never sees it; X drives VR turning and Y a tilt added to the head view.
bool Install();

// Right stick X in [-1, 1] after the dead zone, from the most recent poll of pad 0.
float TurnAxis();
// Right stick Y in [-1, 1] after the dead zone; +1 = pushed up.
float TiltAxis();

// When false the stick passes through to the game unchanged (menus, no VR).
void SetConsumeRightStick(bool on);
}  // namespace pc2vr::input
