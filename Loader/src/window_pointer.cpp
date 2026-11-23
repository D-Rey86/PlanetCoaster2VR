#include "window_pointer.h"
#include <atomic>

namespace pc2vr::pointer::window {
namespace {
SRWLOCK stateLock = SRWLOCK_INIT, attachLock = SRWLOCK_INIT;
struct State {
    HWND hwnd{};
    ReleaseFn release{};
    PumpFn pump{};
    POINT point{};
    LONG width{}, height{};
    DWORD thread{};
    ULONGLONG time{};
    uint64_t generation{};
    UINT message{};
    UINT_PTR timer{};
    bool attempted{}, installed{}, active{}, focused{}, pending{};
    bool owned{}, cancel{}, releaseInFlight{};
    bool pumping{};
} s;
std::atomic<WNDPROC> previous{};

bool Fresh(ULONGLONG now) {
    return s.installed && s.active && s.focused && now >= s.time && now - s.time <= kStaleMs;
}
void CancelLocked() {
    if (s.active || (s.owned && !s.cancel)) ++s.generation;
    s.active = false;
    s.cancel = s.owned;
}
bool WakeLocked() {
    if (!s.hwnd || (!s.owned && !s.pump) || s.pending) return false;
    s.pending = true; return true;
}
bool PostWake(HWND hwnd, UINT message, bool post) {
    if (post && !PostMessageW(hwnd, message, reinterpret_cast<WPARAM>(&s), 0)) {
        AcquireSRWLockExclusive(&stateLock);
        if (s.hwnd == hwnd) { s.pending = false; CancelLocked(); }
        ReleaseSRWLockExclusive(&stateLock);
        // The existing window timer retries cancellation. Never release native
        // input from a publishing thread or return position ownership early.
        return false;
    }
    return true;
}
LPARAM Coordinates(POINT point) {
    return MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const DWORD error = GetLastError();
    const auto old = previous.load(std::memory_order_acquire);
    ReleaseFn release = nullptr;
    PumpFn pump = nullptr;
    ULONGLONG pumpTime{};
    bool accepting=false;
    uint64_t releaseGeneration = 0;
    bool handled = false, move = false, attemptRelease = false, destroy = false;
    POINT point{};
    UINT_PTR stopTimer = 0;
    AcquireSRWLockExclusive(&stateLock);
    if (hwnd == s.hwnd) {
        const auto now = GetTickCount64();
        if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wparam)) {
            s.focused = false; CancelLocked();
        } else if (message == WM_SETFOCUS || (message == WM_ACTIVATEAPP && wparam)) {
            s.focused = GetForegroundWindow() == hwnd;
        }
        if (s.owned && !Fresh(now)) CancelLocked();
        if (message == s.message && wparam == reinterpret_cast<WPARAM>(&s)) {
            s.pending = false; handled = true;
            if (Fresh(now)) { point = s.point; move = true; }
        } else if (message == WM_TIMER && wparam == s.timer && s.timer) {
            handled = true;
            if (s.owned && GetForegroundWindow() != hwnd) CancelLocked();
        } else if (message == WM_MOUSEMOVE && s.owned) {
            // Keep the final target through cancellation until native UP has
            // actually been consumed, including messages re-entered by release.
            lparam = Coordinates(s.point);
        } else if (message == WM_SIZE) {
            const LONG width = LOWORD(lparam), height = HIWORD(lparam);
            if (width != s.width || height != s.height) {
                s.width = width; s.height = height; CancelLocked();
            }
        } else if (message == WM_NCDESTROY) {
            CancelLocked(); destroy = true; stopTimer = s.timer;
        }
        if (s.cancel && !s.releaseInFlight) {
            s.releaseInFlight = true;
            attemptRelease = true; release = s.release;
            releaseGeneration = s.generation;
        }
        if (s.installed && s.pump && !s.pumping && (handled || message==WM_KILLFOCUS || message==WM_SETFOCUS ||
            message==WM_ACTIVATEAPP || message==WM_SIZE || destroy)) {
            pump=s.pump;pumpTime=now;s.pumping=true;
            accepting=s.focused && GetForegroundWindow()==hwnd && !destroy;
        }
    }
    ReleaseSRWLockExclusive(&stateLock);
    if (pump) {
        pump(pumpTime,accepting);
        AcquireSRWLockExclusive(&stateLock);s.pumping=false;ReleaseSRWLockExclusive(&stateLock);
    }
    if (attemptRelease) {
        const bool neutral = !release || release();
        AcquireSRWLockExclusive(&stateLock);
        s.releaseInFlight = false;
        if (neutral && s.cancel && s.generation == releaseGeneration) s.owned = s.cancel = false;
        ReleaseSRWLockExclusive(&stateLock);
    }
    if (stopTimer) KillTimer(hwnd, stopTimer);
    SetLastError(error);
    // Existing plugin/game procedures may re-enter. Never call them or the
    // release adapter while holding our lock.
    if (move) CallWindowProcW(old, hwnd, WM_MOUSEMOVE, 0, Coordinates(point));
    const LRESULT result = handled ? 0 : CallWindowProcW(old, hwnd, message, wparam, lparam);
    if (destroy) {
        AcquireSRWLockExclusive(&stateLock);
        s.hwnd = nullptr; s.timer = 0;
        s.attempted = s.installed = s.active = s.focused = s.pending = false;
        s.owned = s.cancel = s.releaseInFlight = false;
        ReleaseSRWLockExclusive(&stateLock);
    }
    return result;
}
} // namespace

bool Attach(HWND hwnd, ReleaseFn release, PumpFn pump) {
    DWORD pid = 0;
    const DWORD thread = hwnd ? GetWindowThreadProcessId(hwnd, &pid) : 0;
    if (!thread || pid != GetCurrentProcessId()) return false;
    AcquireSRWLockExclusive(&attachLock);
    AcquireSRWLockExclusive(&stateLock);
    if (s.attempted) {
        const bool same = s.installed && s.hwnd == hwnd && s.release == release && s.pump==pump;
        ReleaseSRWLockExclusive(&stateLock);
        ReleaseSRWLockExclusive(&attachLock);
        return same;
    }
    ReleaseSRWLockExclusive(&stateLock);
    RECT client{};
    const UINT message = RegisterWindowMessageW(L"PC2VR.Pointer.5c3eaf84-3292-4b76-831f-4cc513f7f8b9");
    const auto old = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!message || !old || !GetClientRect(hwnd, &client)) {
        ReleaseSRWLockExclusive(&attachLock); return false;
    }
    previous.store(old, std::memory_order_release);
    AcquireSRWLockExclusive(&stateLock);
    s.attempted = true; s.hwnd = hwnd; s.release = release; s.message = message; s.thread = thread;
    s.pump=pump;s.pumping=false;
    s.width = client.right; s.height = client.bottom;
    s.focused = GetForegroundWindow() == hwnd;
    s.active = s.pending = s.owned = s.cancel = s.releaseInFlight = false;
    ++s.generation;
    ReleaseSRWLockExclusive(&stateLock);
    SetLastError(0);
    const auto replaced = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Procedure)));
    if (!replaced) {
        AcquireSRWLockExclusive(&stateLock); s.hwnd = nullptr; ReleaseSRWLockExclusive(&stateLock);
        ReleaseSRWLockExclusive(&attachLock); return false;
    }
    previous.store(replaced, std::memory_order_release);
    const auto timer = SetTimer(hwnd, reinterpret_cast<UINT_PTR>(&s), 25, nullptr);
    if (!timer) {
        if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(Procedure))
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(replaced));
        AcquireSRWLockExclusive(&stateLock); s.hwnd = nullptr; ReleaseSRWLockExclusive(&stateLock);
        ReleaseSRWLockExclusive(&attachLock); return false;
    }
    AcquireSRWLockExclusive(&stateLock);
    s.timer = timer; s.installed = true;
    ReleaseSRWLockExclusive(&stateLock);
    ReleaseSRWLockExclusive(&attachLock);
    return true;
}

void Clear() {
    AcquireSRWLockExclusive(&stateLock);
    CancelLocked();
    const auto hwnd = s.hwnd;
    const auto message = s.message;
    const bool post = WakeLocked();
    ReleaseSRWLockExclusive(&stateLock);
    PostWake(hwnd, message, post);
}
bool Publish(POINT point, ULONGLONG sampleTime) {
    const auto now = GetTickCount64();
    const auto foreground = GetForegroundWindow();
    AcquireSRWLockExclusive(&stateLock);
    const bool valid = s.installed && !s.cancel && foreground == s.hwnd && s.focused && now >= sampleTime && now - sampleTime <= kStaleMs &&
        point.x >= 0 && point.y >= 0 && point.x < s.width && point.y < s.height && point.x <= 32767 && point.y <= 32767;
    if (valid) { s.point = point; s.time = sampleTime; s.active = s.owned = true; }
    else CancelLocked();
    const HWND hwnd = s.hwnd;
    const UINT message = s.message;
    const bool post = WakeLocked();
    ReleaseSRWLockExclusive(&stateLock);
    return PostWake(hwnd, message, post) && valid;
}
Sample Read() {
    const auto now = GetTickCount64();
    const auto foreground = GetForegroundWindow();
    AcquireSRWLockShared(&stateLock);
    const bool active = Fresh(now) && foreground == s.hwnd;
    const Sample result{s.point, s.time, s.generation, active, s.owned && !active};
    ReleaseSRWLockShared(&stateLock);
    return result;
}
bool DeliverRelease(HRAWINPUT packet) {
    const DWORD thread = GetCurrentThreadId();
    AcquireSRWLockShared(&stateLock);
    const HWND hwnd = s.hwnd;
    const bool allowed = packet && hwnd && s.releaseInFlight && thread == s.thread;
    ReleaseSRWLockShared(&stateLock);
    if (!allowed) return false;
    CallWindowProcW(previous.load(std::memory_order_acquire), hwnd, WM_INPUT, RIM_INPUT, reinterpret_cast<LPARAM>(packet));
    return true;
}
bool Accepting() {
    const auto foreground=GetForegroundWindow();
    AcquireSRWLockShared(&stateLock);
    const bool accepting=s.installed && s.hwnd && s.focused && foreground==s.hwnd;
    ReleaseSRWLockShared(&stateLock);return accepting;
}
void Wake() {
    AcquireSRWLockExclusive(&stateLock);
    const auto hwnd=s.hwnd;const auto message=s.message;const bool post=WakeLocked();
    ReleaseSRWLockExclusive(&stateLock);PostWake(hwnd,message,post);
}
bool DeliverInput(HRAWINPUT packet) {
    const auto thread=GetCurrentThreadId();
    AcquireSRWLockShared(&stateLock);
    const auto hwnd=s.hwnd;const bool allowed=packet && hwnd && s.pumping && thread==s.thread;
    ReleaseSRWLockShared(&stateLock);
    if (!allowed) return false;
    CallWindowProcW(previous.load(std::memory_order_acquire),hwnd,WM_INPUT,RIM_INPUT,reinterpret_cast<LPARAM>(packet));
    return true;
}
} // namespace pc2vr::pointer::window
