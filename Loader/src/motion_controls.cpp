#include "motion_controls.h"
#include <cstring>
#include <cstdio>
#include "log.h"
#include "motion_trace.h"

namespace pc2vr::motion {
void Actions::Destroy() {
    for (auto& space : aimSpace_) { if (space) api_.destroySpace(space); space = XR_NULL_HANDLE; }
    if(controllerSpace_)api_.destroySpace(controllerSpace_);controllerSpace_=XR_NULL_HANDLE;
    if (set_) api_.destroySet(set_);
    set_ = XR_NULL_HANDLE;
    armed_[0] = armed_[1] = false;
}
bool Actions::Create(const Api& api, XrInstance instance, XrSession session) {
    api_ = api;
    auto path = [&](const char* text, XrPath& p) { return XR_SUCCEEDED(api_.stringToPath(instance, text, &p)); };
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "pc2vr_motion"); strcpy_s(si.localizedActionSetName, "PC2VR motion controls");
    if (!path("/user/hand/left", hands_[0]) || !path("/user/hand/right", hands_[1]) ||
        XR_FAILED(api_.createSet(instance, &si, &set_))) return false;
    auto action = [&](const char* name, XrActionType type, XrAction& out) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(ci.actionName, name); strcpy_s(ci.localizedActionName, name);
        ci.actionType = type; ci.countSubactionPaths = 2; ci.subactionPaths = hands_;
        return XR_SUCCEEDED(api_.createAction(set_, &ci, &out));
    };
    if (!action("aim", XR_ACTION_TYPE_POSE_INPUT, aim_) || !action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, stick_) ||
        !action("trigger", XR_ACTION_TYPE_FLOAT_INPUT, trigger_) || !action("grip", XR_ACTION_TYPE_FLOAT_INPUT, grip_) ||
        !action("primary", XR_ACTION_TYPE_BOOLEAN_INPUT, primary_) || !action("secondary", XR_ACTION_TYPE_BOOLEAN_INPUT, secondary_) ||
        !action("thumb", XR_ACTION_TYPE_BOOLEAN_INPUT, thumb_) || !action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, menu_)) {
        Destroy(); return false;
    }
    // Optional controller anchor. Its failure must not disable Xbox input/aim.
    if(!action("controller_anchor",XR_ACTION_TYPE_POSE_INPUT,controllerPose_))controllerPose_=XR_NULL_HANDLE;
    // Standard profiles only. Unsupported profiles are skipped; no headset vendor
    // or GPU assumption. System/home buttons are deliberately not bound.
    struct Profile { const char* name; const char* stick; const char* grip; int face; int menu; };
    const Profile profiles[] = {
        {"/interaction_profiles/oculus/touch_controller", "thumbstick", "squeeze/value", 1, 1},
        {"/interaction_profiles/valve/index_controller", "thumbstick", "squeeze/value", 2, 0},
        {"/interaction_profiles/microsoft/motion_controller", "thumbstick", "squeeze/click", 0, 2},
        {"/interaction_profiles/htc/vive_controller", "trackpad", "squeeze/click", 0, 2},
        {"/interaction_profiles/khr/simple_controller", nullptr, nullptr, 0, 2}
    };
    bool any = false;
    for (const auto& profile : profiles) {
        XrPath pp{}; if (!path(profile.name, pp)) continue;
        XrActionSuggestedBinding bindings[20]{}; uint32_t count = 0;
        bool valid = true;
        auto bind = [&](XrAction a, int hand, const char* component) {
            char text[128]; sprintf_s(text, "/user/hand/%s/input/%s", hand ? "right" : "left", component);
            XrPath p{}; if (!path(text, p)) { valid = false; return; }
            bindings[count++] = {a, p};
        };
        for (int h = 0; h < 2; ++h) {
            bind(aim_, h, "aim/pose");
            if(!h && controllerPose_)bind(controllerPose_,h,"grip/pose");
            bind(trigger_, h, profile.stick ? "trigger/value" : "select/click");
            if (profile.stick) {
                bind(stick_, h, profile.stick);
                char click[32]; sprintf_s(click, "%s/click", profile.stick); bind(thumb_, h, click);
            }
            if (profile.grip) bind(grip_, h, profile.grip);
            if (profile.face) {
                bind(primary_, h, profile.face == 1 && !h ? "x/click" : "a/click");
                bind(secondary_, h, profile.face == 1 && !h ? "y/click" : "b/click");
            }
            if (profile.menu == 2 || (profile.menu == 1 && !h)) bind(menu_, h, "menu/click");
        }
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = pp; sb.countSuggestedBindings = count; sb.suggestedBindings = bindings;
        if (valid && XR_SUCCEEDED(api_.suggest(instance, &sb))) any = true;
    }
    if (!any) { Destroy(); return false; }
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = aim_; ci.subactionPath = hands_[h]; ci.poseInActionSpace.orientation.w = 1.f;
        if (XR_FAILED(api_.createSpace(session, &ci, &aimSpace_[h]))) { Destroy(); return false; }
    }
    if(controllerPose_){
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action=controllerPose_;ci.subactionPath=hands_[0];ci.poseInActionSpace.orientation.w=1.f;
        if(XR_FAILED(api_.createSpace(session,&ci,&controllerSpace_)))controllerSpace_=XR_NULL_HANDLE;
    }
    LOG("motion: controller action set prepared");
#if defined(PC2VR_MOTION_INPUT_TRACE)
    LOG("motion-trace: temporary input diagnostic; controls unchanged; maximum 256 event records");
#endif
    return true;
}
Frame Actions::Read(XrSession session, XrSpace local, XrTime time, bool focused, bool controllerAnchor) {
    Frame frame{};
    if (!set_ || !focused) { armed_[0] = armed_[1] = false; return frame; }
    for (int h = 0; h < 2; ++h) {
        auto& hand = frame.hand[h];
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.subactionPath = hands_[h]; gi.action = aim_;
        XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if (XR_FAILED(api_.pose(session, &gi, &ps)) || !ps.isActive ||
            XR_FAILED(api_.locate(aimSpace_[h], local, time, &loc)) || (loc.locationFlags & required) != required) {
            armed_[h] = false; continue;
        }
        hand.active = hand.aimValid = true; hand.aim = loc.pose;
        if(!h && controllerAnchor && controllerSpace_){
            gi.action=controllerPose_;XrActionStatePose anchor{XR_TYPE_ACTION_STATE_POSE};
            XrSpaceLocation position{XR_TYPE_SPACE_LOCATION};
            if(XR_SUCCEEDED(api_.pose(session,&gi,&anchor)) && anchor.isActive &&
               XR_SUCCEEDED(api_.locate(controllerSpace_,local,time,&position)) && (position.locationFlags&required)==required){
                hand.controllerPoseValid=true;hand.controllerPose=position.pose;
            }
        }
        gi.action = stick_; XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(api_.vector(session, &gi, &v)) && v.isActive) hand.stick = v.currentState;
        auto scalar = [&](XrAction a) {
            gi.action = a; XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
            return XR_SUCCEEDED(api_.scalar(session, &gi, &f)) && f.isActive ? f.currentState : 0.f;
        };
        auto button = [&](XrAction a) {
            gi.action = a; XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
            const auto result=api_.boolean(session, &gi, &b);
#if defined(PC2VR_MOTION_INPUT_TRACE)
            if(a==primary_)motiontrace::Primary(h,result,b);
#endif
            return XR_SUCCEEDED(result) && b.isActive && b.currentState;
        };
        hand.trigger = scalar(trigger_); hand.grip = scalar(grip_);
        hand.primary = button(primary_); hand.secondary = button(secondary_);
        hand.thumb = button(thumb_); hand.menu = button(menu_);
        if (!armed_[h]) {
            armed_[h] = !hand.primary && !hand.secondary && !hand.thumb && !hand.menu &&
                std::fabs(Axis(hand.stick.x)) < .1f && std::fabs(Axis(hand.stick.y)) < .1f &&
                Trigger(hand.trigger) < 26 && Trigger(hand.grip) < 26;
            hand.active = armed_[h];
        }
    }
    return frame;
}
}  // namespace pc2vr::motion
