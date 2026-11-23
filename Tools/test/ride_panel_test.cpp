#include "../../Loader/src/ride_panel_geometry.h"
#include "../../Loader/src/ride_panel_state.h"
#include "../../Loader/src/ride_click_broker.h"
#include "../../Loader/src/laser_controls.h"
#include "../../Loader/src/build_scroll.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <vector>

using namespace pc2vr;
void Check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL %s\n",text);std::exit(1);}}
bool Near(float a,float b){return std::fabs(a-b)<1e-4f;}
bool Same(XrVector3f a,XrVector3f b){return Near(a.x,b.x)&&Near(a.y,b.y)&&Near(a.z,b.z);}
XrPosef AimAt(XrVector3f origin,XrVector3f target){
    auto d=pointer::Sub(target,origin);const float n=std::sqrt(pointer::Dot(d,d));d={d.x/n,d.y/n,d.z/n};
    XrQuaternionf q{d.y,-d.x,0,1-d.z};const float length=std::sqrt(q.x*q.x+q.y*q.y+q.w*q.w);
    q={q.x/length,q.y/length,0,q.w/length};return {q,origin};
}
struct Response {unsigned finished{};bool cancelled{};ui::RideClick click{};};
std::mutex finishMutex;
std::condition_variable finishCondition;
bool holdFinish{},entered{},releaseFinish{};
void Complete(void* p,const ui::RideClick* click){
    {
        std::unique_lock<std::mutex> lock(finishMutex);
        if(holdFinish){entered=true;finishCondition.notify_all();finishCondition.wait(lock,[]{return releaseFinish;});}
    }
    auto& response=*static_cast<Response*>(p);++response.finished;response.cancelled=!click;if(click)response.click=*click;
}
int main(){
    {
        menu::detail::RidePanelState layout;layout.NewOwner(9);
        const std::string prefix="owner=9&seq=1&on=1&x=100&y=200&w=90000&h=50000&vw=100000&vh=100000&pl=0&pt=0&pr=0&pb=0&rx=100&ry=200&rw=30000";
        Check(layout.Update(prefix+"&cx=32000&cy=200&cw=58000&ch=50000"),"measured scroll region accepted");
        Check(!layout.Update(prefix+"&cx=32000&cy=200&cw=58000"),"partial scroll region rejected");
        Check(!layout.Update(prefix+"&cx=32000&cy=200&cw=80000&ch=50000"),"scroll bounds cannot escape catalog surface");
        const auto rect=layout.Read();ui::BuildScroll scroll;
        Check(!scroll.Read(rect,true,100,100,-1,100).consume,"wheel itself retains native right stick");
        auto input=scroll.Read(rect,true,400,100,-1,110);
        Check(input.consume&&input.step==1,"laser-focused catalog scrolls down immediately");
        Check(!scroll.Read(rect,true,400,100,-1,200).step,"initial repeat delay is bounded");
        Check(scroll.Read(rect,true,400,100,-1,410).step==1,"held stick repeats after delay");
        Check(!scroll.Read(rect,true,400,100,-1,500).step,"repeat rate limited");
        Check(scroll.Read(rect,true,400,100,1,510).step==-1,"direction reversal immediate");
        Check(scroll.Read(rect,true,400,100,0,520).consume,"neutral focused stick prevents camera drift");
        Check(!scroll.Read(rect,false,400,100,-1,530).consume,"lost hit returns stick to camera");
        Check(scroll.Read(rect,true,400,100,-1,540).step==1,"reenter starts fresh without queued repeats");
        Check(!scroll.Read(rect,true,900,100,-1,550).consume,"right boundary excluded");
    }
    menu::detail::RidePanelState state;state.NewOwner(7);
    const char* layout="owner=7&seq=1&on=1&x=75000&y=1200&w=19400&h=42000&vw=94400&vh=94400";
    Check(state.Update(layout),"native layout accepted");const auto first=state.Read();
    Check(first.visible && first.generation==1 && Near(first.x,750) && Near(first.width,194),"fixed-point layout retains CSS units");
    Check(!state.Update("owner=7&seq=2&on=1&x=75000&y=1200&w=20000&h=42000&vw=94400&vh=94400"),"reject out-of-view capture");
    Check(!state.Update(std::string(layout)+"&seq=2"),"reject duplicate fields");
    Check(!state.Update(std::string(layout)+"&extra=2"),"reject unknown fields");
    Check(!state.Update(std::string(layout)+"&pl=1200"),"reject partial paint bounds");
    Check(state.Update(std::string(layout).replace(std::string(layout).find("seq=1"),5,"seq=2")+"&pl=1416&pt=1200&pr=0&pb=1416"),"native rem paint bounds accepted");
    const auto painted=state.Read();Check(Near(painted.paintLeft,14.16f)&&Near(painted.paintTop,12),"paint stays separate from logical control bounds");
    const std::string popupLayout="owner=7&seq=2&on=1&x=73000&y=1200&w=21400&h=50800&vw=94400&vh=94400&pl=1416&pt=1200&pr=0&pb=1416&rx=75000&ry=1200&rw=19400";
    Check(state.Update(popupLayout),"owned portal union carries native root anchor");
    Check(!state.Update(popupLayout+"&rw=19400"),"duplicate root anchor rejected");
    Check(!state.Update(std::string(layout)+"&rx=75000"),"partial root anchor rejected");
    Check(state.Update("owner=7&seq=3&on=0&x=0&y=0&w=0&h=0&vw=94400&vh=94400"),"empty layout acknowledged");
    Check(state.Update(layout) && !state.Read().visible,"late layout cannot reopen old panel");
    state.NewOwner(8);Check(state.Update(layout)&&!state.Read().visible,"old document cannot publish into new owner");
    ui::RidePanelGeometry geometry;motion::Frame frame{};
    frame.hand[0].active=frame.hand[0].aimValid=frame.hand[1].active=frame.hand[1].aimValid=true;
    frame.hand[0].aim={{0,0,0,1},{-.2f,1.3f,-.35f}};
    frame.hand[1].aim={{0,0,0,1},{.22f,1.25f,-.35f}};
    XrPosef head{{0,0,0,1},{0,1.6f,0}};
    {
        const std::string root="owner=9&seq=2&on=1&x=100&y=200&w=90000&h=50000&vw=100000&vh=100000&pl=0&pt=0&pr=0&pb=0&rx=100&ry=200&rw=30000&cx=0&cy=0&cw=0&ch=0";
        menu::detail::RidePanelState measured;measured.NewOwner(9);
        Check(measured.Update(root+"&rh=42000") && Near(measured.Read().rootHeight,420),"native root height retained separately from portal union");
        Check(!measured.Update(root+"&rh=50001"),"root height cannot escape measured source capture");
        auto r=first;r.rootX=r.x;r.rootY=r.y;r.rootWidth=r.width;r.rootHeight=r.height;
        auto f=frame;f.hand[0].controllerPoseValid=true;f.hand[0].controllerPose={{0,0,0,1},{-.31f,1.1f,-.42f}};
        ui::RidePanelGeometry onController;
        auto p=onController.Read(r,f,head,true,.18f,false,true,true);
        Check(p.visible && Near(p.width,.24f*.75f) && Near(p.height,.24f*.75f*420/194) && Same(p.pose.position,f.hand[0].controllerPose.position),"ride panel is exactly 25 percent smaller and centered on grip rather than aim/offset");
        f.hand[1].aim=AimAt(f.hand[1].aim.position,p.pose.position);p=onController.Read(r,f,head,true,.18f,false,true,true);
        Check(p.hit.valid && Near(p.hit.u,.5f) && Near(p.hit.v,.5f),"controller-centered panel hit and drawing share the exact center");
        const auto point=[](const ui::RideGeometry& p,float x,float y){const auto d=ui::RotatePanelVector(p.pose.orientation,{(x-p.layout.x-p.layout.width*.5f)*p.width/p.layout.width,(p.layout.y+p.layout.height*.5f-y)*p.height/p.layout.height,0});return XrVector3f{p.pose.position.x+d.x,p.pose.position.y+d.y,p.pose.position.z+d.z};};
        const auto control=point(p,832,44);r.x-=20;r.width+=20;r.y-=5;r.height+=100;
        auto popup=onController.Read(r,f,head,true,.18f,false,true,true);Check(Same(point(popup,832,44),control),"portal expansion cannot shift controls away from the controller anchor");
        r=first;r.rootX=r.x;r.rootY=r.y;r.rootWidth=r.width;r.rootHeight=r.height/=2;
        p=onController.Read(r,f,head,true,.18f,false,true,true);Check(Same(p.pose.position,f.hand[0].controllerPose.position),"native tab resizing keeps the panel centered on the controller");
        f.hand[0].controllerPoseValid=false;p=onController.Read(r,f,head,true,.18f,false,true,true);Check(!p.visible&&!p.hit.valid,"lost grip pose hides the panel without an estimated aim fallback");
    }
    {
        auto r=first;r.rootX=r.x;r.rootY=r.y;r.rootWidth=r.width;r.rootHeight=r.height;
        auto f=frame;f.hand[0].controllerPoseValid=true;f.hand[0].controllerPose={{0,0,0,1},{-.35f,1.15f,-.45f}};
        XrPosef tiltedHead{{0,0,std::sin(.15f),std::cos(.15f)},{.1f,1.65f,.2f}};
        ui::RidePanelGeometry card;
        auto p=card.Read(r,f,tiltedHead,true,.20f,false,false,true,true);
        const auto offset=ui::RotatePanelVector(tiltedHead.orientation,{0,.20f*420/194*.5f+.06f,0});
        const auto anchor=f.hand[0].controllerPose.position;
        const XrVector3f expected{anchor.x+offset.x,anchor.y+offset.y,anchor.z+offset.z};
        Check(p.visible && Near(p.width,.20f) && Same(p.pose.position,expected),"notification root uses measured height and head-up clearance above actual grip");
        f.hand[1].aim=AimAt(f.hand[1].aim.position,p.pose.position);
        p=card.Read(r,f,tiltedHead,true,.20f,false,false,true,true);
        Check(p.hit.valid && Near(p.hit.u,.5f) && Near(p.hit.v,.5f),"notification drawing and laser hit share one pose for translated/rotated head");
        f.hand[0].controllerPose.position.x+=.15f;
        p=card.Read(r,f,tiltedHead,true,.20f,false,false,true,true);
        Check(Near(p.pose.position.x,expected.x+.15f),"notification follows cached runtime grip without extra queries");
        f.hand[0].controllerPoseValid=false;
        p=card.Read(r,f,tiltedHead,true,.20f,false,false,true,true);
        Check(!p.visible && !p.hit.valid,"notification hides on grip tracking loss");
    }
    {
        ui::RidePanelGeometry wrist;auto movingFrame=frame;
        const auto open=wrist.Read(first,movingFrame,head,true,.24f,false,true);
        Check(open.visible&&Near(open.width,.24f)&&Near(open.height,.24f*420/194),"small ride panel retains native aspect");
        movingFrame.hand[0].aim.position.x+=.15f;movingFrame.hand[0].aim.position.y+=.1f;
        auto moved=wrist.Read(first,movingFrame,head,true,.24f,false,true);
        const auto offset=ui::RotatePanelVector(head.orientation,{0,.18f,-.18f});
        const auto centre=ui::RotatePanelVector(moved.pose.orientation,{0,(.24f-moved.height)*.5f,0});
        Check(Same(moved.pose.position,{movingFrame.hand[0].aim.position.x+offset.x+centre.x,movingFrame.hand[0].aim.position.y+offset.y+centre.y,movingFrame.hand[0].aim.position.z+offset.z+centre.z}),"ride panel follows current left controller with native header anchored to existing wrist placement");
        movingFrame.hand[1].aim=AimAt(movingFrame.hand[1].aim.position,moved.pose.position);
        movingFrame.hand[0].grip=1;moved=wrist.Read(first,movingFrame,head,true,.24f,false,true);
        Check(moved.hit.valid&&Near(moved.hit.u,.5f)&&Near(moved.hit.v,.5f)&&!moved.consumeGrip&&!moved.grabbing,"moving ride panel shares exact laser geometry without stealing left grip");
        const auto hidden=wrist.Read(first,movingFrame,head,false,.24f,false,true);
        Check(!hidden.visible&&!hidden.hit.valid,"unavailable wrist panel cannot retain a stale hit");
        auto shorter=first;shorter.height/=2;shorter.sequence++;
        const auto resizedWrist=wrist.Read(shorter,movingFrame,head,true,.24f,false,true);
        const auto delta=ui::RotatePanelVector(resizedWrist.pose.orientation,{0,(resizedWrist.height-moved.height)*.5f,0});
        Check(Same(resizedWrist.pose.position,{moved.pose.position.x-delta.x,moved.pose.position.y-delta.y,moved.pose.position.z-delta.z}),"ride tab resize retains wrist header anchor");
    }
    auto panel=geometry.Read(first,frame,head,true,.34f);
    Check(panel.visible && panel.pose.position.x<-.2f && panel.pose.position.z<-.7f,"panel opens beside left hand away from view centre");
    Check(Near(panel.height,.34f*420/194),"native panel aspect retained");
    {
        ui::RidePanelGeometry popupGeometry;auto r=first;r.rootX=r.x;r.rootY=r.y;r.rootWidth=r.width;
        const auto original=popupGeometry.Read(r,frame,head,true,.34f);
        r.x-=20;r.width+=20;r.y-=5;r.height+=100;
        const auto expanded=popupGeometry.Read(r,frame,head,true,.34f);
        const auto at=[](const ui::RideGeometry& p,float x,float y){
            const auto d=ui::RotatePanelVector(p.pose.orientation,{(x-p.layout.x-p.layout.width*.5f)*p.width/p.layout.width,(p.layout.y+p.layout.height*.5f-y)*p.height/p.layout.height,0});
            return XrVector3f{p.pose.position.x+d.x,p.pose.position.y+d.y,p.pose.position.z+d.z};
        };
        Check(Near(original.width/original.layout.width,expanded.width/expanded.layout.width) && Same(at(original,832,44),at(expanded,832,44)),"portal overflow preserves exact native control position and pixel scale");
    }
    frame.hand[1].aim=AimAt(frame.hand[1].aim.position,panel.pose.position);
    panel=geometry.Read(first,frame,head,true,.34f);
    Check(panel.hit.valid && Near(panel.hit.u,.5f)&&Near(panel.hit.v,.5f),"independent aim intersects drawn panel centre");
    auto withPaint=panel;withPaint.layout=painted;
    const auto paint=ui::PaintGeometry(withPaint,423,423);
    const float fx=ui::FilterSupport(944,423)*944;
    Check(Near(fx,2.1737589f) && Near(paint.x*944,750-14.16f-fx),"native box-filter and bilinear support included in crop");
    const auto paintHit=pointer::Quad(pointer::AimRay(frame.hand[1].aim),paint.pose,paint.width,paint.height);
    Check(paintHit.valid && Near((paint.x+paintHit.u*paint.sourceWidth)*944,first.x+first.width*.5f) &&
        Near((paint.y+paintHit.v*paint.sourceHeight)*944,first.y+first.height*.5f),"expanded paint projects the same exact native control coordinate");
    Check(paint.x>=0 && paint.y>=0 && paint.x+paint.sourceWidth<=1.00001f && paint.y+paint.sourceHeight<=1.00001f,"paint clamped to native view");
    const auto pinned=panel.pose.position;frame.hand[0].aim.position.x-=.3f;head.position.x+=.2f;
    panel=geometry.Read(first,frame,head,true,.34f);Check(Same(panel.pose.position,pinned),"head/left hand movement does not drag pinned panel");
    frame.hand[0].grip=1;panel=geometry.Read(first,frame,head,true,.34f);
    Check(panel.grabbing && panel.consumeGrip && !Same(panel.pose.position,pinned),"aim and grip grabs panel");
    panel=geometry.Read(first,frame,head,false,.34f);
    Check(!panel.visible && panel.consumeGrip,"modal/pause hides panel and quarantines held grip");
    frame.hand[0].grip=0;panel=geometry.Read(first,frame,head,true,.34f);
    Check(panel.visible&&!panel.grabbing&&!panel.consumeGrip,"release pins and returns shoulder ownership");
    const auto placed=panel.pose.position;
    const auto originalTop=ui::RotatePanelVector(panel.pose.orientation,{0,panel.height*.5f,0});
    const XrVector3f header{placed.x+originalTop.x,placed.y+originalTop.y,placed.z+originalTop.z};
    auto resized=first;resized.height/=2;resized.sequence++;
    panel=geometry.Read(resized,frame,head,true,.34f);
    const auto resizedTop=ui::RotatePanelVector(panel.pose.orientation,{0,panel.height*.5f,0});
    Check(Same({panel.pose.position.x+resizedTop.x,panel.pose.position.y+resizedTop.y,panel.pose.position.z+resizedTop.z},header)&&
        Near(panel.height,.34f*210/194),"native tab resize preserves header position and changes aspect");
    frame.hand[1].aimValid=false;panel=geometry.Read(resized,frame,head,true,.34f);Check(panel.visible&&!panel.hit.valid,"right tracking loss leaves pinned panel but no target");
    frame.hand[1].aimValid=true;frame.hand[1].aim=AimAt(frame.hand[1].aim.position,panel.pose.position);
    frame.hand[0].grip=1;panel=geometry.Read(resized,frame,head,true,.34f,false);
    Check(panel.visible&&!panel.grabbing&&!panel.consumeGrip,"laser-off native left shoulder remains available over the panel");
    frame.hand[0].grip=0;geometry.Read(resized,frame,head,true,.34f,false);
    motion::LaserControls policy;motion::SelectionPanel native{7,true,true,true,false};
    frame.hand[1].aimValid=true;policy.Read(frame,100,true,native,true);
    frame.hand[1].primary=true;auto input=policy.Read(frame,110,true,native,true);
    Check(input.uiClick&&!input.gamepad.hand[1].primary&&!input.gamepad.hand[1].secondary,"UI A emits one click without native Select/Back");
    input=policy.Read(frame,120,true,native,false);
    Check(!input.uiClick&&!input.gamepad.hand[1].primary&&!input.gamepad.hand[1].secondary,"held UI A cannot leak into world after leaving panel");
    frame.hand[1].primary=false;policy.Read(frame,130,true,native,false);
    frame.hand[1].primary=true;input=policy.Read(frame,140,true,native,false);
    Check(!input.uiClick&&input.gamepad.hand[1].secondary,"next world A retains accepted native panel handoff");
    policy={};frame.hand[1].primary=false;policy.Read(frame,200,true,native,true);native.blocked=true;
    frame.hand[1].primary=true;input=policy.Read(frame,210,true,native,true);
    Check(!input.uiClick&&input.gamepad.hand[1].primary,"global modal A remains native even with stale geometric hit");
    {
        motion::LaserControls buildPolicy;native.blocked=false;
        frame.hand[1].primary=false;buildPolicy.Read(frame,300,true,native,false,true);
        frame.hand[1].primary=true;auto buildInput=buildPolicy.Read(frame,310,true,native,false,true);
        Check(!buildInput.uiClick&&!buildInput.gamepad.hand[1].primary&&!buildInput.gamepad.hand[1].secondary,"open build UI consumes off-panel A without closing ride or placing behind it");
        frame.hand[1].primary=false;buildPolicy.Read(frame,320,true,native,true,true);
        frame.hand[1].primary=true;buildInput=buildPolicy.Read(frame,330,true,native,true,true);
        Check(buildInput.uiClick&&!buildInput.gamepad.hand[1].primary,"build A delivers a UI click without native placement");
        buildInput=buildPolicy.Read(frame,340,true,native,false,false);
        Check(!buildInput.gamepad.hand[1].primary,"held build click cannot place when catalog disappears");
        ui::RidePanelGeometry wrist;frame.hand[0].grip=1;
        auto wheel=first;wheel.width=wheel.height=wheel.rootWidth=360;wheel.rootX=wheel.x;wheel.rootY=wheel.y;
        const auto initial=wrist.Read(wheel,frame,head,true,.28f,false,true);
        frame.hand[0].aim.position.x+=.1f;
        const auto moved=wrist.Read(wheel,frame,head,true,.28f,false,true);
        Check(initial.visible&&moved.visible&&!moved.consumeGrip&&!moved.grabbing&&Near(moved.pose.position.x-initial.pose.position.x,.1f),"build surface follows cached left pose without stealing native grip");
    }
    menu::detail::RideClickBroker broker(Complete);Response a,b,c,d,e;
    Check(!broker.Wait(&a,7,100),"no held responses before lifetime guard");broker.Start();
    Check(broker.Wait(&a,7,100),"one async wait accepted");
    const ui::RideClick click{7,1,110,820,40};Check(broker.Post(click),"press delivers to waiter");
    Check(a.finished==1&&!a.cancelled&&Near(a.click.x,820),"exact native point delivered once");
    Check(broker.Post({7,2,120,830,42})&&broker.Wait(&b,7,130),"short press retained while JS re-arms");
    Check(b.finished==1&&b.click.sequence==2,"queued click identity retained");
    Check(broker.Post({7,3,140,840,44})&&broker.Wait(&c,8,150),"new owner cannot consume old click");
    Check(!c.finished,"old-owner click dropped");broker.Cancel();Check(c.finished==1&&c.cancelled,"owner close completes waiter once");
    broker.Post({7,4,160,850,46});broker.Wait(&d,7,500);Check(!d.finished,"stale press never replayed");
    broker.Wait(&e,7,510);Check(d.finished==1&&d.cancelled,"replacement drains previous pending request");
    broker.Stop();Check(e.finished==1&&e.cancelled&&!broker.Running(),"shutdown finishes pending response before native destruction");
    Response concurrent;broker.Start();broker.Wait(&concurrent,7,1000);
    {
        menu::detail::RideClickBroker pending(Complete);Response selected;pending.Start();
        Check(pending.Post({7,1,900,820,40,0,1}),"scroll may wait for frontend rearm");
        Check(pending.Post({7,1,910,820,40}),"physical selection replaces a queued scroll repeat");
        pending.Wait(&selected,7,920);Check(selected.finished==1&&!selected.click.scroll,"queued scrolling cannot discard a selection");pending.Stop();
    }
    holdFinish=true;std::thread delivery([&]{broker.Post({7,5,1010,860,48});});
    {std::unique_lock<std::mutex> lock(finishMutex);finishCondition.wait(lock,[]{return entered;});}
    std::atomic<bool> stopped{};std::thread stop([&]{broker.Stop();stopped=true;});
    while(broker.Running())std::this_thread::yield();
    Check(!stopped&&!broker.Wait(&e,7,1020),"shutdown closes admission and waits for an in-flight Finish");
    {std::lock_guard<std::mutex> lock(finishMutex);releaseFinish=true;}finishCondition.notify_all();
    delivery.join();stop.join();Check(stopped&&concurrent.finished==1,"in-flight response completes once before shutdown returns");
    std::puts("PASS native layout ownership/bounds, pinned geometry/aim/grip, UI-vs-world A and async response shutdown contracts");
}
