#pragma once
#include "ride_panel.h"
#include <cmath>

namespace pc2vr::ui {
struct BuildScrollInput { bool consume{}; int step{}; };
class StickScroll {
public:
    int Read(float axis,uint64_t owner,uint64_t now) {
        if(!std::isfinite(axis))axis=0;
        const float threshold=direction_ && (axis>0? -1:1)==direction_?.2f:.35f;
        const int direction=axis>=threshold?-1:axis<=-threshold?1:0;
        if(!direction){direction_=0;next_=0;return 0;}
        int step=0;
        if(direction!=direction_ || owner_!=owner || now<last_){step=direction;next_=now+300;}
        else if(now>=next_){step=direction;next_=now+150;}
        owner_=owner;direction_=direction;last_=now;return step;
    }
    void Reset(){owner_=next_=last_=0;direction_=0;}
private:
    uint64_t owner_{},next_{},last_{};
    int direction_{};
};
// Uses the existing panel hit and the native catalog's measured CSS bounds.
class BuildScroll {
public:
    BuildScrollInput Read(const RideLayout& layout,bool hit,float x,float y,float axis,uint64_t now) {
        if(!hit || !layout.visible || layout.scrollWidth<=0 || layout.scrollHeight<=0 ||
           x<layout.scrollX || y<layout.scrollY || x>=layout.scrollX+layout.scrollWidth || y>=layout.scrollY+layout.scrollHeight){
            Reset();return {};
        }
        return {true,scroll_.Read(axis,layout.owner,now)};
    }
    void Reset(){scroll_.Reset();}
private:
    StickScroll scroll_;
};
}
