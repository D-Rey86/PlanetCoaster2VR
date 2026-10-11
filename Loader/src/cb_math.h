#pragma once

// Rebuilds the camera-dependent rows of the game's per-view constant buffer
// (Notes/02-frame-map.md) for a new camera orientation and field of view.
// Every formula here is checked against captured game buffers by
// Tests/cb_math_test.cpp: with the game's own basis and FOV the rebuilt rows
// reproduce the captured ones.
//
// Conventions measured from captures:
// - World is y-up and left-handed: right x up = forward.
// - View-projection rows act on camera-relative positions p = world - cameraPos:
//     row4 = P00*right + P02*forward, row5 = P11*up + P12*forward,
//     row6 = k*forward (w = near; reversed-Z, infinite far), row7 = forward.
//   Rows 0-3 add TAA jitter along forward; rows 8-15 are the previous frame's.
// - Frustum side planes (79-82) are world-space (n, n.cameraPos), normals outward;
//   row 83 is (forward, 0).
// - Rows 90-92 are world-space ray vectors: top-left corner, full-width span
//   along right, full-height span along -up; the corner includes TAA jitter.
//   Row 95.zw are those spans' scales.

namespace pc2vr::cbmath {

struct Vec3 {
    float x, y, z;
};

struct Basis {
    Vec3 right, up, forward;
};

// Tangents of the half-angles; left and down are negative for a centred view.
struct Fov {
    float tanLeft, tanRight, tanUp, tanDown;
};

struct ParsedView {
    Basis basis;
    Vec3 pos;
    Fov fov;
    float jitterX, jitterY;  // added to P02 / P12 in the jittered rows
    float depthK, nearZ;     // row 6
};

constexpr int kRows = 106;
using Rows = float[kRows][4];

ParsedView Parse(const Rows& rows);

// Writes rows 0-15 (current and previous projections), 79-83, 86-92, 95.zw and
// 99.w into 'out' (which must already hold a copy of the original rows).
// prevRows0to7: the rows 0-7 this function produced for the previous frame, or
// nullptr to keep the game's previous-frame rows.
void Rebuild(const ParsedView& game, const Basis& basis, const Fov& fov, const float (*prevRows0to7)[4], Rows& out);

// Helpers
Vec3 Cross(const Vec3& a, const Vec3& b);
float Dot(const Vec3& a, const Vec3& b);
Vec3 Normalize(const Vec3& v);

}  // namespace pc2vr::cbmath

