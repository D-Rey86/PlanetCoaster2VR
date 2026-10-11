#include "input_hook.h"

#include <windows.h>
#include <winternl.h>  // NTSTATUS, needed by hidpi.h
#include <hidusage.h>
#include <hidpi.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "inline_hook.h"
#include "log.h"

namespace pc2vr::input {
namespace {

// ---- shared state ----
std::atomic<bool> g_consume{false};
std::atomic<float> g_turn{0.f};
std::atomic<float> g_tilt{0.f};  // right stick up = +1 (looks down, like the game)
std::atomic<ULONGLONG> g_lastHidTurnMs{0};

constexpr float kDeadZone = 0.24f;

float Shape(float f) {
    if (f < -1.f) f = -1.f;
    if (f > 1.f) f = 1.f;
    const float a = std::fabs(f);
    if (a < kDeadZone) return 0.f;
    return (f > 0 ? 1.f : -1.f) * (a - kDeadZone) / (1.f - kDeadZone);
}

// ---- XInput ----
// First 16 bytes of xinput1_4!XInputGetState (Windows 11, verified 2026-10-01):
// mov [rsp+8],rbx / push rsi / push rdi / push r14 / sub rsp,30h / mov rdi,rdx.
constexpr unsigned char kXInputPrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x56, 0x57, 0x41,
                                               0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xfa};
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
GetStateFn g_xinputOriginal = nullptr;

// The right stick of every pad index: one physical pad can appear under more than one
// index (measured 2026-10-02: a virtual pad was also reported at a second index, as
// Steam Input does when it mirrors a pad), and the game reads them all, so a stick
// zeroed only at index 0 still turned the game camera. The mod turns with the most
// deflected stick, and the game sees every one centred while the mod owns them.
std::atomic<float> g_userTurn[XUSER_MAX_COUNT], g_userTilt[XUSER_MAX_COUNT];
std::atomic<ULONGLONG> g_userMs[XUSER_MAX_COUNT];

DWORD WINAPI HookGetState(DWORD user, XINPUT_STATE* state) {
    DWORD r = g_xinputOriginal(user, state);

    if (r != ERROR_SUCCESS || !state || user >= XUSER_MAX_COUNT) return r;

    g_userTurn[user].store(Shape(state->Gamepad.sThumbRX / 32767.f), std::memory_order_relaxed);
    g_userTilt[user].store(Shape(state->Gamepad.sThumbRY / 32767.f), std::memory_order_relaxed);
    g_userMs[user].store(GetTickCount64(), std::memory_order_relaxed);
    if (g_consume.load(std::memory_order_relaxed)) {
        state->Gamepad.sThumbRX = 0;
        state->Gamepad.sThumbRY = 0;
    }
    return r;
}

// The most deflected value among the pads read in the last half second (XInput), or
// the HID path's value when the pad is read that way.
float Strongest(const std::atomic<float>* perUser, const std::atomic<float>& hid) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastHidTurnMs.load(std::memory_order_relaxed) <= 500) return hid.load(std::memory_order_relaxed);
    float best = 0.f;
    for (int u = 0; u < XUSER_MAX_COUNT; ++u) {
        if (now - g_userMs[u].load(std::memory_order_relaxed) > 500) continue;
        const float v = perUser[u].load(std::memory_order_relaxed);
        if (std::fabs(v) > std::fabs(best)) best = v;
    }
    return best;
}

// ---- XInput keystrokes ----
// The game also imports XInputGetKeystroke (xinput1_4 ordinal 8). While the mod owns the
// right stick, its events (VK_PAD_RTHUMB_*, any pad index) are dropped so no game
// binding sees the stick; the next event, if any, is returned in their place.
// First 16 bytes of xinput1_4!XInputGetKeystroke (Windows 11, verified 2026-10-02):
// mov [rsp+20h],rbx / push rbp / push rdi / push r12 / mov rbp,rsp / sub rsp,60h.
constexpr unsigned char kKeystrokePrologue[16] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x57, 0x41,
                                                  0x54, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x60};
using GetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);
GetKeystrokeFn g_keystrokeOriginal = nullptr;

DWORD WINAPI HookGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE ks) {
    for (;;) {
        const DWORD r = g_keystrokeOriginal(user, reserved, ks);
        if (r != ERROR_SUCCESS || !ks || !g_consume.load(std::memory_order_relaxed) ||
            ks->VirtualKey < VK_PAD_RTHUMB_UP || ks->VirtualKey > VK_PAD_RTHUMB_DOWNLEFT)
            return r;
    }
}

// ---- raw HID (the game also reads gamepads with HidP_GetData) ----
// First 17 bytes of hid!HidP_GetData (Windows 11, verified 2026-10-02):
// mov [rsp+20h],r9 / mov [rsp+18h],r8 / mov [rsp+10h],rdx / push r12.
constexpr unsigned char kHidGetDataPrologue[17] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24,
                                                   0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x41, 0x54};
using HidGetDataFn = NTSTATUS(__stdcall*)(HIDP_REPORT_TYPE, PHIDP_DATA, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
HidGetDataFn g_hidOriginal = nullptr;

using GetValueCapsFn = NTSTATUS(__stdcall*)(HIDP_REPORT_TYPE, PHIDP_VALUE_CAPS, PUSHORT, PHIDP_PREPARSED_DATA);
using GetCapsFn = NTSTATUS(__stdcall*)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
GetValueCapsFn g_getValueCaps = nullptr;
GetCapsFn g_getCaps = nullptr;

struct AxisInfo {
    USHORT dataIndex;
    LONG logicalMin, logicalMax;
    USHORT usagePage, usage, linkCollection;  // for HidP_SetUsageValue on raw reports
};
struct Device {
    bool gamepad = false;
    bool haveRight = false;
    AxisInfo rightX{}, rightY{};
    const char* layout = "";
};
std::mutex g_devMutex;
std::unordered_map<PHIDP_PREPARSED_DATA, Device> g_devices;

// Right stick usages differ by pad: Xbox HID reports it as Rx/Ry (Z = triggers),
// PlayStation and most generic pads as Z/Rz.
Device Describe(PHIDP_PREPARSED_DATA pp) {
    Device d;
    HIDP_CAPS caps{};
    if (!g_getCaps || g_getCaps(pp, &caps) != HIDP_STATUS_SUCCESS) return d;
    d.gamepad = caps.UsagePage == HID_USAGE_PAGE_GENERIC &&
                (caps.Usage == HID_USAGE_GENERIC_GAMEPAD || caps.Usage == HID_USAGE_GENERIC_JOYSTICK);
    if (!d.gamepad) return d;
    USHORT n = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> vc(n);
    if (!n || g_getValueCaps(HidP_Input, vc.data(), &n, pp) != HIDP_STATUS_SUCCESS) return d;
    const AxisInfo* found[0x40] = {};
    AxisInfo store[0x40];
    for (USHORT i = 0; i < n; ++i) {
        if (vc[i].UsagePage != HID_USAGE_PAGE_GENERIC || vc[i].IsRange) continue;
        const USHORT u = vc[i].NotRange.Usage;
        if (u < 0x40) {
            store[u] = {vc[i].NotRange.DataIndex, vc[i].LogicalMin, vc[i].LogicalMax, vc[i].UsagePage, u, vc[i].LinkCollection};
            found[u] = &store[u];
        }
    }
    if (found[HID_USAGE_GENERIC_RX] && found[HID_USAGE_GENERIC_RY]) {
        d.rightX = *found[HID_USAGE_GENERIC_RX];
        d.rightY = *found[HID_USAGE_GENERIC_RY];
        d.haveRight = true;
        d.layout = "Rx/Ry (Xbox-style)";
    } else if (found[HID_USAGE_GENERIC_Z] && found[HID_USAGE_GENERIC_RZ]) {
        d.rightX = *found[HID_USAGE_GENERIC_Z];
        d.rightY = *found[HID_USAGE_GENERIC_RZ];
        d.haveRight = true;
        d.layout = "Z/Rz (PlayStation/generic)";
    }
    LOG("input: HID gamepad %p: usage %04x:%04x, %u value caps, right stick %s", pp, caps.UsagePage, caps.Usage, n,
        d.haveRight ? d.layout : "not identified");
    return d;
}

float Normalise(ULONG raw, const AxisInfo& a) {
    const float mid = 0.5f * (static_cast<float>(a.logicalMin) + static_cast<float>(a.logicalMax));
    const float half = 0.5f * (static_cast<float>(a.logicalMax) - static_cast<float>(a.logicalMin));
    return half > 0 ? (static_cast<float>(static_cast<LONG>(raw)) - mid) / half : 0.f;
}

ULONG Centre(const AxisInfo& a) { return static_cast<ULONG>((a.logicalMin + a.logicalMax) / 2); }

NTSTATUS __stdcall HookHidGetData(HIDP_REPORT_TYPE type, PHIDP_DATA data, PULONG length, PHIDP_PREPARSED_DATA pp,
                                  PCHAR report, ULONG reportLength) {
    const NTSTATUS st = g_hidOriginal(type, data, length, pp, report, reportLength);
    if (st != HIDP_STATUS_SUCCESS || type != HidP_Input || !data || !length) return st;
    Device dev;
    {
        std::lock_guard<std::mutex> lock(g_devMutex);
        auto it = g_devices.find(pp);
        if (it == g_devices.end()) it = g_devices.emplace(pp, Describe(pp)).first;
        dev = it->second;
    }
    if (!dev.gamepad || !dev.haveRight) return st;
    const bool consume = g_consume.load(std::memory_order_relaxed);
    for (ULONG i = 0; i < *length; ++i) {
        if (data[i].DataIndex == dev.rightX.dataIndex) {
            g_turn.store(Shape(Normalise(data[i].RawValue, dev.rightX)));
            g_lastHidTurnMs.store(GetTickCount64(), std::memory_order_relaxed);
            if (consume) data[i].RawValue = Centre(dev.rightX);
        } else if (data[i].DataIndex == dev.rightY.dataIndex) {
            g_tilt.store(Shape(-Normalise(data[i].RawValue, dev.rightY)));  // HID Y grows downward
            if (consume) data[i].RawValue = Centre(dev.rightY);
        }
    }
    return st;
}

// ---- raw input (WM_INPUT reports) ----
// The game registers for raw input (imports GetRawInputData) and receives gamepad HID
// reports that way too. While the mod owns the right stick, each gamepad report's
// right-stick values are set to centre with HidP_SetUsageValue (the report layout
// comes from the device's own descriptor), so no input path shows the game the stick.
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using SetUsageValueFn = NTSTATUS(__stdcall*)(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE, ULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
GetRawInputDataFn g_rawOriginal = nullptr;
SetUsageValueFn g_setUsageValue = nullptr;
struct RawDevice {
    std::vector<uint8_t> preparsed;
    Device dev;
};
std::unordered_map<HANDLE, RawDevice> g_rawDevices;  // by raw input device handle (g_devMutex)

UINT WINAPI HookGetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize) {
    const UINT r = g_rawOriginal(input, command, data, size, headerSize);
    if (command != RID_INPUT || !data || r == static_cast<UINT>(-1) || r < sizeof(RAWINPUTHEADER)) return r;
    RAWINPUT* ri = static_cast<RAWINPUT*>(data);
    if (ri->header.dwType != RIM_TYPEHID) return r;
    if (!g_consume.load(std::memory_order_relaxed) || !g_setUsageValue) return r;
    std::lock_guard<std::mutex> lock(g_devMutex);
    auto it = g_rawDevices.find(ri->header.hDevice);
    if (it == g_rawDevices.end()) {
        RawDevice d;
        UINT n = 0;
        if (GetRawInputDeviceInfoW(ri->header.hDevice, RIDI_PREPARSEDDATA, nullptr, &n) == 0 && n) {
            d.preparsed.resize(n);
            if (GetRawInputDeviceInfoW(ri->header.hDevice, RIDI_PREPARSEDDATA, d.preparsed.data(), &n) == n)
                d.dev = Describe(reinterpret_cast<PHIDP_PREPARSED_DATA>(d.preparsed.data()));
        }
        it = g_rawDevices.emplace(ri->header.hDevice, std::move(d)).first;
    }
    const Device& dev = it->second.dev;
    if (!dev.gamepad || !dev.haveRight) return r;
    auto pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(it->second.preparsed.data());
    const DWORD each = ri->data.hid.dwSizeHid;
    for (DWORD k = 0; k < ri->data.hid.dwCount; ++k) {
        PCHAR report = reinterpret_cast<PCHAR>(ri->data.hid.bRawData) + k * each;
        for (const AxisInfo* a : {&dev.rightX, &dev.rightY})
            g_setUsageValue(HidP_Input, a->usagePage, a->linkCollection, a->usage, Centre(*a), pp, report, each);
    }
    return r;
}

// user32!GetRawInputData is an indirect jump into win32u: its pointer slot (user32's
// own import data) is re-pointed at the hook; no code is changed.
bool HookRawInput(HMODULE user32, HMODULE hid) {
    g_setUsageValue = reinterpret_cast<SetUsageValueFn>(GetProcAddress(hid, "HidP_SetUsageValue"));
    auto* p = reinterpret_cast<uint8_t*>(GetProcAddress(user32, "GetRawInputData"));
    if (!p || !g_setUsageValue) return false;
    if (p[0] == 0xFF && p[1] == 0x25) {
        void** slot = reinterpret_cast<void**>(p + 6 + *reinterpret_cast<int32_t*>(p + 2));
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
        g_rawOriginal = reinterpret_cast<GetRawInputDataFn>(*slot);
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookGetRawInputData));
        VirtualProtect(slot, sizeof(void*), old, &old);
        LOG("hook GetRawInputData: OK (import slot)");
        return true;
    }
    if (p[0] == 0xE9) {  // already hooked by someone else (5-byte jump): chain after it
        void* orig = nullptr;
        if (inlinehook::Install(p, p, 16, reinterpret_cast<void*>(&HookGetRawInputData), &orig, "GetRawInputData")) {
            g_rawOriginal = reinterpret_cast<GetRawInputDataFn>(orig);
            return true;
        }
    }
    LOG("hook GetRawInputData: unexpected entry bytes %02x %02x; right stick may still reach the game", p[0], p[1]);
    return false;
}

HMODULE SystemModule(const wchar_t* name) {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    return LoadLibraryW((std::wstring(sysdir) + L"\\" + name).c_str());
}

}  // namespace

bool Install() {
    bool any = false;
    if (HMODULE x = SystemModule(L"xinput1_4.dll")) {
        void* orig = nullptr;
        if (inlinehook::Install(GetProcAddress(x, "XInputGetState"), kXInputPrologue, sizeof(kXInputPrologue),
                                reinterpret_cast<void*>(&HookGetState), &orig, "XInputGetState")) {
            g_xinputOriginal = reinterpret_cast<GetStateFn>(orig);
            any = true;
        }
        if (inlinehook::Install(GetProcAddress(x, "XInputGetKeystroke"), kKeystrokePrologue, sizeof(kKeystrokePrologue),
                                reinterpret_cast<void*>(&HookGetKeystroke), &orig, "XInputGetKeystroke")) {
            g_keystrokeOriginal = reinterpret_cast<GetKeystrokeFn>(orig);
            any = true;
        }
    }
    if (HMODULE hid = SystemModule(L"hid.dll")) {
        g_getValueCaps = reinterpret_cast<GetValueCapsFn>(GetProcAddress(hid, "HidP_GetValueCaps"));
        g_getCaps = reinterpret_cast<GetCapsFn>(GetProcAddress(hid, "HidP_GetCaps"));
        void* orig = nullptr;
        if (g_getValueCaps && g_getCaps &&
            inlinehook::Install(GetProcAddress(hid, "HidP_GetData"), kHidGetDataPrologue, sizeof(kHidGetDataPrologue),
                                reinterpret_cast<void*>(&HookHidGetData), &orig, "HidP_GetData")) {
            g_hidOriginal = reinterpret_cast<HidGetDataFn>(orig);
            any = true;
        }
        if (HMODULE user32 = SystemModule(L"user32.dll")) any = HookRawInput(user32, hid) || any;
    }
    return any;
}

float TurnAxis() { return Strongest(g_userTurn, g_turn); }
float TiltAxis() { return Strongest(g_userTilt, g_tilt); }

void SetConsumeRightStick(bool on) { g_consume.store(on, std::memory_order_relaxed); }

}  // namespace pc2vr::input
