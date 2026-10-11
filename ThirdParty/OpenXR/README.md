# Khronos OpenXR loader

This directory contains the x64 Windows desktop `openxr_loader.dll` from the
Khronos Group's official OpenXR SDK release 1.1.63.

- Release: <https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/tag/release-1.1.63>
- Archive: `openxr_loader_windows-1.1.63.zip`
- Archive SHA-256: `01C631AEABBFE0879540F77EF833416C532A20746285B494630160C23588B771`
- Loader SHA-256: `66946EA6E9CC2649E299EC412521C5EAFE56C635D05055097FE4DB81E11932DF`
- Architecture: x64 Windows desktop (`x64/bin/openxr_loader.dll`)

The PC2VR build copies the loader to `PC2VR/openxr_loader.dll` beside the game.
The mod loads that copy explicitly. The selected OpenXR runtime still needs to
be installed and ready; the application does not force SteamVR or VDXR.

The upstream binary is not Authenticode-signed. Its provenance and hashes are
recorded above so the vendored file can be checked against the official release.
See `LICENSE.txt` for the upstream license.

The checked-in API headers report1.1.62;they are preserved as the core build's
actual inputs. The loader's1.1.63 source revision and archive hash are recorded in
`SOURCE.json`. Release packaging retains both the exact core headers and the
corresponding loader SDK source archive.
