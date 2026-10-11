#pragma once

namespace pc2vr::ridecam {

// The on-ride camera (the ride menu's camera button). Measured 2026-10-03 on a coaster:
// the game keeps the ride camera's look direction as a (yaw, pitch) pair relative to
// the seat, in radians, pitch positive = looking down (the park camera's convention).
// The right stick turns it, and the game renders exactly what is written there and
// keeps it, over any range (the stick stops at about 80 degrees; writes do not). It is a
// tagged property record next to a marked FOV record; the render camera has its own
// marked FOV record, the one the rendered FOV follows when nudged (see ride_camera.cpp
// and Notes/06-ride-camera.md).

// Once per frame while the game draws a camera the mod does not drive (renderedFov:
// that frame's vertical FOV; pos: its camera position). Searches, verifies, and checks
// a located camera is still the one drawn (the game's values still hold ours).
void Update(float renderedFov, const float pos[3]);
// Not on the ride camera: puts the game's FOV and look back (restore) and forgets them.
// restore = false when the park has closed (its records may be freed): writes nothing.
void Release(bool restore = true);
bool Located();
// Writes the look pair (seat-relative) and the FOV; false if the camera is gone.
bool Write(float yaw, float pitch, float verticalFov);

}  // namespace pc2vr::ridecam
