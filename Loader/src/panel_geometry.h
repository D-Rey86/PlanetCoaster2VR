#pragma once
#include <cmath>
#include <openxr/openxr.h>
#include "ui_layer.h"

namespace pc2vr::ui {
inline XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline XrVector3f RotatePanelVector(const XrQuaternionf& q, const XrVector3f& v) {
    const auto rotated=Multiply(Multiply(q,{v.x,v.y,v.z,0}),{-q.x,-q.y,-q.z,q.w});
    return {rotated.x,rotated.y,rotated.z};
}
inline XrQuaternionf PanelOffset(const PanelSettings& p) {
    const float yaw=-p.horizontalDeg*0.01745329252f, pitch=p.heightDeg*0.01745329252f;
    return Multiply({0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)},
                    {std::sin(pitch*.5f),0,0,std::cos(pitch*.5f)});
}
inline XrPosef PanelPose(const PanelSettings& p,const XrPosef& anchor,const XrPosef& head,bool positionValid) {
    XrPosef base=anchor;
    if (p.followHmd) {
        base.orientation=head.orientation;
        if (positionValid) base.position=head.position;
    }
    XrPosef pose{};
    pose.orientation=Multiply(base.orientation,PanelOffset(p));
    const auto offset=RotatePanelVector(pose.orientation,{0,0,-p.distanceM});
    pose.position={base.position.x+offset.x,base.position.y+offset.y,base.position.z+offset.z};
    return pose;
}
}
