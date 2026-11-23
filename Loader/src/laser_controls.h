#pragma once
#include "motion_controls.h"
#include "selection_panel.h"

namespace pc2vr::motion {
struct LaserInput {
    Frame gamepad{};
    bool enabled{}, down{}, uiClick{};
};

// Render-thread policy for one synchronized controller snapshot. Capability is
// supplied by the tracked park view. A uses native Xbox selection with the same
// hand ray; no synthetic mouse transport or desktop-hover dependency.
class LaserControls {
public:
    static constexpr ULONGLONG kHoldMs = 600, kTapMs = 60, kStaleMs = 250;
    bool Enabled() const {return mode_;}
    LaserInput Read(const Frame& frame, ULONGLONG now, bool available,const SelectionPanel& panel={},bool uiHit=false,bool uiExclusive=false) {
        LaserInput out{frame};
        if(uiExclusive)phase_=SelectionPhase::Idle;
        const auto& hand=frame.hand[1];
        auto& pad=out.gamepad.hand[1];
        const bool neutral=!hand.thumb && !hand.primary;
        if (seen_ && now-last_>kStaleMs) Cancel();
        last_=now; seen_=true;
        if (!available) {
            Cancel();
            // A held laser click must not become a second native Xbox A press
            // after capability loss. RT always retains native zoom control.
            if (ready_) {
                pad.primary=false; pad.thumb=false;
                if (neutral) ready_=false;
            }
            return out;
        }
        ready_=true;
        pad.thumb=false;
        if (!hand.active || !hand.aimValid) {
            Cancel(); pad.primary=false; return out;
        }
        if (!armed_ && neutral) armed_=true;
        // After tracking/capability loss, held buttons cannot start a new chord.
        if (armed_ || thumb_) {
            if (hand.thumb) {
                if (!thumb_) { since_=now; held_=false; tapUntil_=0; }
                if (!held_ && now-since_>=kHoldMs) {
                    mode_=!mode_; held_=true; armed_=false; down_=false;
                }
            } else if (thumb_ && !held_) {
                tapUntil_=now+kTapMs;
            }
            thumb_=hand.thumb;
        }
        if (now<tapUntil_) pad.thumb=true;
        out.enabled=mode_;
        if (mode_) {
            if (armed_) down_=hand.primary;
            pad.primary=down_;
            if(phase_==SelectionPhase::Idle && armed_ && hand.primary && !primary_ && uiHit &&
               panel.known && !panel.blocked && !hand.secondary && !hand.thumb){
                uiHeld_=true;out.uiClick=true;
            }
            if(uiHeld_){pad.primary=false;if(!hand.primary)uiHeld_=false;}
            else if(!uiExclusive)PanelSelection(pad,hand,panel,now);
            if(uiExclusive)pad.primary=false;
            out.down=pad.primary;
        } else if (!armed_) {
            pad.primary=false;
        }
        if(!mode_){phase_=SelectionPhase::Idle;uiHeld_=false;}
        primary_=hand.primary;
        return out;
    }
private:
    enum class SelectionPhase {Idle,Closing,ReleaseBack,Selecting,Neutral};
    void PanelSelection(Hand& pad,const Hand& hand,const SelectionPanel& panel,ULONGLONG now) {
        if(phase_==SelectionPhase::Idle && armed_ && hand.primary && !primary_ &&
           !hand.secondary && !hand.thumb && panel.known && panel.visible && panel.canClose && !panel.blocked){
            phase_=SelectionPhase::Closing;selectionOwner_=panel.owner;selectionSince_=now;
        }
        if(phase_==SelectionPhase::Idle)return;
        // Native component acknowledgement, not a guessed panel-animation delay.
        // Losing ownership, tracking/capability, or a newly opened modal cancels
        // the queued select rather than forwarding it into a different context.
        if(hand.secondary || hand.thumb || !panel.known || panel.blocked || panel.owner!=selectionOwner_ ||
           (panel.visible && !panel.canClose) || now-selectionSince_>=1000)
            phase_=SelectionPhase::Neutral;
        pad.primary=false;
        switch(phase_){
        case SelectionPhase::Closing:
            if(panel.visible)pad.secondary=true;
            else phase_=SelectionPhase::ReleaseBack;
            break;
        case SelectionPhase::ReleaseBack:
            phase_=SelectionPhase::Selecting;pulseSince_=now;
            pad.primary=true;
            break;
        case SelectionPhase::Selecting:
            if(now-pulseSince_<kTapMs)pad.primary=true;
            else phase_=SelectionPhase::Neutral;
            break;
        case SelectionPhase::Neutral:
            if(!hand.primary)phase_=SelectionPhase::Idle;
            break;
        case SelectionPhase::Idle:break;
        }
    }
    void Cancel() { armed_=thumb_=held_=down_=primary_=uiHeld_=false; since_=tapUntil_=0;phase_=SelectionPhase::Idle; }
    bool mode_=true, ready_{}, armed_{}, thumb_{}, held_{}, down_{}, seen_{};
    ULONGLONG since_{}, tapUntil_{}, last_{};
    SelectionPhase phase_{};
    bool primary_{};
    bool uiHeld_{};
    uint64_t selectionOwner_{};
    ULONGLONG selectionSince_{},pulseSince_{};
};
} // namespace pc2vr::motion
