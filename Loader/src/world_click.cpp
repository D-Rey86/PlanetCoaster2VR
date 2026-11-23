#include "world_click.h"
#include "native_input_hook.h"
#include "native_mouse_sources.h"
#include "pointer_device.h"
#include "window_pointer.h"
#include <atomic>
#include <array>
#include <cstring>
#include <mutex>

namespace pc2vr::pointer::click {
namespace {
using DataFn=UINT(WINAPI*)(HRAWINPUT,UINT,LPVOID,PUINT,UINT);
using UiMoveFn=bool(*)(int,int);
std::atomic<void*> dataOriginal{},infoOriginal{},listOriginal{};
native::Hook uiHook;
std::once_flag preparation;
SRWLOCK packetLock=SRWLOCK_INIT,bindLock=SRWLOCK_INIT;
RawPackets packets;
RawDeviceApi metadata(packets);
std::atomic<bool> prepared{},bound{},fault{},uiKnown{},uiBlocked{true};
std::atomic<HWND> target{};
std::atomic<DWORD> targetThread{};
struct Stream {
    ULONGLONG time{};
    uint64_t generation=1;
    std::array<bool,16> edges{};
    size_t head{},count{};
    bool valid{},desired{},posted{},requireNeutral=true;
} stream;

bool InvalidateLocked() {
    const bool changed=stream.valid || stream.count;
    if (changed) ++stream.generation;
    stream.valid=stream.desired=false;stream.head=stream.count=0;stream.requireNeutral=true;
    return changed;
}
bool FreshLocked(ULONGLONG now) {
    return stream.valid && now>=stream.time && now-stream.time<=250;
}
bool UiMove(int x,int y) {
    const bool handled=reinterpret_cast<UiMoveFn>(uiHook.original.load(std::memory_order_acquire))(x,y);
    const DWORD error=GetLastError();
    uiBlocked.store(handled,std::memory_order_release);uiKnown.store(true,std::memory_order_release);
    if (handled) Clear(); // No synthetic DOWN may start a native menu gesture.
    SetLastError(error);return handled;
}
UINT WINAPI Data(HRAWINPUT token,UINT command,LPVOID data,PUINT size,UINT headerSize) {
    if (!packets.Owns(token))
        return reinterpret_cast<DataFn>(dataOriginal.load(std::memory_order_acquire))(token,command,data,size,headerSize);
    const DWORD incoming=GetLastError();
    if (command==RID_INPUT && data && GetCurrentThreadId()!=targetThread.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&packetLock);packets.Cancel(token);InvalidateLocked();ReleaseSRWLockExclusive(&packetLock);
        fault.store(true,std::memory_order_release);window::Wake();SetLastError(ERROR_INVALID_THREAD_ID);return UINT(-1);
    }
    const auto source=sources::Read();const bool ready=sources::Available() && !fault.load(std::memory_order_acquire);
    const auto now=GetTickCount64();
    AcquireSRWLockExclusive(&packetLock);
    UINT result;DWORD error;
    if (source.removed) {packets.Cancel(token);result=UINT(-1);error=ERROR_INVALID_HANDLE;}
    else {
        const bool allow=ready && FreshLocked(now) && uiKnown.load(std::memory_order_acquire) && !uiBlocked.load(std::memory_order_acquire);
        SetLastError(incoming);result=packets.Read(token,command,data,size,headerSize,allow,stream.generation);error=GetLastError();
    }
    ReleaseSRWLockExclusive(&packetLock);SetLastError(error);return result;
}
UINT WINAPI Info(HANDLE device,UINT command,LPVOID data,PUINT size) {
    return metadata.Info(device,command,data,size,reinterpret_cast<RawDeviceApi::InfoFn>(infoOriginal.load(std::memory_order_acquire)));
}
UINT WINAPI List(PRAWINPUTDEVICELIST data,PUINT count,UINT stride) {
    return metadata.List(data,count,stride,reinterpret_cast<RawDeviceApi::ListFn>(listOriginal.load(std::memory_order_acquire)));
}

struct Import {
    const char* name;uintptr_t address;void* replacement;std::atomic<void*>* original;
    void** slot{};void* expected{};
};
bool Imports() {
    auto* base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto& directory=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return false;
    Import wanted[]={
        {"GetRawInputData",native::Address(native::Landmark::RawDataImport),reinterpret_cast<void*>(&Data),&dataOriginal},
        {"GetRawInputDeviceInfoA",native::Address(native::Landmark::RawInfoImport),reinterpret_cast<void*>(&Info),&infoOriginal},
        {"GetRawInputDeviceList",native::Address(native::Landmark::RawListImport),reinterpret_cast<void*>(&List),&listOriginal}
    };
    const auto* descriptors=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+directory.VirtualAddress);
    const size_t limit=directory.Size/sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (size_t i=0;i<limit && descriptors[i].Name;++i) {
        const auto& descriptor=descriptors[i];
        if (_stricmp(reinterpret_cast<const char*>(base+descriptor.Name),"USER32.dll") || !descriptor.OriginalFirstThunk) continue;
        const auto* names=reinterpret_cast<const IMAGE_THUNK_DATA64*>(base+descriptor.OriginalFirstThunk);
        auto* slots=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+descriptor.FirstThunk);
        for (;names->u1.AddressOfData;++names,++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* name=reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData)->Name;
            for (auto& entry:wanted) if (!std::strcmp(reinterpret_cast<const char*>(name),entry.name)) {
                entry.slot=reinterpret_cast<void**>(&slots->u1.Function);entry.expected=*entry.slot;
                if (reinterpret_cast<uintptr_t>(entry.slot)!=entry.address || !entry.expected) return false;
                MEMORY_BASIC_INFORMATION region{};
                if (!VirtualQuery(entry.expected,&region,sizeof(region)) || region.State!=MEM_COMMIT ||
                    (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
                    !(region.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
            }
        }
    }
    for (const auto& entry:wanted) if (!entry.slot) return false;
    // All names/RVAs are checked first. A partial installation remains invisible
    // and forwards ordinary calls. Never hot-unhook code executing elsewhere.
    for (auto& entry:wanted) {
        DWORD protection{};
        if (!VirtualProtect(entry.slot,sizeof(void*),PAGE_READWRITE,&protection)) return false;
        entry.original->store(entry.expected,std::memory_order_release);
        const bool ok=InterlockedCompareExchangePointer(entry.slot,entry.replacement,entry.expected)==entry.expected;
        DWORD ignored{};VirtualProtect(entry.slot,sizeof(void*),protection,&ignored);
        if (!ok) return false;
    }
    return true;
}
// Only the previously measured native window thread dispatches these tokens.
// No lock crosses the native callback, which re-enters Data and source filtering.
void Pump(ULONGLONG now,bool accepting) {
    if (!sources::Available()) sources::Enable();
    const auto source=sources::Read();
    const bool available=accepting && sources::Available() && !fault.load(std::memory_order_acquire) &&
        uiKnown.load(std::memory_order_acquire) && !uiBlocked.load(std::memory_order_acquire);
    for(size_t i=0;i<17;++i) {
        AcquireSRWLockExclusive(&packetLock);
        if (!available || !FreshLocked(now)) InvalidateLocked();
        if (source.removed) {stream.posted=false;ReleaseSRWLockExclusive(&packetLock);return;}
        const bool release=!stream.valid && stream.posted;
        if (!release && (!stream.valid || !stream.count)) {ReleaseSRWLockExclusive(&packetLock);return;}
        const bool down=release ? false : stream.edges[stream.head];
        const auto token=packets.Acquire(down,stream.generation);
        if (!token) {InvalidateLocked();ReleaseSRWLockExclusive(&packetLock);return;}
        if (!release) {stream.head=(stream.head+1)%stream.edges.size();--stream.count;}
        ReleaseSRWLockExclusive(&packetLock);
        const bool delivered=window::DeliverInput(token);
        const auto after=sources::Read();
        AcquireSRWLockExclusive(&packetLock);
        const bool consumed=!packets.Pending(token);
        if (!delivered || !consumed) {packets.Cancel(token);InvalidateLocked();fault.store(true,std::memory_order_release);}
        // The source confirms the native call's result, including a cancelled
        // DOWN masked during Data. Never mistake a queued token for native hold.
        stream.posted=!after.removed && (after.ownHeld || after.nativeLevel!=0);
        if (after.removed) InvalidateLocked();
        ReleaseSRWLockExclusive(&packetLock);
        if (!delivered || !consumed || after.removed) return;
    }
}
bool ReleaseUnusedPosition() {return sources::OwnNeutral();}
} // namespace

bool Prepare() {
    std::call_once(preparation,[]{
        if (!native::VerifyRetail() || !sources::Install(packets.Device()) ||
            !native::Install(native::Site::UiMove,reinterpret_cast<void*>(UiMove),uiHook) || !Imports()) return;
        prepared.store(true,std::memory_order_release);
    });
    return prepared.load(std::memory_order_acquire);
}
bool Available() {
    return prepared.load(std::memory_order_acquire) && bound.load(std::memory_order_acquire) &&
        !fault.load(std::memory_order_acquire) && sources::Available() && window::Accepting();
}
bool UiOwnsInput() {
    return !uiKnown.load(std::memory_order_acquire) || uiBlocked.load(std::memory_order_acquire);
}
bool BindWindow(HWND hwnd) {
    if (!prepared.load(std::memory_order_acquire) || !hwnd) return false;
    AcquireSRWLockExclusive(&bindLock);
    if (bound.load(std::memory_order_acquire) && target.load(std::memory_order_acquire)==hwnd) {
        ReleaseSRWLockExclusive(&bindLock);return true;
    }
    DWORD pid{};const auto thread=GetWindowThreadProcessId(hwnd,&pid);
    if (!thread || pid!=GetCurrentProcessId() || !window::Attach(hwnd,ReleaseUnusedPosition,Pump)) {
        ReleaseSRWLockExclusive(&bindLock);return false;
    }
    target.store(hwnd,std::memory_order_release);targetThread.store(thread,std::memory_order_release);
    metadata.Expose(true);
    const bool announced=PostMessageW(hwnd,WM_INPUT_DEVICE_CHANGE,GIDC_ARRIVAL,reinterpret_cast<LPARAM>(packets.Device()))!=0;
    bound.store(announced,std::memory_order_release);ReleaseSRWLockExclusive(&bindLock);return announced;
}
void Clear() {
    AcquireSRWLockExclusive(&packetLock);const bool wake=stream.count || stream.posted;
    InvalidateLocked();ReleaseSRWLockExclusive(&packetLock);
    if (wake) window::Wake();
}
bool Publish(bool down,ULONGLONG time) {
    if (!Available()) {Clear();return false;}
    AcquireSRWLockExclusive(&packetLock);
    if (down && (stream.requireNeutral || !uiKnown.load(std::memory_order_acquire) || uiBlocked.load(std::memory_order_acquire))) {
        const bool wake=stream.count || stream.posted;InvalidateLocked();ReleaseSRWLockExclusive(&packetLock);
        if(wake)window::Wake();return false;
    }
    if (!down) stream.requireNeutral=false;
    bool wake=false;
    if (stream.desired!=down) {
        if (stream.count==stream.edges.size()) {InvalidateLocked();ReleaseSRWLockExclusive(&packetLock);window::Wake();return false;}
        stream.edges[(stream.head+stream.count)%stream.edges.size()]=down;++stream.count;wake=true;
    }
    stream.time=time;stream.valid=true;stream.desired=down;
    ReleaseSRWLockExclusive(&packetLock);if(wake)window::Wake();return true;
}
} // namespace pc2vr::pointer::click
