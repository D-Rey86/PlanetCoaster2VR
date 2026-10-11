#pragma once

#include "cb_math.h"

namespace pc2vr::camctl {

// The game's camera controller keeps its orientation as a float pair
// (yaw, pitch) in radians: yaw = atan2(forward.x, forward.z), pitch positive =
// looking down (measured 2026-10-01). Writing the pair turns the game camera in
// place and the game keeps the written value. The game's own input (Q/E,
// mouse) changes the same pair.
//
// Several heap copies of the pair exist (2-3 seen) and their neighbours differ
// between runs, so candidates are found by value and then verified one at a
// time: nudge the yaw, and keep the copy that the rendered camera follows.

// Called once per rendered frame on the render thread with the game's own
// (unmodified) camera. Drives scanning and verification.
void Update(const cbmath::Vec3& cameraPos, const cbmath::Vec3& forward, float renderedVerticalFov);

// True once a verified controller is known.
bool IsLocated();

bool Read(float& yaw, float& pitch);
bool Write(float yaw, float pitch);
// Distance (game metres) from the camera to the point its orbit/zoom pivots on, i.e.
// how far away what you are looking at is. False until the pivot has been located.
bool OrbitDistance(float& metres);

// The engine camera's vertical FOV (radians) decides what the game culls and
// streams. It is a property next to the controller, found and verified the same
// way after the controller is located. Widening it to cover the headset view
// removes the black edges; the image itself always uses the headset FOV.
bool SetEngineFov(float verticalRadians);
void RestoreEngineFov();
// Test only: off = keep the controller while the game resets its camera (park reload).
void SetResetGuard(bool on);

}  // namespace pc2vr::camctl
