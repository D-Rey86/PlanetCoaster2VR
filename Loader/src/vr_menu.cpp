#include "vr_menu.h"
#include "vr_menu_values.h"
#include "ui_layer.h"
#include "xr_runtime.h"
#include "vrs.h"
#include "log.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace pc2vr::menu {
namespace {
using InitFn = void*(*)(const char*, const void*);
using SystemFn = void*(*)(void*, const void*);
using RequestFn = void(*)(void*, const void*, void*);
using SpaceFn = void*(*)(void*, uint64_t);
using FinishFn = void(*)(void*, unsigned);
using DataFn = void(*)(void*, const void*);
using ProgressFn = void(*)(void*);
using SnapshotFn = bool(*)(char*, uint32_t);
using CommandFn = bool(*)(const char*);
InitFn g_init{};
SystemFn g_system{};
RequestFn g_request{};
SpaceFn g_space{};
FinishFn g_finish{};
DataFn g_optimized{};
ProgressFn g_progress{};
HMODULE g_cohtml{};
std::wstring g_ini;
std::string g_script;
std::mutex g_pendingMutex, g_commandMutex, g_hookMutex;
struct Pending {void* response{}; uint64_t size{}; const char* bytes{}; bool incremental{};};
std::array<Pending, 16> g_pending{};
std::atomic<unsigned> g_pendingCount{};
std::atomic<bool> g_nvidia{};
SnapshotFn g_snapshot{};
CommandFn g_command{};
uint64_t g_requestId{};
int g_scale = 100;
int g_expandedCoverage = 0;
constexpr char kBaseURL[] = "coui://UIGameface/js/project/modules/options/components/OptionsMenuBase.js";
constexpr char kPrefix[] = "coui://UIGameface/pc2vr/";
constexpr char kAppend[] = "\nimport {installVRMenu} from '/pc2vr/VRMenu.js';\ninstallVRMenu(_OptionsMenuBase, preact, Tab);\n";

bool Replace(void** slot, void* hook, void** previous) {
    DWORD old{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    void* current=*slot;
    *previous=current; // publish the original before another thread can enter the hook
    const bool changed=InterlockedCompareExchangePointer(slot,hook,current)==current;
    DWORD unused{}; VirtualProtect(slot, sizeof(void*), old, &unused);
    return changed;
}
template<typename Fn> Fn Method(const void* object, unsigned index) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void* const* const*>(object))[index]);
}
bool ShippingResponse(const void* response) {
    auto* table=*reinterpret_cast<void* const* const*>(response);
    return table[1] == reinterpret_cast<char*>(g_cohtml)+0x496c30 &&
           table[8] == reinterpret_cast<char*>(g_cohtml)+0x4972e0;
}
bool IsPending(void* response) {
    if (!g_pendingCount.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    for (const auto& slot : g_pending) if (slot.response == response) return true;
    return false;
}
void* Space(void* response, uint64_t size) {
    void* bytes=g_space(response,size);
    if (g_pendingCount.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        for (auto& slot : g_pending) if (slot.response == response) {
            slot.size=size; slot.bytes=static_cast<const char*>(bytes); break;
        }
    }
    return bytes;
}
void Optimized(void* response, const void* data) {
    // A cached compilation of the original module cannot execute an appended import.
    // Only this single source module is recompiled; unrelated resources keep their cache.
    if (!IsPending(response)) g_optimized(response,data);
}
void Progress(void* response) {
    if (g_pendingCount.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        for (auto& slot : g_pending) if (slot.response == response) {slot.bytes=nullptr;slot.incremental=true; break;}
    }
    g_progress(response);
}
void Finish(void* response, unsigned status) {
    Pending pending{};
    if (g_pendingCount.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        for (auto& slot : g_pending) if (slot.response == response) {
            pending=slot; slot={}; --g_pendingCount; break;
        }
    }
    if (!status && !pending.incremental && pending.bytes && pending.size && pending.size < 512*1024 &&
        std::string_view(pending.bytes,static_cast<size_t>(pending.size)).find("class _OptionsMenuBase extends preact.Component") != std::string_view::npos &&
        std::string_view(pending.bytes,static_cast<size_t>(pending.size)).find("export const OptionsMenuBase = Focusable.decorate(_OptionsMenuBase)") != std::string_view::npos) {
        const uint64_t size=pending.size+sizeof(kAppend)-1;
        // Proven shipping GetSpace/Finish path; GetSpace preserves the original bytes.
        if (auto* bytes=static_cast<char*>(g_space(response,size))) {
            memcpy(bytes+pending.size,kAppend,sizeof(kAppend)-1);
            LOG("VR menu: native settings extension loaded");
        }
    }
    g_finish(response,status); // response is invalid after this call
}
bool TrackModule(void* response) {
    std::lock_guard<std::mutex> hookLock(g_hookMutex);
    // Repeated module requests use the already-installed table. Validate against the
    // saved pointers as well as the unmodified shipping table before touching it.
    auto** table=*reinterpret_cast<void***>(response);
    if (!g_space) {
        if (!ShippingResponse(response)) return false;
        if (!Replace(&table[1],reinterpret_cast<void*>(&Space),reinterpret_cast<void**>(&g_space))) return false;
        if (!Replace(&table[3],reinterpret_cast<void*>(&Optimized),reinterpret_cast<void**>(&g_optimized))) return false;
        if (!Replace(&table[4],reinterpret_cast<void*>(&Progress),reinterpret_cast<void**>(&g_progress))) return false;
        if (!Replace(&table[8],reinterpret_cast<void*>(&Finish),reinterpret_cast<void**>(&g_finish))) return false;
    }
    if (table[1]!=reinterpret_cast<void*>(&Space) || table[8]!=reinterpret_cast<void*>(&Finish)) return false;
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    for (auto& slot : g_pending) if (!slot.response) {
        slot={response,0,nullptr}; ++g_pendingCount; return true;
    }
    return false;
}
bool ResolveCheeky() {
    if (!g_nvidia.load(std::memory_order_relaxed)) return false;
    if (!g_snapshot || !g_command) {
        HMODULE host=GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
        if (!host) return false;
        g_snapshot=reinterpret_cast<SnapshotFn>(GetProcAddress(host,"CheekyHost_Snapshot"));
        g_command=reinterpret_cast<CommandFn>(GetProcAddress(host,"CheekyHost_Command"));
    }
    return g_snapshot && g_command;
}
std::string StateJSON() {
    std::lock_guard<std::mutex> lock(g_commandMutex);
    const auto p=ui::Settings();
    const auto resolution=xr::ResolutionStatus();
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"ok\":true,\"Resolution\":" << g_scale
        << ",\"ExpandedCoverage\":" << g_expandedCoverage
        << ",\"Foveated\":" << (vrs::Enabled()?1:0)
        << ",\"renderWidth\":" << resolution.width << ",\"renderHeight\":" << resolution.height
        << ",\"UIWidth\":" << p.widthDeg << ",\"UIDistance\":" << std::lround(p.distanceM*100.f)
        << ",\"UIHeight\":" << p.heightDeg << ",\"UIHorizontal\":" << p.horizontalDeg
        << ",\"UIFollowHmd\":" << (p.followHmd?1:0)
        << ",\"nvidia\":" << (g_nvidia.load()?"true":"false") << ",\"cheeky\":";
    std::array<char,32768> snapshot{};
    if (ResolveCheeky() && g_snapshot(snapshot.data(),static_cast<uint32_t>(snapshot.size())) &&
        snapshot[0]=='{' && snapshot.back()=='\0') out << snapshot.data();
    else out << "null";
    out << '}'; return out.str();
}
std::string Apply(std::string_view query) {
    // All writes are finite, bounded integers from a fixed setting allowlist. No
    // file paths, arbitrary Cheeky commands, defaults resets or driver profile writes.
    if (query.substr(0,4)!="key=") return "{\"ok\":false,\"error\":\"Invalid request\"}";
    const auto separator=query.find("&value=");
    if (separator==std::string_view::npos || separator<4 || separator>60) return "{\"ok\":false}";
    const std::string key(query.substr(4,separator-4));
    const Setting* setting=FindSetting(key.c_str());
    auto valueText=query.substr(separator+7); valueText=valueText.substr(0,valueText.find('&'));
    int value{}; const auto parsed=std::from_chars(valueText.data(),valueText.data()+valueText.size(),value);
    if (!setting || parsed.ec!=std::errc{} || parsed.ptr!=valueText.data()+valueText.size() || !ValidValue(*setting,value))
        return "{\"ok\":false,\"error\":\"Value outside supported range\"}";
    std::lock_guard<std::mutex> lock(g_commandMutex);
    if (setting->cheeky) {
        if (!ResolveCheeky()) return "{\"ok\":false,\"error\":\"Cheeky DLSS is unavailable\"}";
        std::string number=std::to_string(value);
        if (setting->fractional) {
            const int magnitude=value<0?-value:value; // bounded by the setting allowlist
            number=(value<0?"-":"")+std::to_string(magnitude/100)+"."+
                (magnitude%100<10?"0":"")+std::to_string(magnitude%100);
        }
        const std::string command="1\n"+std::to_string(++g_requestId)+"\nset\n"+setting->cheeky+"="+number+"\n";
        if (!g_command(command.c_str())) return "{\"ok\":false,\"error\":\"Cheeky rejected this change\"}";
    } else {
        const auto number=std::to_wstring(value);
        if (!WritePrivateProfileStringW(setting->section,setting->key,number.c_str(),g_ini.c_str()))
            return "{\"ok\":false,\"error\":\"Could not save VR settings\"}";
        if (key=="Resolution") {g_scale=value; xr::RequestResolutionScale(value);}
        else if (key=="ExpandedCoverage") {g_expandedCoverage=value; xr::RequestExpandedCoverage(value!=0);}
        else if (key=="Foveated") vrs::SetEnabled(value!=0);
        else {
            auto p=ui::Settings();
            if (key=="UIWidth") p.widthDeg=static_cast<float>(value);
            if (key=="UIDistance") p.distanceM=value/100.f;
            if (key=="UIHeight") p.heightDeg=static_cast<float>(value);
            if (key=="UIHorizontal") p.horizontalDeg=static_cast<float>(value);
            if (key=="UIFollowHmd") p.followHmd=value!=0;
            ui::ApplySettings(p);
        }
    }
    return "{\"ok\":true}";
}
std::string RecalibrateCheeky() {
    std::lock_guard<std::mutex> lock(g_commandMutex);
    if (!ResolveCheeky()) return "{\"ok\":false,\"error\":\"Cheeky DLSS is unavailable\"}";
    // One explicit, session-only action. Never clear learned methods or user settings.
    const std::string command="1\n"+std::to_string(++g_requestId)+"\ncalibration_recalibrate\n";
    if (!g_command(command.c_str())) return "{\"ok\":false,\"error\":\"Cheeky rejected recalibration\"}";
    return "{\"ok\":true}";
}
void Respond(void* response, std::string_view data) {
    // The shipping 1.52 interface has nine slots: GetSpace=1, SetHeader=7,
    // Finish=8. SDK3.1's additional SetStreamReader is absent here.
    auto space=Method<SpaceFn>(response,1);
    auto finish=Method<FinishFn>(response,8);
    auto* bytes=static_cast<char*>(space(response,data.size()));
    if (!bytes) {finish(response,1); return;}
    memcpy(bytes,data.data(),data.size()); finish(response,0);
}
void Request(void* self, const void* request, void* response) {
    const auto url=Method<const char*(*)(const void*)>(request,2)(request);
    if (!url) {g_request(self,request,response);return;}
    if (!std::strncmp(url,kPrefix,sizeof(kPrefix)-1)) {
        const std::string_view path(url+sizeof(kPrefix)-1);
        if (path=="VRMenu.js") Respond(response,g_script);
        else if (path.substr(0,10)=="state.json") Respond(response,StateJSON());
        else if (path.substr(0,13)=="recenter.json") {xr::RequestRecenter();Respond(response,"{\"ok\":true}");}
        else if (path.substr(0,path.find('?'))=="recalibrate.json") Respond(response,RecalibrateCheeky());
        else if (path.substr(0,9)=="set.json?") Respond(response,Apply(path.substr(9)));
        else Respond(response,"{\"ok\":false,\"error\":\"Unknown VR operation\"}");
        return;
    }
    if (!std::strcmp(url,kBaseURL)) TrackModule(response);
    g_request(self,request,response);
}
void* CreateSystem(void* library, const void* settings) {
    auto* handler=*reinterpret_cast<void* const*>(static_cast<const char*>(settings)+0x10);
    if (handler && !g_request) {
        auto** table=*reinterpret_cast<void***>(handler);
        Replace(&table[1],reinterpret_cast<void*>(&Request),reinterpret_cast<void**>(&g_request));
    }
    return g_system(library,settings);
}
void* Initialise(const char* key, const void* params) {
    auto* library=g_init(key,params);
    if (library && !g_system) {
        auto** table=*reinterpret_cast<void***>(library);
        if (table[1]==reinterpret_cast<char*>(g_cohtml)+0x493800)
            Replace(&table[1],reinterpret_cast<void*>(&CreateSystem),reinterpret_cast<void**>(&g_system));
    }
    return library;
}
}
void Install(const wchar_t* directory) {
    g_ini=std::wstring(directory)+L"\\pc2vr.ini";
    if (!GetPrivateProfileIntW(L"UI",L"VRMenu",1,g_ini.c_str())) return;
    g_scale=static_cast<int>(GetPrivateProfileIntW(L"Resolution",L"ScalePercent",100,g_ini.c_str()));
    g_expandedCoverage=GetPrivateProfileIntW(L"Resolution",L"ExpandedCoverage",0,g_ini.c_str())==1?1:0;
    const auto path=std::wstring(directory)+L"\\VRMenu.js";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if (file==INVALID_HANDLE_VALUE) {LOG("VR menu: VRMenu.js absent; native settings unchanged");return;}
    const auto size=GetFileSize(file,nullptr);
    if (size && size < 256*1024) {
        g_script.resize(size); DWORD read{};
        if (!ReadFile(file,g_script.data(),size,&read,nullptr) || read!=size) g_script.clear();
    }
    CloseHandle(file); if (g_script.empty()) return;
    g_cohtml=GetModuleHandleW(L"cohtml.WindowsDesktop.dll");
    const auto version=g_cohtml?reinterpret_cast<const char*(*)()>(GetProcAddress(g_cohtml,"?GetLibraryVersion@Library@cohtml@@SAPEBDXZ")):nullptr;
    if (!version || std::strcmp(version(),"1.52.3.7")) {LOG("VR menu: unsupported Gameface version; native settings unchanged");return;}
    auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto directoryEntry=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+directoryEntry.VirtualAddress);
    for (;imports->Name;++imports) {
        if (_stricmp(reinterpret_cast<const char*>(base+imports->Name),"cohtml.WindowsDesktop.dll")) continue;
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports->OriginalFirstThunk);
        auto* slots=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports->FirstThunk);
        for (;names->u1.AddressOfData;++names,++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if (!std::strcmp(reinterpret_cast<const char*>(name->Name),"?Initialize@Library@cohtml@@SAPEAV12@PEBDAEBULibraryParams@2@@Z")) {
                LOG("VR menu: resource bridge %s",Replace(reinterpret_cast<void**>(&slots->u1.Function),reinterpret_cast<void*>(&Initialise),reinterpret_cast<void**>(&g_init))?"installed":"unavailable");
                return;
            }
        }
    }
}
void DeviceAvailable(ID3D12Device* device) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 description{};
    using FactoryFn=HRESULT(WINAPI*)(REFIID,void**);
    const auto dxgi=GetModuleHandleW(L"dxgi.dll");
    const auto create=dxgi?reinterpret_cast<FactoryFn>(GetProcAddress(dxgi,"CreateDXGIFactory1")):nullptr;
    if (device && create && SUCCEEDED(create(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter))) &&
        SUCCEEDED(adapter->GetDesc1(&description))) g_nvidia.store(description.VendorId==0x10de);
}
}
