#pragma once
#include "ride_panel.h"
#include "motion_controls.h"
#include "pointer_geometry.h"
#include <algorithm>

namespace pc2vr::ui {
struct RideGeometry {
    XrPosef pose{{0,0,0,1},{}};
    RideLayout layout{};
    pointer::Hit hit{};
    float width{},height{};
    bool visible{},grabbing{},consumeGrip{};
};
struct RidePaint {
    XrPosef pose{};
    XrVector3f right{},up{};
    float width{},height{},x{},y{},sourceWidth{},sourceHeight{};
};
inline float FilterSupport(float source,float destination){
    // ui_layer's actual box-filter tap support, source bilinear half-texel,
    // then ComposeEye's panel bilinear half-texel. Normalized source UV.
    const float taps=std::clamp(std::ceil(source/destination*.5f),1.f,4.f);
    return (1.f-.5f/taps)/destination+.5f/source;
}
inline RidePaint PaintGeometry(const RideGeometry& panel,uint32_t atlasWidth,uint32_t atlasHeight){
    const auto& r=panel.layout;RidePaint out{};
    if(!panel.visible || !atlasWidth || !atlasHeight)return out;
    const float hx=FilterSupport(r.viewWidth,static_cast<float>(atlasWidth))*r.viewWidth;
    const float hy=FilterSupport(r.viewHeight,static_cast<float>(atlasHeight))*r.viewHeight;
    const float left=std::min(r.x,r.paintLeft+hx),top=std::min(r.y,r.paintTop+hy);
    const float right=std::min(r.viewWidth-r.x-r.width,r.paintRight+hx);
    const float bottom=std::min(r.viewHeight-r.y-r.height,r.paintBottom+hy);
    const float metresPerPixel=panel.width/r.width;
    out.pose=panel.pose;
    out.right=RotatePanelVector(out.pose.orientation,{1,0,0});out.up=RotatePanelVector(out.pose.orientation,{0,1,0});
    const auto shift=RotatePanelVector(out.pose.orientation,{(right-left)*.5f*metresPerPixel,(top-bottom)*.5f*metresPerPixel,0});
    out.pose.position={out.pose.position.x+shift.x,out.pose.position.y+shift.y,out.pose.position.z+shift.z};
    out.width=(r.width+left+right)*metresPerPixel;out.height=(r.height+top+bottom)*metresPerPixel;
    out.x=(r.x-left)/r.viewWidth;out.y=(r.y-top)/r.viewHeight;
    out.sourceWidth=(r.width+left+right)/r.viewWidth;out.sourceHeight=(r.height+top+bottom)/r.viewHeight;
    return out;
}

// One local-space pose shared by input and both eyes. Opening places the panel
// beside the left hand; it stays there. Grip while pointing at it repositions it.
class RidePanelGeometry {
public:
    RideGeometry Read(const RideLayout& layout,const motion::Frame& frame,const XrPosef& head,
                      bool available,float width,bool interactive=true,bool followHand=false,bool controllerAnchor=false,bool aboveController=false) {
        RideGeometry out{};out.layout=layout;
        const auto& left=frame.hand[0];const auto& right=frame.hand[1];
        const bool held=left.active && left.grip>=.5f;
        if(!held)gripConsumed_=false;
        out.consumeGrip=gripConsumed_;
        if(!layout.visible){placed_=grab_=false;owner_=generation_=0;}
        if(owner_!=layout.owner || generation_!=layout.generation){placed_=grab_=false;owner_=layout.owner;generation_=layout.generation;}
        if(!available || !layout.visible || (controllerAnchor && (!left.active || !left.controllerPoseValid || layout.rootHeight<=0 || layout.rootWidth<=0))){grab_=false;grip_=held;return out;}
        const float rootWidth=layout.rootWidth>0?layout.rootWidth:layout.width;
        const float pixel=std::clamp(width,.18f,.6f)/rootWidth;
        out.width=pixel*layout.width;out.height=pixel*layout.height;
        if(controllerAnchor){
            pose_.position=left.controllerPose.position;
            if(aboveController){
                // Notification card design: centre its measured root half-height
                // plus6cm above grip along head-up, with no vendor pose estimate.
                const auto up=RotatePanelVector(head.orientation,{0,layout.rootHeight*pixel*.5f+.06f,0});
                pose_.position={pose_.position.x+up.x,pose_.position.y+up.y,pose_.position.z+up.z};
            }
            FaceHead(head);placed_=true;
        }
        else if((!placed_ || followHand) && left.active && left.aimValid){Place(left.aim,head,followHand?pixel*rootWidth:out.height,followHand);placed_=true;}
        if(!placed_){grip_=held;return out;}
        out.pose=controllerAnchor?pose_:Centre(out.height);out.visible=true;
        const auto shift=[&]{
            const auto offset=RotatePanelVector(out.pose.orientation,{
                layout.rootWidth>0?(layout.x+layout.width*.5f-layout.rootX-rootWidth*.5f)*pixel:0,
                controllerAnchor?(layout.rootY+layout.rootHeight*.5f-layout.y-layout.height*.5f)*pixel:
                    layout.rootWidth>0?(layout.rootY-layout.y)*pixel:0,0});
            out.pose.position={out.pose.position.x+offset.x,out.pose.position.y+offset.y,out.pose.position.z+offset.z};
        };shift();
        if(right.active && right.aimValid)out.hit=pointer::Quad(pointer::AimRay(right.aim),out.pose,out.width,out.height);
        if(interactive && held && !grip_ && out.hit.valid && left.aimValid){grab_=gripConsumed_=true;grabHeight_=out.height;}
        if(!interactive || !held || !left.aimValid)grab_=false;
        if(grab_){
            Place(left.aim,head,grabHeight_);out.pose=Centre(out.height);shift();
            out.hit=right.active && right.aimValid ? pointer::Quad(pointer::AimRay(right.aim),out.pose,out.width,out.height):pointer::Hit{};
        }
        out.grabbing=grab_;out.consumeGrip=gripConsumed_;grip_=held;return out;
    }
    void Reset(){placed_=grab_=grip_=gripConsumed_=false;owner_=generation_=0;}
private:
    void FaceHead(const XrPosef& head){
        const auto d=pointer::Sub(head.position,pose_.position);
        const float yaw=std::atan2(d.x,d.z),pitch=-std::atan2(d.y,std::sqrt(d.x*d.x+d.z*d.z));
        pose_.orientation=Multiply({0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)},
                                  {std::sin(pitch*.5f),0,0,std::cos(pitch*.5f)});
    }
    XrPosef Centre(float height)const{
        auto pose=pose_;const auto down=RotatePanelVector(pose.orientation,{0,-height*.5f,0});
        pose.position={pose.position.x+down.x,pose.position.y+down.y,pose.position.z+down.z};return pose;
    }
    void Place(const XrPosef& hand,const XrPosef& head,float height,bool wrist=false){
        // Head-relative placement axes avoid dependence on vendor aim/grip offsets.
        // The hand's actual tracked position supplies the anchor.
        const auto offset=RotatePanelVector(head.orientation,wrist?XrVector3f{0,.18f,-.18f}:XrVector3f{-.12f,.02f,-.4f});
        pose_.position={hand.position.x+offset.x,hand.position.y+offset.y,hand.position.z+offset.z};
        FaceHead(head);
        const auto up=RotatePanelVector(pose_.orientation,{0,height*.5f,0});
        pose_.position={pose_.position.x+up.x,pose_.position.y+up.y,pose_.position.z+up.z};
    }
    XrPosef pose_{{0,0,0,1},{}};
    uint64_t owner_{},generation_{};
    float grabHeight_{};
    bool placed_{},grab_{},grip_{},gripConsumed_{};
};
}
