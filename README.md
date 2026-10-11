# Planet Coaster 2 VR

A Windows PC VR mod for Planet Coaster 2, with stereo rendering, headset-driven
park and supported ride cameras, controller input, and the game's HUD in VR.
This is an **alpha** release; compatibility and performance feedback are welcome.

CheekyFoveatedDLSS is integrated into the VR menu and bundled in the
alpha. Its foveated DLSS processing is optional at runtime and requires NVIDIA
DLSS support. The core VR mod can run with the game's DLSS setting off.

## Requirements and validation

- A legitimate Steam installation of Planet Coaster 2 on 64-bit Windows.
- An installed OpenXR runtime with D3D12 support and a connected PC VR headset.
- A gamepad for the existing game controls and controller-driven settings menu.
- A DLSS-capable NVIDIA RTX GPU if using Cheeky foveated DLSS. It is not required
  for the core mod or native rendering.

Current development tested game build **1.11.3.986758**. The integration depends
on measured game interfaces and rendering behavior; later game updates may need
a mod update. Physical testing has used Galaxy XR through Virtual Desktop/VDXR
and an NVIDIA GPU. Other physical headsets and AMD GPUs still need community
validation; simulated-headset and capability-fallback tests are not equivalent
to testing those devices.

## Installing an alpha package

1. Close the game and find its installation folder using Steam's **Browse local
   files** command. The target folder contains `PlanetCoaster2.exe`.
2. Back up an existing mod installation and its two settings files before
   updating: `PC2VR/pc2vr.ini` and
   `CheekyFoveatedDLSS/CheekyFoveatedDLSS.ini`. Preserve your settings on updates.
   Other mods that already occupy `version.dll` or `dxgi.dll` need separate
   compatibility work; do not overwrite their files with this package.
3. Copy the package's game-root contents beside `PlanetCoaster2.exe`, preserving
   the `PC2VR` and `CheekyFoveatedDLSS` directory layout. An alpha includes
   `version.dll`, `dxgi.dll`, the OpenXR loader, VR menu, Cheeky host/runtime/layer,
   settings templates, launcher, license notices and corresponding source.
4. Select your preferred OpenXR runtime and connect your headset **before**
   starting the game. For Virtual Desktop, select VDXR if that is your intended
   runtime. Leave Steam running.
5. Run the included `launch.ps1` from the game folder. It enables Cheeky's OpenXR
   layer for that game process and follows your selected runtime. It does not
   register a system-wide layer or switch the runtime. Direct Steam launch does
   not apply these launcher environment settings.

The mod uses a separate `Graphics.vr.config.xml` derived from your original game
graphics profile and keeps VR windowed. Choose AA/DLSS and other graphics settings
in the game. Installing the package does not change NVIDIA driver profiles or
replace NVIDIA DLSS libraries.

## Settings and controls

Open the game's **Settings → VR** tab using its normal controller navigation.
VR settings save automatically; the game's own Apply/Reset controls continue
to belong to its native settings.

- **VR render resolution:** 25–200% of runtime-recommended pixel density.
- **Expanded headset coverage:** optional coverage for wide/angled displays and
  head roll. Default Off preserves the original rendering/FOV policy. On can
  have a substantial performance cost.
- **Foveated rendering:** enabled by default on GPUs supporting image-based
  variable-rate shading (Tier 2); unsupported GPUs keep full shading. It is
  separate from Cheeky; 4×4 shading
  falls back to 2×2 when the GPU lacks that additional rate.
- **UI placement:** width, distance, horizontal/vertical position, recentering,
  and headset-follow mode. In follow mode the HUD and its selection circle move
  together. Fixed-screen menus keep their picture and UI together.
- **Cheeky:** enable foveated DLSS, center size, peripheral quality/resolution,
  supersampling, roundness, transition blending, vertical placement and a region
  border preview.
- **Cheeky eye tracking:** fixed/runtime-gaze choice, actual tracking and mapping
  status, smoothing, tracking-loss hold, and explicit recalibration. A selected
  option is not proof of valid gaze; the status identifies fixed fallback or
  actual gaze use. Headset/runtime gaze support is required.

Native right-stick sensitivity and inversion settings remain in the game's
controller settings. Cheeky's gaze choice does not switch the separate native
VRS gaze path, which already uses valid OpenXR gaze where available.

## Defaults and performance

Fresh package defaults: stereo On, runtime resolution100%, expanded coverage Off,
native VRS On, HUD panel On/headset-follow Off, Cheeky foveation Off, eye tracking
Off, border Off and neural rendering Off. Cheeky's peripheral DLAA defaults Off.
No DLSS mode or driver model is forced. Existing user settings should be preserved
when updating.

For users choosing DLSS, **model E through the game-specific NVIDIA driver profile
is recommended from the current testing**. Cheeky's model hints have not been
proven to override a forced driver preset here. DLSS does not guarantee a gain
over native rendering; peripheral quality and supersampling can add GPU cost.
Roundness changes blending rather than reducing the rectangular DLSS workload.

## Known alpha limitations

- Performance can be demanding, and expanded coverage can reduce FPS substantially.
- Brief black-and-white calibration markers can appear while Cheeky establishes
  or reacquires eye-image mapping. They are not eye-tracking targets. They can
  also appear with ordinary DLSS while Cheeky foveation is off; the game's DLSS-Off
  path does not generate them.
- The integrated menu and saved settings have offline and simulated runtime
  coverage, and the maintainer reports successful use on another installation.
  Galaxy XR gaze-following and other physical hardware remain unverified.
- Cinematic/unsupported cameras and some menus use a fixed-screen presentation.
- Compatibility with other graphics injection mods and other physical hardware
  has not been established.

After requesting Cheeky recalibration, return to gameplay so fresh rendered
frames can complete alignment. Brief calibration markers may appear.

## Removing the mod

Close the game. Remove only the installed mod files listed in the package
manifest, including its `version.dll` and `dxgi.dll`, or restore the files backed
up before installation. Keep personal INIs/logs if you want to reuse them. Do not
delete game saves or the game's original graphics/controller profiles. The separate
VR graphics profile may be left in place; it is unused without the mod.

## Building and source

Install Visual Studio2022 C++ tools with the v143 toolset and Windows SDK10.0.26100.
Build `Loader/PC2VR.Loader.vcxproj` as Release/x64 using MSBuild. The project stages
`version.dll`, `PC2VR/openxr_loader.dll` and `PC2VR/VRMenu.js` into `Builds`.

For the bundled dependency, use the pinned source and combined patch described
in [Tools/CheekyDLSS/README.md](Tools/CheekyDLSS/README.md). Core and Cheeky sources,
build scripts and third-party notices accompany release candidates. No game
executables, extracted game UI/assets, NVIDIA DLLs or personal settings are
redistributed. See [THIRD_PARTY.md](THIRD_PARTY.md).

The mod's original source is licensed under GNU GPL version 3; see [LICENSE](LICENSE).
Third-party components retain their individual licenses. Release packaging is
documented in [RELEASE_PREP.md](RELEASE_PREP.md).
