# Bundled Cheeky DLSS integration

Cheeky is bundled with the Planet Coaster2 VR alpha and integrated into
Settings→VR. Processing is optional:the core mod works with DLSS Off and on
hardware without NVIDIA DLSS. Cheeky's own F8 menu remains available for advanced
controls. The PC2 menu exposes image quality,placement,eye-tracking selection and
status,smoothing/loss hold and explicit recalibration. Current physical gaze
following is unverified;selected mode alone does not establish active tracking.

Dependency: [CheekyFoveatedDLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS), pinned to `e016dc756383f1959fe0ce9257a73785cc18605f`. Build the runtime, standalone host/proxy and OpenXR layer from that same revision. The published v0.5.5 tag/release identification differs; do not substitute or mix components without checking their actual source identity.

`pc2-cheeky-compatibility.patch` combines native D3D12 shared-source calibration and the Planet Coaster 2 hook guard. Calibration waits two frame boundaries for an alternating source, then permits one identity only when source before/after markers and two successful, independent eye readbacks prove the mapping. It does not guess eye order, offsets or projection. Existing two-source and mixed API paths retain their validation requirements. Tests cover acquisition, missing-eye rejection, retained clean pixels, crop/resize, stereo/mixed paths and recording/fence lifetimes. The original calibration-only patch remains as a historical reference; apply only the combined patch for this package.

Planet Coaster 2 requires `D3D12LowerHook=0`. Lower interception loses the wrapper handle's created output extent on the measured PC2 Streamline/core-NGX path; higher interception preserves the real input/output relationship. The combined patch recognizes `PlanetCoaster2.exe` once per module, hides the incompatible checkbox in its standalone menu, keeps the higher hook through Reset SR Defaults, repairs legacy/edited settings requesting the lower hook while preserving their other values, and enforces the requirement at runtime settings publication and hook startup. Other process names retain upstream defaults and the editable selector. This guard does not alter model selection, foveation defaults, rendering resources or the PC2 loader. An already-running old dependency is not repaired by staging this package; installing updated files requires the game to be closed.

Fresh settings leave Cheeky disabled,peripheral DLAA Off,center0.55×0.45,
automatic stereo alignment On,change-only full calibration,border Off and NR Off.
Users choose native DLSS in the game's Graphics settings. Frame generation was
Off during development trials;VR frame-generation support is not established.
ModelE is recommended through the game-specific NVIDIA driver profile for users
choosing DLSS. Do not promise that Cheeky's preset hint overrides a forced driver
model or that DLSS will be faster than native. Peripheral quality and central
supersampling can add substantial GPU work.

The standalone `dxgi.dll` sits alongside PC2's existing `version.dll`. Runtime/host files live in `CheekyFoveatedDLSS/`. The OpenXR layer must be activated for the game process; the prototype launcher uses child-only `XR_API_LAYER_PATH` and `XR_ENABLE_API_LAYERS`, with no system-wide layer registration or runtime change. Without the optional files/layer, the accepted PC2 mod runs normally. DLSS requires supported NVIDIA hardware; do not enable the option or force replacement AA on AMD.

Upstream is GPLv3;its license is retained as `LICENSE.upstream.txt`,and this patch
is provided under the same license. Preserve corresponding source and notices
when distributing the modified dependency. No NVIDIA DLSS libraries or game
assets are included in this source directory.

To reproduce from a pinned Git checkout, run `build.ps1 -SourceDirectory <checkout>`, then `package.ps1 -SourceDirectory <checkout> -OutputDirectory <fresh-directory>`. The build refuses unrelated source changes and applies only the combined patch. It runs CPU settings/menu checks under ordinary and PC2 process names, plus the focused GPU checks when the game is closed. Use `-BuildOnly` to compile and run only CPU checks while leaving a game session alone. Process-name checks execute console-only test copies named PlanetCoaster2.exe in separate ignored test folders; their selected modes never launch the game, create windows or initialize a GPU. Packaging copies only the optional components and records their hashes; it does not deploy or launch. The staged source archive contains the pinned upstream tree. To build from that archive without Git history, extract it, apply the combined patch from its root, build the Release/x64 solution targets listed in `build.ps1` with MSBuild/v143, and run its listed CPU and GPU checks. The checked build logs and runtime receipts are in the project's ignored experiment directory.

After deployment,`launch.ps1` starts the game with the layer enabled only in that
child process. Steam and the selected physical VR runtime/headset must already be
ready. Package it beside the game executable or supply `-GameDirectory`;the source
checkout can also discover the Steam installation. It does not start SteamVR,
switch the runtime,stop/restart the game or reset settings. Use the freshly built
core loader and VRMenu.js from the same source snapshot,not an old accepted build.
Preserve user INIs on updates. Packaging does not deploy or launch anything.
