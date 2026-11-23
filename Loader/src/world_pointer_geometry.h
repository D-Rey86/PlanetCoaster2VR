#pragma once
#include "pointer_geometry.h"
#include "cb_math.h"

namespace pc2vr::pointer {
struct WorldRay { cbmath::Vec3 origin{}, direction{}; };

// The submitted camera and the native render camera describe the SAME image.
// Resolve LOCAL vectors on the submitted right/up/forward axes, then rebuild
// them on the game's corresponding axes. This is the inverse of EngineView's
// camera mapping, including recenter, body turn, tilt and tracking roll. It does
// not assume that the delayed native camera has the current HMD orientation.
inline cbmath::Vec3 LocalToWorldVector(const XrVector3f& v, const XrVector3f (&axes)[3],
                                     const cbmath::Basis& game) {
    const float x=Dot(v,axes[0]), y=Dot(v,axes[1]), z=Dot(v,axes[2]);
    return {x*game.right.x+y*game.up.x+z*game.forward.x,
            x*game.right.y+y*game.up.y+z*game.forward.y,
            x*game.right.z+y*game.up.z+z*game.forward.z};
}
inline WorldRay ToWorld(const Ray& local, const XrVector3f& head,
                        const XrQuaternionf& submitted, const cbmath::ParsedView& game, float scale) {
    const XrVector3f axes[]{ui::RotatePanelVector(submitted,{1,0,0}),
        ui::RotatePanelVector(submitted,{0,1,0}),ui::RotatePanelVector(submitted,{0,0,-1})};
    const auto offset=LocalToWorldVector(Sub(local.origin,head),axes,game.basis);
    const auto direction=LocalToWorldVector(local.direction,axes,game.basis);
    return {{game.pos.x+scale*offset.x,game.pos.y+scale*offset.y,game.pos.z+scale*offset.z},direction};
}
// Exact inverse of the current camera's LOCAL-to-world mapping. A native query
// may finish between XR snapshots; render its measured ray/hit in this frame's
// space rather than moving the old collision point onto a different hand ray.
struct WorldSpace {
    cbmath::Vec3 origin{};
    cbmath::Basis axes{{1,0,0},{0,1,0},{0,0,1}};
    XrVector3f Point(const cbmath::Vec3& point,float scale) const {
        const cbmath::Vec3 delta{point.x-origin.x,point.y-origin.y,point.z-origin.z};
        return {cbmath::Dot(delta,axes.right)/scale,cbmath::Dot(delta,axes.up)/scale,cbmath::Dot(delta,axes.forward)/scale};
    }
    XrVector3f Vector(const cbmath::Vec3& vector) const {
        return {cbmath::Dot(vector,axes.right),cbmath::Dot(vector,axes.up),cbmath::Dot(vector,axes.forward)};
    }
};
inline WorldSpace Space(const XrVector3f& head,const XrQuaternionf& submitted,const cbmath::ParsedView& game,float scale) {
    const XrVector3f axes[]{ui::RotatePanelVector(submitted,{1,0,0}),ui::RotatePanelVector(submitted,{0,1,0}),
        ui::RotatePanelVector(submitted,{0,0,-1})};
    const auto x=LocalToWorldVector({1,0,0},axes,game.basis),y=LocalToWorldVector({0,1,0},axes,game.basis),
        z=LocalToWorldVector({0,0,1},axes,game.basis),offset=LocalToWorldVector({-head.x,-head.y,-head.z},axes,game.basis);
    return {{game.pos.x+scale*offset.x,game.pos.y+scale*offset.y,game.pos.z+scale*offset.z},{x,y,z}};
}
inline bool Finite(const cbmath::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace pc2vr::pointer
