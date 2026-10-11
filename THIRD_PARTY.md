# Third-party components

This mod does not redistribute Planet Coaster 2, extracted game UI/assets or
NVIDIA DLSS/Streamline libraries. These remain supplied by the game or driver.

## CheekyFoveatedDLSS

- Upstream: https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS
- Pinned revision: `e016dc756383f1959fe0ce9257a73785cc18605f`.
- License: GNU GPL version3; see `Tools/CheekyDLSS/LICENSE.upstream.txt`.
- Changes: `Tools/CheekyDLSS/pc2-cheeky-compatibility.patch` adds verified native
  shared-source calibration and a Planet Coaster2-specific higher-hook guard.
- Release dependency packaging includes the pinned upstream source archive,
  patch, build instructions, license and its vendored dependency notices.

The packaged Cheeky build also carries notices for MinHook, Dear ImGui, OpenVR
and its OpenXR SDK. Keep their individual license files from `licenses/` with
the package; the package script copies them from the pinned upstream tree.

## Khronos OpenXR loader and headers

- SDK/source: https://github.com/KhronosGroup/OpenXR-SDK-Source
- Vendored Windows x64 loader release1.1.63.
- License: Apache2.0, `ThirdParty/OpenXR/LICENSE.txt`.
- Binary provenance/hash: `ThirdParty/OpenXR/README.md`.

The core release includes the corresponding SDK source archive for this loader
and preserves available upstream notices. System/runtime software is not bundled.

## Game UI integration

`Loader/ui/VRMenu.js` is the mod's source extension. It references components
already supplied by the user's own game. Their implementation is not copied
into the mod's source or release package.
