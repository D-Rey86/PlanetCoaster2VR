#include "menu_pointer.h"
#include "native_input_hook.h"
#include <algorithm>
#include <array>
#include <atomic>

namespace pc2vr::pointer::menu {
namespace {
using Move = bool (*)(int, int);
using Button = bool (*)(int);
using Wheel = bool (*)(float);
using Update = void (*)(void*);
Move move{};
Button press{}, release{};
Wheel wheel{};
native::Hook hook;
SRWLOCK lock = SRWLOCK_INIT;
std::atomic<bool> pending{};
std::atomic_flag pumping = ATOMIC_FLAG_INIT;

struct Sample { POINT point{}; bool down{}; };
struct Stream {
    HWND window{};
    ULONGLONG time{};
    uint64_t generation{};
    Sample latest{};
    std::array<Sample, 8> edges{};
    size_t count{};
    int wheel{};
} stream;
// Only the thread inside Pump (guarded by pumping) accesses delivery state.
struct Delivery {
    uint64_t generation{};
    bool hovered{}, down{};
    POINT point{};
} delivered;

void ResetLocked() {
    if (stream.window) ++stream.generation;
    stream.window = nullptr;
    stream.count = 0;
    stream.wheel = 0;
    stream.latest = {};
}

void PointAt(POINT point, bool force = false) {
    if (force || !delivered.hovered || point.x != delivered.point.x || point.y != delivered.point.y)
        move(point.x, point.y);
    delivered.point = point;
    delivered.hovered = true;
}

void Apply(Sample sample) {
    const bool changed = sample.down != delivered.down;
    PointAt(sample.point, changed);
    if (changed) {
        if (sample.down) press(0);
        else release(0);
        delivered.down = sample.down;
    }
}

void Pump(void* receiver) {
    reinterpret_cast<Update>(hook.original.load(std::memory_order_acquire))(receiver);
    if (!pending.load(std::memory_order_acquire) || pumping.test_and_set(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&lock);
    const auto now = GetTickCount64();
    if (stream.window && (now - stream.time > 250 || GetForegroundWindow() != stream.window))
        ResetLocked();
    const auto batch = stream;
    stream.count = 0;
    stream.wheel = 0;
    pending.store(batch.window != nullptr, std::memory_order_release);
    ReleaseSRWLockExclusive(&lock);

    if (delivered.generation != batch.generation) {
        // Leave the control before releasing a cancelled press so a click
        // cannot land on the old target or on a newly opened menu.
        if (delivered.hovered) move(-1, -1);
        if (delivered.down) release(0);
        delivered = {};
        delivered.generation = batch.generation;
    }
    if (batch.window) {
        for (size_t i = 0; i < batch.count; ++i) Apply(batch.edges[i]);
        Apply(batch.latest);
        if (batch.wheel) {
            PointAt(batch.latest.point, true);
            wheel(static_cast<float>(batch.wheel) * WHEEL_DELTA);
        }
    }
    pumping.clear(std::memory_order_release);
}
}

bool Prepare() {
    move = reinterpret_cast<Move>(native::Address(native::Landmark::UiMove));
    press = reinterpret_cast<Button>(native::Address(native::Landmark::UiDown));
    release = reinterpret_cast<Button>(native::Address(native::Landmark::UiUp));
    wheel = reinterpret_cast<Wheel>(native::Address(native::Landmark::UiWheel));
    return move && press && release && wheel && native::Install(native::Site::UiUpdate, reinterpret_cast<void*>(&Pump), hook);
}

void Publish(HWND window, POINT pixel, bool down, int wheelStep, ULONGLONG time) {
    AcquireSRWLockExclusive(&lock);
    if (stream.window != window) ResetLocked();
    stream.window = window;
    stream.time = time;
    if (down != stream.latest.down) {
        if (stream.count == stream.edges.size()) {
            ResetLocked();
        } else {
            stream.edges[stream.count++] = {pixel, down};
        }
    }
    stream.latest = {pixel, down};
    stream.wheel = std::clamp(stream.wheel + wheelStep, -4, 4);
    pending.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&lock);
}

void Clear() {
    AcquireSRWLockExclusive(&lock);
    ResetLocked();
    ReleaseSRWLockExclusive(&lock);
}
}
