#pragma once

// Private diagnostic builds only. Normal builds contain no trace state or calls.
// Observe existing action/input samples; never poll a device or issue a raycast.
#if defined(PC2VR_MOTION_INPUT_TRACE)
#include <atomic>
#include "log.h"

namespace pc2vr::motiontrace {
template<class... Args> inline void Write(const char* format,Args... args) {
    const DWORD error=GetLastError();LOG(format,args...);SetLastError(error);
}
inline std::atomic<unsigned> records{};
inline std::atomic<unsigned> clickSequence{};
inline std::atomic<ULONGLONG> clickUntil{};
inline bool Take() {
    const auto n=records.fetch_add(1,std::memory_order_relaxed);
    if(n==256)Write("motion-trace: limit reached; no further records");
    return n<256;
}
inline void Primary(unsigned hand,XrResult result,const XrActionStateBoolean& b) {
    static unsigned previous[2]={~0u,~0u};
    const unsigned key=(XR_SUCCEEDED(result)?1u:0u)|(b.isActive?2u:0u)|(b.currentState?4u:0u);
    if(previous[hand]==key && !b.changedSinceLastSync)return;
    previous[hand]=key;
    if(Take())Write("motion-trace: xr-primary t=%llu hand=%u result=%d active=%u down=%u changed=%u changeTime=%lld",
        GetTickCount64(),hand,static_cast<int>(result),b.isActive,b.currentState,b.changedSinceLastSync,b.lastChangeTime);
}
inline void Context(ULONGLONG now,bool synced,bool park,bool pause,bool eligible,bool enabled,
                    const motion::Frame& raw,const motion::Frame& mapped) {
    static unsigned previous=~0u;
    static ULONGLONG last{},maxGap{};
    if(last && now-last>maxGap)maxGap=now-last;
    last=now;
    const auto& r=raw.hand[1];const auto& m=mapped.hand[1];
    static bool primary{};
    if(r.primary && !primary){clickSequence.fetch_add(1,std::memory_order_relaxed);clickUntil.store(now+300,std::memory_order_relaxed);}
    primary=r.primary;
    const unsigned key=(synced?1u:0u)|(park?2u:0u)|(pause?4u:0u)|(r.primary?8u:0u)|
        (r.active?16u:0u)|(m.primary?32u:0u)|(enabled?64u:0u);
    if(previous==key)return;
    previous=key;
    if(Take())Write("motion-trace: policy t=%llu synced=%u park=%u pause=%u eligible=%u laser=%u rawActive=%u aim=%u rawA=%u mappedActive=%u mappedA=%u menuNav=%u maxSyncGapMs=%llu",
        now,synced,park,pause,eligible,enabled,r.active,r.aimValid,r.primary,m.active,m.primary,raw.menuNavigation,maxGap);
    maxGap=0;
}
inline void State(DWORD user,DWORD result,WORD physical,WORD merged) {
    if(user>=XUSER_MAX_COUNT)return;
    static std::atomic<uint64_t> previous[XUSER_MAX_COUNT]={{~0ull},{~0ull},{~0ull},{~0ull}};
    const uint64_t key=static_cast<uint64_t>(result)<<32 | (physical&XINPUT_GAMEPAD_A) | (static_cast<uint64_t>(merged&XINPUT_GAMEPAD_A)<<16);
    if(previous[user].exchange(key,std::memory_order_relaxed)==key)return;
    if(Take())Write("motion-trace: xinput-state t=%llu user=%lu result=%lu physicalA=%u deliveredA=%u",
        GetTickCount64(),user,result,(physical&XINPUT_GAMEPAD_A)!=0,(merged&XINPUT_GAMEPAD_A)!=0);
}
inline void Key(bool vr,const XINPUT_KEYSTROKE& key) {
    if(key.VirtualKey==VK_PAD_A && Take())Write("motion-trace: xinput-key t=%llu vr=%u user=%u flags=%u",
        GetTickCount64(),vr,key.UserIndex,key.Flags);
}
template<class Vec> inline void Query(bool redirected,uint32_t hits,ULONGLONG now,ULONGLONG frameTime,bool frameValid,
                  const Vec& origin,const Vec& direction,const Vec& expectedOrigin,const Vec& expectedDirection) {
    if(now>clickUntil.load(std::memory_order_relaxed))return;
    const auto sequence=clickSequence.load(std::memory_order_relaxed);
    static std::atomic<unsigned> seen[2]{};
    if(!sequence || seen[redirected?1:0].exchange(sequence,std::memory_order_relaxed)==sequence)return;
    if(Take())Write("motion-trace: pick t=%llu click=%u redirected=%u hits=%u frameValid=%u frameAgeMs=%llu origin=%.6f,%.6f,%.6f direction=%.8f,%.8f,%.8f expectedOrigin=%.6f,%.6f,%.6f expectedDirection=%.8f,%.8f,%.8f",
        now,sequence,redirected,hits,frameValid,now>=frameTime?now-frameTime:0,
        origin.x,origin.y,origin.z,direction.x,direction.y,direction.z,
        expectedOrigin.x,expectedOrigin.y,expectedOrigin.z,expectedDirection.x,expectedDirection.y,expectedDirection.z);
}
} // namespace pc2vr::motiontrace
#endif
