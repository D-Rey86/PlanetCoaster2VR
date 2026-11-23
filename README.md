# Planet Coaster 2 VR

A Windows PC VR mod for Planet Coaster 2, with stereo rendering, headset-driven
park and supported ride cameras, motion-controller lasers and controller-mounted UI.
This is **Beta 1** (`v0.1.0-beta.1`); compatibility and performance feedback are welcome.

Alpha 2 fixes Guest rider views remaining on the fixed screen after switching
parks. Camera detection now resolves settings within the ride camera's own table,
so duplicate setting names left in memory no longer prevent head tracking.
Maintainer testing passed across many rides, including DLC rides. Simulator
checks also covered empty closed/testing rides, park transitions and camera-mode
switching. This does not establish exhaustive coverage of every ride or device.

Beta 1 adds park/menu laser selection, a left-hand build wheel,
wrist selection/rider panels, statistics/objectives/notifications, optional AER
and AFW rendering, Guest-camera transition fades, and Anniversary Update support.

CheekyFoveatedDLSS is integrated into the VR menu and bundled in the
alpha. Its foveated DLSS processing is optional at runtime and requires NVIDIA
DLSS support. The core VR mod can run with the game's DLSS setting off.

## Requirements and validation

- A legitimate Steam installation of Planet Coaster 2 on 64-bit Windows.
- An installed OpenXR runtime with D3D12 support and a connected PC VR headset.
- Motion controllers for laser/wrist controls, or a gamepad for native controls.
- A DLSS-capable NVIDIA RTX GPU if using Cheeky foveated DLSS. It is not required
  for the core mod or native rendering.

Current development supports the Anniversary executable **1.12.12.1045899**.
Its SHA256 is `4082BD57EECCFDA54824409E9BE32E952461CFB422CAA201662C00DEB54217F3`.
The integration depends
on measured game interfaces and rendering behavior; later game updates may need
a mod update. Physical testing has used Galaxy XR through Virtual Desktop/VDXR
and an NVIDIA GPU. Other physical headsets and AMD GPUs still need community
validation; simulated-headset and capability-fallback tests are not equivalent
to testing those devices.

See [CHANGELOG.md](CHANGELOG.md) for changes since the last public release, Alpha 2.

## Installing the beta package

1. Close the game and find its installation folder using Steam's **Browse local
   files** command. The target folder contains `PlanetCoaster2.exe`.
2. Back up an existing mod installation and its two settings files before
   updating: `PC2VR/pc2vr.ini` and
   `CheekyFoveatedDLSS/CheekyFoveatedDLSS.ini`. Preserve your settings on updates.
   Other mods that already occupy `version.dll` or `dxgi.dll` need separate
   compatibility work; do not overwrite their files with this package.
3. Extract the complete package and copy its game-root contents beside `PlanetCoaster2.exe`, preserving
   the `PC2VR` and `CheekyFoveatedDLSS` directory layout. The beta includes
   `version.dll`, `dxgi.dll`, the OpenXR loader, Cheeky host/runtime/layer,
   settings templates, launcher, license notices and corresponding source.
   The VR menu script is embedded in `version.dll`; a loose `VRMenu.js` is no
   longer used and may be removed when updating from an older package.
4. Select your preferred OpenXR runtime and connect your headset **before**
   starting the game. For Virtual Desktop, select VDXR if that is your intended
   runtime. Leave Steam running.
5. Launch normally through Steam for the core VR mod. To run with Cheeky's
   optional process-local OpenXR layer, the included `launch.ps1` is a launcher
   for that session; it does not install files, register a global layer or switch
   the selected runtime.

The manual file layout is `version.dll` and `dxgi.dll` in the game folder;
`openxr_loader.dll` and `pc2vr.ini` in its `PC2VR` folder;
`CheekyOpenXRLayer.dll` and `XR_APILAYER_CHEEKY_foveated_dlss.json` in
`PC2VR/CheekyOpenXR`; and `CheekyFoveatedDLSSHost.dll`,
`CheekyFoveatedDLSSRuntime.dll`, and `CheekyFoveatedDLSS.ini` in
`CheekyFoveatedDLSS`. Keep the included `licenses` and `source` folders.
The package supplies OpenXR loader 1.1.63 and the matching patched Cheeky build
from revision `e016dc756383f1959fe0ce9257a73785cc18605f`; no separate mod dependency
download is needed. On an existing installation, keep your INIs. For a fresh
installation, use the included templates. Settings can be edited in an ordinary
text editor or in the VR tab. To roll back, close the game and restore your
backed-up DLLs and settings together. Manual installation requires no installer
or terminal command.

When upgrading an Alpha 2 configuration, enable the new motion-controller features
by opening `PC2VR/pc2vr.ini` in a text editor and adding these keys. Add keys to
an existing section when present; create the section only when missing. Keep
your other settings:

```ini
[Controls]
MotionControllers=1
[Experiments]
WorldLaser=1
```

Fresh beta settings already include them. The VR tab's left-hand panels, build
wheel and dashboard toggles control the wrist UI; their fresh defaults are On.

The mod uses a separate `Graphics.vr.config.xml` derived from your original game
graphics profile and keeps VR windowed. Choose AA/DLSS and other graphics settings
in the game. Installing the package does not change NVIDIA driver profiles or
replace NVIDIA DLSS libraries.

## Settings and controls

Open the game's **Settings → VR** tab using its normal controller navigation.
VR settings save automatically; the game's own Apply/Reset controls continue
to belong to its native settings.

- **VR render resolution:** 25–200% of runtime-recommended pixel density.
- **Rendering mode:** True Stereo (default), AER, or AFW. AER alternates native
  eyes and rotation-reprojects history; AFW reconstructs the other eye with
  measured game depth and motion guides, falling back to AER when unavailable.
  Alternation follows completed game frames and does not require a fixed FPS.
  Both performance modes can show temporal or reconstruction artifacts.
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

Motion controls map the two sticks, face buttons, triggers and grips to Xbox
controls. Point the right-hand laser and press **A** to select park objects or
main/pause menu controls. **RT** retains zoom. Hold **right stick click** to
toggle the laser; a quick right click retains R3. Point at a catalog, panel or
menu scroll region and move the **right stick** vertically to scroll.
The beam stops at the supported target surface and shows an endpoint dot.

Use **left grip (LB)** for the build wheel. Build categories open the catalog
on the left hand; park statistics, objectives and the notification inbox sit
below the wheel. Ride/building/scenery inspectors and the rider camera panel
also follow the left controller. Native flat park HUD elements are hidden,
while world warning icons remain visible.

Double-click the **left stick** within 350 ms or hold it for 600 ms to open
Pause. A single quick click remains L3. Where the runtime exposes the left
Menu button, tap it for Start/Pause or hold for Back. SteamVR can reserve that
button for Steam, so use the stick gesture there. Normal stick tilt navigates
main/pause menus; the old manual hold-and-tilt D-pad gesture is removed.

Native right-stick sensitivity and inversion settings remain in the game's
controller settings. Cheeky's gaze choice does not switch the separate native
VRS gaze path, which already uses valid OpenXR gaze where available.

## Defaults and performance

Fresh package defaults: True Stereo, motion controls/laser/wrist UI On, runtime
resolution100%, expanded coverage Off, native VRS On, HUD panel On/headset-follow Off,
Cheeky foveation Off, eye tracking
Off, border Off and neural rendering Off. Cheeky's peripheral DLAA defaults Off.
No DLSS mode or driver model is forced. Existing user settings should be preserved
when updating.

For users choosing DLSS, **model E through the game-specific NVIDIA driver profile
is recommended from the current testing**. Cheeky's model hints have not been
proven to override a forced driver preset here. DLSS does not guarantee a gain
over native rendering; peripheral quality and supersampling can add GPU cost.
Roundness changes blending rather than reducing the rectangular DLSS workload.

## Known beta limitations

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
- AFW is not guaranteed to outperform AER. It adds reconstruction work; the gain
  depends on the GPU, scene and rendering settings. Simulator timing does not
  establish performance or comfort on physical hardware.

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
`version.dll` and `PC2VR/openxr_loader.dll` into `Builds`. The menu script and
other mod-owned UI/shader code are compiled into `version.dll`; keep
`Loader/ui/VRMenu.js`, `Loader/ui/embedded_assets.rc` and the source headers
together when building.

For the bundled dependency, use the pinned source and combined patch described
in [Tools/CheekyDLSS/README.md](Tools/CheekyDLSS/README.md). Core and Cheeky sources,
build scripts and third-party notices accompany release candidates. No game
executables, extracted game UI/assets, NVIDIA DLLs or personal settings are
redistributed. See [THIRD_PARTY.md](THIRD_PARTY.md).

The mod's original source is licensed under GNU GPL version 3; see [LICENSE](LICENSE).
Third-party components retain their individual licenses. Release packaging is
documented in [RELEASE_PREP.md](RELEASE_PREP.md).
