#pragma once
#include "pointer_packets.h"
#include <atomic>

namespace pc2vr::pointer {
// Executable-IAT adapters only. All unrelated handles and invalid list shapes
// retain the real Windows implementation. The native bridge owns synchronization.
class RawDeviceApi {
public:
    using ListFn=UINT(WINAPI*)(PRAWINPUTDEVICELIST,PUINT,UINT);
    using InfoFn=UINT(WINAPI*)(HANDLE,UINT,LPVOID,PUINT);
    explicit RawDeviceApi(RawPackets& packets) : packets_(packets) {}
    void Expose(bool value) { expose_.store(value,std::memory_order_release); }
    UINT List(PRAWINPUTDEVICELIST data, UINT* count, UINT stride, ListFn original) const {
        if (!expose_.load(std::memory_order_acquire) || !count || stride!=sizeof(RAWINPUTDEVICELIST)) return original(data,count,stride);
        if (!data) {
            const UINT result=original(data,count,stride);
            if (result==0) ++*count;
            return result;
        }
        const UINT capacity=*count;
        if (!capacity) {
            const UINT result=original(nullptr,count,stride);
            if (result!=0) return result;
            ++*count;SetLastError(ERROR_INSUFFICIENT_BUFFER);return UINT(-1);
        }
        *count=capacity-1;
        const UINT result=original(data,count,stride);
        if (result==UINT(-1)) {
            if (GetLastError()==ERROR_INSUFFICIENT_BUFFER) ++*count;
            return result;
        }
        data[result]={packets_.Device(),RIM_TYPEMOUSE};*count=capacity;
        return result+1;
    }
    UINT Info(HANDLE device, UINT command, void* data, UINT* size, InfoFn original) const {
        if (device!=packets_.Device()) return original(device,command,data,size);
        if (!size) return Error(ERROR_INVALID_PARAMETER);
        const UINT needed=command==RIDI_DEVICENAME ? sizeof(kName) : command==RIDI_DEVICEINFO ? sizeof(RID_DEVICE_INFO) : 0;
        if (!needed) return Error(ERROR_INVALID_PARAMETER);
        if (!data) { *size=needed; return 0; }
        if (*size<needed) { *size=needed; return Error(ERROR_INSUFFICIENT_BUFFER); }
        if (command==RIDI_DEVICENAME) std::memcpy(data,kName,sizeof(kName));
        else {
            // Retail leaves the input cbSize uninitialized. Real Windows fills
            // it for cbSize32,0,A5A5A5A5; don't reject what Windows accepts.
            RID_DEVICE_INFO info{};info.cbSize=sizeof(info);info.dwType=RIM_TYPEMOUSE;
            info.mouse.dwId=0x50433256;info.mouse.dwNumberOfButtons=5;
            std::memcpy(data,&info,sizeof(info));
        }
        *size=needed;return needed;
    }
private:
    static UINT Error(DWORD value) { SetLastError(value);return UINT(-1); }
    static constexpr char kName[]="PC2VR\\MotionPointer";
    RawPackets& packets_;
    std::atomic<bool> expose_{};
};
} // namespace pc2vr::pointer
