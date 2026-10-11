// PC2VR loader: a version.dll proxy that Planet Coaster 2 loads from its own folder.
// All version.dll exports forward to the system copy (version_proxy.cpp); the mod
// starts on a worker thread so nothing heavy runs under the loader lock.

#include <windows.h>

#include <string>

#include "camera_control.h"
#include "config_redirect.h"
#include "control_settings.h"
#include "highlight_mask.h"
#include "cb_patcher.h"
#include "d3d12_hooks.h"
#include "input_hook.h"
#include "ui_layer.h"
#include "vrs.h"
#include "stereo.h"
#include "xr_runtime.h"
#include "log.h"
#include "vr_menu.h"

namespace {

std::wstring GameDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);  // the game exe
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L"\\/"));
    return dir;
}

std::wstring ModDirectory() { return GameDirectory() + L"\\PC2VR"; }

bool IsGameProcess() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"PlanetCoaster2.exe") == 0;
}

// Appends one line using only kernel32, so it is safe from DllMain.
void BootLog(const char* what) {
    const std::wstring dir = ModDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE f = CreateFileW((dir + L"\\boot.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[256];
    const int n = wsprintfA(line, "%02u:%02u:%02u.%03u pid=%lu tid=%lu %s\r\n", t.wHour, t.wMinute, t.wSecond,
                            t.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), what);
    DWORD written = 0;
    WriteFile(f, line, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
}

int ReadIniInt(const wchar_t* section, const wchar_t* key, int fallback) {
    const std::wstring ini = ModDirectory() + L"\\pc2vr.ini";
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

// True once the game has a visible top-level window of its own.
bool GameWindowExists() {
    struct Ctx {
        DWORD pid;
        bool found;
    } ctx{GetCurrentProcessId(), false};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

DWORD WINAPI InitThread(LPVOID) {
    BootLog("init thread running");
    pc2vr::log::Init(ModDirectory().c_str());
    LOG("PC2VR loader starting (build %s %s), pid %lu", __DATE__, __TIME__, GetCurrentProcessId());
    if (ReadIniInt(L"Loader", L"XR", 1) == 1) pc2vr::menu::Install(ModDirectory().c_str());
    pc2vr::cfgredirect::LogStatus();
    if (ReadIniInt(L"Loader", L"XR", 1) == 1) pc2vr::controls::Start();
    // Test only: point the OpenXR loader at another runtime (e.g. the xr-sim simulated
    // headset, with one of its headset profiles) without changing the system default.
    {
        const std::wstring ini = ModDirectory() + L"\\pc2vr.ini";
        wchar_t value[MAX_PATH] = {};
        // The OpenXR loader reads the C runtime's copy of the environment, which
        // SetEnvironmentVariable does not update; set both.
        using PutEnvFn = int(__cdecl*)(const wchar_t*, const wchar_t*);
        HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
        auto putenv = ucrt ? reinterpret_cast<PutEnvFn>(GetProcAddress(ucrt, "_wputenv_s")) : nullptr;
        auto set = [&](const wchar_t* name, const wchar_t* v) {
            SetEnvironmentVariableW(name, v);
            if (putenv) putenv(name, v);
        };
        if (GetPrivateProfileStringW(L"Experiments", L"XrRuntimeJson", L"", value, MAX_PATH, ini.c_str()) && value[0]) {
            set(L"XR_RUNTIME_JSON", value);
            LOG("test: XR_RUNTIME_JSON overridden (C runtime copy %s)", putenv ? "too" : "not reachable");
        }
        if (GetPrivateProfileStringW(L"Experiments", L"XrSimHeadset", L"", value, MAX_PATH, ini.c_str()) && value[0])
            set(L"XRSIM_HEADSET_CONFIG", value);
        if (GetPrivateProfileStringW(L"Experiments", L"XrSimDir", L"", value, MAX_PATH, ini.c_str()) && value[0])
            set(L"XRSIM_DIR", value);
    }

    // HookStage: 0 = load only, 1 = late hooks only (after the game's window appears),
    // 2 = also early resource-tracking hooks (needed to read the camera constant buffer).
    const int stage = ReadIniInt(L"Loader", L"HookStage", 2);
    LOG("HookStage=%d", stage);
    if (stage < 1) return 0;
    if (stage >= 2) LOG("early hooks %s", pc2vr::hooks::InstallEarly() ? "installed" : "NOT installed");

    // Wait for the game to create its own window and device before we create our
    // throwaway device, so we never run ahead of the game's D3D12 initialisation.
    const ULONGLONG start = GetTickCount64();
    while (!GameWindowExists() || !GetModuleHandleW(L"D3D12Core.dll")) {
        if (GetTickCount64() - start > 300000) {
            LOG("gave up waiting for the game window");
            return 0;
        }
        Sleep(250);
    }
    const int extraDelayMs = ReadIniInt(L"Loader", L"HookDelayMs", 3000);
    LOG("game window present after %llu ms; waiting %d ms more", GetTickCount64() - start, extraDelayMs);
    Sleep(static_cast<DWORD>(extraDelayMs));

    // XR=0 runs the hooks without starting OpenXR (no headset needed).
    const bool xr = ReadIniInt(L"Loader", L"XR", 1) != 0;
    LOG("XR=%d", xr ? 1 : 0);
    pc2vr::highlight::SetEnabled(xr && ReadIniInt(L"Loader", L"Stereo", 0) == 1);
    pc2vr::xr::SetEngineFov(ReadIniInt(L"Loader", L"EngineFov", 1) != 0,
                            static_cast<float>(ReadIniInt(L"Loader", L"EngineFovMaxDeg", 0)) * 3.14159265f / 180.f);
    pc2vr::xr::RequestExpandedCoverage(ReadIniInt(L"Resolution", L"ExpandedCoverage", 0) == 1);
    pc2vr::xr::SetPitchMode(ReadIniInt(L"Loader", L"CameraPitchFollowsHead", 1));
    pc2vr::xr::SetEngineView(ReadIniInt(L"Loader", L"EngineView", 1) != 0);
    pc2vr::xr::SetTurning(static_cast<float>(ReadIniInt(L"Controls", L"SnapTurnDeg", 0)),
                          static_cast<float>(ReadIniInt(L"Controls", L"SmoothTurnDegPerSec", 120)));
    pc2vr::xr::SetTiltSpeed(static_cast<float>(ReadIniInt(L"Controls", L"TiltDegPerSec", 90)));
    pc2vr::camctl::SetResetGuard(ReadIniInt(L"Experiments", L"CamResetGuard", 1) != 0);
    pc2vr::xr::SetCameraControl(ReadIniInt(L"Loader", L"CameraControl", 1) != 0);
    pc2vr::patcher::SetWriteEnabled(ReadIniInt(L"Experiments", L"PatchWrite", 1) != 0);
    pc2vr::patcher::SetUseHeadsetFov(ReadIniInt(L"Experiments", L"PatchFov", 1) != 0);
    pc2vr::xr::SetResolution(ReadIniInt(L"Resolution", L"ScalePercent", 100) / 100.f,
                             static_cast<float>(ReadIniInt(L"Resolution", L"FovMarginDeg", 1)));
    pc2vr::xr::SetPerEyeProjection(ReadIniInt(L"Resolution", L"PerEyeProjection", 0) == 1);
    pc2vr::xr::SetSubmitEyeFov(ReadIniInt(L"Resolution", L"SubmitEyeFov", 1) == 1);
    pc2vr::vrs::Configure(ReadIniInt(L"Rendering", L"Foveated", 1) == 1, static_cast<float>(ReadIniInt(L"Rendering", L"FoveatedInnerDeg", 25)),
                          static_cast<float>(ReadIniInt(L"Rendering", L"FoveatedMiddleDeg", 40)));
    pc2vr::vrs::WatchIni((ModDirectory() + L"\\pc2vr.ini").c_str());
    pc2vr::ui::WatchIni((ModDirectory() + L"\\pc2vr.ini").c_str());
    pc2vr::xr::SetZoomDepth(ReadIniInt(L"Stereo", L"ZoomDepth", 1) == 1, ReadIniInt(L"Stereo", L"ModelDistanceCm", 250) / 100.f,
                              ReadIniInt(L"Stereo", L"NearestCm", 100) / 100.f);
    pc2vr::xr::SetStereo(ReadIniInt(L"Loader", L"Stereo", 0) == 1,
                         ReadIniInt(L"Stereo", L"WorldScalePercent", 100) / 100.f);
    const bool ok = pc2vr::hooks::InstallLate(xr);
    if (xr && ReadIniInt(L"Experiments", L"NoInputHooks", 0) == 0) pc2vr::input::Install();
    LOG("hooks %s", ok ? "installed" : "NOT installed");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // The crash reporter and other tools in the folder also import version.dll.
        if (IsGameProcess()) {
            BootLog("attached");
            // Before the game reads its settings: VR uses its own (windowed) graphics profile.
            if (ReadIniInt(L"Loader", L"XR", 1) == 1)
                BootLog(pc2vr::cfgredirect::Install(ReadIniInt(L"Resolution", L"VrGraphicsProfile", 1) == 1));
            HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (IsGameProcess()) BootLog("detached");
    }
    return TRUE;
}
