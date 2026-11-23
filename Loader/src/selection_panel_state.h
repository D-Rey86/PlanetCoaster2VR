#pragma once
#include "selection_panel.h"
#include <atomic>
#include <charconv>
#include <mutex>
#include <string_view>

namespace pc2vr::menu::detail {
class SelectionPanelState {
public:
    uint64_t NewOwner() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++owner_;sequence_=0;state_.store(owner_<<3,std::memory_order_release);return owner_;
    }
    bool Update(std::string_view query) {
        uint64_t owner{},sequence{},visible{},safe{};unsigned seen{};
        while(!query.empty()){
            const auto end=query.find('&'),equal=query.substr(0,end).find('=');
            const auto part=query.substr(0,end);
            if(equal==std::string_view::npos)return false;
            const auto key=part.substr(0,equal),value=part.substr(equal+1);
            uint64_t* target=nullptr;unsigned bit{};
            if(key=="owner"){target=&owner;bit=1;}else if(key=="seq"){target=&sequence;bit=2;}
            else if(key=="visible"){target=&visible;bit=4;}else if(key=="safe"){target=&safe;bit=8;}
            else if(key!="r")return false;
            if(target){
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),*target);
                if((seen&bit) || parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())return false;
                seen|=bit;
            }
            if(end==std::string_view::npos)break;query.remove_prefix(end+1);
        }
        if(seen!=15 || !owner || !sequence || visible>1 || safe>visible)return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if(owner==owner_ && sequence>sequence_){
            sequence_=sequence;state_.store((owner_<<3)|4|(safe<<1)|visible,std::memory_order_release);
        }
        return true;
    }
    motion::SelectionPanel Read() const {
        const auto bits=state_.load(std::memory_order_acquire);
        return {bits>>3,(bits&4)!=0,(bits&1)!=0,(bits&2)!=0};
    }
private:
    std::mutex mutex_;
    uint64_t owner_{},sequence_{};
    std::atomic<uint64_t> state_{};
};
}
