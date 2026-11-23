#pragma once

#include <windows.h>
#include <xinput.h>
#include <openxr/openxr.h>
#include <cmath>

namespace pc2vr::motion {

// One XR action snapshot per predicted frame. No device discovery in the poll path.
struct Hand {
    bool active{}, aimValid{};
    XrPosef aim{{0, 0, 0, 1}, {0, 0, 0}};
    XrVector2f stick{};
    float trigger{}, grip{};
    bool primary{}, secondary{}, thumb{}, menu{};
    bool controllerPoseValid{};
    XrPosef controllerPose{{0,0,0,1},{}}; // runtime grip pose, distinct from laser aim
};
struct Frame { Hand hand[2]; bool menuNavigation{}; };

inline float Axis(float v) { return std::isfinite(v) ? std::fmax(-1.f, std::fmin(1.f, v)) : 0.f; }
inline BYTE Trigger(float v) { return static_cast<BYTE>(std::lround(std::fmax(0.f, Axis(v)) * 255.f)); }
inline SHORT Stick(float v) { return static_cast<SHORT>(std::lround(Axis(v) * (v < 0.f ? 32768.f : 32767.f))); }
inline XINPUT_GAMEPAD Map(const Frame& frame) {
    XINPUT_GAMEPAD p{};
    const auto& l = frame.hand[0]; const auto& r = frame.hand[1];
    if (l.active) {
        p.sThumbLX = Stick(l.stick.x); p.sThumbLY = Stick(l.stick.y);
        p.bLeftTrigger = Trigger(l.trigger);
        if (l.grip >= .5f) p.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (l.primary) p.wButtons |= XINPUT_GAMEPAD_X;
        if (l.secondary) p.wButtons |= XINPUT_GAMEPAD_Y;
        if (l.thumb) p.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (l.menu) p.wButtons |= XINPUT_GAMEPAD_START;
    }
    if (r.active) {
        p.sThumbRX = Stick(r.stick.x); p.sThumbRY = Stick(r.stick.y);
        p.bRightTrigger = Trigger(r.trigger);
        if (r.grip >= .5f) p.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (r.primary) p.wButtons |= XINPUT_GAMEPAD_A;
        if (r.secondary) p.wButtons |= XINPUT_GAMEPAD_B;
        if (r.thumb) p.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        if (r.menu) p.wButtons |= XINPUT_GAMEPAD_BACK;
    }
    return p;
}

// Classify button gestures once per XR snapshot, independently of XInput polling.
class Mapper {
public:
    static constexpr ULONGLONG kHoldMs = 600, kTapMs = 60, kDoubleClickMs = 350;
    XINPUT_GAMEPAD Read(const Frame& frame, ULONGLONG now) {
        auto p = Map(frame);
        const auto& l = frame.hand[0];
        // Native menu focus consumes D-pad events. Ignore tilt while pressing
        // the stick in a menu so the pause gesture cannot also move focus.
        if (l.active && frame.menuNavigation) {
            const auto stick = l.thumb ? XrVector2f{} : l.stick;
            navigationDirections_ = Direction(stick.x, XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_LEFT, navigationDirections_) |
                Direction(stick.y, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, navigationDirections_);
            p.sThumbLX = p.sThumbLY = 0;
            p.wButtons |= navigationDirections_;
        } else navigationDirections_=0;
        p.wButtons &= ~(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_START);
        if (!l.active) { Reset(); return p; }
        if (thumbTapArmed_ && (now - thumbReleased_ > kDoubleClickMs || l.menu)) thumbTapArmed_ = false;
        if (l.thumb && !thumb_) {
            thumbSince_ = now;
            thumbHeld_ = false;
            thumbTapUntil_ = 0;
        } else if (thumb_ && !thumbHeld_) {
            if (now - thumbSince_ >= kHoldMs) {
                thumbHeld_ = true;
                thumbTapArmed_ = false;
                thumbPauseUntil_ = now + kTapMs;
            } else if (!l.thumb) {
                // Keep the first L3 immediate; consume the second quick click
                // as Pause. A hold cancels the pair and uses its existing path.
                if (thumbTapArmed_) {
                    thumbPauseUntil_ = now + kTapMs;
                    thumbTapArmed_ = false;
                } else {
                    thumbTapUntil_ = now + kTapMs;
                    thumbReleased_ = now;
                    thumbTapArmed_ = true;
                }
            }
        }
        thumb_ = l.thumb;
        if (now < thumbTapUntil_) p.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (l.menu) {
            if (!menu_) { menuSince_ = now; menuHeld_ = false; startTapUntil_ = 0; }
            menuHeld_ |= now - menuSince_ >= kHoldMs;
            if (menuHeld_) p.wButtons |= XINPUT_GAMEPAD_BACK;
        } else if (menu_ && !menuHeld_) {
            startTapUntil_ = now + kTapMs;
        }
        menu_ = l.menu;
        if (now < startTapUntil_ || now < thumbPauseUntil_) p.wButtons |= XINPUT_GAMEPAD_START;
        return p;
    }
    void Reset() { *this = Mapper{}; }
private:
    static WORD Direction(float value, WORD positive, WORD negative, WORD held) {
        value = Axis(value);
        const float threshold = held & (value >= 0.f ? positive : negative) ? .35f : .55f;
        return value >= threshold ? positive : value <= -threshold ? negative : 0;
    }
    bool thumb_{}, thumbHeld_{}, thumbTapArmed_{}, menu_{}, menuHeld_{};
    WORD navigationDirections_{};
    ULONGLONG thumbSince_{}, thumbReleased_{}, menuSince_{}, thumbTapUntil_{}, thumbPauseUntil_{}, startTapUntil_{};
};
inline XINPUT_GAMEPAD Merge(XINPUT_GAMEPAD physical, const XINPUT_GAMEPAD& vr) {
    physical.wButtons |= vr.wButtons;
    physical.bLeftTrigger = physical.bLeftTrigger > vr.bLeftTrigger ? physical.bLeftTrigger : vr.bLeftTrigger;
    physical.bRightTrigger = physical.bRightTrigger > vr.bRightTrigger ? physical.bRightTrigger : vr.bRightTrigger;
    auto strongest = [](SHORT a, SHORT b) { return std::abs(static_cast<int>(b)) > std::abs(static_cast<int>(a)) ? b : a; };
    physical.sThumbLX = strongest(physical.sThumbLX, vr.sThumbLX);
    physical.sThumbLY = strongest(physical.sThumbLY, vr.sThumbLY);
    physical.sThumbRX = strongest(physical.sThumbRX, vr.sThumbRX);
    physical.sThumbRY = strongest(physical.sThumbRY, vr.sThumbRY);
    return physical;
}

struct Api {
    PFN_xrStringToPath stringToPath{};
    PFN_xrCreateActionSet createSet{};
    PFN_xrDestroyActionSet destroySet{};
    PFN_xrCreateAction createAction{};
    PFN_xrSuggestInteractionProfileBindings suggest{};
    PFN_xrCreateActionSpace createSpace{};
    PFN_xrDestroySpace destroySpace{};
    PFN_xrGetActionStatePose pose{};
    PFN_xrGetActionStateBoolean boolean{};
    PFN_xrGetActionStateFloat scalar{};
    PFN_xrGetActionStateVector2f vector{};
    PFN_xrLocateSpace locate{};
};

// Shared session lifecycle for gaze and controllers. OpenXR permits one attach
// per session; never synchronize one set independently and deactivate the other.
class ActionSets {
public:
    bool Attach(PFN_xrAttachSessionActionSets fn, XrSession session, XrActionSet gaze, XrActionSet controllers) {
        if (attempted_) return false;
        attempted_ = true;
        XrActionSet handles[2]{};
        if (gaze) handles[count_++] = gaze;
        if (controllers) handles[count_++] = controllers;
        if (!count_) return true;
        XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        info.countActionSets = count_; info.actionSets = handles;
        if (XR_FAILED(fn(session, &info))) { count_ = 0; return false; }
        for (uint32_t i = 0; i < count_; ++i) active_[i] = {handles[i], XR_NULL_PATH};
        return true;
    }
    bool Sync(PFN_xrSyncActions fn, XrSession session, bool focused) const {
        if (!count_ || !focused) return false;
        XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO};
        info.countActiveActionSets = count_; info.activeActionSets = active_;
        return fn(session, &info) == XR_SUCCESS; // NOT_FOCUSED is a positive result
    }
private:
    XrActiveActionSet active_[2]{};
    uint32_t count_{};
    bool attempted_{};
};

// Render thread only; the parent owns the ONE attachment and synchronization of
// both gaze and controller sets. Setup failure is isolated from gaze/rendering.
class Actions {
public:
    bool Create(const Api& api, XrInstance instance, XrSession session);
    XrActionSet Set() const { return set_; }
    Frame Read(XrSession session, XrSpace local, XrTime time, bool focused, bool controllerAnchor=false);
    void Destroy();
private:
    Api api_{};
    XrActionSet set_{};
    XrPath hands_[2]{};
    XrSpace aimSpace_[2]{};
    XrSpace controllerSpace_{};
    XrAction controllerPose_{};
    XrAction aim_{}, stick_{}, trigger_{}, grip_{}, primary_{}, secondary_{}, thumb_{}, menu_{};
    bool armed_[2]{}; // focus/reconnect requires neutral controls before reactivation
};
}  // namespace pc2vr::motion
