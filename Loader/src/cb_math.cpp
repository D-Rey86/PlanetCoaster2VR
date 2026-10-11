#include "cb_math.h"

#include <cmath>
#include <cstring>

namespace pc2vr::cbmath {
namespace {

Vec3 Row3(const float* r) { return {r[0], r[1], r[2]}; }

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

void Set(float* row, const Vec3& v, float w) {
    row[0] = v.x;
    row[1] = v.y;
    row[2] = v.z;
    row[3] = w;
}

void Plane(float* row, const Vec3& n, const Vec3& pos) { Set(row, n, Dot(n, pos)); }

}  // namespace

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 Normalize(const Vec3& v) {
    const float len = std::sqrt(Dot(v, v));
    return len > 0 ? Scale(v, 1.f / len) : v;
}

ParsedView Parse(const Rows& rows) {
    ParsedView v{};
    v.basis.right = Row3(rows[86]);
    v.basis.up = Row3(rows[87]);
    v.basis.forward = Row3(rows[88]);
    v.pos = Row3(rows[84]);
    const Vec3& r = v.basis.right;
    const Vec3& u = v.basis.up;
    const Vec3& f = v.basis.forward;

    const float p00 = Dot(Row3(rows[4]), r), p02 = Dot(Row3(rows[4]), f);
    const float p11 = Dot(Row3(rows[5]), u), p12 = Dot(Row3(rows[5]), f);
    const float w = 2.f / p00, sx = -p02 * w;
    const float h = 2.f / p11, sy = -p12 * h;
    v.fov.tanRight = (sx + w) * 0.5f;
    v.fov.tanLeft = (sx - w) * 0.5f;
    v.fov.tanUp = (sy + h) * 0.5f;
    v.fov.tanDown = (sy - h) * 0.5f;

    v.jitterX = Dot(Row3(rows[0]), f) - p02;
    v.jitterY = Dot(Row3(rows[1]), f) - p12;
    v.depthK = Dot(Row3(rows[6]), f);
    v.nearZ = rows[6][3];
    return v;
}

void Rebuild(const ParsedView& game, const Basis& b, const Fov& fov, const float (*prev)[4], Rows& out) {
    const Vec3& r = b.right;
    const Vec3& u = b.up;
    const Vec3& f = b.forward;
    const float width = fov.tanRight - fov.tanLeft;
    const float height = fov.tanUp - fov.tanDown;
    const float p00 = 2.f / width, p02 = -(fov.tanRight + fov.tanLeft) / width;
    const float p11 = 2.f / height, p12 = -(fov.tanUp + fov.tanDown) / height;

    if (prev) {
        std::memcpy(out[8], prev[0], sizeof(float) * 4 * 8);
    }

    // Unjittered 4-7, jittered 0-3.
    Set(out[4], Add(Scale(r, p00), Scale(f, p02)), 0.f);
    Set(out[5], Add(Scale(u, p11), Scale(f, p12)), 0.f);
    Set(out[6], Scale(f, game.depthK), game.nearZ);
    Set(out[7], f, 0.f);
    Set(out[0], Add(Scale(r, p00), Scale(f, p02 + game.jitterX)), 0.f);
    Set(out[1], Add(Scale(u, p11), Scale(f, p12 + game.jitterY)), 0.f);
    std::memcpy(out[2], out[6], sizeof(float) * 4);
    std::memcpy(out[3], out[7], sizeof(float) * 4);

    // Frustum planes through the camera, normals pointing out of the frustum (as
    // captured); near plane is (forward, 0).
    const Vec3 dL = Add(f, Scale(r, fov.tanLeft));
    const Vec3 dR = Add(f, Scale(r, fov.tanRight));
    const Vec3 dU = Add(f, Scale(u, fov.tanUp));
    const Vec3 dD = Add(f, Scale(u, fov.tanDown));
    Plane(out[79], Normalize(Cross(dL, u)), game.pos);
    Plane(out[80], Normalize(Cross(u, dR)), game.pos);
    Plane(out[81], Normalize(Cross(dU, r)), game.pos);
    Plane(out[82], Normalize(Cross(r, dD)), game.pos);
    Set(out[83], f, 0.f);

    Set(out[86], r, 0.f);
    Set(out[87], u, 0.f);
    Set(out[88], f, 0.f);
    Set(out[89], f, 0.f);

    // The corner ray includes the TAA jitter (measured: offset = -jitter * half-span).
    const float cornerX = fov.tanLeft - game.jitterX * width * 0.5f;
    const float cornerY = fov.tanUp - game.jitterY * height * 0.5f;
    Set(out[90], Add(f, Add(Scale(r, cornerX), Scale(u, cornerY))), 0.f);
    Set(out[91], Scale(r, width), 0.f);
    Set(out[92], Scale(u, -height), 0.f);
    out[95][2] = width;
    out[95][3] = height;
    out[99][3] = std::atan(fov.tanUp) - std::atan(fov.tanDown);
}

}  // namespace pc2vr::cbmath

