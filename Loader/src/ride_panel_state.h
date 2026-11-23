#pragma once
#include "ride_panel.h"
#include <array>
#include <charconv>
#include <mutex>
#include <string_view>

namespace pc2vr::menu::detail {
class RidePanelState {
public:
    void NewOwner(uint64_t owner) {
        std::lock_guard<std::mutex> lock(mutex_);state_={};state_.owner=owner;
    }
    bool Update(std::string_view query) {
        constexpr std::array<std::string_view,21> names={"owner","seq","on","x","y","w","h","vw","vh","pl","pt","pr","pb","rx","ry","rw","cx","cy","cw","ch","rh"};
        std::array<uint64_t,21> values{};unsigned seen{};
        while(!query.empty()){
            const auto end=query.find('&');const auto part=query.substr(0,end);const auto eq=part.find('=');
            if(eq==std::string_view::npos)return false;
            const auto key=part.substr(0,eq),value=part.substr(eq+1);bool recognized=key=="r";
            for(size_t i=0;i<names.size();++i)if(key==names[i]){
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),values[i]);
                if((seen&(1u<<i)) || parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())return false;
                seen|=1u<<i;recognized=true;break;
            }
            if(!recognized)return false;
            if(end==std::string_view::npos)break;query.remove_prefix(end+1);
        }
        if((seen!=511 && seen!=8191 && seen!=65535 && seen!=1048575 && seen!=2097151) || !values[0] || !values[1] || values[2]>1)return false;
        for(size_t i=3;i<values.size();++i)if(values[i]>3276800)return false;
        const auto x=values[3],y=values[4],w=values[5],h=values[6],vw=values[7],vh=values[8];
        if(values[2] && (!w || !h || !vw || !vh || x+w>vw || y+h>vh))return false;
        if(values[2] && (values[9]>x || values[10]>y || x+w+values[11]>vw || y+h+values[12]>vh))return false;
        if(seen>=65535 && values[2] && (!values[15] || values[13]<x || values[14]<y || values[13]+values[15]>x+w || values[14]>=y+h))return false;
        if(seen>=1048575 && values[2] && (values[18] || values[19]) &&
           (!values[18] || !values[19] || values[16]<x || values[17]<y || values[16]+values[18]>x+w || values[17]+values[19]>y+h))return false;
        if(seen==2097151 && values[2] && (!values[20] || values[14]+values[20]>y+h))return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if(values[0]!=state_.owner || values[1]<=state_.sequence)return true;
        const bool visible=values[2]!=0;
        if(visible && !state_.visible)++state_.generation;
        state_={values[0],values[1],state_.generation,static_cast<float>(x)*.01f,static_cast<float>(y)*.01f,
            static_cast<float>(w)*.01f,static_cast<float>(h)*.01f,static_cast<float>(vw)*.01f,static_cast<float>(vh)*.01f,visible,
            static_cast<float>(values[9])*.01f,static_cast<float>(values[10])*.01f,static_cast<float>(values[11])*.01f,static_cast<float>(values[12])*.01f,
            static_cast<float>(seen>=65535?values[13]:x)*.01f,static_cast<float>(seen>=65535?values[14]:y)*.01f,static_cast<float>(seen>=65535?values[15]:w)*.01f,
            static_cast<float>(values[16])*.01f,static_cast<float>(values[17])*.01f,static_cast<float>(values[18])*.01f,static_cast<float>(values[19])*.01f,static_cast<float>(values[20])*.01f};
        return true;
    }
    ui::RideLayout Read() const {std::lock_guard<std::mutex> lock(mutex_);return state_;}
private:
    mutable std::mutex mutex_;
    ui::RideLayout state_{};
};
}
