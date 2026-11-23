# Changelog

## Beta 1 — v0.1.0-beta.1

Changes since the last public release, v0.1.0-alpha.2.

### Motion controllers and lasers

- Xbox-style motion-controller input with physical gamepad coexistence and
  controller focus, tracking-loss and stale-input safeguards.
- Right-hand laser selection for park objects and main/pause menus, including
  supported click, drag and scrolling controls.
- A selects; right trigger retains zoom. Hold right stick click to toggle the
  laser, with quick R3 preserved.
- The beam stops at the selected native hit or menu surface, with an endpoint
  dot and corrected stereo alignment.
- Right-stick scrolling when pointing at catalog, inspector and supported menu
  scroll regions.
- Hold left stick click for Pause, or double-click within 350 ms for SteamVR
  users whose Menu button opens Steam. Quick L3 is preserved. The old manual
  hold-and-tilt D-pad gesture is removed; native menu focus navigation remains.

### Controller UI

- A left-controller radial build wheel replaces the bottom build toolbar.
- Native catalogs, filters, research/DLC availability and placement callbacks
  remain in use. Catalogs follow the left controller.
- Ride/building/scenery/shop/pool inspectors and rider camera controls follow
  the left grip, with an 18 cm default panel width.
- Switch park selections without first closing the current inspector.
- Park statistics, objectives and a notification inbox below the build wheel,
  plus brief left-hand notification cards.
- Hide remaining flat park HUD chrome and the center reticle while preserving
  world warning icons and native modal ownership.

### Rendering and ride-camera fixes

- Optional AER and Alternate Frame Warp (AFW) alongside default True Stereo.
  Alternation follows completed game frames and is not tied to a fixed FPS.
- AFW depth/motion-guided eye reconstruction with AER fallback when guides
  are unavailable, and separate marker composition to reduce world-icon jitter.
- Improved laser visibility with OFXR Bridge through the existing beam shader;
  native selection endpoints are preserved.
- Preserve full-rate shading on integer visibility/G-buffer passes to address
  blue ground streaks and green tunnel artifacts.
- Recover Guest camera tracking when Lua camera records relocate or competing
  records remain after a seat/car change.
- Hide only the confirmed selected test dummy that could briefly enter the
  rider camera, restoring visibility on seat/cabin changes and exit.
- Fade park-to-Guest and Guest-to-park camera transitions while tracking is
  reacquired, with a bounded timeout.

### Compatibility and packaging

- Anniversary executable 1.12.12.1045899 support through verified native function
  layouts, retaining exact-build validation and older build entries.
- Embed the VR menu script in version.dll; no loose PC2VR/VRMenu.js is required.
- Enable controller, laser and wrist features in fresh settings while keeping
  True Stereo the default. Preserve existing settings when updating.
- Complete dependency/source/license bundle and file-manager installation steps.

### Verification and limits

Maintainer reports accepted the controller UI, Anniversary compatibility and
double-click Pause on the development builds. Native/frontend/GPU and isolated
simulator checks cover additional paths. Performance modes can show temporal or
reconstruction artifacts; AFW is not guaranteed faster than AER. Other physical
headsets, AMD behavior and active gaze tracking remain incompletely validated.

## Alpha 2 — v0.1.0-alpha.2

- Fix Guest camera detection after career/sandbox park changes by resolving
  camera-selection settings inside their own table rather than requiring one
  global string identity.
- Maintainer validation across many rides, including DLC and testing rides.

## Alpha 1 — v0.1.0-alpha.1

- Initial public stereo VR release, headset-driven park/Guest cameras, VR settings,
  native VRS, and bundled optional Cheeky foveated DLSS integration.
