#pragma once
#include <windows.h>
#include <cstdint>

namespace pc2vr::pointer::window {
// One existing game HWND. The caller supplies it from the game's swapchain;
// this component never discovers windows or changes the desktop cursor.
// Runs on the window's thread, outside our lock. True acknowledges that the
// native logical hold is neutral. False retains the last position and retries.
using ReleaseFn = bool (*)();
// Optional button-only transport on the SAME existing window timer/thread.
// Does not acquire position ownership or rewrite WM_MOUSEMOVE. Called outside
// the window lock on wake/timer/focus/resize/destruction; no native calls held.
using PumpFn = void (*)(ULONGLONG now, bool accepting);
struct Sample {
    POINT point{};
    ULONGLONG time{};
    uint64_t generation{};
    bool active{}, releasing{};
};
constexpr ULONGLONG kStaleMs = 250;
bool Attach(HWND hwnd, ReleaseFn release, PumpFn pump = nullptr);
bool Accepting();
void Wake();
// Only inside PumpFn, on the attached native window thread.
bool DeliverInput(HRAWINPUT packet);
bool Publish(POINT point, ULONGLONG sampleTime);
Sample Read();
// Invalidates presses immediately; release/position handoff runs on the window
// thread. The transport must serialize its own packet/device state.
void Clear();
// Only valid inside ReleaseFn on the attached window's thread. Dispatches an
// owned release packet through the previous procedure synchronously. The caller
// still verifies packet consumption and native neutral before acknowledging.
bool DeliverRelease(HRAWINPUT packet);
} // namespace pc2vr::pointer::window
