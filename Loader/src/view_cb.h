#pragma once

#include <cstdint>

namespace pc2vr::viewcb {

// Per-view constant buffer layout (cbuffer0, 1700 bytes = 106 float4), see
// Notes/02-frame-map.md. Indices are float4 rows.
constexpr uint32_t kSize = 1700;
constexpr uint32_t kRows = 106;
constexpr uint32_t kRowViewProjJittered = 0;
constexpr uint32_t kRowViewProj = 4;
constexpr uint32_t kRowRenderSize = 17;
constexpr uint32_t kRowFrustumPlanes = 79;
constexpr uint32_t kRowCameraPos = 84;
constexpr uint32_t kRowCameraPrevPos = 85;
constexpr uint32_t kRowCameraBasis = 86;  // right, up, forward
constexpr uint32_t kRowFov = 99;          // .w = vertical FOV (radians)

}  // namespace pc2vr::viewcb
