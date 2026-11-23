#pragma once
#include "panel_geometry.h"
#include <cmath>

namespace pc2vr::pointer {
// Geometry shared by rendering and hit testing. OpenXR LOCAL space, metres,
// aim's -Z axis. Pixel mapping is left->right and top->bottom, as the UI texture.
// No native world-ray substitution is assumed by this helper.
struct Ray { XrVector3f origin{}, direction{}; };
struct Hit { bool valid{}; float distance{}, u{}, v{}; XrVector3f point{}; };
inline Ray AimRay(const XrPosef& aim) { return {aim.position, ui::RotatePanelVector(aim.orientation, {0, 0, -1})}; }
inline float Dot(const XrVector3f& a, const XrVector3f& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline XrVector3f Sub(const XrVector3f& a, const XrVector3f& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Hit Quad(const Ray& ray, const XrPosef& plane, float width, float height) {
    Hit hit{};
    if (!(width > 0.f && height > 0.f)) return hit;
    const auto normal = ui::RotatePanelVector(plane.orientation, {0, 0, 1});
    const float den = Dot(ray.direction, normal);
    if (!(den < -1e-6f)) return hit; // parallel/back-facing rays don't select
    const float t = Dot(Sub(plane.position, ray.origin), normal) / den;
    if (!(t > 0.f && std::isfinite(t))) return hit;
    const XrVector3f point{ray.origin.x+t*ray.direction.x, ray.origin.y+t*ray.direction.y, ray.origin.z+t*ray.direction.z};
    const auto relative = Sub(point, plane.position);
    const float u = .5f + Dot(relative, ui::RotatePanelVector(plane.orientation, {1, 0, 0})) / width;
    const float v = .5f - Dot(relative, ui::RotatePanelVector(plane.orientation, {0, 1, 0})) / height;
    if (!(u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f)) return hit;
    return {true, t, u, v, point};
}
}  // namespace pc2vr::pointer
