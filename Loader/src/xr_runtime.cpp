#include "xr_runtime.h"

#include <algorithm>
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera_control.h"
#include "game_state.h"
#include "depth_probe.h"
#include "config_redirect.h"
#include "control_settings.h"
#include "input_hook.h"
#include "log.h"
#include "ride_camera.h"
#include "ui_layer.h"
#include "panel_geometry.h"
#include "vr_menu.h"
#include "frustum_bounds.h"

using Microsoft::WRL::ComPtr;

namespace pc2vr::xr {
namespace {

// ---------------------------------------------------------------- loader --
#define PC2VR_XR_FUNCS(X)                     \
    X(xrEnumerateInstanceExtensionProperties) \
    X(xrCreateInstance)                       \
    X(xrDestroyInstance)                      \
    X(xrGetInstanceProperties)                \
    X(xrGetSystem)                            \
    X(xrGetSystemProperties)                  \
    X(xrEnumerateViewConfigurationViews)      \
    X(xrCreateSession)                        \
    X(xrDestroySession)                       \
    X(xrBeginSession)                         \
    X(xrEndSession)                           \
    X(xrPollEvent)                            \
    X(xrCreateReferenceSpace)                 \
    X(xrDestroySpace)                         \
    X(xrLocateSpace)                          \
    X(xrLocateViews)                          \
    X(xrEnumerateSwapchainFormats)            \
    X(xrCreateSwapchain)                      \
    X(xrDestroySwapchain)                     \
    X(xrEnumerateSwapchainImages)             \
    X(xrAcquireSwapchainImage)                \
    X(xrWaitSwapchainImage)                   \
    X(xrReleaseSwapchainImage)                \
    X(xrWaitFrame)                            \
    X(xrBeginFrame)                           \
    X(xrEndFrame)                             \
    X(xrResultToString)                       \
    X(xrStringToPath)                         \
    X(xrCreateActionSet)                      \
    X(xrCreateAction)                         \
    X(xrSuggestInteractionProfileBindings)    \
    X(xrAttachSessionActionSets)              \
    X(xrCreateActionSpace)                    \
    X(xrSyncActions)                          \
    X(xrGetActionStatePose)

struct Api {
    PFN_xrGetInstanceProcAddr getProc = nullptr;
#define X(name) PFN_##name name = nullptr;
    PC2VR_XR_FUNCS(X)
#undef X
    PFN_xrGetD3D12GraphicsRequirementsKHR getD3D12Requirements = nullptr;
} g;

template <typename T>
bool Resolve(XrInstance instance, const char* name, T& out) {
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(g.getProc(instance, name, &fn)) || !fn) return false;
    out = reinterpret_cast<T>(fn);
    return true;
}

// ----------------------------------------------------------------- state --
enum class Phase { Uninitialised, Ready, Failed };

struct State {
    Phase phase = Phase::Uninitialised;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool running = false;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;

    // One swapchain per eye (a double-width one can exceed a runtime's maximum image
    // width; seen with a wide-FOV profile: 16384). Each is the game image's shape,
    // fitted inside the runtime's maximum image size.
    XrSwapchain swapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    DXGI_FORMAT swapFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t swapW = 0, swapH = 0;  // each eye's swapchain
    uint32_t srcW = 0, srcH = 0;    // the game image they were sized for
    uint32_t maxSwapW = 16384, maxSwapH = 16384;
    std::vector<ID3D12Resource*> swapImages[2];
    ComPtr<ID3D12DescriptorHeap> rtvHeap[2];
    uint32_t rtvStride = 0;

    // blit
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    DXGI_FORMAT psoFormat = DXGI_FORMAT_UNKNOWN;
    // eye composition (ComposeEye): the game image, and the UI panel, into each eye's view
    ComPtr<ID3D12RootSignature> composeRootSig;
    ComPtr<ID3D12PipelineState> composePso;
    DXGI_FORMAT composeFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    uint32_t srvStride = 0;
    static constexpr int kRing = 3;
    ComPtr<ID3D12CommandAllocator> alloc[kRing];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue[kRing] = {};
    uint64_t nextFence = 1;
    int ring = 0;

    // frame
    bool frameBegun = false;
    XrTime displayTime = 0;
    bool shouldRender = false;
    bool poseAcquired = false;
    XrPosef headPose{};
    bool headPositionValid = false;
    XrFovf fovUnion{};
    RenderView renderView{};

    bool recenterPending = true;
    float headYaw0 = 0.f;
    XrVector3f headPos0{};  // the head's position when recentred: the UI panel is placed from there

    // game camera control (camctl)
    bool cameraControl = true;
    bool bodyInit = false;
    float bodyYaw = 0.f;
    bool lastWrittenValid = false;
    float lastWrittenYaw = 0.f;
    bool injecting = true;
    float pausedOffset = 0.f;
    int stillFrames = 0;
    float lastSeenYaw = 0.f, lastSeenPitch = 0.f;
    float lastWrittenPitch = 0.f;
    float bodyPitch = 0.f;
    float pausedPitchOffset = 0.f;
    bool engineView = true;
    int64_t lastTurnTime = 0;  // ns: display time, QPC fallback if no display time is available
    float snapTurnDeg = 0.f;            // 0 = smooth turning
    float smoothTurnDegPerSec = 120.f;
    bool snapArmed = true;
    float tilt = 0.f;               // right-stick tilt (radians, positive = down)
    float tiltDegPerSec = 90.f;  // show the engine's own rendered view (no view-buffer rewrite)
    XrPosef submitPose{};
    XrFovf submitFov{};
    bool engineFovControl = true;
    float engineFovMax = 130.f * 3.14159265f / 180.f;
    float engineFovOverride = 0.f;  // zero selects the mode's original130 / expanded155 ceiling
    bool expandedCoverage = false;
    int pitchMode = 1;  // 0 = game keeps its pitch, 1 = pitch follows the head downward from the body pitch
    uint64_t frames = 0;

    // True stereo: the game's render batch runs once per eye (stereo.cpp); each run's
    // finished back buffer is copied into eyeTex[eye] on the game's render queue, and
    // both are blitted side by side into the OpenXR swapchain at Present.
    bool stereo = false;
    float worldScale = 1.f;
    float eyeWorld[2][3] = {};  // each eye's offset from the head in game space (metres, unscaled)
    // Eye gaze (XR_EXT_eye_gaze_interaction), when offered.
    bool gazeSupported = false, gazeValid = false, gazeLogged = false;
    XrActionSet gazeSet = XR_NULL_HANDLE;
    XrAction gazeAction = XR_NULL_HANDLE;
    XrSpace gazeSpace = XR_NULL_HANDLE;
    float gazeTan[2] = {};
    // Zoom-linked depth: the eye separation grows with the distance to what the camera
    // looks at (its orbit pivot), so that point appears modelDistance metres away, like
    // a model on a table; never below real scale. Smoothed so zooming does not jump.
    bool zoomDepth = true;
    float modelDistance = 2.5f;
    float zoomScale = 1.f;     // smoothed
    float orbitDistance = 0.f; // last measured, for the log
    float nearestComfort = 1.f;  // metres: the nearest visible thing never appears closer
    float nearestDistance = 0.f;  // last measured, for the log
    float centerDistance = 0.f;  // last measured distance at the centre of the view
    static constexpr int kDepthRing = 12;
    float nearRing[kDepthRing] = {}, centerRing[kDepthRing] = {};
    int ringPos = 0, ringCount = 0;
    float eyeLocal[2][3] = {};  // eye position relative to the head on the head's axes (right, up, forward), metres
    XrPosef eyeSubmitPose[2]{};
    // This frame's real eye views (pose in local space, FOV), from xrLocateViews: a fixed
    // screen is composed into these (ComposeEye).
    XrPosef eyeViewPose[2]{};
    XrFovf eyeViewFov[2]{};
    IDXGISwapChain* gameSwapRaw = nullptr;
    ComPtr<IDXGISwapChain3> gameSwap;
    ComPtr<ID3D12Device> captureDevice;
    ComPtr<ID3D12Resource> eyeTex[2];  // kept in COPY_DEST between uses
    uint32_t eyeW = 0, eyeH = 0;
    DXGI_FORMAT eyeFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12CommandAllocator> copyAlloc[kRing][2];
    ComPtr<ID3D12GraphicsCommandList> copyList[2];
    ComPtr<ID3D12Fence> copyFence;
    HANDLE copyEvent = nullptr;
    uint64_t copyValue = 0;
    uint64_t copySlotValue[kRing][2] = {};
    int copyRing = 0;
    bool eyesCaptured = false;   // both eyes of the current frame are in eyeTex
    uint64_t eyesValue = 0;      // copyFence value once they are
    uint64_t lastBlitValue = 0;  // fence value after the last headset blit that read eyeTex
    // Render size from the headset (Metro lesson: never one headset's numbers). The
    // runtime's recommended per-eye size (it includes the runtime's own resolution
    // setting) gives the pixel density; the eyes' own FOV gives the shape.
    uint32_t recW = 0, recH = 0;          // recommended per-eye image size
    uint32_t recEyeW[2]{}, recEyeH[2]{};
    float pixelDensity = 0.f, panelDensity = 0.f;
    XrFovf eyeFov[2]{};                   // last located per-eye FOV (each eye's own space)
    XrFovf eyeFovHead[2]{};               // the same in head space (canted displays)
    frustum::Cache headBounds[2], outputBounds[2];
    bool boundsWarning[2]{};
    bool eyeFovValid = false;
    float renderScale = 1.f;              // [Resolution] ScalePercent
    float fovMargin = 0.0175f;            // [Resolution] FovMarginDeg, total, radians (each eye is cropped to its FOV, so a margin is never shown)
    bool submitEyeFov = true;             // [Resolution] SubmitEyeFov: send each eye only its own FOV
    bool perEyeProjection = false;        // [Resolution] PerEyeProjection (experimental, off: lighting breaks; see Notes/04)
    XrFovf eyeRenderFov[2]{};             // per eye, head space: what that eye's run renders
    XrFovf eyeRenderFovFrame[2]{};        // the ones the current frame's eyes were rendered with
    uint32_t targetW = 0, targetH = 0;
    ULONGLONG lastSizeCheck = 0;
    bool sizeLogged = false;
    uint64_t presents = 0;
    // The game's UI panel image (ui_layer writes it on the game's queue; ComposeEye draws
    // it into each eye's image).
    bool panelCaptured = false;            // this frame's panel image is ready
    ID3D12Resource* panelImage = nullptr;  // owned by ui_layer
    uint32_t panelW = 0, panelH = 0;
    uint64_t panelValue = 0;               // copyFence value once written
    // Frames rendered from a camera the mod does not drive (menus, pause screen,
    // cutscenes, the park camera before it is located): shown at a fixed orientation in
    // front of the player instead of following that camera, and the right stick goes
    // back to the game. "Driven" = the rendered camera matches a yaw/pitch pair the
    // controller held (written by the mod or moved by the game's own input) recently.
    struct PairSeen {
        float yaw, pitch, body, tilt;
    };
    static constexpr int kPairRing = 16;
    PairSeen pairRing[kPairRing] = {};
    int pairCount = 0, pairPos = 0;
    bool driven = false;
    int matchRun = 0, mismatchRun = 0;
    bool lastMatchValid = false;
    PairSeen lastMatch{};
    bool anchorValid = false;
    XrQuaternionf anchor{0.f, 0.f, 0.f, 1.f};
    bool anchorKept = false;  // the anchor is the paused park's own orientation (else level where you faced)
    uint64_t undrivenLogs = 0;
    bool shownDriven = false;  // the current frame
    bool parkDepth = false;    // this frame's eye spacing follows the park's depth (zoomScale)
    // On-ride camera (ridecam): stick turn/tilt in the seat, and the look writes of the
    // last frames with the head direction each came from.
    bool rideInit = false;
    float rideYaw = 0.f, rideTilt = 0.f;
    int64_t rideLastTime = 0;
    struct RideSeen {
        float lookYaw, lookPitch, headYaw, headPitch;
    };
    static constexpr int kRideRing = 8;
    RideSeen rideRing[kRideRing] = {};
    int rideCount = 0, ridePos = 0;
    bool rideSeatValid = false;
    cbmath::Vec3 rideSeatF{}, rideSeatU{};
} s;

// Shader-visible descriptors per frame in flight: each eye's game image and UI panel.
constexpr int kSrvPerRing = 4;

std::atomic<bool> g_recenterRequest{false};
std::atomic<int> g_resolutionRequest{0};
std::atomic<int> g_coverageRequest{0}; // 0=no request, 1=original, 2=expanded
std::atomic<uint64_t> g_resolutionSize{0};
// The engine FOV margin (head motion during the frame or two of lag) is s.fovMargin.
constexpr float kMinBodyPitch = 0.26f;     // ~15 degrees below level
constexpr int kStillFramesToResume = 15;
constexpr float kMaxZoomScale = 400.f;  // eye separation at most 400x real (about 25 m)
constexpr float kMaxViewPitch = 1.40f;  // ~80 degrees    // game camera at rest this long before the head takes over again

const char* ResultStr(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g.xrResultToString && s.instance && XR_SUCCEEDED(g.xrResultToString(s.instance, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "%d", static_cast<int>(r));
    return buf;
}

#define XR_CHECK(call)                                                   \
    do {                                                                 \
        const XrResult _r = (call);                                      \
        if (XR_FAILED(_r)) {                                             \
            LOG("OpenXR: %s failed: %s", #call, ResultStr(_r));          \
            return false;                                                \
        }                                                                \
    } while (0)

std::wstring ModDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L"\\/"));
    return dir + L"\\PC2VR";
}

// ------------------------------------------------------------- creation --
bool CreateInstance() {

    const std::wstring loaderPath = ModDirectory() + L"\\openxr_loader.dll";
    HMODULE loader = LoadLibraryW(loaderPath.c_str());
    if (!loader) {
        LOG("OpenXR: cannot load %ls", loaderPath.c_str());
        return false;
    }
    g.getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
    if (!g.getProc) return false;
    Resolve(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", g.xrEnumerateInstanceExtensionProperties);
    Resolve(XR_NULL_HANDLE, "xrCreateInstance", g.xrCreateInstance);
    if (!g.xrCreateInstance || !g.xrEnumerateInstanceExtensionProperties) return false;

    uint32_t count = 0;
    XR_CHECK(g.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(g.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data()));
    bool hasD3D12 = false, hasGaze = false;
    for (const auto& e : exts) {
        hasD3D12 |= strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0;
        hasGaze |= strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0;
    }
    if (!hasD3D12) {
        LOG("OpenXR: runtime does not offer %s", XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        return false;
    }

    // Eye gaze (foveated rendering follows the eyes) whenever the runtime offers it.
    const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "PC2VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "PC2VR");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = hasGaze ? 2 : 1;
    ci.enabledExtensionNames = enabled;
    XR_CHECK(g.xrCreateInstance(&ci, &s.instance));

#define X(name) Resolve(s.instance, #name, g.name);
    PC2VR_XR_FUNCS(X)
#undef X
    Resolve(s.instance, "xrGetD3D12GraphicsRequirementsKHR", g.getD3D12Requirements);

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(g.xrGetInstanceProperties(s.instance, &ip)))
        LOG("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
            XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(g.xrGetSystem(s.instance, &si, &s.system));
    XrSystemEyeGazeInteractionPropertiesEXT gp{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (hasGaze) sp.next = &gp;
    if (XR_SUCCEEDED(g.xrGetSystemProperties(s.instance, s.system, &sp))) LOG("OpenXR system: %s", sp.systemName);
    s.gazeSupported = hasGaze && gp.supportsEyeGazeInteraction;
    LOG("OpenXR: eye gaze %s", s.gazeSupported ? "supported (foveated rendering follows the eyes)"
                              : hasGaze ? "extension present but the system reports no eye tracking"
                                        : "not offered by this runtime (foveated rendering stays centred)");
    return true;
}

cbmath::Vec3 Rotate(const XrQuaternionf& q, const cbmath::Vec3& v);

// Eye gaze: one pose action bound to the eye-gaze interaction profile, located in view
// space each frame. Failure only disables gaze (foveation then stays centred).
void SetUpGaze() {
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asi.actionSetName, "pc2vr_gaze");
    strcpy_s(asi.localizedActionSetName, "PC2VR eye gaze");
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(ai.actionName, "gaze");
    strcpy_s(ai.localizedActionName, "Eye gaze");
    XrPath profile = XR_NULL_PATH, pose = XR_NULL_PATH;
    if (XR_FAILED(g.xrCreateActionSet(s.instance, &asi, &s.gazeSet)) || XR_FAILED(g.xrCreateAction(s.gazeSet, &ai, &s.gazeAction)) ||
        XR_FAILED(g.xrStringToPath(s.instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile)) ||
        XR_FAILED(g.xrStringToPath(s.instance, "/user/eyes_ext/input/gaze_ext/pose", &pose))) {
        LOG("OpenXR: eye gaze setup failed (actions)");
        s.gazeSupported = false;
        return;
    }
    XrActionSuggestedBinding bind{s.gazeAction, pose};
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = profile;
    sb.countSuggestedBindings = 1;
    sb.suggestedBindings = &bind;
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &s.gazeSet;
    XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    sci.action = s.gazeAction;
    sci.poseInActionSpace.orientation.w = 1.f;
    if (XR_FAILED(g.xrSuggestInteractionProfileBindings(s.instance, &sb)) || XR_FAILED(g.xrAttachSessionActionSets(s.session, &at)) ||
        XR_FAILED(g.xrCreateActionSpace(s.session, &sci, &s.gazeSpace))) {
        LOG("OpenXR: eye gaze setup failed (bindings)");
        s.gazeSupported = false;
    }
}

// The gaze direction in head space for this frame's display time, as tan(x), tan(y).
void UpdateGaze() {
    s.gazeValid = false;
    if (!s.gazeSupported || s.gazeSpace == XR_NULL_HANDLE) return;
    XrActiveActionSet active{s.gazeSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(g.xrSyncActions(s.session, &sync))) return;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = s.gazeAction;
    XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
    if (XR_FAILED(g.xrGetActionStatePose(s.session, &gi, &ps)) || !ps.isActive) return;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(g.xrLocateSpace(s.gazeSpace, s.viewSpace, s.displayTime, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
        return;
    const cbmath::Vec3 f = Rotate(loc.pose.orientation, {0.f, 0.f, -1.f});
    if (!(f.z < -0.2f)) return;  // further than ~78 degrees off: ignore
    s.gazeTan[0] = f.x / -f.z;
    s.gazeTan[1] = f.y / -f.z;
    s.gazeValid = true;
    if (!s.gazeLogged) {
        s.gazeLogged = true;
        LOG("OpenXR: eye gaze tracking active (gaze tan %.3f %.3f)", s.gazeTan[0], s.gazeTan[1]);
    }
}

bool CreateSession(ID3D12CommandQueue* queue) {
    if (!queue) {
        LOG("OpenXR: no direct queue seen on the present thread yet");
        return false;
    }
    s.queue = queue;
    const HRESULT hr = s.queue->GetDevice(IID_PPV_ARGS(&s.device));
    if (FAILED(hr)) {
        LOG("OpenXR: queue->GetDevice failed 0x%08lx", hr);
        return false;
    }
    LOG("OpenXR: using present-thread queue %p, device %p", queue, s.device.Get());
    menu::DeviceAvailable(s.device.Get());
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XR_CHECK(g.getD3D12Requirements(s.instance, s.system, &req));
    const LUID luid = s.device->GetAdapterLuid();
    if (memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0) {
        LOG("OpenXR: the headset runtime wants a different GPU than the game is using; VR disabled");
        return false;
    }

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = s.device.Get();
    binding.queue = s.queue.Get();
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = s.system;
    XR_CHECK(g.xrCreateSession(s.instance, &ci, &s.session));
    s.eyeFovValid = false;  // session recommendations can change even if its FOV does not
    s.pixelDensity = s.panelDensity = 0.f;
    {
        uint32_t n = 0;
        XR_CHECK(g.xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
        std::vector<XrViewConfigurationView> vc(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        XR_CHECK(g.xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, vc.data()));
        if (n >= 1) {
            s.recW = vc[0].recommendedImageRectWidth;
            s.recH = vc[0].recommendedImageRectHeight;
            for (uint32_t i = 0; i < 2 && i < n; ++i) {
                s.recEyeW[i] = vc[i].recommendedImageRectWidth;
                s.recEyeH[i] = vc[i].recommendedImageRectHeight;
            }
            s.maxSwapW = s.maxSwapH = 16384;
            for (uint32_t i = 0; i < n; ++i) {
                s.maxSwapW = std::min(s.maxSwapW, vc[i].maxImageRectWidth);
                s.maxSwapH = std::min(s.maxSwapH, vc[i].maxImageRectHeight);
            }
        }
        for (uint32_t i = 0; i < n; ++i)
            LOG("OpenXR: view %u recommended %ux%u (max %ux%u)", i, vc[i].recommendedImageRectWidth,
                vc[i].recommendedImageRectHeight, vc[i].maxImageRectWidth, vc[i].maxImageRectHeight);
    }

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(g.xrCreateReferenceSpace(s.session, &rs, &s.localSpace));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(g.xrCreateReferenceSpace(s.session, &rs, &s.viewSpace));
    if (s.gazeSupported) SetUpGaze();
    LOG("OpenXR: session created on the game's device");
    return true;
}

bool CreateBlitPipeline(DXGI_FORMAT rtvFormat) {
    static const char* kShader = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 Decode(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
// The game's back buffer holds sRGB-encoded values in a UNORM format; the
// target view is *_SRGB, so decode here and let the hardware re-encode.
float4 PS(V i) : SV_Target { return float4(Decode(saturate(src.SampleLevel(smp, i.uv, 0).rgb)), 1); }
)";
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "pc2vr_blit", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "pc2vr_blit", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err))) {
        LOG("blit shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }

    if (!s.rootSig) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC smp{};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{1, &param, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&s.rootSig)))) {
            LOG("blit root signature failed");
            return false;
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = s.rootSig.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = rtvFormat;
    pd.SampleDesc.Count = 1;
    if (FAILED(s.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s.pso)))) {
        LOG("blit PSO failed");
        return false;
    }
    s.psoFormat = rtvFormat;

    if (!s.srvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, State::kRing * kSrvPerRing,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.srvHeap)))) return false;
        s.srvStride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (auto& a : s.alloc)
            if (FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
        if (FAILED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc[0].Get(), nullptr,
                                               IID_PPV_ARGS(&s.list))))
            return false;
        s.list->Close();
        if (FAILED(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) return false;
        s.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    return true;
}

// Each eye's image as a normal VR game submits it: the eye's own view, one projection
// layer. The game's image goes in as rendered (head-driven frames) or, for a fixed
// screen, turned into the eye's view from the shot's fixed orientation, with black
// around it; the UI panel is drawn over it where its plane is in front of the eye.
// Before (2026-10-04) fixed screens went out as a projection layer of the shot's own
// 60-degree FOV and the UI as a quad layer; in the headset (Virtual Desktop) their edges
// stayed blurry wherever you looked, while the images themselves were sharp (dumps).
// Constants (float4 rows):
//  0 outTan: the output view's tan left, right, up, down
//  1-3 the output view's axes in local space as matrix rows (xyz), w: mode, panel on, -
//  4 sceneTan: the game image's tan left, right, up, down (mode 1)
//  5-7 local space to the game image's view, rows (mode 1)
//  8 panel centre relative to the eye (local), 9 panel right / width, 10 panel up / height,
//  11 panel normal, 12 one output pixel in uv (xy)
struct ComposeConstants {
    float outTan[4];
    float out[3][4];
    float sceneTan[4];
    float scene[3][4];
    float panelCentre[4], panelX[4], panelY[4], panelN[4];
    float pixel[4];
};
static_assert(sizeof(ComposeConstants) == 13 * 16, "compose constants");

bool CreateComposePipeline(DXGI_FORMAT rtvFormat) {
    static const char* kShader = R"(
Texture2D scene : register(t0);
Texture2D panel : register(t1);
SamplerState smp : register(s0);
cbuffer C : register(b0) {
    float4 outTan; float4 out0; float4 out1; float4 out2;
    float4 sceneTan; float4 sc0; float4 sc1; float4 sc2;
    float4 pCentre; float4 pX; float4 pY; float4 pN; float4 pixel;
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 Decode(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
// The direction (local space) through output uv.
float3 Ray(float2 uv) {
    float3 d = float3(lerp(outTan.x, outTan.y, uv.x), lerp(outTan.z, outTan.w, uv.y), -1);
    return float3(dot(out0.xyz, d), dot(out1.xyz, d), dot(out2.xyz, d));
}
// The game image (sRGB-encoded UNORM) in linear light; black outside a fixed screen.
float3 Scene(float2 uv) {
    if (out0.w < 0.5) return Decode(saturate(scene.SampleLevel(smp, uv, 0).rgb));
    float3 d = Ray(uv);
    float3 s = float3(dot(sc0.xyz, d), dot(sc1.xyz, d), dot(sc2.xyz, d));
    if (s.z > -1e-4) return 0;
    float2 t = s.xy / -s.z;
    float2 suv = float2((t.x - sceneTan.x) / (sceneTan.y - sceneTan.x), (sceneTan.z - t.y) / (sceneTan.z - sceneTan.w));
    if (any(suv < 0) || any(suv > 1)) return 0;
    return Decode(saturate(scene.SampleLevel(smp, suv, 0).rgb));
}
float4 PS(V i) : SV_Target {
    float3 c;
    if (out0.w < 0.5) {
        c = Scene(i.uv);
    } else {
        // A fixed screen is denser than the eye's view (a 60-degree shot rendered at the
        // eye's size): 2x2 samples per output pixel instead of one.
        c = 0.25 * (Scene(i.uv + pixel.xy * float2(-0.25, -0.25)) + Scene(i.uv + pixel.xy * float2(0.25, -0.25)) +
                    Scene(i.uv + pixel.xy * float2(-0.25, 0.25)) + Scene(i.uv + pixel.xy * float2(0.25, 0.25)));
    }
    if (out1.w > 0.5) {
        float3 d = Ray(i.uv);
        float den = dot(d, pN.xyz);
        if (abs(den) > 1e-6) {
            float t = dot(pCentre.xyz, pN.xyz) / den;
            float3 h = d * t - pCentre.xyz;
            float2 puv = float2(0.5 + dot(h, pX.xyz), 0.5 - dot(h, pY.xyz));
            if (t > 0 && all(puv >= 0) && all(puv <= 1)) {
                float4 p = panel.SampleLevel(smp, puv, 0);  // premultiplied, linear (sRGB view)
                c = c * (1 - p.a) + p.rgb;
            }
        }
    }
    return float4(c, 1);
}
)";
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "pc2vr_compose", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "pc2vr_compose", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err))) {
        LOG("compose shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (!s.composeRootSig) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = sizeof(ComposeConstants) / 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC smp{};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&s.composeRootSig)))) {
            LOG("compose root signature failed");
            return false;
        }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = s.composeRootSig.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = rtvFormat;
    pd.SampleDesc.Count = 1;
    if (FAILED(s.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&s.composePso)))) {
        LOG("compose PSO failed");
        return false;
    }
    s.composeFormat = rtvFormat;
    return true;
}

void DestroySwapchain() {
    for (int e = 0; e < 2; ++e) {
        if (s.swapchain[e]) g.xrDestroySwapchain(s.swapchain[e]);
        s.swapchain[e] = XR_NULL_HANDLE;
        s.swapImages[e].clear();
        s.rtvHeap[e].Reset();
    }
}

bool CreateSwapchain(uint32_t srcWidth, uint32_t srcHeight) {
    DestroySwapchain();
    uint32_t count = 0;
    XR_CHECK(g.xrEnumerateSwapchainFormats(s.session, 0, &count, nullptr));
    std::vector<int64_t> formats(count);
    XR_CHECK(g.xrEnumerateSwapchainFormats(s.session, count, &count, formats.data()));
    s.swapFormat = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT want : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        for (int64_t f : formats)
            if (f == want) s.swapFormat = want;
        if (s.swapFormat != DXGI_FORMAT_UNKNOWN) break;
    }
    if (s.swapFormat == DXGI_FORMAT_UNKNOWN) {
        LOG("OpenXR: runtime offers no sRGB 8-bit swapchain format");
        return false;
    }
    // Same shape as the game's image, inside the runtime's limit.
    const float fit = std::min({1.f, static_cast<float>(s.maxSwapW) / srcWidth, static_cast<float>(s.maxSwapH) / srcHeight});
    const uint32_t width = std::max(16u, static_cast<uint32_t>(srcWidth * fit));
    const uint32_t height = std::max(16u, static_cast<uint32_t>(srcHeight * fit));

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = s.swapFormat;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    s.rtvStride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (int e = 0; e < 2; ++e) {
        XR_CHECK(g.xrCreateSwapchain(s.session, &ci, &s.swapchain[e]));
        XR_CHECK(g.xrEnumerateSwapchainImages(s.swapchain[e], 0, &count, nullptr));
        std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        XR_CHECK(g.xrEnumerateSwapchainImages(s.swapchain[e], count, &count,
                                              reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtvHeap[e])))) return false;
        for (uint32_t i = 0; i < count; ++i) {
            s.swapImages[e].push_back(images[i].texture);
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = s.swapFormat;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtvHeap[e]->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(i) * s.rtvStride;
            s.device->CreateRenderTargetView(images[i].texture, &rv, h);
        }
    }
    s.swapW = width;
    s.swapH = height;
    s.srcW = srcWidth;
    s.srcH = srcHeight;
    g_resolutionSize.store((static_cast<uint64_t>(srcWidth)<<32)|srcHeight,std::memory_order_relaxed);
    if (s.psoFormat != s.swapFormat && !CreateBlitPipeline(s.swapFormat)) return false;
    if (s.composeFormat != s.swapFormat && !CreateComposePipeline(s.swapFormat)) return false;
    LOG("OpenXR: 2 swapchains %ux%u format %d, %u images each (game image %ux%u, runtime max %ux%u)", width, height,
        s.swapFormat, count, srcWidth, srcHeight, s.maxSwapW, s.maxSwapH);
    return true;
}

// ---------------------------------------------------------------- events --
void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (g.xrPollEvent(s.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // The runtime's own recentre (e.g. holding the headset's menu button): the
            // local space moves to where you face. Everything placed from the old forward
            // direction (the UI panel, fixed screens, the park's heading) is placed again.
            const auto& e = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
            if (e.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                s.recenterPending = true;
                LOG("OpenXR: the runtime recentred the local space");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            s.sessionState = e.state;
            LOG("OpenXR: session state %d", e.state);
            if (e.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult r = g.xrBeginSession(s.session, &bi);
                s.running = XR_SUCCEEDED(r);
                s.recenterPending = true;
                LOG("OpenXR: xrBeginSession %s", ResultStr(r));
            } else if (e.state == XR_SESSION_STATE_STOPPING) {
                camctl::RestoreEngineFov();
                g.xrEndSession(s.session);
                s.running = false;
                s.frameBegun = false;
            } else if (e.state == XR_SESSION_STATE_EXITING || e.state == XR_SESSION_STATE_LOSS_PENDING) {
                camctl::RestoreEngineFov();
                s.running = false;
                s.frameBegun = false;
                s.phase = Phase::Failed;  // restart the game to reconnect (handled properly later)
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            s.running = false;
            s.frameBegun = false;
            s.phase = Phase::Failed;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ------------------------------------------------------------------ math --
cbmath::Vec3 Rotate(const XrQuaternionf& q, const cbmath::Vec3& v) {
    // v' = q v q*
    const cbmath::Vec3 u{q.x, q.y, q.z};
    const float s0 = q.w;
    const cbmath::Vec3 c1 = cbmath::Cross(u, v);
    const cbmath::Vec3 c2 = cbmath::Cross(u, c1);
    return {v.x + 2.f * (s0 * c1.x + c2.x), v.y + 2.f * (s0 * c1.y + c2.y), v.z + 2.f * (s0 * c1.z + c2.z)};
}

// OpenXR is right-handed (-z forward); the game is left-handed (+z forward).
cbmath::Vec3 ToGame(const cbmath::Vec3& v) { return {v.x, v.y, -v.z}; }

cbmath::Vec3 Yaw(const cbmath::Vec3& v, float angle) {
    const float c = std::cos(angle), sn = std::sin(angle);
    return {v.x * c + v.z * sn, v.y, -v.x * sn + v.z * c};
}

float YawOf(const cbmath::Vec3& forward) { return std::atan2(forward.x, forward.z); }

// Rodrigues rotation of v about unit axis k (in game space, positive angle about the
// body's right axis tilts forward downward).
cbmath::Vec3 RotateAxis(const cbmath::Vec3& v, const cbmath::Vec3& k, float angle) {
    const float c = std::cos(angle), sn = std::sin(angle);
    const cbmath::Vec3 kxv = cbmath::Cross(k, v);
    const float kd = cbmath::Dot(k, v);
    return {v.x * c + kxv.x * sn + k.x * kd * (1 - c), v.y * c + kxv.y * sn + k.y * kd * (1 - c),
            v.z * c + kxv.z * sn + k.z * kd * (1 - c)};
}

// Orientation whose local axes are right (+x), up (+y) and forward (-z), OpenXR convention.
XrQuaternionf QuatFromBasis(const cbmath::Vec3& r, const cbmath::Vec3& u, const cbmath::Vec3& f) {
    const float m00 = r.x, m10 = r.y, m20 = r.z;     // column 0 = x axis
    const float m01 = u.x, m11 = u.y, m21 = u.z;     // column 1 = y axis
    const float m02 = -f.x, m12 = -f.y, m22 = -f.z;  // column 2 = z axis (backward)
    XrQuaternionf q;
    const float trace = m00 + m11 + m22;
    if (trace > 0.f) {
        const float k = std::sqrt(trace + 1.f) * 2.f;
        q = {(m21 - m12) / k, (m02 - m20) / k, (m10 - m01) / k, 0.25f * k};
    } else if (m00 > m11 && m00 > m22) {
        const float k = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q = {0.25f * k, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k};
    } else if (m11 > m22) {
        const float k = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q = {(m01 + m10) / k, 0.25f * k, (m12 + m21) / k, (m02 - m20) / k};
    } else {
        const float k = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q = {(m02 + m20) / k, (m12 + m21) / k, 0.25f * k, (m10 - m01) / k};
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

bool EyeHalfExtents(const XrQuaternionf& reference, float& halfH, float& halfV) {
    float h = 0.f, v = 0.f;
    for (int i = 0; i < 2; ++i) {
        XrFovf f{};
        if (!s.outputBounds[i].Get(s.eyeViewFov[i], s.eyeViewPose[i].orientation, reference, f)) return false;
        h = std::max(h, std::max(std::fabs(f.angleLeft), std::fabs(f.angleRight)));
        v = std::max(v, std::max(std::fabs(f.angleUp), std::fabs(f.angleDown)));
    }
    halfH = h;
    halfV = v;
    return true;
}

float AngleDiff(float a, float b) {
    const float twoPi = 6.28318530718f;
    float d = std::fmod(a - b, twoPi);
    if (d > twoPi * 0.5f) d -= twoPi;
    if (d < -twoPi * 0.5f) d += twoPi;
    return d;
}

// ----------------------------------------------------------------- frame --
// Virtual screen 2.5 m ahead in the recentred direction, 16:9 at about 70 degrees wide.
void PlaceScreen(XrCompositionLayerQuad& quad, const XrRect2Di& rect) {
    if (s.recenterPending) {
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(g.xrLocateSpace(s.viewSpace, s.localSpace, s.displayTime, &head)) &&
            (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            s.headYaw0 = YawOf(ToGame(Rotate(head.pose.orientation, {0, 0, -1})));
            s.headPos0 = head.pose.position;
            s.recenterPending = false;
            LOG("OpenXR: recentred for the virtual screen (yaw %.1f deg)", s.headYaw0 * 57.29578f);
        }
    }
    const float distance = 2.5f;
    const float width = 2.f * distance * std::tan(35.f * 3.14159265f / 180.f);
    const float yaw = s.headYaw0;  // game-space yaw; XR direction is (sin, 0, -cos)
    quad.space = s.localSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = s.swapchain[0];
    quad.subImage.imageRect = rect;
    quad.pose.position = {std::sin(yaw) * distance, 0.f, -std::cos(yaw) * distance};
    quad.pose.orientation = {0.f, std::sin(-yaw * 0.5f), 0.f, std::cos(-yaw * 0.5f)};
    quad.size = {width, width * rect.extent.height / static_cast<float>(rect.extent.width)};
}

// The game's UI panel: in the recentred direction (it stays in front of your body while
// the stick turns the park), at the panel's distance, centre heightDeg above or below eye
// level and tilted to face you, widthDeg wide; the UI layer's shape. Pose in local space.
void PanelGeometry(XrPosef& pose, float& width, float& height, const ui::PanelSettings& p) {
    const float yaw = s.headYaw0, d = p.distanceM;
    width = 2.f * d * std::tan(0.5f * p.widthDeg * 0.0174533f);
    height = width * s.panelH / static_cast<float>(std::max(1u, s.panelW));
    // From where the head was when recentred: the local space's origin is wherever the
    // runtime put it (floor level on some), not necessarily the head.
    const XrPosef anchor{{0.f,std::sin(-yaw*.5f),0.f,std::cos(-yaw*.5f)},s.headPos0};
    pose=ui::PanelPose(p,anchor,s.headPose,s.headPositionValid);
}

// The game image covers both eyes plus a margin, centred on the head. Each eye gets
// only the part its own FOV (head space) covers, the way a normal game's image matches
// the eye, with the angles recomputed from the whole-pixel rectangle. Measured in the
// headset (2026-10-02): sending the whole image made Virtual Desktop's Foveated
// Streaming blur the middle and sharpen the edges; cropped, it looks right.
void CropToEye(int eye, XrCompositionLayerProjectionView& v, const XrFovf& image) {
    XrFovf e = s.eyeFovHead[eye];
    // Expanded coverage: the engine may lag tracking or keep a level horizon. Crop in the coordinates
    // of the actual rendered/declared image, including the physical eye's full cant.
    // If any ray crosses its forward plane, keep the full image rather than invent
    // a rectangular crop for a frustum this camera cannot represent.
    if(s.expandedCoverage && !s.outputBounds[eye].Get(s.eyeViewFov[eye],s.eyeViewPose[eye].orientation,v.pose.orientation,e)) return;
    const float TL = std::tan(image.angleLeft), TR = std::tan(image.angleRight);
    const float TU = std::tan(image.angleUp), TD = std::tan(image.angleDown);
    if (!(TR - TL > 0.01f && TU - TD > 0.01f)) return;
    auto clampf = [](float x, float a, float b) { return std::max(a, std::min(b, x)); };
    const float W = static_cast<float>(s.swapW), H = static_cast<float>(s.swapH);
    const int x0 = static_cast<int>(std::floor(clampf((std::tan(e.angleLeft) - TL) / (TR - TL), 0.f, 1.f) * W));
    const int x1 = static_cast<int>(std::ceil(clampf((std::tan(e.angleRight) - TL) / (TR - TL), 0.f, 1.f) * W));
    const int y0 = static_cast<int>(std::floor(clampf((TU - std::tan(e.angleUp)) / (TU - TD), 0.f, 1.f) * H));
    const int y1 = static_cast<int>(std::ceil(clampf((TU - std::tan(e.angleDown)) / (TU - TD), 0.f, 1.f) * H));
    if (x1 - x0 < 16 || y1 - y0 < 16) return;
    v.subImage.imageRect = {{x0, y0}, {x1 - x0, y1 - y0}};
    v.fov.angleLeft = std::atan(TL + (TR - TL) * x0 / W);
    v.fov.angleRight = std::atan(TL + (TR - TL) * x1 / W);
    v.fov.angleUp = std::atan(TU - (TU - TD) * y0 / H);
    v.fov.angleDown = std::atan(TU - (TU - TD) * y1 / H);
}

// Draws `src` (sRGB-encoded UNORM) over a whole eye swapchain image (or, with the
// panel pipeline, the panel image over the panel swapchain image: w x h).
// `slot` picks this frame's descriptor (0-2) so several blits can share a list.
void RecordBlit(ID3D12Resource* src, DXGI_FORMAT format, D3D12_RESOURCE_STATES srcState, int slot,
                D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12PipelineState* pso = nullptr, uint32_t w = 0, uint32_t h = 0) {
    if (!w || !h) {
        w = s.swapW;
        h = s.swapH;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = s.srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(s.ring * kSrvPerRing + slot) * s.srvStride;
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = s.srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(s.ring * kSrvPerRing + slot) * s.srvStride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(src, &sv, srvCpu);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = srcState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (srcState != b.Transition.StateAfter) s.list->ResourceBarrier(1, &b);

    ID3D12DescriptorHeap* heaps[] = {s.srvHeap.Get()};
    s.list->SetDescriptorHeaps(1, heaps);
    s.list->SetGraphicsRootSignature(s.rootSig.Get());
    s.list->SetPipelineState(pso ? pso : s.pso.Get());
    s.list->SetGraphicsRootDescriptorTable(0, srvGpu);
    const D3D12_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    const D3D12_RECT sr{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    s.list->RSSetViewports(1, &vp);
    s.list->RSSetScissorRects(1, &sr);
    s.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.list->DrawInstanced(3, 1, 0, 0);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    if (srcState != b.Transition.StateBefore) s.list->ResourceBarrier(1, &b);
}

// One eye's image (ComposeEye): `src` is the game's image for this eye (sRGB-encoded
// UNORM), drawn over the eye's swapchain image with the constants' view, and the UI
// panel image over it when `panel` is set.
void RecordCompose(ID3D12Resource* src, DXGI_FORMAT format, D3D12_RESOURCE_STATES srcState, int eye,
                   D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12Resource* panel, const ComposeConstants& c) {
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = s.srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(s.ring * kSrvPerRing + eye * 2) * s.srvStride;
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = s.srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(s.ring * kSrvPerRing + eye * 2) * s.srvStride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(src, &sv, srvCpu);
    // The panel image is read through an sRGB view: linear, premultiplied. Without a
    // panel the slot holds a null view (the shader does not read it).
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srvCpu.ptr += s.srvStride;
    s.device->CreateShaderResourceView(panel, &sv, srvCpu);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = srcState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (srcState != b.Transition.StateAfter) s.list->ResourceBarrier(1, &b);

    ID3D12DescriptorHeap* heaps[] = {s.srvHeap.Get()};
    s.list->SetDescriptorHeaps(1, heaps);
    s.list->SetGraphicsRootSignature(s.composeRootSig.Get());
    s.list->SetPipelineState(s.composePso.Get());
    s.list->SetGraphicsRootDescriptorTable(0, srvGpu);
    s.list->SetGraphicsRoot32BitConstants(1, sizeof(ComposeConstants) / 4, &c, 0);
    const D3D12_VIEWPORT vp{0, 0, static_cast<float>(s.swapW), static_cast<float>(s.swapH), 0, 1};
    const D3D12_RECT sr{0, 0, static_cast<LONG>(s.swapW), static_cast<LONG>(s.swapH)};
    s.list->RSSetViewports(1, &vp);
    s.list->RSSetScissorRects(1, &sr);
    s.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.list->DrawInstanced(3, 1, 0, 0);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    if (srcState != b.Transition.StateBefore) s.list->ResourceBarrier(1, &b);
}

// The view an eye's image is submitted with this frame, and what goes into it.
struct EyeOutput {
    XrPosef pose;
    XrFovf fov;        // of the whole swapchain image (before any crop)
    bool fixedScreen;  // the game's shot turned into the eye's own view
    bool crop;         // cropped to the eye's FOV on submit (head-driven, oversized image)
};

EyeOutput EyeOutputFor(int eye, bool eyes) {
    EyeOutput o{};
    if (s.engineView && !s.shownDriven) {
        o.pose = s.eyeViewPose[eye];
        o.fov = s.eyeViewFov[eye];
        o.fixedScreen = true;
        return o;
    }
    o.pose = eyes ? s.eyeSubmitPose[eye] : s.submitPose;
    o.fov = (eyes && s.perEyeProjection) ? s.eyeRenderFovFrame[eye] : s.submitFov;
    o.crop = eyes && s.submitEyeFov;
    return o;
}

ComposeConstants ComposeConstantsFor(const EyeOutput& o, bool panel, const ui::PanelSettings& panelSettings) {
    ComposeConstants c{};
    c.outTan[0] = std::tan(o.fov.angleLeft);
    c.outTan[1] = std::tan(o.fov.angleRight);
    c.outTan[2] = std::tan(o.fov.angleUp);
    c.outTan[3] = std::tan(o.fov.angleDown);
    // The output view's axes (right, up, back) in local space, as the columns of the
    // matrix whose rows go in out[].
    const cbmath::Vec3 r = Rotate(o.pose.orientation, {1, 0, 0}), u = Rotate(o.pose.orientation, {0, 1, 0}),
                       bk = Rotate(o.pose.orientation, {0, 0, 1});
    const float m[3][3] = {{r.x, u.x, bk.x}, {r.y, u.y, bk.y}, {r.z, u.z, bk.z}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c.out[i][j] = m[i][j];
    c.out[0][3] = o.fixedScreen ? 1.f : 0.f;
    c.out[1][3] = panel ? 1.f : 0.f;
    if (o.fixedScreen) {
        c.sceneTan[0] = std::tan(s.submitFov.angleLeft);
        c.sceneTan[1] = std::tan(s.submitFov.angleRight);
        c.sceneTan[2] = std::tan(s.submitFov.angleUp);
        c.sceneTan[3] = std::tan(s.submitFov.angleDown);
        // Local space to the shot's view: the shot's axes as rows.
        // Fixed screens keep their native picture and UI together. Following the HMD
        // rotates the whole shot, avoiding the old menu/globe separation regression.
        const auto q = panelSettings.followHmd ? ui::Multiply(s.headPose.orientation,ui::PanelOffset(panelSettings)) : s.submitPose.orientation;
        const cbmath::Vec3 rows[3] = {Rotate(q, {1, 0, 0}), Rotate(q, {0, 1, 0}), Rotate(q, {0, 0, 1})};
        for (int i = 0; i < 3; ++i) {
            c.scene[i][0] = rows[i].x;
            c.scene[i][1] = rows[i].y;
            c.scene[i][2] = rows[i].z;
        }
    }
    if (panel) {
        XrPosef pp;
        float w, h;
        PanelGeometry(pp, w, h, panelSettings);
        const cbmath::Vec3 px = Rotate(pp.orientation, {1, 0, 0}), py = Rotate(pp.orientation, {0, 1, 0}),
                           pn = Rotate(pp.orientation, {0, 0, 1});
        const float centre[3] = {pp.position.x - o.pose.position.x, pp.position.y - o.pose.position.y,
                                 pp.position.z - o.pose.position.z};
        const float ax[3] = {px.x / w, px.y / w, px.z / w}, ay[3] = {py.x / h, py.y / h, py.z / h}, an[3] = {pn.x, pn.y, pn.z};
        memcpy(c.panelCentre, centre, sizeof(centre));
        memcpy(c.panelX, ax, sizeof(ax));
        memcpy(c.panelY, ay, sizeof(ay));
        memcpy(c.panelN, an, sizeof(an));
    }
    c.pixel[0] = 1.f / static_cast<float>(s.swapW);
    c.pixel[1] = 1.f / static_cast<float>(s.swapH);
    return c;
}

void SubmitFrame(IDXGISwapChain* swapChain) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = s.displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};

    // The game's UI was taken out of this frame's image (ui_layer): it is drawn into each
    // eye's image on its panel.
    const bool panelOn = s.panelCaptured && s.panelImage && s.poseAcquired;

    bool submitted = false;
    ComPtr<IDXGISwapChain3> sc3;
    if (s.shouldRender && SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        sc3->GetDesc1(&desc);
        if ((desc.Width != s.srcW || desc.Height != s.srcH) && !CreateSwapchain(desc.Width, desc.Height)) {
            s.phase = Phase::Failed;
        } else {
            ComPtr<ID3D12Resource> backBuffer;
            sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
            uint32_t index[2] = {};
            int acquired = 0;
            for (int e = 0; e < 2 && backBuffer; ++e) {
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_FAILED(g.xrAcquireSwapchainImage(s.swapchain[e], &ai, &index[e]))) break;
                ++acquired;
                if (XR_FAILED(g.xrWaitSwapchainImage(s.swapchain[e], &wi))) break;
            }
            if (acquired == 2) {
                // Recycle the allocator from kRing frames ago.
                s.ring = (s.ring + 1) % State::kRing;
                if (s.fence->GetCompletedValue() < s.fenceValue[s.ring]) {
                    s.fence->SetEventOnCompletion(s.fenceValue[s.ring], s.fenceEvent);
                    WaitForSingleObject(s.fenceEvent, 1000);
                }
                s.alloc[s.ring]->Reset();
                s.list->Reset(s.alloc[s.ring].Get(), s.pso.Get());

                // Both eyes of this frame were captured from the game's render batch;
                // otherwise (menus, loading) the one image goes to both.
                const bool eyes = s.stereo && s.eyesCaptured && s.eyeTex[0] && s.eyeTex[1];
                const auto panelSettings=ui::Settings(); // one snapshot for both eyes
                for (int e = 0; e < 2; ++e) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s.rtvHeap[e]->GetCPUDescriptorHandleForHeapStart();
                    rtv.ptr += static_cast<SIZE_T>(index[e]) * s.rtvStride;
                    ID3D12Resource* src = eyes ? s.eyeTex[e].Get() : backBuffer.Get();
                    const DXGI_FORMAT fmt = eyes ? s.eyeFormat : desc.Format;
                    const D3D12_RESOURCE_STATES state = eyes ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_PRESENT;
                    if (s.poseAcquired)
                        RecordCompose(src, fmt, state, e, rtv, panelOn ? s.panelImage : nullptr,
                                      ComposeConstantsFor(EyeOutputFor(e, eyes), panelOn, panelSettings));
                    else
                        RecordBlit(src, fmt, state, e, rtv);
                }
                s.list->Close();
                // The eyes and the panel image were written on the game's render queue;
                // wait for them there.
                if (eyes || panelOn) s.queue->Wait(s.copyFence.Get(), std::max(eyes ? s.eyesValue : 0, panelOn ? s.panelValue : 0));
                ID3D12CommandList* lists[] = {s.list.Get()};
                s.queue->ExecuteCommandLists(1, lists);
                s.fenceValue[s.ring] = s.nextFence++;
                s.queue->Signal(s.fence.Get(), s.fenceValue[s.ring]);
                if (eyes || panelOn) s.lastBlitValue = s.fenceValue[s.ring];

            }
            for (int e = 0; e < acquired; ++e) {
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                g.xrReleaseSwapchainImage(s.swapchain[e], &ri);
            }
            if (acquired == 2) {
                const bool eyes = s.stereo && s.eyesCaptured && s.eyeTex[0] && s.eyeTex[1];
                const XrRect2Di rect{{0, 0}, {static_cast<int32_t>(s.swapW), static_cast<int32_t>(s.swapH)}};
                if (s.poseAcquired) {
                    // A head-tracked 3D frame: shown with the pose it was rendered with (per
                    // eye in stereo, where each eye was rendered from its own position); a
                    // fixed screen, with the eye's own view it was composed into.
                    for (int i = 0; i < 2; ++i) {
                        XrCompositionLayerProjectionView& v = views[i];
                        const EyeOutput o = EyeOutputFor(i, eyes);
                        v.pose = o.pose;
                        v.fov = o.fov;
                        v.subImage.swapchain = s.swapchain[i];
                        v.subImage.imageRect = rect;
                        if (o.crop) CropToEye(i, v, o.fov);
                    }
                    layer.space = s.localSpace;
                    layer.viewCount = 2;
                    layer.views = views;
                } else {
                    // Nothing was head-tracked this frame (intro video, loading, 2D-only
                    // screens): show the game's image on a virtual screen instead of
                    // leaving the frame empty, which the headset shows as black flicker.
                    PlaceScreen(quad, rect);
                    layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
                }
                ei.layerCount = 1;
                ei.layers = layers;
                submitted = true;
            }
        }
    }
    s.eyesCaptured = false;
    s.panelCaptured = false;
    const XrResult r = g.xrEndFrame(s.session, &ei);
    if (XR_FAILED(r)) LOG("OpenXR: xrEndFrame %s", ResultStr(r));
    ++s.frames;
}

}  // namespace

namespace {
void RefreshBackBuffers();

// The engine renders one symmetric frustum (its own camera) for both eyes, so the
// game's image must cover the union of both eyes' FOV plus the lag margin. In
// windowed mode the game renders at exactly its window's client size, so the window
// is sized to that frustum's shape at the headset's pixel density: the density of
// the runtime's recommended per-eye image over that eye's own FOV, times ScalePercent.
void UpdateRenderSize(IDXGISwapChain* swapChain) {
    const ULONGLONG now = GetTickCount64();
    if (now - s.lastSizeCheck < 1000) return;
    s.lastSizeCheck = now;
    if (!s.recW || !s.recH || !s.eyeFovValid) return;
    const float m = 0.5f * s.fovMargin;
    float halfH = 0.f, halfV = 0.f;
    for (const XrFovf& f : s.eyeFovHead) {
        halfH = std::max(halfH, std::max(-f.angleLeft, f.angleRight) + m);
        halfV = std::max(halfV, std::max(f.angleUp, -f.angleDown) + m);
    }
    // A forward pinhole cannot reach 90 degrees. 83 degrees was an arbitrary
    // earlier cap and discarded valid outer rays on wide/canted displays.
    const float forwardLimit=std::nextafter(1.57079632679489661923f,0.f);
    const float sizingLimit = s.expandedCoverage ? forwardLimit : 1.45f;
    halfH = std::min(halfH, sizingLimit);
    halfV = std::min(halfV, sizingLimit);
    // Pixels per unit of tan at the image centre. The game assumes square pixels (its
    // horizontal FOV follows from the vertical one and the window's aspect), so one
    // density for both axes - the higher of the two, to lose no detail - and the
    // window's aspect is exactly tan(halfH) / tan(halfV). Measured with a wide-FOV
    // profile: separate per-axis densities gave a window shape that did not match
    // the FOV, and the engine then under-covered horizontally.
    float density = s.pixelDensity;
    if (!s.expandedCoverage) {
        const XrFovf& e = s.eyeFov[0];
        const float spanX = std::tan(e.angleRight) - std::tan(e.angleLeft);
        const float spanY = std::tan(e.angleUp) - std::tan(e.angleDown);
        if (!(spanX > 0.1f && spanY > 0.1f)) return;
        density = std::max(s.recW / spanX, s.recH / spanY);
    }
    if (!(density > 0.f)) return;
    const float dens = density * s.renderScale;
    float fw = dens * 2.f * std::tan(halfH), fh = dens * 2.f * std::tan(halfV);
    if (s.perEyeProjection && s.stereo) {
        // Each run renders one eye's own frustum: the window is that frustum's shape
        // (both eyes' frusta are mirror images; the larger span of the two is used).
        float sx = 0.f, sy = 0.f;
        for (const XrFovf& f : s.eyeRenderFov) {
            sx = std::max(sx, std::tan(f.angleRight) - std::tan(f.angleLeft));
            sy = std::max(sy, std::tan(f.angleUp) - std::tan(f.angleDown));
        }
        fw = dens * sx;
        fh = dens * sy;
        halfH = std::atan(0.5f * sx);  // for the log: the equivalent symmetric FOV
        halfV = std::atan(0.5f * sy);
    }
    // Keeps the shape: within 8192 (headroom under the D3D12 texture limit) and within
    // the runtime's maximum image size (no point rendering more than it accepts).
    const float fit = std::min({1.f, 8192.f / std::max(fw, fh), s.maxSwapW / fw, s.maxSwapH / fh});
    fw *= fit;
    fh *= fit;
    auto round8 = [](float v) { return static_cast<uint32_t>(std::max(256.f, std::round(v / 8.f) * 8.f)); };
    const uint32_t w = round8(fw), h = round8(fh);
    if (w != s.targetW || h != s.targetH || !s.sizeLogged) {
        s.targetW = w;
        s.targetH = h;
        s.sizeLogged = true;
        LOG("render size: %ux%u for FOV %.1f x %.1f deg (eyes in head space L%.1f R%.1f U%.1f D%.1f / L%.1f R%.1f U%.1f D%.1f, margin %.1f), "
            "recommended %ux%u per eye, scale %.2f",
            w, h, 2 * halfH * 57.29578f, 2 * halfV * 57.29578f, s.eyeFovHead[0].angleLeft * 57.29578f,
            s.eyeFovHead[0].angleRight * 57.29578f, s.eyeFovHead[0].angleUp * 57.29578f, s.eyeFovHead[0].angleDown * 57.29578f,
            s.eyeFovHead[1].angleLeft * 57.29578f, s.eyeFovHead[1].angleRight * 57.29578f, s.eyeFovHead[1].angleUp * 57.29578f,
            s.eyeFovHead[1].angleDown * 57.29578f, s.fovMargin * 57.29578f, s.recW, s.recH, s.renderScale);
    }
    ComPtr<IDXGISwapChain1> sc1;
    HWND hwnd = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&sc1))) || FAILED(sc1->GetHwnd(&hwnd)) || !hwnd) return;
    RECT client{};
    GetClientRect(hwnd, &client);
    const LONG cw = client.right - client.left, ch = client.bottom - client.top;
    if (std::abs(cw - static_cast<LONG>(w)) <= 4 && std::abs(ch - static_cast<LONG>(h)) <= 4) return;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    if (!(style & WS_CAPTION)) {
        static bool warned = false;
        if (!warned) LOG("render size: the game is not in windowed mode (VR graphics profile not active); size left alone");
        warned = true;
        return;
    }
    RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRectEx(&r, style, FALSE, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
    // Asynchronous: the window belongs to the game's main thread, and this is the
    // render thread; a synchronous resize could wait on a thread that waits on us.
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    LOG("render size: window client %ldx%ld -> %ux%u", cw, ch, w, h);
}
}  // namespace

void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue) {
    if (const int percent=g_resolutionRequest.exchange(0)) {
        s.renderScale=percent/100.f;
        s.lastSizeCheck=0;
    }
    if (s.running) UpdateRenderSize(swapChain);
    if (++s.presents == 600) cfgredirect::LogStatus();
    if (swapChain != s.gameSwapRaw) {
        s.gameSwap.Reset();
        swapChain->QueryInterface(IID_PPV_ARGS(&s.gameSwap));
        s.gameSwapRaw = swapChain;
        RefreshBackBuffers();
    }

    if (s.phase == Phase::Failed) return;
    if (s.phase == Phase::Uninitialised) {
        if (!presentQueue) return;  // wait until the render thread has submitted once
        s.phase = Phase::Failed;
        if (!CreateInstance() || !CreateSession(presentQueue)) return;
        s.phase = Phase::Ready;
    }
    if (presentQueue && presentQueue != s.queue.Get() && s.frames < 5)
        LOG("OpenXR: note: present-thread queue changed to %p", presentQueue);
    if (s.frameBegun) {
        SubmitFrame(swapChain);
        s.frameBegun = false;
    }
    // Finish the old frame with its own geometry before switching the entire
    // policy. The next AcquireRenderView rebuilds both eyes before another resize.
    if (g_coverageRequest.load(std::memory_order_relaxed)) {
        const int request=g_coverageRequest.exchange(0);
        const bool expanded=request==2;
        if (expanded!=s.expandedCoverage) {
            s.expandedCoverage=expanded;
            s.engineFovMax=frustum::CameraLimit(s.engineFovOverride,expanded);
            s.eyeFovValid=false;
            s.pixelDensity=s.panelDensity=0.f;
            s.lastSizeCheck=0;
            s.sizeLogged=false;
        }
    }
    PollEvents();
    if (!s.running || s.phase != Phase::Ready) return;

    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    const XrResult wr = g.xrWaitFrame(s.session, &wi, &fs);
    if (XR_FAILED(wr)) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const bool begun = XR_SUCCEEDED(g.xrBeginFrame(s.session, &bi));
    if (!begun) return;
    s.frameBegun = true;
    s.displayTime = fs.predictedDisplayTime;
    s.shouldRender = fs.shouldRender == XR_TRUE;
    s.poseAcquired = false;
}

namespace {
// ---- which camera the frame shows (State::driven) ----
// Measured: the rendered camera follows the controller pair one or two frames late, to
// float precision (camctl compares at 1e-4). The ring holds the pairs of the last frames,
// each with the body yaw and tilt it was shown with.
constexpr float kPairTolerance = 2e-3f;  // radians; a stick turn moves ~0.03 per frame
constexpr int kMismatchFrames = 3, kMatchFrames = 2;

void PushPair(float yaw, float pitch) {
    s.pairRing[s.pairPos] = {yaw, pitch, s.bodyYaw, s.tilt};
    s.pairPos = (s.pairPos + 1) % State::kPairRing;
    s.pairCount = std::min(s.pairCount + 1, State::kPairRing);
}

void ResetDriven() {
    if (s.driven && ++s.undrivenLogs <= 100) LOG("view: the game camera is no longer driven by the head");
    s.driven = false;
    s.pairCount = 0;
    s.matchRun = s.mismatchRun = 0;
    s.lastMatchValid = false;
}

// memYaw/memPitch: the controller's pair now (before this frame's write).
void UpdateDriven(const cbmath::Vec3& renderedForward, float memYaw, float memPitch) {
    PushPair(memYaw, memPitch);
    const float ry = YawOf(renderedForward);
    const float rp = -std::asin(std::fmax(-1.f, std::fmin(1.f, renderedForward.y)));
    bool match = false;
    for (int i = 0; i < s.pairCount && !match; ++i) {  // newest first
        const State::PairSeen& e = s.pairRing[(s.pairPos - 1 - i + 2 * State::kPairRing) % State::kPairRing];
        if (std::fabs(AngleDiff(ry, e.yaw)) < kPairTolerance && std::fabs(rp - e.pitch) < kPairTolerance) {
            match = true;
            s.lastMatch = e;
            s.lastMatchValid = true;
        }
    }
    s.matchRun = match ? s.matchRun + 1 : 0;
    s.mismatchRun = match ? 0 : s.mismatchRun + 1;
    if (s.driven && s.mismatchRun >= kMismatchFrames) {
        s.driven = false;
        // Undo any stick turn or tilt of the last frames: it moved nothing you could see.
        if (s.lastMatchValid) {
            s.bodyYaw = s.lastMatch.body;
            s.tilt = s.lastMatch.tilt;
        }
        if (++s.undrivenLogs <= 100)
            LOG("view: the game draws another camera (rendered yaw %.4f pitch %.4f, controller %.4f %.4f)", ry, rp, memYaw, memPitch);
    } else if (!s.driven && s.matchRun >= kMatchFrames) {
        s.driven = true;
        s.anchorValid = false;
        if (s.undrivenLogs <= 100) LOG("view: the game draws the head-driven camera");
    }
}
}  // namespace

bool AcquireRenderView(const cbmath::ParsedView& game, RenderView& out) {
    if (!s.frameBegun || !s.shouldRender) return false;
    if (s.poseAcquired) {
        out = s.renderView;
        return true;
    }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

    if (XR_FAILED(g.xrLocateSpace(s.viewSpace, s.localSpace, s.displayTime, &head)) ||
        !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return false;
    UpdateGaze();

    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = s.displayTime;
    li.space = s.localSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    if (XR_FAILED(g.xrLocateViews(s.session, &li, &vs, 2, &n, views)) || n != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;

    s.headPose = head.pose;
    s.headPositionValid = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    // Opt-in expanded coverage encloses all physical eye corners after full
    // eye-to-head rotation; Off retains the prior guarded yaw/pitch offsets.
    for (int i = 0; i < 2; ++i) {
        if (!s.expandedCoverage) {
            // Keep the known-good implementation, including its original guarded
            // yaw/pitch offsets. Expanded corner/roll work is strictly opt-in.
            XrFovf f = views[i].fov;
            const XrQuaternionf& hq = head.pose.orientation;
            const cbmath::Vec3 ef = Rotate(views[i].pose.orientation, {0, 0, -1});
            const float fx = cbmath::Dot(ef, Rotate(hq, {1, 0, 0}));
            const float fy = cbmath::Dot(ef, Rotate(hq, {0, 1, 0}));
            const float fz = cbmath::Dot(ef, Rotate(hq, {0, 0, -1}));
            const float yawOff = std::atan2(fx, fz), pitchOff = std::atan2(fy, std::sqrt(fx * fx + fz * fz));
            if (std::isfinite(yawOff) && std::isfinite(pitchOff) && std::fabs(yawOff) < 0.6f && std::fabs(pitchOff) < 0.6f) {
                f.angleLeft += yawOff;
                f.angleRight += yawOff;
                f.angleUp += pitchOff;
                f.angleDown += pitchOff;
            }
            s.eyeFovHead[i] = f;
        } else if(!s.headBounds[i].Get(views[i].fov,views[i].pose.orientation,head.pose.orientation,s.eyeFovHead[i])) {
            // Unsupported beyond-forward-plane views retain a finite fallback.
            // This does not claim coverage of a >180-degree head-aligned view.
            s.eyeFovHead[i]=views[i].fov;
            if (!s.boundsWarning[i]) {
                s.boundsWarning[i] = true;
                LOG("OpenXR: eye %d has no finite forward head-space frustum; very-wide native-camera views may be clipped", i);
            }
        }
    }
    s.fovUnion.angleLeft = std::min(s.eyeFovHead[0].angleLeft, s.eyeFovHead[1].angleLeft);
    s.fovUnion.angleRight = std::max(s.eyeFovHead[0].angleRight, s.eyeFovHead[1].angleRight);
    s.fovUnion.angleUp = std::max(s.eyeFovHead[0].angleUp, s.eyeFovHead[1].angleUp);
    s.fovUnion.angleDown = std::min(s.eyeFovHead[0].angleDown, s.eyeFovHead[1].angleDown);
    const bool densityChanged = s.expandedCoverage && (!s.eyeFovValid ||
        std::memcmp(&s.eyeFov[0], &views[0].fov, sizeof(XrFovf)) ||
        std::memcmp(&s.eyeFov[1], &views[1].fov, sizeof(XrFovf)));
    s.eyeFov[0] = views[0].fov;
    s.eyeFov[1] = views[1].fov;
    if (s.expandedCoverage && densityChanged)
        frustum::PixelDensity(s.eyeFov, s.recEyeW, s.recEyeH, s.pixelDensity, &s.panelDensity);
    for (int i = 0; i < 2; ++i) {
        s.eyeViewFov[i] = views[i].fov;
        s.eyeViewPose[i] = views[i].pose;

    }
    s.eyeFovValid = true;
    // Per-eye projection: the eye's own FOV plus half a degree each side for pixel
    // rounding; the image goes to that eye only (cropped to its exact FOV on submit).
    for (int i = 0; i < 2; ++i) {
        const float pad = 0.0087f;
        s.eyeRenderFov[i] = {s.eyeFovHead[i].angleLeft - pad, s.eyeFovHead[i].angleRight + pad,
                             s.eyeFovHead[i].angleUp + pad, s.eyeFovHead[i].angleDown - pad};
    }

    const XrQuaternionf& q = head.pose.orientation;
    const cbmath::Vec3 right = ToGame(Rotate(q, {1, 0, 0}));
    const cbmath::Vec3 up = ToGame(Rotate(q, {0, 1, 0}));
    const cbmath::Vec3 fwd = ToGame(Rotate(q, {0, 0, -1}));
    if (s.recenterPending || g_recenterRequest.exchange(false)) {
        s.headYaw0 = YawOf(fwd);
        s.headPos0 = head.pose.position;
        s.recenterPending = false;
        s.bodyInit = false;
        s.anchorValid = false;
        LOG("OpenXR: recentred (head yaw %.1f deg, at %.2f %.2f %.2f m)", s.headYaw0 * 57.29578f, s.headPos0.x, s.headPos0.y,
            s.headPos0.z);
    }
    const float headRel = AngleDiff(YawOf(fwd), s.headYaw0);

    // The game camera follows the head (camctl) so the engine culls, streams and
    // lights for what the headset sees. "Body yaw" is the game-input part of the
    // heading: when the game changes the yaw we wrote (Q/E, mouse), that change is
    // added to the body, so normal camera turning keeps working.
    float bodyYaw = YawOf(game.basis.forward);  // fallback: game camera does not follow
    bool shownDriven = false;                   // this frame shows the head-driven camera
    if (s.cameraControl) {
        const float drawnPos[3] = {game.pos.x, game.pos.y, game.pos.z};
        gamestate::Update(drawnPos);
        camctl::Update(game.pos, game.basis.forward, std::atan(game.fov.tanUp) - std::atan(game.fov.tanDown));
        float memYaw = 0, memPitch = 0;
        if (camctl::Read(memYaw, memPitch)) {
            const float headPitch = -std::asin(std::fmax(-1.f, std::fmin(1.f, fwd.y)));
            if (!s.bodyInit) {
                s.bodyYaw = AngleDiff(memYaw, headRel);
                s.bodyPitch = memPitch;
                // A game camera at or above level orbits Q/E around a point on the
                // horizon; start it at least kMinBodyPitch below level.
                s.bodyPitch = std::max(s.bodyPitch, kMinBodyPitch);
                s.bodyInit = true;
                s.injecting = true;
                s.lastWrittenValid = false;
                LOG("camctl: following the head (body yaw %.1f deg)", s.bodyYaw * 57.29578f);
            }
            // Measured (Tools/camsample.py, mod passive): Q/E give the controller an
            // angular velocity that keeps turning the yaw for ~2 s, and while it moves
            // the game places the camera on an orbit around the point it looks at.
            // Writing a head yaw during that motion swung the camera hundreds of metres
            // and crashed the game. At rest, a written yaw/pitch turns the camera in
            // place. So the head is written only while the game is not moving the
            // camera itself; while it is, the game drives and the body follows it.
            // Only yaw counts as the game taking over. Measured (FakePad soak with
            // logging): while the left stick pans near hills, the game raises the pitch
            // on its own (up to ~0.88 rad) without any pitch input; in VR the head owns
            // pitch, so those changes are simply overwritten each frame.
            const bool gameMoved = s.lastWrittenValid && std::fabs(AngleDiff(memYaw, s.lastWrittenYaw)) > 1e-4f;  // above float noise
            if (s.injecting && gameMoved) {
                s.injecting = false;
                s.pausedOffset = AngleDiff(s.lastWrittenYaw, s.bodyYaw);  // head yaw contained in the engine yaw
                s.pausedPitchOffset = s.lastWrittenPitch - s.bodyPitch;
                s.stillFrames = 0;
                s.lastSeenYaw = memYaw;
                s.lastSeenPitch = memPitch;
            }
            if (!s.injecting) {
                s.bodyYaw = AngleDiff(memYaw, s.pausedOffset);
                s.bodyPitch = std::max(memPitch - s.pausedPitchOffset, kMinBodyPitch);  // game pitch input moves the body pitch
                // Yaw only: the game keeps nudging pitch by itself (auto-tilt near hills),
                // which kept this from ever resuming in testing.
                const bool still = std::fabs(AngleDiff(memYaw, s.lastSeenYaw)) < 1e-5f;
                s.stillFrames = still ? s.stillFrames + 1 : 0;
                s.lastSeenYaw = memYaw;
                s.lastSeenPitch = memPitch;
                s.lastWrittenValid = false;
                if (s.stillFrames >= kStillFramesToResume) s.injecting = true;
            }
            // Is this frame rendered from the controller? Pause screens draw a frozen
            // camera, cutscenes their own; then the image is not the head's view.
            UpdateDriven(game.basis.forward, memYaw, memPitch);
            shownDriven = s.driven;
            // VR turning on the right stick (the game never sees that stick; see
            // input_hook). Changing the body yaw at rest turns the camera in place. While
            // the game shows another camera, turning could not show and the stick is the
            // game's (menus scroll with it).
            input::SetConsumeRightStick(s.driven);
            // Turning and tilt advance by the time between displayed frames (the
            // runtime's predicted display times), so each frame's step matches what is
            // shown. GetTickCount64 moves in ~15.6 ms steps (measured), so at 60-90 fps
            // the per-frame step alternated between none and double: choppy stick turns.
            int64_t nowNs = s.displayTime;
            if (!nowNs) {
                LARGE_INTEGER qc, qf;
                QueryPerformanceCounter(&qc);
                QueryPerformanceFrequency(&qf);
                nowNs = static_cast<int64_t>(double(qc.QuadPart) * 1e9 / double(qf.QuadPart));
            }
            const float dt = s.lastTurnTime && nowNs > s.lastTurnTime
                                 ? std::min(0.1f, static_cast<float>(double(nowNs - s.lastTurnTime) * 1e-9))
                                 : 0.f;
            s.lastTurnTime = nowNs;
            if (s.zoomDepth) {
                // Eye spacing from what is actually in view, measured from the game's depth
                // buffer each frame: the centre of the view appears modelDistance away
                // (like a model on a table), and nothing visible ever appears closer than
                // nearestComfort. Basing it on the camera's zoom distance alone gave metres
                // of eye spacing with the camera low and a ride tens of metres away, which
                // the eyes could not fuse (reported in the headset 2026-10-03). The zoom
                // distance is only the fallback while no depth measurement is available.
                float d = 0.f;
                if (camctl::OrbitDistance(d)) s.orbitDistance = d;
                float nearest = 0.f, center = 0.f;
                const bool measured = depth::NearestDistance(nearest, center);
                if (measured) {
                    // The measurement comes from the second eye's view, which itself moves
                    // with the eye spacing, so a single frame can feed back into the next.
                    // Steadied over the last 12 frames: the nearest of them for the near
                    // limit (cautious), the median for the view centre.
                    s.nearRing[s.ringPos] = nearest;
                    s.centerRing[s.ringPos] = center;
                    s.ringPos = (s.ringPos + 1) % State::kDepthRing;
                    s.ringCount = std::min(s.ringCount + 1, State::kDepthRing);
                    nearest = *std::min_element(s.nearRing, s.nearRing + s.ringCount);
                    float c[State::kDepthRing];
                    int nc = 0;
                    for (int i = 0; i < s.ringCount; ++i)
                        if (s.centerRing[i] > 0.f) c[nc++] = s.centerRing[i];
                    if (nc) {
                        std::nth_element(c, c + nc / 2, c + nc);
                        s.centerDistance = c[nc / 2];  // looking at sky: keeps the last
                    }
                    s.nearestDistance = nearest;
                }
                const float typical = measured ? s.centerDistance : s.orbitDistance;
                if (typical > 0.f) {
                    float target = std::min(std::max(typical / s.modelDistance, 1.f), kMaxZoomScale);
                    if (measured) target = std::max(1.f, std::min(target, nearest / s.nearestComfort));
                    // Ratio space: shrinking quickly (~0.15 s), growing slowly (~0.6 s).
                    const float k = 1.f - std::exp(-dt / (target < s.zoomScale ? 0.15f : 0.6f));
                    s.zoomScale = std::exp(std::log(s.zoomScale) + (std::log(target) - std::log(s.zoomScale)) * k);

                }
            }
            const controls::Settings stick = controls::Current();
            const float axis = s.driven ? input::TurnAxis() * (stick.invertXOrbit ? -1.f : 1.f) : 0.f;
            if (s.snapTurnDeg > 0.f) {
                if (s.snapArmed && std::fabs(axis) > 0.7f) {
                    s.bodyYaw = AngleDiff(s.bodyYaw + (axis > 0 ? 1.f : -1.f) * s.snapTurnDeg * 0.0174533f, 0.f);
                    s.snapArmed = false;
                } else if (std::fabs(axis) < 0.3f) {
                    s.snapArmed = true;
                }
            } else {
                s.bodyYaw = AngleDiff(s.bodyYaw + axis * s.smoothTurnDegPerSec * stick.gain * 0.0174533f * dt, 0.f);
            }
            // Right stick up/down: a tilt the mod owns, added to the head view (the
            // game never sees the stick). Native default: pushing up looks up.
            if (s.driven)
                s.tilt = std::fmax(-1.4f, std::fmin(1.4f, s.tilt + input::TiltAxis() *
                    (stick.invertYOrbit ? 1.f : -1.f) * s.tiltDegPerSec * stick.gain * 0.0174533f * dt));
            const cbmath::Vec3 bodyRight = Yaw({1.f, 0.f, 0.f}, s.bodyYaw);
            const cbmath::Vec3 headWorldFwd = Yaw(fwd, AngleDiff(s.bodyYaw, s.headYaw0));
            cbmath::Vec3 viewFwd = RotateAxis(headWorldFwd, bodyRight, s.tilt);
            float viewPitch = -std::asin(std::fmax(-1.f, std::fmin(1.f, viewFwd.y)));
            // Keep head + tilt clear of straight up/down (yaw is undefined there): give
            // back the excess tilt and recompute, rather than clamping only the pitch.
            for (int iter = 0; iter < 4 && std::fabs(viewPitch) > kMaxViewPitch; ++iter) {
                s.tilt -= (viewPitch > 0 ? 1.f : -1.f) * (std::fabs(viewPitch) - kMaxViewPitch + 0.01f);
                viewFwd = RotateAxis(headWorldFwd, bodyRight, s.tilt);
                viewPitch = -std::asin(std::fmax(-1.f, std::fmin(1.f, viewFwd.y)));
            }
            viewPitch = std::fmax(-kMaxViewPitch, std::fmin(kMaxViewPitch, viewPitch));
            const float viewYaw = AngleDiff(s.bodyYaw + headRel, 0.f);
            float engineYawOffset = 0.f, enginePitch = memPitch;
            if (s.injecting) {
                s.lastWrittenYaw = std::fabs(viewFwd.x) + std::fabs(viewFwd.z) > 1e-3f ? YawOf(viewFwd) : viewYaw;
                // Pitch: never above the game's own ("body") pitch. Looking down, the game
                // camera follows the head, so terrain and shadows are prepared for what
                // you see. Looking up it stays at the body pitch: Q/E orbit around the
                // point the camera looks at, and a level or upward pitch put that point
                // at the horizon (turns then swung the camera hundreds of metres, measured
                // with FakeHead). The upward difference is covered by the engine FOV.
                s.lastWrittenPitch = s.engineView ? viewPitch
                                     : s.pitchMode == 1 ? std::max(headPitch, s.bodyPitch) : memPitch;
                s.lastWrittenValid = camctl::Write(s.lastWrittenYaw, s.lastWrittenPitch);
                if (s.lastWrittenValid) PushPair(s.lastWrittenYaw, s.lastWrittenPitch);
                enginePitch = s.lastWrittenPitch;
            } else {
                engineYawOffset = std::fabs(AngleDiff(headRel, s.pausedOffset));  // head vs engine while paused
            }
            // Engine FOV wide enough to contain the headset view (plus how far the head
            // is from the engine camera) and a margin for the frame of lag.
            const float halfV = std::fabs(viewPitch - enginePitch) +
                                std::max(std::fabs(s.fovUnion.angleUp), std::fabs(s.fovUnion.angleDown));
            const float halfH = engineYawOffset + std::max(std::fabs(s.fovUnion.angleLeft), std::fabs(s.fovUnion.angleRight));
            const float aspect = game.fov.tanRight / std::max(1e-3f, game.fov.tanUp);  // render target aspect
            const float needV = 2.f * std::max(halfV, std::atan(std::tan(std::min(halfH, 1.5f)) / std::max(0.1f, aspect)));
            // Per-eye projection: the engine's own (symmetric) frustum is only used for
            // culling and streaming and is never shown, so it just has to contain both
            // eyes' frusta; the game's window now has one eye's aspect, so it is wider
            // than the earlier displayed-pinhole limit. Keep this experimental
            // culling ceiling independent of the native displayed-camera path.
            const float engineFov = (s.perEyeProjection && s.stereo) ? std::min(needV + 0.035f, 2.62f)
                                                                     : std::min(needV + s.fovMargin, s.engineFovMax);
            // Expanded EngineView controls FOV below in the submitted camera axes.
            if (s.engineFovControl && (!s.expandedCoverage || !s.engineView || s.perEyeProjection)) camctl::SetEngineFov(engineFov);
            bodyYaw = s.bodyYaw;
        } else {
            s.bodyInit = false;
            input::SetConsumeRightStick(false);
            ResetDriven();
        }
    } else {
        ResetDriven();
    }
    s.shownDriven = shownDriven;
    bool parkDepth = shownDriven;
    const float yaw = AngleDiff(bodyYaw, s.headYaw0);
    // On-ride camera (ridecam): the game keeps its look direction relative to the seat;
    // the head drives it every frame, as it drives the park camera, and its FOV is
    // widened to the headset's. Then the frame is the head's view from the seat.
    // Only the Guest (first-person) mode: Cinematic and Interest cameras are the game's own
    // shots and are shown as a fixed screen (game_state.h has the mode).
    bool ride = false;
    if (s.engineView && !shownDriven && s.cameraControl && gamestate::RideCamera() == gamestate::Ride::Guest) {
        const float camPos[3] = {game.pos.x, game.pos.y, game.pos.z};
        ridecam::Update(std::atan(game.fov.tanUp) - std::atan(game.fov.tanDown), camPos);
        ride = ridecam::Located();
    } else {
        ridecam::Release(gamestate::InPark());
    }
    if (!ride) s.rideInit = false;
    if (ride) {
        const cbmath::Basis& rb = game.basis;  // the rendered camera (seat x look)
        int64_t nowNs = s.displayTime;
        if (!nowNs) {
            LARGE_INTEGER qc, qf;
            QueryPerformanceCounter(&qc);
            QueryPerformanceFrequency(&qf);
            nowNs = static_cast<int64_t>(double(qc.QuadPart) * 1e9 / double(qf.QuadPart));
        }
        if (!s.rideInit) {
            s.rideInit = true;
            s.rideYaw = 0.f;  // the seat faces where you face
            s.rideTilt = 0.f;
            s.rideCount = s.ridePos = 0;
            s.rideSeatValid = false;
            s.rideLastTime = nowNs;
            if (++s.undrivenLogs <= 100) LOG("view: on-ride camera; the head drives its look direction");
        }
        const float dt = nowNs > s.rideLastTime ? std::min(0.1f, static_cast<float>(double(nowNs - s.rideLastTime) * 1e-9)) : 0.f;
        s.rideLastTime = nowNs;
        // Right stick: turn and tilt in the seat, as in the park.
        input::SetConsumeRightStick(true);
        const controls::Settings stick = controls::Current();
        const float axis = input::TurnAxis() * (stick.invertXFirstPerson ? -1.f : 1.f);
        if (s.snapTurnDeg > 0.f) {
            if (s.snapArmed && std::fabs(axis) > 0.7f) {
                s.rideYaw = AngleDiff(s.rideYaw + (axis > 0 ? 1.f : -1.f) * s.snapTurnDeg * 0.0174533f, 0.f);
                s.snapArmed = false;
            } else if (std::fabs(axis) < 0.3f) {
                s.snapArmed = true;
            }
        } else {
            s.rideYaw = AngleDiff(s.rideYaw + axis * s.smoothTurnDegPerSec * stick.gain * 0.0174533f * dt, 0.f);
        }
        s.rideTilt = std::fmax(-1.4f, std::fmin(1.4f, s.rideTilt + input::TiltAxis() *
            (stick.invertYFirstPerson ? 1.f : -1.f) * s.tiltDegPerSec * stick.gain * 0.0174533f * dt));
        const float headPitch = -std::asin(std::fmax(-1.f, std::fmin(1.f, fwd.y)));
        const float lookYaw = AngleDiff(headRel + s.rideYaw, 0.f);
        const float lookPitch = std::fmax(-kMaxViewPitch, std::fmin(kMaxViewPitch, headPitch + s.rideTilt));
        // FOV covering the headset view (as for the park camera's engine FOV).
        float halfV = std::max(std::fabs(s.fovUnion.angleUp), std::fabs(s.fovUnion.angleDown));
        float halfH = std::max(std::fabs(s.fovUnion.angleLeft), std::fabs(s.fovUnion.angleRight));
        // As in the park, the ride's native camera has a level horizon. The
        // submitted tracking orientation below removes head roll; include that
        // same rotation when sizing its FOV, while preserving seat/stick controls.
        if (s.expandedCoverage) {
            const cbmath::Vec3 trackingForward = Rotate(q, {0, 0, -1});
            const cbmath::Vec3 horizonRight = cbmath::Cross(trackingForward, {0, 1, 0});
            if (cbmath::Dot(horizonRight, horizonRight) > 1e-8f) {
                const cbmath::Vec3 r = cbmath::Normalize(horizonRight);
                const XrQuaternionf level = QuatFromBasis(r, cbmath::Cross(r, trackingForward), trackingForward);
                EyeHalfExtents(level, halfH, halfV);
            }
        }
        const float aspect = game.fov.tanRight / std::max(1e-3f, game.fov.tanUp);
        const float needV = 2.f * std::max(halfV, std::atan(std::tan(std::min(halfH, 1.5f)) / std::max(0.1f, aspect)));
        const float rideFov = std::min(needV + s.fovMargin, s.engineFovMax);
        if (ridecam::Write(lookYaw, lookPitch, rideFov)) {
            s.rideRing[s.ridePos] = {lookYaw, lookPitch, YawOf(fwd), headPitch};
            s.ridePos = (s.ridePos + 1) % State::kRideRing;
            s.rideCount = std::min(s.rideCount + 1, State::kRideRing);
        }
        // Which write this frame was rendered with: the seat it implies (rendered = seat x
        // look) must agree with the last frame's seat; a wrong one moves the seat by the
        // head's motion between the writes.
        int best = -1;
        float bestScore = -10.f;
        cbmath::Vec3 bestF{}, bestU{};
        for (int k = 0; k < s.rideCount; ++k) {
            const State::RideSeen& e = s.rideRing[(s.ridePos - 1 - k + 2 * State::kRideRing) % State::kRideRing];
            const float cy = std::cos(e.lookYaw), sy = std::sin(e.lookYaw), cp = std::cos(e.lookPitch), sp = std::sin(e.lookPitch);
            const cbmath::Vec3 lr{cy, 0.f, -sy}, lf{sy * cp, -sp, cy * cp};
            const cbmath::Vec3 lu = cbmath::Cross(lf, lr);
            const auto apply = [&](float a, float b, float c) {
                return cbmath::Vec3{rb.right.x * a + rb.up.x * b + rb.forward.x * c, rb.right.y * a + rb.up.y * b + rb.forward.y * c,
                                    rb.right.z * a + rb.up.z * b + rb.forward.z * c};
            };
            const cbmath::Vec3 sf = apply(lr.z, lu.z, lf.z), su = apply(lr.y, lu.y, lf.y);
            const float score = s.rideSeatValid ? cbmath::Dot(sf, s.rideSeatF) + cbmath::Dot(su, s.rideSeatU) : (k == 1 ? 1.f : 0.f);
            if (score > bestScore) {
                bestScore = score;
                best = k;
                bestF = sf;
                bestU = su;
            }
        }
        float showYaw = YawOf(fwd), showPitch = headPitch;
        if (best >= 0) {
            const State::RideSeen& e = s.rideRing[(s.ridePos - 1 - best + 2 * State::kRideRing) % State::kRideRing];
            showYaw = e.headYaw;
            showPitch = e.headPitch;
            s.rideSeatF = bestF;
            s.rideSeatU = bestU;
            s.rideSeatValid = true;
        }
        // Shown with the head direction that write came from (no roll: the runtime
        // reprojects head roll, as in the park).
        const float cy = std::cos(showYaw), sy = std::sin(showYaw), cp = std::cos(showPitch), sp = std::sin(showPitch);
        const cbmath::Vec3 gf{sy * cp, -sp, cy * cp}, gr{cy, 0.f, -sy};
        const cbmath::Vec3 gu = cbmath::Cross(gf, gr);
        const auto flip = [](const cbmath::Vec3& v) { return cbmath::Vec3{v.x, v.y, -v.z}; };
        s.submitPose.orientation = QuatFromBasis(flip(gr), flip(gu), flip(gf));
        s.submitPose.position = head.pose.position;
        s.submitFov = {std::atan(game.fov.tanLeft), std::atan(game.fov.tanRight), std::atan(game.fov.tanUp),
                       std::atan(game.fov.tanDown)};
        const cbmath::Vec3 axR = Rotate(q, {1, 0, 0}), axU = Rotate(q, {0, 1, 0}), axF = Rotate(q, {0, 0, -1});
        for (int i = 0; i < 2; ++i) {
            const cbmath::Vec3 d = {views[i].pose.position.x - head.pose.position.x, views[i].pose.position.y - head.pose.position.y,
                     views[i].pose.position.z - head.pose.position.z};
            const float l[3] = {cbmath::Dot(d, axR), cbmath::Dot(d, axU), cbmath::Dot(d, axF)};
            memcpy(s.eyeLocal[i], l, sizeof(l));
            // On the rendered camera's axes (the head's view from the seat).
            s.eyeWorld[i][0] = l[0] * rb.right.x + l[1] * rb.up.x + l[2] * rb.forward.x;
            s.eyeWorld[i][1] = l[0] * rb.right.y + l[1] * rb.up.y + l[2] * rb.forward.y;
            s.eyeWorld[i][2] = l[0] * rb.right.z + l[1] * rb.up.z + l[2] * rb.forward.z;
            s.eyeSubmitPose[i].orientation = s.submitPose.orientation;
            s.eyeSubmitPose[i].position = {head.pose.position.x + d.x, head.pose.position.y + d.y, head.pose.position.z + d.z};
        }
        shownDriven = true;
        s.shownDriven = true;
        s.anchorValid = false;
    } else if (s.engineView && !shownDriven) {
        // The frame shows a camera the mod does not drive (menus, the pause screen,
        // cutscenes, the park before its camera is found): a screen into the game at a
        // fixed orientation in front of you. Following that camera's own orientation
        // (as for the head's view) swung the image around as cutscene cameras moved,
        // let the stick turn it, and with the stick tilt applied could turn a shot upside
        // down (reported in the headset 2026-10-03).
        const cbmath::Vec3 headF = Rotate(q, {0, 0, -1});
        // The paused park: the camera drawn is still where the head last put it.
        const bool frozen = s.lastMatchValid && std::fabs(AngleDiff(YawOf(game.basis.forward), s.lastMatch.yaw)) < kPairTolerance &&
                            std::fabs(-std::asin(std::fmax(-1.f, std::fmin(1.f, game.basis.forward.y))) - s.lastMatch.pitch) < kPairTolerance;
        // Kept where the park was only while the paused park is drawn. Measured in the
        // headset log (2026-10-04, Restart Scenario): the pause screen's anchor (the park
        // camera's pitch, looking down) stayed for the loading screen and the scenario's
        // opening cutscene, which then showed tilted.
        if (s.anchorValid && s.anchorKept && !frozen) s.anchorValid = false;
        if (!s.anchorValid) {
            bool kept = false;
            if (frozen) {
                // Where the last head-driven frame put this camera: a pause screen's
                // frozen park stays exactly where it was. Only when that is in front of
                // you and upright; a cutscene's own first shot can face anywhere.
                const float yawM = AngleDiff(s.lastMatch.body, s.headYaw0);
                const cbmath::Vec3 rightM = Yaw({1.f, 0.f, 0.f}, s.lastMatch.body);
                const auto toXrM = [&](const cbmath::Vec3& v) {
                    const cbmath::Vec3 w = Yaw(RotateAxis(v, rightM, -s.lastMatch.tilt), -yawM);
                    return cbmath::Vec3{w.x, w.y, -w.z};
                };
                const cbmath::Vec3 f = cbmath::Normalize(toXrM(game.basis.forward)), u = toXrM(game.basis.up);
                if (cbmath::Dot(f, headF) > std::cos(35.f * 0.0174533f) && u.y > 0.6f) {
                    s.anchor = QuatFromBasis(toXrM(game.basis.right), u, toXrM(game.basis.forward));
                    kept = true;
                }
            }
            if (!kept) {  // level, in the direction you face
                const float hy = std::fabs(headF.x) + std::fabs(headF.z) > 1e-3f ? std::atan2(headF.x, -headF.z) : s.headYaw0;
                s.anchor = {0.f, std::sin(-hy * 0.5f), 0.f, std::cos(-hy * 0.5f)};
            }
            s.anchorValid = true;
            s.anchorKept = kept;
            if (++s.undrivenLogs <= 100)
                LOG("view: showing a camera the mod does not drive (%s); fixed in front, %s", s.cameraControl && camctl::IsLocated() ? "menu, pause or cutscene" : "camera not located",
                    kept ? "where the park was" : "level where you face");
        }
        // Eye spacing: the park's depth-linked spacing only for the park itself, frozen
        // behind a pause screen (the camera drawn where the head last put it). Any other
        // camera (cutscenes, the ride camera's Cinematic and Interest shots, menus) is
        // the game's own shot at real size: the park's spacing, up to x13.5 in the headset
        // log, was being applied to close-up shots too.
        parkDepth = frozen;
        if (parkDepth != s.parkDepth && ++s.undrivenLogs <= 100)
            LOG("view: fixed-screen eye spacing %s", parkDepth ? "follows the park (paused park view)" : "real size (the game's own shot)");
        s.submitPose.orientation = s.anchor;
        s.submitPose.position = head.pose.position;
        s.submitFov = {std::atan(game.fov.tanLeft), std::atan(game.fov.tanRight), std::atan(game.fov.tanUp),
                       std::atan(game.fov.tanDown)};
        // Stereo: the eyes sit on that camera's own axes (as yours sit on your head's).
        const cbmath::Vec3 axR = Rotate(q, {1, 0, 0}), axU = Rotate(q, {0, 1, 0}), axF = Rotate(q, {0, 0, -1});
        for (int i = 0; i < 2; ++i) {
            const cbmath::Vec3 d = {views[i].pose.position.x - head.pose.position.x, views[i].pose.position.y - head.pose.position.y,
                     views[i].pose.position.z - head.pose.position.z};
            const float l[3] = {cbmath::Dot(d, axR), cbmath::Dot(d, axU), cbmath::Dot(d, axF)};
            memcpy(s.eyeLocal[i], l, sizeof(l));
            const cbmath::Basis& b = game.basis;
            s.eyeWorld[i][0] = l[0] * b.right.x + l[1] * b.up.x + l[2] * b.forward.x;
            s.eyeWorld[i][1] = l[0] * b.right.y + l[1] * b.up.y + l[2] * b.forward.y;
            s.eyeWorld[i][2] = l[0] * b.right.z + l[1] * b.up.z + l[2] * b.forward.z;
            s.eyeSubmitPose[i].orientation = s.anchor;
            s.eyeSubmitPose[i].position = {head.pose.position.x + d.x, head.pose.position.y + d.y, head.pose.position.z + d.z};
        }
    } else if (s.engineView) {
        // Show exactly what the engine rendered: its camera orientation for this
        // frame (from the frame's own, unmodified view buffer) and its symmetric FOV.
        // The runtime reprojects that image to the real head pose, which covers the
        // frame or two the engine camera lags behind the head. Mapping game space back
        // to OpenXR space inverts the one used for the head: undo the body yaw offset,
        // then flip z (left-handed game, right-handed OpenXR).
        const cbmath::Vec3 bodyRightNow = Yaw({1.f, 0.f, 0.f}, bodyYaw);
        const auto toXr = [&](const cbmath::Vec3& v) {
            const cbmath::Vec3 w = Yaw(RotateAxis(v, bodyRightNow, -s.tilt), -yaw);
            return cbmath::Vec3{w.x, w.y, -w.z};
        };
        s.submitPose.orientation = QuatFromBasis(toXr(game.basis.right), toXr(game.basis.up), toXr(game.basis.forward));
        s.submitPose.position = head.pose.position;
        s.submitFov = {std::atan(game.fov.tanLeft), std::atan(game.fov.tanRight), std::atan(game.fov.tanUp),
                       std::atan(game.fov.tanDown)};
        // Stereo: each eye's offset from the head in the game world. The tracking-space
        // offset (the real eye positions, so head roll is included) is mapped into the
        // game the same way as the head direction: flip z, add the body yaw, add the
        // stick tilt (the inverse of toXr). The offsets used to go on the engine camera's
        // axes, which have no roll, so the eyes stayed level whatever the head roll; with
        // zoom-linked depth spreading them metres apart, a tilted head saw the two eyes
        // looking in different directions (reported in the headset 2026-10-03).
        const cbmath::Vec3 axR = Rotate(q, {1, 0, 0}), axU = Rotate(q, {0, 1, 0}), axF = Rotate(q, {0, 0, -1});
        for (int i = 0; i < 2; ++i) {
            const cbmath::Vec3 d = {views[i].pose.position.x - head.pose.position.x, views[i].pose.position.y - head.pose.position.y,
                     views[i].pose.position.z - head.pose.position.z};
            const float l[3] = {cbmath::Dot(d, axR), cbmath::Dot(d, axU), cbmath::Dot(d, axF)};
            memcpy(s.eyeLocal[i], l, sizeof(l));
            const cbmath::Vec3 w = RotateAxis(Yaw({d.x, d.y, -d.z}, yaw), bodyRightNow, s.tilt);
            s.eyeWorld[i][0] = w.x;
            s.eyeWorld[i][1] = w.y;
            s.eyeWorld[i][2] = w.z;
            // Shown from where the eye really is.
            s.eyeSubmitPose[i].orientation = s.submitPose.orientation;
            s.eyeSubmitPose[i].position = {head.pose.position.x + d.x, head.pose.position.y + d.y, head.pose.position.z + d.z};
        }

    } else {
        s.submitPose = head.pose;
        s.submitFov = s.fovUnion;
    }
    if (s.expandedCoverage && s.engineView && shownDriven && !ride && !s.perEyeProjection && s.engineFovControl) {
        // The rendered camera has no tracking roll. Head-space extents alone do
        // not cover a rolled physical eye view. Prepare the next native camera
        // frame from full eye rays in this rendered camera's axes, without touching
        // the game's projection/history buffers or its lighting path.
        float halfH = 0.f, halfV = 0.f;
        if (EyeHalfExtents(s.submitPose.orientation, halfH, halfV)) {
            const float aspect = game.fov.tanRight / std::max(1e-3f, game.fov.tanUp);
            const float needV = 2.f * std::max(halfV, std::atan(std::tan(halfH) / std::max(0.1f, aspect)));
            camctl::SetEngineFov(std::min(needV + s.fovMargin, s.engineFovMax));
        } else {
            // A rear-plane crossing cannot be covered, but keep the largest
            // allowed finite native image instead of leaving the game's narrow FOV.
            camctl::SetEngineFov(s.engineFovMax);
        }
    }
    s.renderView.basis = {Yaw(right, yaw), Yaw(up, yaw), Yaw(fwd, yaw)};
    s.renderView.fov = {std::tan(s.fovUnion.angleLeft), std::tan(s.fovUnion.angleRight), std::tan(s.fovUnion.angleUp),
                        std::tan(s.fovUnion.angleDown)};
    s.parkDepth = parkDepth;
    s.poseAcquired = true;
    out = s.renderView;
    return true;
}

void RequestRecenter() { g_recenterRequest.store(true); }

void SetCameraControl(bool on) { s.cameraControl = on; }

void SetPitchMode(int mode) { s.pitchMode = mode; }
void SetEngineView(bool on) { s.engineView = on; }
void SetTiltSpeed(float degPerSec) { s.tiltDegPerSec = degPerSec; }

void SetTurning(float snapDeg, float smoothDegPerSec) {
    s.snapTurnDeg = snapDeg;
    s.smoothTurnDegPerSec = smoothDegPerSec;
}
bool EngineView() { return s.engineView; }

void SetEngineFov(bool on, float maxRadians) {
    s.engineFovControl = on;
    // A malformed override must not drive tan(FOV/2) through its singularity.
    s.engineFovOverride = maxRadians;
    s.engineFovMax = frustum::CameraLimit(maxRadians,s.expandedCoverage);
}

void SetSubmitEyeFov(bool on) { s.submitEyeFov = on; }

void SetResolution(float scale, float fovMarginDeg) {
    s.renderScale = scale > 0.1f ? scale : 1.f;
    s.fovMargin = std::max(0.f, fovMarginDeg) * 0.0174533f;

}

void RequestResolutionScale(int percent) {
    if (percent>=25 && percent<=200) g_resolutionRequest.store(percent);
}
void RequestExpandedCoverage(bool on) {
    g_coverageRequest.store(on ? 2 : 1);
}
ResolutionInfo ResolutionStatus() {
    const auto size=g_resolutionSize.load(std::memory_order_relaxed);
    return {static_cast<uint32_t>(size>>32),static_cast<uint32_t>(size)};
}

bool IsRunning() { return s.running; }

// ------------------------------------------------------------------ stereo --
bool UiOnPanel() { return s.shownDriven || s.parkDepth; }

bool EyeCentersTan(float out[2][2], bool& fullRate) {
    if (!s.eyeFovValid) return false;
    // A fixed screen into the game (menus, pause, cutscenes, Cinematic and Interest ride
    // shots): the whole shot is in view at once and you look around inside it, so it is
    // shaded at full rate. Centred foveation drew everything over 25 degrees from the
    // shot's centre at 2x2 (measured in an eye dump of the main menu: 86-100% of the 2x2
    // pixel blocks uniform at the edges, 0-5% in the centre), so its edges stayed blurry
    // wherever you looked (reported in the headset 2026-10-04).
    fullRate = !s.shownDriven;
    if (fullRate) {
        memset(out, 0, sizeof(float) * 4);
        return true;
    }
    if (s.gazeValid) {  // eye tracking: both eyes' sharp area where you look (head space)
        for (int i = 0; i < 2; ++i) {
            out[i][0] = s.gazeTan[0];
            out[i][1] = s.gazeTan[1];
        }
        return true;
    }
    for (int i = 0; i < 2; ++i) {
        const XrFovf& f = s.eyeFovHead[i];
        out[i][0] = std::tan(0.5f * (f.angleLeft + f.angleRight));
        out[i][1] = std::tan(0.5f * (f.angleUp + f.angleDown));
    }
    return true;
}

void SetZoomDepth(bool on, float modelDistanceM, float nearestM) {
    s.zoomDepth = on;
    s.modelDistance = modelDistanceM > 0.1f ? modelDistanceM : 2.5f;
    s.nearestComfort = nearestM > 0.1f ? nearestM : 1.f;
    LOG("XR: zoom-linked depth %s (what you look at appears %.2f m away)", on ? "on" : "off", s.modelDistance);
}

void SetStereo(bool on, float worldScale) {
    s.stereo = on;
    s.worldScale = worldScale > 0.f ? worldScale : 1.f;
    if (on) LOG("XR: true stereo on (world scale %.2f)", s.worldScale);
}

bool StereoEnabled() { return s.stereo && s.engineView; }

bool StereoEyesForFrame(float local[2][3], float& worldScale, float tanFov[2][4], bool& perEye) {
    if (!s.stereo || !s.engineView || !s.poseAcquired) return false;
    memcpy(local, s.eyeWorld, sizeof(s.eyeWorld));
    // On a ride you sit in the park at real size: no zoom-linked depth.
    // A fixed screen shows the game's own shot at real size unless it is the paused park.
    worldScale = s.worldScale * (s.zoomDepth && s.parkDepth ? s.zoomScale : 1.f);
    perEye = s.perEyeProjection && s.eyeFovValid;
    for (int i = 0; i < 2; ++i) {
        const XrFovf& f = s.eyeRenderFov[i];
        tanFov[i][0] = std::tan(f.angleLeft);
        tanFov[i][1] = std::tan(f.angleRight);
        tanFov[i][2] = std::tan(f.angleUp);
        tanFov[i][3] = std::tan(f.angleDown);
        s.eyeRenderFovFrame[i] = f;
    }
    return true;
}

void SetPerEyeProjection(bool on) { s.perEyeProjection = on; }

namespace {

bool InitCapture(ID3D12CommandQueue* queue, const D3D12_RESOURCE_DESC& bb) {
    if (!s.captureDevice && FAILED(queue->GetDevice(IID_PPV_ARGS(&s.captureDevice)))) return false;
    ID3D12Device* dev = s.captureDevice.Get();
    if (!s.copyFence) {
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.copyFence)))) return false;
        s.copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (auto& slot : s.copyAlloc)
            for (auto& a : slot)
                if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
        for (int e = 0; e < 2; ++e) {
            if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.copyAlloc[0][e].Get(), nullptr,
                                              IID_PPV_ARGS(&s.copyList[e]))))
                return false;
            s.copyList[e]->Close();
        }
    }
    if (s.eyeTex[0] && bb.Width == s.eyeW && bb.Height == s.eyeH && bb.Format == s.eyeFormat) return true;
    // New size or format: no blit may still read the old textures.
    if (s.fence && s.lastBlitValue && s.fence->GetCompletedValue() < s.lastBlitValue) {
        s.fence->SetEventOnCompletion(s.lastBlitValue, s.copyEvent);
        WaitForSingleObject(s.copyEvent, 1000);
    }
    D3D12_RESOURCE_DESC d = bb;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    d.MipLevels = 1;
    d.DepthOrArraySize = 1;
    d.SampleDesc = {1, 0};
    d.Alignment = 0;
    const D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    for (auto& t : s.eyeTex) {
        t.Reset();
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&t))))
            return false;
    }
    s.eyeW = static_cast<uint32_t>(bb.Width);
    s.eyeH = bb.Height;
    s.eyeFormat = bb.Format;
    LOG("XR stereo: eye images %ux%u format %d", s.eyeW, s.eyeH, s.eyeFormat);
    return true;
}
}  // namespace

bool CaptureEye(int eye, ID3D12CommandQueue* queue, ID3D12Resource* image) {
    if (!image || eye < 0 || eye > 1) return false;
    // The image the batch rendered (COMMON at the batch's end; a back buffer would be
    // PRESENT, the same value).
    ID3D12Resource* bb = image;
    if (!InitCapture(queue, bb->GetDesc())) return false;
    if (eye == 0) {
        s.copyRing = (s.copyRing + 1) % State::kRing;
        // GPU-side: the headset blit of an earlier frame may still be reading eyeTex.
        if (s.fence && s.lastBlitValue) queue->Wait(s.fence.Get(), s.lastBlitValue);
    }
    // Recycle this slot's allocator (used kRing frames ago; normally long finished).
    const uint64_t slotValue = s.copySlotValue[s.copyRing][eye];
    for (int tries = 0; s.copyFence->GetCompletedValue() < slotValue && tries < 20; ++tries) {
        s.copyFence->SetEventOnCompletion(slotValue, s.copyEvent);
        WaitForSingleObject(s.copyEvent, 50);
    }
    if (s.copyFence->GetCompletedValue() < slotValue) return false;
    ID3D12GraphicsCommandList* list = s.copyList[eye].Get();
    s.copyAlloc[s.copyRing][eye]->Reset();
    list->Reset(s.copyAlloc[s.copyRing][eye].Get(), nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyResource(s.eyeTex[eye].Get(), bb);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    s.copySlotValue[s.copyRing][eye] = ++s.copyValue;
    queue->Signal(s.copyFence.Get(), s.copyValue);
    return true;
}

namespace {

}  // namespace

void PanelPixels(float widthDeg, uint32_t srcW, uint32_t srcH, uint32_t& w, uint32_t& h) {
    w = h = 0;
    // copyFence: created with the first eye capture; the panel is signalled on it.
    if (!s.recW || !s.eyeFovValid || !srcW || !srcH || !s.copyFence) return;
    // The headset's pixel density at the centre of view (pixels per unit of tan, as for
    // the render size), across the panel's width, with 25% to spare for the runtime's
    // own resampling of the layer; never more than the UI has.
    float dens = s.panelDensity;
    if (!s.expandedCoverage) {
        const XrFovf& e = s.eyeFov[0];
        const float spanX = std::tan(e.angleRight) - std::tan(e.angleLeft);
        if (!(spanX > 0.1f)) return;
        dens = s.recW / spanX;
    }
    if (!(dens > 0.f)) return;
    float fw = 1.25f * dens * 2.f * std::tan(0.5f * widthDeg * 0.0174533f);
    fw = std::min({fw, static_cast<float>(srcW), static_cast<float>(s.maxSwapW)});
    float fh = fw * srcH / srcW;
    if (fh > s.maxSwapH) {
        fw *= s.maxSwapH / fh;
        fh = static_cast<float>(s.maxSwapH);
    }
    w = std::max(64u, static_cast<uint32_t>(std::lround(fw)));
    h = std::max(64u, static_cast<uint32_t>(std::lround(fh)));
}

void WaitPanelFree(ID3D12CommandQueue* queue) {
    if (s.fence && s.lastBlitValue) queue->Wait(s.fence.Get(), s.lastBlitValue);
}

void PanelCaptured(ID3D12CommandQueue* queue, ID3D12Resource* image, uint32_t w, uint32_t h) {
    if (!s.copyFence) return;  // PanelPixels gave no size without it
    s.panelValue = ++s.copyValue;
    queue->Signal(s.copyFence.Get(), s.panelValue);
    s.panelImage = image;
    s.panelW = w;
    s.panelH = h;
    s.panelCaptured = true;
}

void EyesCaptured(ID3D12CommandQueue*) {
    s.eyesValue = s.copyValue;
    s.eyesCaptured = true;

}

// ---- the game's finished frame image ----
// Measured (StereoRecon=2, park, 2560x1440): the last list of the render batch
// composites into a 2560x1440 R10G10B10A2 texture (barrier COMMON -> RENDER_TARGET,
// draw, -> COMMON), and a list on a second direct queue then copies it into the back
// buffer. There are two such textures, used in alternate frames. So the back buffer
// still holds an older frame while the batch runs, and "the last copy source" is the
// other texture. The image this batch writes is identified exactly instead: a
// candidate (a copy source into the back buffer, or a back buffer itself) that one
// of the batch's lists transitions to RENDER_TARGET.
namespace {
constexpr int kMaxCandidates = 8;
std::atomic<ID3D12Resource*> g_backBuffers[4] = {};
std::atomic<ID3D12Resource*> g_frameImages[kMaxCandidates] = {};  // fast membership test
std::mutex g_frameImageMutex;
ComPtr<ID3D12Resource> g_frameImageRefs[kMaxCandidates];  // keep them valid while referenced
std::unordered_map<ID3D12GraphicsCommandList*, ID3D12Resource*> g_listWrites;  // list -> candidate it renders to

bool IsCandidate(ID3D12Resource* r) {
    for (auto& b : g_backBuffers)
        if (b.load(std::memory_order_relaxed) == r) return true;
    for (auto& f : g_frameImages)
        if (f.load(std::memory_order_relaxed) == r) return true;
    return false;
}
}  // namespace

void OnGameCopy(ID3D12Resource* dst, ID3D12Resource* src) {
    if (!dst || !src) return;
    bool toBackBuffer = false;
    for (auto& b : g_backBuffers)
        if (b.load(std::memory_order_relaxed) == dst) toBackBuffer = true;
    if (!toBackBuffer) return;
    for (auto& f : g_frameImages)
        if (f.load(std::memory_order_relaxed) == src) return;
    std::lock_guard<std::mutex> lock(g_frameImageMutex);
    for (int i = 0; i < kMaxCandidates; ++i) {
        if (g_frameImages[i].load() == src) return;
        if (!g_frameImages[i].load()) {
            g_frameImageRefs[i] = src;
            g_frameImages[i].store(src);
            const D3D12_RESOURCE_DESC d = src->GetDesc();
            LOG("XR stereo: frame image #%d %p (%llux%u format %d): the game copies it into the back buffer", i, src,
                static_cast<unsigned long long>(d.Width), d.Height, d.Format);
            return;
        }
    }
}

void OnGameBarrier(ID3D12GraphicsCommandList* list, unsigned count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (!s.stereo || !barriers) return;
    for (unsigned i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
            b.Transition.StateAfter != D3D12_RESOURCE_STATE_RENDER_TARGET || !IsCandidate(b.Transition.pResource))
            continue;
        std::lock_guard<std::mutex> lock(g_frameImageMutex);
        g_listWrites[list] = b.Transition.pResource;
    }
}

void OnListReset(ID3D12GraphicsCommandList* list) {
    if (!s.stereo) return;
    std::lock_guard<std::mutex> lock(g_frameImageMutex);
    g_listWrites.erase(list);
}

ID3D12Resource* BatchFrameImage(uint32_t count, ID3D12CommandList* const* lists, int* index) {
    std::lock_guard<std::mutex> lock(g_frameImageMutex);
    ID3D12Resource* found = nullptr;
    if (index) *index = -1;
    for (uint32_t i = 0; i < count; ++i) {
        auto it = g_listWrites.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (it != g_listWrites.end()) {  // the last one written wins
            found = it->second;
            if (index) *index = static_cast<int>(i);
        }
    }
    return found;
}

void OnResizeBuffers() {
    for (auto& b : g_backBuffers) b.store(nullptr);
    s.gameSwapRaw = nullptr;  // re-read the buffers at the next Present
    std::lock_guard<std::mutex> lock(g_frameImageMutex);
    for (int i = 0; i < kMaxCandidates; ++i) {
        g_frameImages[i].store(nullptr);
        g_frameImageRefs[i].Reset();
    }
    g_listWrites.clear();
}

namespace {
void RefreshBackBuffers() {
    if (!s.gameSwap) return;
    DXGI_SWAP_CHAIN_DESC1 d{};
    s.gameSwap->GetDesc1(&d);
    for (UINT i = 0; i < 4; ++i) {
        ID3D12Resource* r = nullptr;
        if (i < d.BufferCount && SUCCEEDED(s.gameSwap->GetBuffer(i, IID_PPV_ARGS(&r)))) {
            g_backBuffers[i].store(r);  // identification only; the swap chain keeps it alive
            r->Release();
        } else {
            g_backBuffers[i].store(nullptr);
        }
    }
}
}  // namespace

}  // namespace pc2vr::xr
