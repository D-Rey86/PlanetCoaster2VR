# Alpha release preparation

## Step1: source, documentation and package baseline

- Consolidate development notes and commit/merge the VR-menu branch locally.
- Build the current source;do not package an older accepted DLL from Builds.
- Bundle Cheeky with its matching host/runtime/OpenXR layer,combined compatibility
  patch,pinned corresponding source and third-party notices.
- Include portable launch instructions,stereo-enabled fresh defaults,installation,
  update/uninstall guidance and accurate hardware/eye-tracking limitations.
- Export public source through an explicit allowlist. Exclude private handoffs,
  research notes/history,game files/extractions,captures,dumps,personal settings,
  test instrumentation and local binaries.
- Generate a SHA256 manifest and preserve the exact source identity/build inputs.
- Select the original mod's public license before producing a distributable package.

`Tools/package-alpha.ps1` creates a local draft package from a committed source
snapshot. It does not install,launch,register a layer,change a runtime,or upload.
It requires the selected root LICENSE,the pinned Cheeky checkout and the pinned
OpenXR SDK source archive specified by ThirdParty/OpenXR/SOURCE.json. Public source
is archived before building,so private build artifacts cannot enter the archive.
The packager replaces local CodeView PDB paths with filenames in the five built
mod/dependency DLLs and scans them for private build details. Executable code,
debug GUIDs and all other bytes remain unchanged; the upstream OpenXR loader is
kept byte-identical to its pinned hash. PDBs themselves are not distributed.
`-DraftWithoutLicense` permits a private staging test while the license choice is
pending;that output carries DO-NOT-DISTRIBUTE.txt and a license-pending manifest.
The default publication-candidate path still requires a committed LICENSE.

## Publication workflow

1. Test the exact packaged files on a fresh staged installation:startup,normal
   exit,controller menu navigation/resolution/UI behavior,new Cheeky controls,
   update/settings preservation and uninstall. Physical gaze must be measured
   separately from merely selecting its option. Private package/install/simulated
   runtime checks passed, and the maintainer accepted the default-enabled package
   on another game installation. Do not assume identical FPS or working gaze
   without a matched comparison or observed tracking.
2. Review the public export,license/notices,manifest and release description.
   Choose the GitHub repository destination and authorize publication. This local
   repository has no GitHub remote and contains private research in its history;
   do not push that history or rewrite it automatically.
3. Publish the reviewed public source and alpha archive together,with their source
   identity and checksums. Keep defaults/limitations visible to users.

Bundled Cheeky remains a deliberate product decision. Do not replace it with a
separate-download requirement based on ambiguous experimental crash records.

## Current verification boundary

Core offline checks cover native control XML parsing,upload-buffer lifetime/read
logic,frustum geometry,original/expanded policy,VR menu bridge/frontend and D3D12
VRS capabilities. Prior simulated/headset checks cover additional rendering paths.
The integrated menu and persistence have simulated runtime coverage and maintainer
acceptance on another installation. Physical eye-tracking and other headset/AMD
tests remain unperformed. These gaps must accompany the alpha rather than be
silently represented as proven compatibility.
