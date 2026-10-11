#pragma once

#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace pc2vr::frustum {

inline float CameraLimit(float radians, bool expanded = true) {
    constexpr float pi = 3.14159265358979323846f;
    const float defaultLimit = (expanded ? 155.f : 130.f) * pi / 180.f;
    return std::isfinite(radians) && radians > 0.f && radians < pi ? radians : defaultLimit;
}

inline bool PixelDensity(const XrFovf (&fov)[2], const uint32_t (&width)[2],
                         const uint32_t (&height)[2], float& density, float* horizontal = nullptr) {
    density = 0.f;
    if (horizontal) *horizontal = 0.f;
    float image = 0.f, panel = 0.f;
    for (int i = 0; i < 2; ++i) {
        const float sx = std::tan(fov[i].angleRight) - std::tan(fov[i].angleLeft);
        const float sy = std::tan(fov[i].angleUp) - std::tan(fov[i].angleDown);
        if (!width[i] || !height[i] || !std::isfinite(sx) || !std::isfinite(sy) || sx <= 0.1f || sy <= 0.1f) return false;
        panel = std::max(panel, width[i] / sx);
        image = std::max(image, std::max(width[i] / sx, height[i] / sy));
    }
    density = image;
    if (horizontal) *horizontal = panel;
    return true;
}

// Bounds only: the rendered image retains its own projection and pose. The runtime
// reprojects that declared image into the physical (possibly canted) eye view.
// In front of the reference plane, a ratio of affine functions attains its extrema
// on a rectangle's corners. Behind/crossing that plane needs a different camera
// architecture; it cannot be represented by four forward pinhole FOV angles.
struct Rotation { double x, y, z, w; };

inline bool Normalize(const XrQuaternionf& q, Rotation& r) {
    const double n=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
    if(!std::isfinite(n) || n<1e-12) return false;
    const double inv=1.0/std::sqrt(n);
    r={q.x*inv,q.y*inv,q.z*inv,q.w*inv};
    return true;
}

inline bool Relative(const XrQuaternionf& eye, const XrQuaternionf& reference, Rotation& r) {
    Rotation e{},h{};
    if(!Normalize(eye,e)||!Normalize(reference,h))return false;
    r={h.w*e.x-h.x*e.w-h.y*e.z+h.z*e.y,
       h.w*e.y+h.x*e.z-h.y*e.w-h.z*e.x,
       h.w*e.z-h.x*e.y+h.y*e.x-h.z*e.w,
       h.w*e.w+h.x*e.x+h.y*e.y+h.z*e.z};
    // q and -q describe the same rotation; canonicalize without an angular cutoff.
    if(r.w<0) r={-r.x,-r.y,-r.z,-r.w};
    return true;
}

inline bool Bounds(const XrFovf& f, const Rotation& q, XrFovf& out) {
    constexpr double halfPi=1.57079632679489661923;
    const float angles[]{f.angleLeft,f.angleRight,f.angleUp,f.angleDown};
    for(float a:angles)if(!std::isfinite(a)||std::fabs(a)>=halfPi)return false;
    if(!(f.angleLeft<f.angleRight && f.angleDown<f.angleUp))return false;
    if(q.x==0 && q.y==0 && q.z==0) {out=f;return true;}
    const double xx=q.x*q.x,yy=q.y*q.y,zz=q.z*q.z;
    const double m[3][3]={{1-2*(yy+zz),2*(q.x*q.y-q.z*q.w),2*(q.x*q.z+q.y*q.w)},
                          {2*(q.x*q.y+q.z*q.w),1-2*(xx+zz),2*(q.y*q.z-q.x*q.w)},
                          {2*(q.x*q.z-q.y*q.w),2*(q.y*q.z+q.x*q.w),1-2*(xx+yy)}};
    const double xs[]{std::tan(double(f.angleLeft)),std::tan(double(f.angleRight))};
    const double ys[]{std::tan(double(f.angleDown)),std::tan(double(f.angleUp))};
    double l=halfPi,r=-halfPi,u=-halfPi,d=halfPi;
    for(double x:xs)for(double y:ys){
        const double px=m[0][0]*x+m[0][1]*y-m[0][2];
        const double py=m[1][0]*x+m[1][1]*y-m[1][2];
        const double pz=m[2][0]*x+m[2][1]*y-m[2][2];
        if(!(pz<0) || !std::isfinite(pz))return false;
        const double ax=std::atan2(px,-pz),ay=std::atan2(py,-pz);
        l=std::min(l,ax);r=std::max(r,ax);u=std::max(u,ay);d=std::min(d,ay);
    }
    // Round outward so a float conversion cannot exclude an exact corner ray.
    const float infinity=std::numeric_limits<float>::infinity();
    out={std::nextafter(static_cast<float>(l),-infinity),std::nextafter(static_cast<float>(r),infinity),
         std::nextafter(static_cast<float>(u),infinity),std::nextafter(static_cast<float>(d),-infinity)};
    // The exact bound can be in front but too close to 90 degrees to encode as
    // outward-rounded float OpenXR angles. Do not return a wrapped tangent.
    const float result[]{out.angleLeft,out.angleRight,out.angleUp,out.angleDown};
    for(float a:result)if(std::fabs(double(a))>=halfPi)return false;
    return true;
}

struct Cache {
    bool initialized=false,valid=false;
    XrFovf fov{}; Rotation rotation{}; XrFovf bounds{};
    bool Get(const XrFovf& f, const XrQuaternionf& eye, const XrQuaternionf& reference, XrFovf& out) {
        Rotation q{};
        if(!Relative(eye,reference,q))return false;
        if(!initialized || std::memcmp(&f,&fov,sizeof(f)) || std::memcmp(&q,&rotation,sizeof(q))){
            fov=f;rotation=q;initialized=true;valid=Bounds(f,q,bounds);
        }
        if(valid)out=bounds;
        return valid;
    }
};

} // namespace pc2vr::frustum
