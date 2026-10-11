#pragma once

namespace pc2vr::gamestate {

// Which scene and camera the game is in, from the game's own named properties
// (measured 2026-10-03 on xr-sim, Notes/06):
//  - "nDisableChangeCameraModeCount" (integer) belongs to the park's camera-mode state:
//    one record in a park (0 with the park camera, 1 in the ride camera; also 0 while the
//    pause and park-management menus are up), none in the main menu.
//  - The ride camera's settings: the table held by "tSelection", with "sMode" ("guest",
//    "cinematic" or "interest", following the camera settings panel),
//    "sSelectedInterestCam", "oTrackHolder", the ride's entity and seat. The game makes
//    short-lived copies of it for its UI; only this one changes with the panel.

enum class Ride { None, Guest, Cinematic, Interest };

// Once per frame on the render thread with the drawn camera's position: checks the found
// records are still live, and searches in the background when they are not known.
void Update(const float cameraPos[3]);

// In a park: its camera-mode record is live.
bool InPark();
// The park's count of camera-mode locks (1 in the ride camera); 0 outside a park.
int CameraModeLocks();
// The ride camera up and its mode (None when there is no ride camera).
Ride RideCamera();

}  // namespace pc2vr::gamestate
