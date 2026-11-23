#include "native_mouse_sources.h"
#include "native_input_hook.h"
#include "mouse_events.h"
#include <array>
#include <atomic>
#include <cstring>

namespace pc2vr::pointer::sources {
namespace {
SRWLOCK stateLock=SRWLOCK_INIT;
native::Hook registrationHook,appendHook;
using RegisterFn=void(*)(void*,void*);
using AppendFn=void(*)(void*,const Event*,uint32_t);
ButtonSources buttons;
HANDLE ownHandle{};
uint8_t* provider{};
uint8_t* ownObject{}; // pinned native ownership pair; no borrowed physical objects
uint32_t ownId{}, known{};
bool installed{}, captured{}, enabled{}, failed{}, removed{}, removalQueued{};
std::atomic<uint8_t*> publishedProvider{};

template<class T> T Load(const uint8_t* p) { T value; std::memcpy(&value,p,sizeof(value));return value; }
CRITICAL_SECTION* Queue(uint8_t* p) { return reinterpret_cast<CRITICAL_SECTION*>(p+0x100); }
int32_t NativeLevel() {
    return ownObject ? Load<int32_t>(Load<uint8_t*>(ownObject+0xb0)+12) : 0;
}
bool Remember(uint8_t* object) {
    if (!object) return false;
    if (Load<uint32_t>(object+8)!=3) return true;
    const auto count=Load<uint32_t>(object+0xa8);
    const auto* state=Load<uint8_t*>(object+0xb0);
    if (Load<uint16_t>(object+0x88)!=3 || count<4 || count>32 || !state) return false;
    const auto id=Load<uint32_t>(object+12);
    const auto level=Load<int32_t>(state+12);
    if (level!=0 && level!=1) return false;
    if (buttons.Known(id)) return true;
    if (!buttons.Add(id)) return false;
    ++known;
    Event seed{0,id,4,0,level,0};buttons.Filter(seed);
    return true;
}
// One bounded initial traversal under the game's map lock, while the native
// queue lock serializes our state with appends. Subsequent arrivals/removals are
// direct callbacks; no per-frame enumeration or physical-object retention.
bool Capture(uint8_t* p) {
    std::array<uint8_t*,128> stack{};size_t depth=0,visited=0;
    auto* root=Load<uint8_t*>(p+0x40);
    if (root) stack[depth++]=root;
    while (depth) {
        auto* node=stack[--depth];
        if (++visited>256 || !Remember(Load<uint8_t*>(node+0x30))) return false;
        for (const size_t offset : {size_t{0},size_t{8}}) {
            auto* child=Load<uint8_t*>(node+offset);
            if (!child) continue;
            if (depth==stack.size()) return false;
            stack[depth++]=child;
        }
    }
    return true;
}
bool CaptureSafely(uint8_t* p) {
    __try { return Capture(p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RememberSafely(uint8_t* object) {
    __try { return Remember(object); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Register(void* inputProvider,void* ownershipPair) {
    auto* p=static_cast<uint8_t*>(inputProvider);
    auto* pair=static_cast<uint8_t*>(ownershipPair);
    auto* object=Load<uint8_t*>(pair+8);
    // Native Register and Remove release map+8 before appending to queue+100.
    // Never hold our state lock across original Register/native callbacks.
    EnterCriticalSection(Queue(p));
    EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(p+8));
    AcquireSRWLockExclusive(&stateLock);
    if (!failed && installed) {
        if (!captured) {
            provider=p;captured=CaptureSafely(p);
            if (!captured) failed=true;
            else publishedProvider.store(p,std::memory_order_release);
        } else if (provider!=p) failed=true;
        if (!failed && !RememberSafely(object)) failed=true;
        if (!failed && Load<uint32_t>(object+8)==3 && Load<HANDLE>(object+0x28)==ownHandle) {
            auto* control=Load<uint8_t*>(pair);
            if (ownObject || !control || Load<uint32_t>(control+8)==0 || Load<uint32_t>(control+12)==0) failed=true;
            else {
                // Exact two native lock-inc operations measured at1612957.
                // One deliberate process-lifetime pin for our own object only.
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(control+8));
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(control+12));
                ownObject=object;ownId=Load<uint32_t>(object+12);
            }
        }
    }
    ReleaseSRWLockExclusive(&stateLock);
    LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(p+8));
    LeaveCriticalSection(Queue(p));
    reinterpret_cast<RegisterFn>(registrationHook.original.load(std::memory_order_acquire))(inputProvider,ownershipPair);
}
void Append(void* inputProvider,const Event* data,uint32_t count) {
    const auto original=reinterpret_cast<AppendFn>(appendHook.original.load(std::memory_order_acquire));
    if (!count) { original(inputProvider,data,count);return; }
    auto* p=static_cast<uint8_t*>(inputProvider);
    // Match filter order to the actual native queue order. The original append
    // re-enters this CRITICAL_SECTION on the same thread. State is always dropped
    // before calling it, avoiding a bridge-lock -> native-callback lock cycle.
    EnterCriticalSection(Queue(p));
    std::array<Event,256> output{};uint32_t kept=0;bool ownRemovalInBatch=false;
    for (uint32_t i=0;i<count;++i) {
        Event event=data[i],release{};bool keep=true,emitRelease=false;
        AcquireSRWLockExclusive(&stateLock);
        if (captured && p==provider) {
            if (buttons.Known(event.device) && event.kind==0 && event.control==1) {
                emitRelease=buttons.Remove(event.device,event.time,release)&&enabled;
                --known;
                if (event.device==ownId) { removed=true;ownRemovalInBatch=true; }
            } else {
                const bool filtered=buttons.Filter(event);
                if (enabled) keep=filtered;
                else event=data[i]; // observation must preserve original identity
            }
        }
        ReleaseSRWLockExclusive(&stateLock);
        if (emitRelease) output[kept++]=release;
        if (keep) output[kept++]=event;
        if (kept>output.size()-2 || i==count-1) {
            if (kept) original(inputProvider,output.data(),kept);
            if (ownRemovalInBatch) {
                AcquireSRWLockExclusive(&stateLock);removalQueued=true;ReleaseSRWLockExclusive(&stateLock);
                ownRemovalInBatch=false;
            }
            kept=0;
        }
    }
    LeaveCriticalSection(Queue(p));
}
} // namespace
bool Install(HANDLE ownDevice) {
    if (!ownDevice || !native::VerifyRetail()) return false;
    // Setup must complete before arrival. Partial hooks stay as pass-through;
    // fail closed rather than removing code while another thread may execute it.
    AcquireSRWLockExclusive(&stateLock);
    if (ownHandle) { ReleaseSRWLockExclusive(&stateLock);return false; }
    ownHandle=ownDevice;
    ReleaseSRWLockExclusive(&stateLock);
    if (!native::Install(native::Site::Append,reinterpret_cast<void*>(&Append),appendHook) ||
        !native::Install(native::Site::Register,reinterpret_cast<void*>(&Register),registrationHook)) return false;
    AcquireSRWLockExclusive(&stateLock);installed=true;ReleaseSRWLockExclusive(&stateLock);
    return true;
}
bool Enable() {
    auto* p=publishedProvider.load(std::memory_order_acquire);
    if (!p) return false;
    EnterCriticalSection(Queue(p));
    AcquireSRWLockExclusive(&stateLock);
    const bool ready=installed && captured && !failed && !removed && ownObject && !buttons.Held() && NativeLevel()==0;
    if (ready) enabled=true;
    ReleaseSRWLockExclusive(&stateLock);
    LeaveCriticalSection(Queue(p));
    return ready;
}
bool Available() {
    AcquireSRWLockShared(&stateLock);
    const bool ready=enabled && !failed && !removed && ownObject && Load<uint32_t>(ownObject+0x10)>0;
    ReleaseSRWLockShared(&stateLock);return ready;
}
bool OwnNeutral() {
    AcquireSRWLockShared(&stateLock);
    // Removal has no subsequent RawMouse state update: the pinned object's old
    // level may stay1. Only acknowledge after its normalized removal/release
    // batch reached the native queue; never send raw input to a removed handle.
    const bool neutral=ownObject && !buttons.Level(ownId) && (removed ? removalQueued : NativeLevel()==0);
    ReleaseSRWLockShared(&stateLock);return neutral;
}
Snapshot Read() {
    AcquireSRWLockShared(&stateLock);
    const Snapshot result{ownId,known,captured,enabled,failed,removed,buttons.Level(ownId),NativeLevel()};
    ReleaseSRWLockShared(&stateLock);return result;
}
} // namespace pc2vr::pointer::sources
