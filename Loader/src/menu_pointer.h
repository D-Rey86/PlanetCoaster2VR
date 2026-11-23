#pragma once
#include <windows.h>
#include "pointer_geometry.h"

namespace pc2vr::pointer::menu {
// Use the game's normal menu input queue thread. No desktop cursor ownership.
bool Prepare();
void Publish(HWND window, POINT pixel, bool down, int wheel, ULONGLONG time);
void Clear();

// The game's fixed image is a frustum viewed from the cyclopean camera. At
// distance d its four tangents give the edges of the same plane in both eyes.
struct Screen {
    XrPosef origin{};
    float left{}, right{}, up{}, down{};
    float distance{};
    Hit Intersect(const Ray& ray) const {
        const float x = distance * (left + right) * .5f;
        const float y = distance * (up + down) * .5f;
        const auto offset = ui::RotatePanelVector(origin.orientation, {x, y, -distance});
        const XrPosef plane{origin.orientation,
            {origin.position.x + offset.x, origin.position.y + offset.y, origin.position.z + offset.z}};
        return Quad(ray, plane, distance * (right - left), distance * (up - down));
    }
};
}
