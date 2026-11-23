#pragma once
#include "ride_panel.h"
#include <condition_variable>
#include <mutex>

namespace pc2vr::menu::detail {
// Own exactly one asynchronous Gameface response. An aborted response remains
// valid until Finish (vendor contract); native System destruction drains it
// BEFORE uninitialization. No thread, timer, polling or input enumeration.
class RideClickBroker {
public:
    using FinishFn=void(*)(void*,const ui::RideClick*);
    explicit RideClickBroker(FinishFn finish):finish_(finish){}
    void Start(){std::lock_guard<std::mutex> lock(mutex_);running_=true;}
    bool Running() const {std::lock_guard<std::mutex> lock(mutex_);return running_;}
    bool Wait(void* response,uint64_t owner,uint64_t now){
        void* old=nullptr;bool deliver=false;ui::RideClick click{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!running_)return false;
            old=response_;if(old)++inFlight_;
            response_=response;owner_=owner;
            if(queued_ && queuedClick_.owner==owner && now>=queuedClick_.time && now-queuedClick_.time<=250){
                click=queuedClick_;response_=nullptr;deliver=true;++inFlight_;
            }
            queued_=false;
        }
        if(old)Finish(old,nullptr);
        if(deliver)Finish(response,&click);
        return true;
    }
    bool Post(const ui::RideClick& click){
        void* response=nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!running_)return false;
            if(response_ && owner_==click.owner){response=response_;response_=nullptr;++inFlight_;}
            else if(!queued_ || (queuedClick_.scroll && !click.scroll)){queuedClick_=click;queued_=true;}
            else return false;
        }
        if(response)Finish(response,&click);
        return true;
    }
    void Cancel(){
        void* response=nullptr;
        {std::lock_guard<std::mutex> lock(mutex_);queued_=false;response=response_;response_=nullptr;if(response)++inFlight_;}
        if(response)Finish(response,nullptr);
    }
    void Stop(){
        void* response=nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);running_=false;queued_=false;response=response_;response_=nullptr;
            drained_.wait(lock,[this]{return inFlight_==0;});
        }
        if(response)finish_(response,nullptr);
    }
private:
    void Finish(void* response,const ui::RideClick* click){
        finish_(response,click);
        std::lock_guard<std::mutex> lock(mutex_);--inFlight_;if(!inFlight_)drained_.notify_all();
    }
    FinishFn finish_;
    mutable std::mutex mutex_;
    std::condition_variable drained_;
    void* response_{};
    uint64_t owner_{};
    unsigned inFlight_{};
    ui::RideClick queuedClick_{};
    bool running_{},queued_{};
};
}
