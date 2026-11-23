// Exercise the actual native adapters with measured-layout stand-ins. No hooks,
// XR session, game, desktop input or real device is created in this process.
#include "../../Loader/src/native_mouse_sources.cpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace pc2vr::pointer::native {
bool VerifyRetail() { return false; }
bool Install(Site,void*,Hook&) { return false; }
}
namespace {
using namespace pc2vr::pointer;
using namespace pc2vr::pointer::sources;
void Check(bool value,const char* why) { if (!value) {std::fprintf(stderr,"%s\n",why);std::exit(1);} }
template<class T> void Store(uint8_t* p,T value) { std::memcpy(p,&value,sizeof(value)); }
std::vector<Event> received;
uint32_t registrationCalls{},appendCalls{};
bool sawUncommittedRemoval{};
void OriginalRegister(void*,void*) {++registrationCalls;Read();}
void OriginalAppend(void* p,const Event* events,uint32_t count) {
    auto* queue=Queue(static_cast<uint8_t*>(p));
    Check(queue->OwningThread==reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentThreadId())),"filter not serialized with native queue");
    // A native callback can re-enter source status without deadlocking SRWLOCK.
    const auto state=Read();
    if (state.removed) {Check(!OwnNeutral(),"removal acknowledged before original append");sawUncommittedRemoval=true;}
    ++appendCalls;
    for(uint32_t i=0;i<count;++i)received.push_back(events[i]);
}
struct Mouse {
    alignas(8) uint8_t bytes[0xd0]{};
    int32_t levels[4]{};
    Mouse(uint32_t id,HANDLE handle,int32_t held=0) {
        Store(bytes+8,uint32_t{3});Store(bytes+12,id);Store(bytes+0x10,uint32_t{6});
        Store(bytes+0x28,handle);Store(bytes+0x88,uint16_t{3});Store(bytes+0xa8,uint32_t{4});
        Store(bytes+0xb0,reinterpret_cast<uint8_t*>(levels));levels[3]=held;
    }
};
}
int main() {
    alignas(8) uint8_t providerBytes[0x800]{},nodeA[0x38]{},nodeB[0x38]{},control[0x20]{},pair[16]{};
    auto* queue=Queue(providerBytes);auto* map=reinterpret_cast<CRITICAL_SECTION*>(providerBytes+8);
    InitializeCriticalSection(queue);InitializeCriticalSection(map);
    Mouse physicalA(23,reinterpret_cast<HANDLE>(uintptr_t{1})),physicalB(24,reinterpret_cast<HANDLE>(uintptr_t{2}));
    ownHandle=reinterpret_cast<HANDLE>(uintptr_t{3});Mouse owned(27,ownHandle);
    Store(nodeA+0x30,physicalA.bytes);Store(nodeA+8,nodeB);Store(nodeB+0x30,physicalB.bytes);Store(providerBytes+0x40,nodeA);
    Store(control+8,uint32_t{1});Store(control+12,uint32_t{1});Store(pair,control);Store(pair+8,owned.bytes);
    installed=true;registrationHook.original= reinterpret_cast<void*>(&OriginalRegister);appendHook.original=reinterpret_cast<void*>(&OriginalAppend);
    Register(providerBytes,pair);
    auto state=Read();Check(registrationCalls==1 && state.captured && !state.failed && state.known==3 && state.ownId==27,"late map snapshot or registration identity");
    Check(Load<uint32_t>(control+8)==2 && Load<uint32_t>(control+12)==2,"own native ownership not pinned exactly once");
    // Observation preserves even the final release's original device identity.
    const Event observation[]={{1,23,4,0,1,11},{2,24,4,0,1,12},{3,23,4,0,0,13},{4,24,4,0,0,14}};
    Append(providerBytes,observation,4);Check(received.size()==4 && !std::memcmp(received.data(),observation,sizeof(observation)),"observation changed input");
    Check(Enable() && Available(),"neutral activation failed");received.clear();
    const Event overlap[]={{5,27,4,0,1,1},{6,23,4,0,1,2},{7,23,4,0,0,3},{8,27,4,0,0,4}};
    Append(providerBytes,overlap,4);
    Check(received.size()==2 && received[0].device==27 && received[0].value==1 && received[1].device==27 && received[1].value==0,"native adapter overlap cancellation");
    Check(OwnNeutral(),"released own source not neutral");
    // Contending producers must not interleave filter state and queue writes.
    received.clear();
    auto producer=[&](uint32_t id) {for(uint32_t i=0;i<1000;++i) {Event e{100+i,id,4,0,static_cast<int32_t>((i%2)==0),0};Append(providerBytes,&e,1);}};
    std::thread a(producer,23),b(producer,27);a.join();b.join();
    bool held=false;uint32_t owner=0;
    for(const auto& e:received) {Check(e.value!=static_cast<int32_t>(held),"concurrent queue duplicate/out-of-order edge");held=e.value!=0;if(held)owner=e.device;else Check(e.device==owner,"concurrent final release lost owner");}
    Check(!held && OwnNeutral(),"concurrent input stranded hold");
    // Arbitrarily large batches preserve unrelated input without a guessed cap.
    received.clear();std::vector<Event> large(1025);
    for(size_t i=0;i<large.size();++i)large[i]={i,100,3,2,static_cast<int32_t>(i),0x1234};
    Append(providerBytes,large.data(),static_cast<uint32_t>(large.size()));
    Check(received.size()==large.size() && !std::memcmp(received.data(),large.data(),large.size()*sizeof(Event)),"large native batch changed/lost records");
    // Removed own object can retain native level1. Release acknowledgement waits
    // for the normalized release/removal append, not an impossible raw update.
    received.clear();owned.levels[3]=1;Event down{5000,27,4,0,1,0};Append(providerBytes,&down,1);
    Event remove{5001,27,0,1,0,0};Append(providerBytes,&remove,1);
    Check(sawUncommittedRemoval && received.size()==3 && received[1].kind==4 && received[1].value==0 && received[2].kind==0 && received[2].control==1,"release must precede native removal");
    Check(OwnNeutral() && !Available() && Read().nativeLevel==1,"removed pinned object blocked handoff or remained available");
    DeleteCriticalSection(map);DeleteCriticalSection(queue);
    std::puts("PASS native source snapshot, ownership pin, observation, overlap, concurrent queue order, large batches and removal acknowledgement");
}
