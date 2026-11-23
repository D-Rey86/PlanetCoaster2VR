#include "vr_menu.h"
#include "vr_menu_values.h"
#include "ui_layer.h"
#include "xr_runtime.h"
#include "vrs.h"
#include "log.h"
#include "info_panel_bridge.h"
#include "selection_panel_state.h"
#include "ride_panel_state.h"
#include "ride_panel_bridge.h"
#include "camera_panel_bridge.h"
#include "ride_popup_bridge.h"
#include "ride_click_broker.h"
#include "build_panel_bridge.h"
#include "dashboard_bridge.h"
#include "notification_bridge.h"
#include "embedded_assets.h"

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
struct Pending {void* response{}; uint64_t size{}; const char* bytes{}; bool incremental{},pause{},info{},dialog{},popup{},build{},layer{},dashboard{},camera{},credits{};};
std::array<Pending, 16> g_pending{};
std::atomic<unsigned> g_pendingCount{};
std::atomic<bool> g_nvidia{};
SnapshotFn g_snapshot{};
CommandFn g_command{};
uint64_t g_requestId{};
int g_scale = 100;
int g_expandedCoverage = 0;
int g_stereoMode = 0;
bool g_optionsEnabled=true,g_motionContext{};
bool g_selectionContext{};
detail::SelectionPanelState g_selectionPanel;
detail::SelectionPanelState g_dialogPanel;
detail::RidePanelState g_ridePanel;
void RespondRide(void*,const ui::RideClick*);
detail::RideClickBroker g_rideClicks(RespondRide);
detail::RidePanelState g_cameraPanel;
std::atomic<uint64_t> g_cameraOwner{};
void RespondCamera(void*,const ui::RideClick*);
detail::RideClickBroker g_cameraClicks(RespondCamera);
detail::RidePanelState g_buildPanel;
std::atomic<uint64_t> g_buildOwner{};
std::atomic<bool> g_buildAvailable{};
void RespondBuild(void*,const ui::RideClick*);
detail::RideClickBroker g_buildClicks(RespondBuild);
std::mutex g_buildPressMutex;
ui::RideClick g_buildPress;
detail::RidePanelState g_dashboardPanel;
std::atomic<uint64_t> g_dashboardOwner{};
void RespondDashboard(void*,const ui::RideClick*);
detail::RideClickBroker g_dashboardClicks(RespondDashboard);
std::mutex g_dashboardPressMutex;
ui::RideClick g_dashboardPress;
using DestroySystemFn=void*(*)(void*,unsigned);
DestroySystemFn g_destroySystem{};
std::atomic<void*> g_rideSystem{};
std::atomic<uint64_t> g_rideInput{};
std::mutex g_ridePressMutex;
ui::RideClick g_ridePress;
ui::RideClick g_cameraPress;
std::atomic<bool> g_pauseVisible{};
std::mutex g_pauseMutex;
uint64_t g_pauseOwner{},g_pauseSequence{};
constexpr char kBaseURL[] = "coui://UIGameface/js/project/modules/options/components/OptionsMenuBase.js";
constexpr char kPauseURL[] = "coui://UIGameface/js/project/modules/gameMenu/components/GameMenuModule.js";
constexpr char kInfoURL[] = "coui://UIGameface/js/project/modules/infoPanel/InfoPanelModule.js";
constexpr char kDropDownURL[] = "coui://UIGameface/js/project/components/DropDown.js";
constexpr char kBuildURL[] = "coui://UIGameface/js/project/modules/browser/components/Browser.js";
constexpr char kLayerURL[] = "coui://UIGameface/js/common/components/Layer.js";
constexpr char kHUDURL[] = "coui://UIGameface/js/project/modules/HUD/HUD.js";
constexpr char kCameraURL[] = "coui://UIGameface/js/project/modules/toolsPanel/cameraSettingsPanel/CameraSettingsPanel.js";
constexpr char kCreditsURL[] = "coui://UIGameface/js/project/modules/credits/CreditsModule.js";
constexpr std::string_view kDialogSuffix="/DialogApp.js";
constexpr char kPrefix[] = "coui://UIGameface/pc2vr/";
constexpr char kAppend[] = "\nimport {installVRMenu} from '/pc2vr/VRMenu.js';\ninstallVRMenu(_OptionsMenuBase, preact, Tab);\n";
bool LoadEmbeddedScript() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&g_script), &module)) return false;
    const auto resource=FindResourceW(module,MAKEINTRESOURCEW(PC2VR_MENU_SCRIPT_RESOURCE),MAKEINTRESOURCEW(10));
    const auto size=resource?SizeofResource(module,resource):0;
    const auto loaded=resource?LoadResource(module,resource):nullptr;
    const auto* bytes=loaded?static_cast<const char*>(LockResource(loaded)):nullptr;
    if (!bytes || !size || size>=256*1024) return false;
    g_script.assign(bytes,size);
    return true;
}
constexpr char kCreditsAppend[] = R"(
// Native credits prompts are labels. Give them the same actions as the
// native Cancel and left-click controls when pointed at with the mouse.
(() => {
    const render = Credits.prototype.render;
    Credits.prototype.render = function(...args) {
        const tree = render.apply(this, args);
        const prompts = tree.children.find(child => child?.attributes?.className === 'Credits_prompts');
        if (prompts?.children?.length === 2) {
            const [back, speed] = prompts.children;
            prompts.children = [
                preact.h('div', {style: {pointerEvents: 'auto', cursor: 'pointer'}, onClick: () => this.props.onClose()}, back),
                preact.h('div', {style: {pointerEvents: 'auto', cursor: 'pointer'}, onClick: event => {
                    const content = event.currentTarget.closest('.Credits_root')?.querySelector('.Credits_content');
                    content?.dispatchEvent(new MouseEvent('mousedown', {button: MouseButton.Left}));
                }}, speed)
            ];
        }
        return tree;
    };
})();
)";
constexpr char kPauseAppend[] = R"(
// PC2VR: mirror the native pause component lifecycle; no UI polling or surface.
(() => {
    let requestId=0, sequence=0;
    const nonce=Date.now()+'-'+Math.random();
    function rpc(path) {
        return new Promise((resolve,reject) => {
            const r=new XMLHttpRequest();
            r.open('GET','/pc2vr/'+path+(path.includes('?')?'&':'?')+'r='+nonce+'-'+(++requestId),true);
            r.timeout=5000;
            r.onload=()=>{try{const v=JSON.parse(r.responseText);if(!v.ok)throw Error('pause context');resolve(v);}catch(e){reject(e);}};
            r.onerror=r.ontimeout=()=>reject(Error('pause context'));r.send();
        });
    }
    const owner=rpc('pause-owner.json').then(v=>v.owner);
    function notify(visible) {
        const seq=++sequence;
        owner.then(id=>rpc('pause-state.json?owner='+id+'&seq='+seq+'&visible='+(visible===true?1:0))).catch(()=>{});
    }
    const p=GameMenuModule.prototype;
    const mount=p.componentDidMount,update=p.componentWillReceiveProps,unmount=p.componentWillUnmount;
    p.componentDidMount=function(...args){const v=mount?.apply(this,args);notify(this.props.visible);return v;};
    p.componentWillReceiveProps=function(props,...args){const v=update?.call(this,props,...args);notify(props.visible);return v;};
    p.componentWillUnmount=function(...args){try{return unmount?.apply(this,args);}finally{notify(false);}};
})();
)";

std::string PauseOwner() {
    std::lock_guard<std::mutex> lock(g_pauseMutex);
    ++g_pauseOwner;g_pauseSequence=0;g_pauseVisible.store(false,std::memory_order_release);
    return "{\"ok\":true,\"owner\":"+std::to_string(g_pauseOwner)+"}";
}
std::string PauseState(std::string_view query) {
    uint64_t owner{},sequence{},visible{};unsigned seen{};
    while(!query.empty()) {
        const auto end=query.find('&');const auto part=query.substr(0,end);const auto equal=part.find('=');
        if(equal==std::string_view::npos)return "{\"ok\":false}";
        const auto key=part.substr(0,equal),value=part.substr(equal+1);
        uint64_t* target=nullptr;unsigned bit{};
        if(key=="owner"){target=&owner;bit=1;}else if(key=="seq"){target=&sequence;bit=2;}else if(key=="visible"){target=&visible;bit=4;}
        else if(key!="r")return "{\"ok\":false}";
        if(target){
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),*target);
            if((seen&bit) || parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())return "{\"ok\":false}";
            seen|=bit;
        }
        if(end==std::string_view::npos)break;query.remove_prefix(end+1);
    }
    if(seen!=7 || !owner || !sequence || visible>1)return "{\"ok\":false}";
    std::lock_guard<std::mutex> lock(g_pauseMutex);
    if(owner==g_pauseOwner && sequence>g_pauseSequence){g_pauseSequence=sequence;g_pauseVisible.store(visible!=0,std::memory_order_release);}
    return "{\"ok\":true}";
}

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
    if (!status && !pending.incremental && pending.bytes && pending.size && pending.size < 512*1024) {
        const std::string_view source(pending.bytes,static_cast<size_t>(pending.size));
        const bool pause=pending.pause && source.find("export class GameMenuModule extends preact.Component")!=std::string_view::npos &&
            source.find("Engine.sendEvent('PauseMenuVisibleChanged', newProps.visible)")!=std::string_view::npos;
        const bool options=!pending.pause && source.find("class _OptionsMenuBase extends preact.Component")!=std::string_view::npos &&
            source.find("export const OptionsMenuBase = Focusable.decorate(_OptionsMenuBase)")!=std::string_view::npos;
        const bool info=pending.info && source.find("export class InfoPanelModule extends preact.Component")!=std::string_view::npos &&
            source.find("Input.createGameOwnerControlContext('infoPanel', ControlContextPriority.SidePanel")!=std::string_view::npos &&
            source.find("Engine.sendEvent('InfoPanelCancel')")!=std::string_view::npos;
        const bool dialog=pending.dialog && source.find("class _DialogApp extends preact.Component")!=std::string_view::npos &&
            source.find("Input.createGlobalPlayerControlContext('UI_Dialog', ControlContextPriority.Dialog")!=std::string_view::npos &&
            source.find("Engine.addListener('Show', this.OnShow)")!=std::string_view::npos;
        const bool popup=pending.popup && source.find("class DropDownSkin extends preact.Component")!=std::string_view::npos &&
            source.find("Focus.getFocusable(this._scrollPaneFK)")!=std::string_view::npos &&
            source.find("className: 'DropDown_menuContainer'")!=std::string_view::npos && g_rideClicks.Running();
        const bool build=pending.build && source.find("class _Browser extends preact.Component")!=std::string_view::npos &&
            source.find("id: 'browserRoot'")!=std::string_view::npos && source.find("Engine.sendEvent('SelectTool', 'paths')")!=std::string_view::npos &&
            source.find("export const Browser = FocusableComponent.decorateEx(_Browser)")!=std::string_view::npos && g_buildClicks.Running();
        const bool layer=pending.layer && source.find("export class Layer extends preact.Component")!=std::string_view::npos &&
            source.find("this._childElements[i] = preact.render(children[i], this._layerElement")!=std::string_view::npos && g_buildClicks.Running();
        const bool dashboard=pending.dashboard && source.find("class _HUD extends preact.Component")!=std::string_view::npos &&
            source.find("export const HUD = Focusable.decorate(_HUD)")!=std::string_view::npos &&
            source.find("NotificationsDataModel.setupListeners()")!=std::string_view::npos &&
            source.find("preact.h(StatusBar,")!=std::string_view::npos && source.find("preact.h(ObjectivesModule,")!=std::string_view::npos && g_dashboardClicks.Running();
        const bool camera=pending.camera && source.find("class _CameraSettingsPanel extends preact.Component")!=std::string_view::npos &&
            source.find("DataStoreUtil.bindDSPropsToState(this, ['cameraSettings']")!=std::string_view::npos &&
            source.find("className: 'CameraSettings_root'")!=std::string_view::npos && g_cameraClicks.Running();
        const bool credits=pending.credits && source.find("export class Credits extends preact.Component")!=std::string_view::npos &&
            source.find("className: 'Credits_prompts'")!=std::string_view::npos && source.find("onMouseDown: this.onMDownRoot")!=std::string_view::npos;
        if(!pause && !options && !info && !dialog && !popup && !build && !layer && !dashboard && !camera && !credits){g_finish(response,status);return;}
        const char* append=credits?kCreditsAppend:camera?detail::kCameraAppend:dashboard?detail::kDashboardAppend:build?detail::kBuildAppend:layer?detail::kBuildLayerAppend:popup?detail::kRidePopupAppend:dialog?detail::kDialogAppend:info?detail::kInfoAppend:pause?kPauseAppend:kAppend;
        const size_t length=credits?sizeof(kCreditsAppend)-1:camera?sizeof(detail::kCameraAppend)-1:dashboard?sizeof(detail::kDashboardAppend)-1:build?sizeof(detail::kBuildAppend)-1:layer?sizeof(detail::kBuildLayerAppend)-1:popup?sizeof(detail::kRidePopupAppend)-1:dialog?sizeof(detail::kDialogAppend)-1:info?sizeof(detail::kInfoAppend)-1:pause?sizeof(kPauseAppend)-1:sizeof(kAppend)-1;
        const bool ride=(info && g_rideClicks.Running()) || camera;
        const size_t rideLength=ride?sizeof(detail::kRideAppend)-1:0;
        const size_t notificationLength=dashboard?sizeof(detail::kNotificationAppend)-1:0;
        const auto startupUI=ui::Settings();
        const std::string_view buildPrefix=build?(startupUI.on && startupUI.buildWheel && g_buildAvailable.load(std::memory_order_acquire)?"\nconst VRBuildInitiallyEnabled=true;\n":"\nconst VRBuildInitiallyEnabled=false;\n"):"";
        const uint64_t size=pending.size+length+rideLength+notificationLength+buildPrefix.size();
        // Proven shipping GetSpace/Finish path; GetSpace preserves the original bytes.
        if (auto* bytes=static_cast<char*>(g_space(response,size))) {
            if(ride)memcpy(bytes+pending.size,detail::kRideAppend,rideLength);
            if(dashboard)memcpy(bytes+pending.size,detail::kNotificationAppend,notificationLength);
            if(!buildPrefix.empty())memcpy(bytes+pending.size+rideLength,buildPrefix.data(),buildPrefix.size());
            memcpy(bytes+pending.size+rideLength+notificationLength+buildPrefix.size(),append,length);
            LOG(credits?"motion: clickable credits prompts loaded":camera?"motion: native rider-menu bridge loaded":dashboard?"motion: native wrist dashboard bridge loaded":build?"motion: native build-wheel bridge loaded":layer?"motion: native browser filter ownership bridge loaded":popup?"motion: native ride dropdown lifecycle bridge loaded":dialog?"motion: native dialog visibility bridge loaded":info?"motion: native selection-panel visibility bridge loaded":pause?"motion: native pause visibility bridge loaded":"VR menu: native settings extension loaded");
        }
    }
    g_finish(response,status); // response is invalid after this call
}
bool TrackModule(void* response,bool pause=false,bool info=false,bool dialog=false,bool popup=false,bool build=false,bool layer=false,bool dashboard=false,bool camera=false,bool credits=false) {
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
        slot={response,0,nullptr,false,pause,info,dialog,popup,build,layer,dashboard,camera,credits}; ++g_pendingCount; return true;
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
        << ",\"StereoMode\":" << g_stereoMode
        << ",\"Foveated\":" << (vrs::Enabled()?1:0)
        << ",\"renderWidth\":" << resolution.width << ",\"renderHeight\":" << resolution.height
        << ",\"UIWidth\":" << p.widthDeg << ",\"UIDistance\":" << std::lround(p.distanceM*100.f)
        << ",\"UIHeight\":" << p.heightDeg << ",\"UIHorizontal\":" << p.horizontalDeg
        << ",\"UIFollowHmd\":" << (p.followHmd?1:0)
        << ",\"UIRidePanel\":" << (p.ridePanel?1:0) << ",\"UIRideWidth\":" << std::lround(p.rideWidthM*100.f) << ",\"UIDashboard\":" << (p.dashboard?1:0)
        << ",\"UIBuildWheel\":" << (p.buildWheel?1:0)
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
        else if (key=="StereoMode") {g_stereoMode=value; xr::RequestStereoMode(static_cast<xr::StereoMode>(value));}
        else if (key=="Foveated") vrs::SetEnabled(value!=0);
        else {
            auto p=ui::Settings();
            if (key=="UIWidth") p.widthDeg=static_cast<float>(value);
            if (key=="UIDistance") p.distanceM=value/100.f;
            if (key=="UIHeight") p.heightDeg=static_cast<float>(value);
            if (key=="UIHorizontal") p.horizontalDeg=static_cast<float>(value);
            if (key=="UIFollowHmd") p.followHmd=value!=0;
            if (key=="UIRidePanel") p.ridePanel=value!=0;
            if (key=="UIBuildWheel") p.buildWheel=value!=0;
            if (key=="UIDashboard") p.dashboard=value!=0;
            if (key=="UIRideWidth") p.rideWidthM=value*.01f;
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
void RespondPanel(void* response,const ui::RideClick* click,bool camera){
    // Formatting happens only for a physical press or a lifecycle cancellation.
    const auto panel=SelectionPanelState();const auto settings=ui::Settings();const auto now=GetTickCount64();
    if(!click || !click->window || GetForegroundWindow()!=reinterpret_cast<HWND>(click->window) ||
       now<click->time || now-click->time>250 || !panel.known || panel.blocked || PauseVisible() ||
       !settings.on || !settings.ridePanel){Respond(response,"{\"ok\":true,\"cancelled\":true}");return;}
    {std::lock_guard<std::mutex> lock(g_ridePressMutex);(camera?g_cameraPress:g_ridePress)=*click;}
    std::ostringstream out;out.imbue(std::locale::classic());
    out << "{\"ok\":true,\"owner\":" << click->owner << ",\"seq\":" << click->sequence
        << ",\"x\":" << click->x << ",\"y\":" << click->y << '}';
    Respond(response,out.str());
}
void RespondRide(void* response,const ui::RideClick* click){RespondPanel(response,click,false);}
void RespondCamera(void* response,const ui::RideClick* click){RespondPanel(response,click,true);}
std::string RideOwner(){
    g_rideClicks.Cancel();const auto owner=g_selectionPanel.NewOwner();g_ridePanel.NewOwner(owner);
    return "{\"ok\":true,\"owner\":"+std::to_string(owner)+"}";
}
bool RideWait(void* response,std::string_view query){
    const auto end=query.find('&');const auto part=query.substr(0,end);
    if(part.substr(0,6)!="owner=")return false;
    uint64_t owner{};const auto number=part.substr(6);
    const auto parsed=std::from_chars(number.data(),number.data()+number.size(),owner);
    const auto layout=g_ridePanel.Read();
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !owner ||
       owner!=layout.owner || !layout.visible)return false;
    return g_rideClicks.Wait(response,owner,GetTickCount64());
}
std::string CameraOwner(){
    g_cameraClicks.Cancel();const auto owner=++g_cameraOwner;g_cameraPanel.NewOwner(owner);
    return "{\"ok\":true,\"owner\":"+std::to_string(owner)+"}";
}
bool CameraWait(void* response,std::string_view query){
    const auto part=query.substr(0,query.find('&'));if(part.substr(0,6)!="owner=")return false;
    uint64_t owner{};const auto number=part.substr(6);
    const auto parsed=std::from_chars(number.data(),number.data()+number.size(),owner);const auto layout=g_cameraPanel.Read();
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !owner || owner!=layout.owner || !layout.visible)return false;
    return g_cameraClicks.Wait(response,owner,GetTickCount64());
}
bool RideValidate(std::string_view query,bool camera=false){
    const auto part=query.substr(0,query.find('&'));if(part.substr(0,6)!="owner=")return false;
    const auto number=part.substr(6);uint64_t owner{};
    const auto parsed=std::from_chars(number.data(),number.data()+number.size(),owner);
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !owner)return false;
    ui::RideClick press;{std::lock_guard<std::mutex> lock(g_ridePressMutex);press=camera?g_cameraPress:g_ridePress;}
    const auto now=GetTickCount64(),input=g_rideInput.load(std::memory_order_acquire),time=input>>1;
    const auto panel=SelectionPanelState();const auto layout=(camera?g_cameraPanel:g_ridePanel).Read();const auto settings=ui::Settings();
    return (input&1) && now>=time && now-time<=250 && now>=press.time && now-press.time<=250 &&
        press.owner==owner && layout.owner==owner && layout.visible && panel.known && !panel.blocked &&
        !PauseVisible() && settings.on && settings.ridePanel && press.window &&
        GetForegroundWindow()==reinterpret_cast<HWND>(press.window);
}
bool BuildEnabled(){
    const auto settings=ui::Settings();
    return settings.on && settings.buildWheel && g_buildAvailable.load(std::memory_order_acquire) && g_buildClicks.Running();
}
void RespondBuild(void* response,const ui::RideClick* click){
    const bool enabled=BuildEnabled();
    const auto panel=SelectionPanelState();const auto now=GetTickCount64();
    if(!click || !enabled || !click->window || now<click->time || now-click->time>250 || !panel.known || panel.blocked || PauseVisible() ||
       GetForegroundWindow()!=reinterpret_cast<HWND>(click->window)){
        Respond(response,enabled?"{\"ok\":true,\"cancelled\":true,\"enabled\":true}":"{\"ok\":true,\"cancelled\":true,\"enabled\":false}");return;
    }
    {std::lock_guard<std::mutex> lock(g_buildPressMutex);g_buildPress=*click;}
    std::ostringstream out;out.imbue(std::locale::classic());
    out << "{\"ok\":true,\"enabled\":true,\"owner\":" << click->owner << ",\"seq\":" << click->sequence << ",\"x\":" << click->x << ",\"y\":" << click->y << ",\"scroll\":" << click->scroll << '}';
    Respond(response,out.str());
}
std::string BuildOwner(){
    g_buildClicks.Cancel();const auto owner=++g_buildOwner;g_buildPanel.NewOwner(owner);
    return "{\"ok\":true,\"owner\":"+std::to_string(owner)+",\"enabled\":"+(BuildEnabled()?"true}":"false}");
}
bool BuildWait(void* response,std::string_view query){
    const auto part=query.substr(0,query.find('&'));if(part.substr(0,6)!="owner=")return false;
    uint64_t owner{};const auto number=part.substr(6);const auto parsed=std::from_chars(number.data(),number.data()+number.size(),owner);
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !owner || owner!=g_buildOwner.load())return false;
    return g_buildClicks.Wait(response,owner,GetTickCount64());
}
bool BuildValidate(std::string_view query){
    const auto part=query.substr(0,query.find('&'));if(part.substr(0,6)!="owner=")return false;
    uint64_t owner{};const auto number=part.substr(6);const auto parsed=std::from_chars(number.data(),number.data()+number.size(),owner);
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !owner)return false;
    ui::RideClick press;{std::lock_guard<std::mutex> lock(g_buildPressMutex);press=g_buildPress;}
    const auto now=GetTickCount64(),input=g_rideInput.load(std::memory_order_acquire),time=input>>1;
    const auto panel=SelectionPanelState();const auto layout=g_buildPanel.Read();
    return BuildEnabled() && (input&1) && now>=time && now-time<=250 && now>=press.time && now-press.time<=250 &&
        press.owner==owner && layout.owner==owner && layout.visible && panel.known && !panel.blocked && !PauseVisible() &&
        press.window && GetForegroundWindow()==reinterpret_cast<HWND>(press.window);
}
bool DashboardEnabled(){
    const auto settings=ui::Settings();
    return settings.on && settings.dashboard && settings.buildWheel && g_buildAvailable.load(std::memory_order_acquire) && g_dashboardClicks.Running();
}
void RespondDashboard(void* response,const ui::RideClick* click){
    const bool enabled=DashboardEnabled();
    const auto panel=SelectionPanelState();const auto now=GetTickCount64();
    if(!click || !enabled || !click->window || now<click->time || now-click->time>250 || !panel.known || panel.blocked || PauseVisible() ||
       GetForegroundWindow()!=reinterpret_cast<HWND>(click->window)){
        Respond(response,enabled?"{\"ok\":true,\"cancelled\":true,\"enabled\":true}":"{\"ok\":true,\"cancelled\":true,\"enabled\":false}");return;
    }
    {std::lock_guard<std::mutex> lock(g_dashboardPressMutex);g_dashboardPress=*click;}
    std::ostringstream out;out.imbue(std::locale::classic());
    out << "{\"ok\":true,\"enabled\":true,\"owner\":" << click->owner << ",\"seq\":" << click->sequence << ",\"x\":" << click->x << ",\"y\":" << click->y << ",\"scroll\":" << click->scroll << '}';
    Respond(response,out.str());
}
std::string DashboardOwner(){
    g_dashboardClicks.Cancel();const auto owner=++g_dashboardOwner;g_dashboardPanel.NewOwner(owner);
    return "{\"ok\":true,\"owner\":"+std::to_string(owner)+",\"enabled\":"+(DashboardEnabled()?"true}":"false}");
}
bool DashboardWait(void* response,std::string_view query){
    const auto part=query.substr(0,query.find('&'));if(part.substr(0,6)!="owner=")return false;
    uint64_t owner{};const auto number=part.substr(6);const auto parsed=std::from_chars(number.data(),number.data()+number.size(),owner);
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !owner || owner!=g_dashboardOwner.load())return false;
    return g_dashboardClicks.Wait(response,owner,GetTickCount64());
}
bool DashboardValidate(std::string_view query){
    const auto part=query.substr(0,query.find('&'));if(part.substr(0,6)!="owner=")return false;
    uint64_t owner{};const auto number=part.substr(6);const auto parsed=std::from_chars(number.data(),number.data()+number.size(),owner);
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !owner)return false;
    ui::RideClick press;{std::lock_guard<std::mutex> lock(g_dashboardPressMutex);press=g_dashboardPress;}
    const auto now=GetTickCount64(),input=g_rideInput.load(std::memory_order_acquire),time=input>>1;
    const auto panel=SelectionPanelState();const auto layout=g_dashboardPanel.Read();
    return DashboardEnabled() && (input&1) && now>=time && now-time<=250 && now>=press.time && now-press.time<=250 &&
        press.owner==owner && layout.owner==owner && layout.visible && panel.known && !panel.blocked && !PauseVisible() &&
        press.window && GetForegroundWindow()==reinterpret_cast<HWND>(press.window);
}
void Request(void* self, const void* request, void* response) {
    const auto url=Method<const char*(*)(const void*)>(request,2)(request);
    if (!url) {g_request(self,request,response);return;}
    if (!std::strncmp(url,kPrefix,sizeof(kPrefix)-1)) {
        const std::string_view path(url+sizeof(kPrefix)-1);
        const auto operation=path.substr(0,path.find('?'));
        const auto dashboardQuery=path.find('?')==std::string_view::npos?std::string_view{}:path.substr(path.find('?')+1);
        if (path=="VRMenu.js") Respond(response,g_script);
        else if(g_selectionContext && operation=="camera-owner.json")Respond(response,CameraOwner());
        else if(g_selectionContext && operation=="camera-layout.json"){
            const bool ok=g_cameraPanel.Update(dashboardQuery);
            if(ok && !g_cameraPanel.Read().visible)g_cameraClicks.Cancel();
            Respond(response,ok?"{\"ok\":true}":"{\"ok\":false}");
        }
        else if(g_selectionContext && operation=="camera-click.json"){
            if(!CameraWait(response,dashboardQuery))Respond(response,"{\"ok\":true,\"cancelled\":true}");
        }
        else if(g_selectionContext && operation=="camera-validate.json")Respond(response,RideValidate(dashboardQuery,true)?"{\"ok\":true,\"allow\":true}":"{\"ok\":true,\"allow\":false}");
        else if(g_selectionContext && operation=="dashboard-owner.json")Respond(response,DashboardOwner());
        else if(g_selectionContext && operation=="dashboard-layout.json")Respond(response,g_dashboardPanel.Update(dashboardQuery)?"{\"ok\":true}":"{\"ok\":false}");
        else if(g_selectionContext && operation=="dashboard-click.json"){
            if(!DashboardWait(response,dashboardQuery))Respond(response,"{\"ok\":false}");
        }
        else if(g_selectionContext && operation=="dashboard-validate.json")Respond(response,DashboardValidate(dashboardQuery)?"{\"ok\":true,\"allow\":true}":"{\"ok\":true,\"allow\":false}");
        else if(g_selectionContext && path.substr(0,path.find('?'))=="build-owner.json")Respond(response,BuildOwner());
        else if(g_selectionContext && path.substr(0,18)=="build-layout.json?")Respond(response,g_buildPanel.Update(path.substr(18))?"{\"ok\":true}":"{\"ok\":false}");
        else if(g_selectionContext && path.substr(0,17)=="build-click.json?"){
            if(!BuildWait(response,path.substr(17)))Respond(response,"{\"ok\":false}");
        }
        else if(g_selectionContext && path.substr(0,20)=="build-validate.json?")Respond(response,BuildValidate(path.substr(20))?"{\"ok\":true,\"allow\":true}":"{\"ok\":true,\"allow\":false}");
        else if(g_motionContext && path.substr(0,path.find('?'))=="pause-owner.json")Respond(response,PauseOwner());
        else if(g_motionContext && path.substr(0,17)=="pause-state.json?")Respond(response,PauseState(path.substr(17)));
        else if(g_selectionContext && path.substr(0,path.find('?'))=="selection-owner.json")
            Respond(response,RideOwner());
        else if(g_selectionContext && path.substr(0,17)=="ride-layout.json?"){
            const bool ok=g_ridePanel.Update(path.substr(17));
            if(ok && !g_ridePanel.Read().visible)g_rideClicks.Cancel();
            Respond(response,ok?"{\"ok\":true}":"{\"ok\":false}");
        }
        else if(g_selectionContext && path.substr(0,16)=="ride-click.json?"){
            if(!RideWait(response,path.substr(16)))Respond(response,"{\"ok\":false}");
        }
        else if(g_selectionContext && path.substr(0,19)=="ride-validate.json?")
            Respond(response,RideValidate(path.substr(19))?"{\"ok\":true,\"allow\":true}":"{\"ok\":true,\"allow\":false}");
        else if(g_selectionContext && path.substr(0,21)=="selection-state.json?")
            Respond(response,g_selectionPanel.Update(path.substr(21))?"{\"ok\":true}":"{\"ok\":false}");
        else if(g_selectionContext && path.substr(0,path.find('?'))=="dialog-owner.json")
            Respond(response,"{\"ok\":true,\"owner\":"+std::to_string(g_dialogPanel.NewOwner())+"}");
        else if(g_selectionContext && path.substr(0,18)=="dialog-state.json?")
            Respond(response,g_dialogPanel.Update(path.substr(18))?"{\"ok\":true}":"{\"ok\":false}");
        else if (path.substr(0,10)=="state.json") Respond(response,StateJSON());
        else if (path.substr(0,13)=="recenter.json") {xr::RequestRecenter();Respond(response,"{\"ok\":true}");}
        else if (path.substr(0,path.find('?'))=="recalibrate.json") Respond(response,RecalibrateCheeky());
        else if (path.substr(0,9)=="set.json?") Respond(response,Apply(path.substr(9)));
        else Respond(response,"{\"ok\":false,\"error\":\"Unknown VR operation\"}");
        return;
    }
    if (g_optionsEnabled && !std::strcmp(url,kBaseURL)) TrackModule(response);
    else if(g_motionContext && !std::strcmp(url,kPauseURL))TrackModule(response,true);
    else if(g_selectionContext && !std::strcmp(url,kCreditsURL))TrackModule(response,false,false,false,false,false,false,false,false,true);
    else if(g_selectionContext && !std::strcmp(url,kInfoURL))TrackModule(response,false,true);
    else if(g_selectionContext && !std::strcmp(url,kCameraURL))TrackModule(response,false,false,false,false,false,false,false,true);
    else if(g_selectionContext && !std::strcmp(url,kDropDownURL))TrackModule(response,false,false,false,true);
    else if(g_selectionContext && !std::strcmp(url,kHUDURL))TrackModule(response,false,false,false,false,false,false,true);
    else if(g_selectionContext && !std::strcmp(url,kBuildURL))TrackModule(response,false,false,false,false,true);
    else if(g_selectionContext && !std::strcmp(url,kLayerURL))TrackModule(response,false,false,false,false,false,true);
    else if(g_selectionContext){
        const std::string_view path(url);
        if(path.size()>=kDialogSuffix.size() && path.substr(path.size()-kDialogSuffix.size())==kDialogSuffix)
            TrackModule(response,false,false,true);
    }
    g_request(self,request,response);
}
void* DestroySystem(void* system,unsigned flags){
    if(g_rideSystem.load(std::memory_order_acquire)==system){
        g_cameraClicks.Stop();g_dashboardClicks.Stop();g_buildClicks.Stop();g_rideClicks.Stop();g_rideSystem.store(nullptr,std::memory_order_release);
    }
    return g_destroySystem(system,flags);
}
void* CreateSystem(void* library, const void* settings) {
    auto* handler=*reinterpret_cast<void* const*>(static_cast<const char*>(settings)+0x10);
    if (handler && !g_request) {
        auto** table=*reinterpret_cast<void***>(handler);
        Replace(&table[1],reinterpret_cast<void*>(&Request),reinterpret_cast<void**>(&g_request));
    }
    auto* system=g_system(library,settings);
    if(system && g_selectionContext){
        std::lock_guard<std::mutex> lock(g_hookMutex);
        auto** table=*reinterpret_cast<void***>(system);
        // Shipping1.52 ctor51d2a0 sets6e2520; slots0/1 are Get/SetId,
        // slot2 is the scalar destructor51dd70 (EDX flags preserved).
        auto* base=reinterpret_cast<char*>(g_cohtml);
        if(table==reinterpret_cast<void**>(base+0x6e2520) &&
           (table[2]==base+0x51dd70 || table[2]==reinterpret_cast<void*>(&DestroySystem)) &&
           table[0]==base+0x51e010 && table[1]==base+0x51e020 &&
           (!g_destroySystem || table[2]==reinterpret_cast<void*>(&DestroySystem))){
            if(g_destroySystem || Replace(&table[2],reinterpret_cast<void*>(&DestroySystem),reinterpret_cast<void**>(&g_destroySystem))){
                if(!g_rideSystem.load(std::memory_order_acquire)){
                    g_rideSystem.store(system,std::memory_order_release);g_rideClicks.Start();g_cameraClicks.Start();g_buildClicks.Start();g_dashboardClicks.Start();
                    LOG("motion: floating ride-panel resource lifetime guarded");
                }
            }
        }
    }
    return system;
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
bool PauseVisible() {return g_pauseVisible.load(std::memory_order_acquire);}
motion::SelectionPanel SelectionPanelState(){
    auto panel=g_selectionPanel.Read();const auto dialog=g_dialogPanel.Read();
    panel.known=panel.known && dialog.known;
    panel.blocked=!dialog.known || dialog.visible;
    panel.canClose=panel.canClose && !panel.blocked;
    return panel;
}
ui::RideLayout RidePanelLayout(){return g_rideClicks.Running()?g_ridePanel.Read():ui::RideLayout{};}
ui::RideLayout CameraPanelLayout(){return g_cameraClicks.Running()?g_cameraPanel.Read():ui::RideLayout{};}
ui::RideLayout BuildPanelLayout(){return BuildEnabled()?g_buildPanel.Read():ui::RideLayout{};}
ui::RideLayout DashboardLayout(){return DashboardEnabled()?g_dashboardPanel.Read():ui::RideLayout{};}
void BuildAvailability(bool ready){if(g_buildAvailable.exchange(ready,std::memory_order_acq_rel)!=ready){g_buildClicks.Cancel();g_dashboardClicks.Cancel();}}
void BuildConfigurationChanged(){g_buildClicks.Cancel();g_dashboardClicks.Cancel();}
bool PostBuildClick(const ui::RideClick& click){
    const auto panel=SelectionPanelState();const auto layout=g_buildPanel.Read();
    if(!BuildEnabled() || !click.window || GetForegroundWindow()!=reinterpret_cast<HWND>(click.window) || !panel.known || panel.blocked ||
       !layout.visible || click.owner!=layout.owner || click.sequence!=layout.sequence)return false;
    return g_buildClicks.Post(click);
}
bool PostDashboardClick(const ui::RideClick& click){
    const auto panel=SelectionPanelState();const auto layout=g_dashboardPanel.Read();
    if(!DashboardEnabled() || !click.window || GetForegroundWindow()!=reinterpret_cast<HWND>(click.window) || !panel.known || panel.blocked ||
       !layout.visible || click.owner!=layout.owner || click.sequence!=layout.sequence)return false;
    return g_dashboardClicks.Post(click);
}
bool PostRideClick(const ui::RideClick& click){
    const auto panel=SelectionPanelState();const auto layout=g_ridePanel.Read();
    if(!click.window || GetForegroundWindow()!=reinterpret_cast<HWND>(click.window) ||
       !panel.known || panel.blocked || !layout.visible || click.owner!=layout.owner || click.sequence!=layout.sequence)return false;
    return g_rideClicks.Post(click);
}
bool PostCameraClick(const ui::RideClick& click){
    const auto panel=SelectionPanelState();const auto layout=g_cameraPanel.Read();
    if(!click.window || GetForegroundWindow()!=reinterpret_cast<HWND>(click.window) ||
       !panel.known || panel.blocked || !layout.visible || click.owner!=layout.owner || click.sequence!=layout.sequence)return false;
    return g_cameraClicks.Post(click);
}
void RideInputSnapshot(bool active,uint64_t time){g_rideInput.store((time<<1)|(active?1u:0u),std::memory_order_release);}
void Install(const wchar_t* directory) {
    g_ini=std::wstring(directory)+L"\\pc2vr.ini";
    g_optionsEnabled=GetPrivateProfileIntW(L"UI",L"VRMenu",1,g_ini.c_str())!=0;
    g_motionContext=GetPrivateProfileIntW(L"Controls",L"MotionControllers",0,g_ini.c_str())==1;
    g_selectionContext=g_motionContext && GetPrivateProfileIntW(L"Experiments",L"WorldLaser",0,g_ini.c_str())==1;
    if(!g_optionsEnabled && !g_motionContext)return;
    g_scale=static_cast<int>(GetPrivateProfileIntW(L"Resolution",L"ScalePercent",100,g_ini.c_str()));
    g_expandedCoverage=GetPrivateProfileIntW(L"Resolution",L"ExpandedCoverage",0,g_ini.c_str())==1?1:0;
    g_stereoMode=static_cast<int>(xr::ReadStereoMode(
        static_cast<int>(GetPrivateProfileIntW(L"Stereo",L"Mode",-1,g_ini.c_str())),
        static_cast<int>(GetPrivateProfileIntW(L"Stereo",L"AlternateEyes",0,g_ini.c_str()))));
    if (!LoadEmbeddedScript()) {
        g_optionsEnabled=false;
        LOG("VR menu: embedded script unavailable; native settings unchanged");
        if (!g_motionContext) return;
    }
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
