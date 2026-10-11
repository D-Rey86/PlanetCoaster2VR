// Tests the real bridge's buffer ownership, cache handling, bounds and persistence.
#include "../../Loader/src/vr_menu.cpp"
#include "../../Loader/src/panel_geometry.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <iterator>

namespace pc2vr::ui {
PanelSettings testPanel;
PanelSettings Settings() {return testPanel;}
void ApplySettings(const PanelSettings& settings) {testPanel=settings;}
}
namespace pc2vr::xr {
int testPercent{};
bool testCoverage{};
void RequestResolutionScale(int percent) {testPercent=percent;}
void RequestExpandedCoverage(bool on) {testCoverage=on;}
ResolutionInfo ResolutionStatus() {return {2096,2096};}
void RequestRecenter() {}
}
namespace pc2vr::log {void Write(const char*,...) {}}
namespace pc2vr::vrs {
bool testEnabled{};
bool Enabled() {return testEnabled;}
void SetEnabled(bool on) {testEnabled=on;}
}

namespace {
void Require(bool condition,const char* what) {if(!condition){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
bool Near(float a,float b) {return std::fabs(a-b)<1e-5f;}
struct Response {std::vector<char> bytes;std::string final;unsigned status{};};
unsigned optimized{},progress{},finished{};
void* Allocate(void* object,uint64_t size) {auto& r=*static_cast<Response*>(object);r.bytes.resize(static_cast<size_t>(size));return r.bytes.data();}
void Complete(void* object,unsigned status) {auto& r=*static_cast<Response*>(object);r.final.assign(r.bytes.begin(),r.bytes.end());r.bytes.clear();r.status=status;++finished;}
void Cache(void*,const void*) {++optimized;}
void Partial(void*) {++progress;}
void Register(Response& response) {
    pc2vr::menu::g_pending[0]={&response,0,nullptr,false};pc2vr::menu::g_pendingCount=1;
}
std::string lastCommand;
bool acceptCommand=true;
unsigned commandCount{};
bool Snapshot(char* out,uint32_t size) {return strcpy_s(out,size,R"({"protocol":1,"ready":true,"settings":{"Enabled":true}})")==0;}
bool Command(const char* command) {lastCommand=command;++commandCount;return acceptCommand;}
}
int wmain(int argc,wchar_t** argv) {
    using namespace pc2vr::menu;
    Require(argc==3,"supply test-only ini path and privately extracted shipping settings module");g_ini=argv[1];
    g_space=Allocate;g_finish=Complete;g_optimized=Cache;g_progress=Partial;
    std::ifstream source(argv[2],std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(source)),std::istreambuf_iterator<char>());
    Require(original.size()>10000,"real shipping module fixture must be loaded");
    Response target;Register(target);
    auto* bytes=static_cast<char*>(Space(&target,original.size()/2));memcpy(bytes,original.data(),original.size()/2);
    bytes=static_cast<char*>(Space(&target,original.size()));memcpy(bytes,original.data(),original.size());
    Optimized(&target,nullptr);Require(optimized==0,"patched source must not reuse original compiled cache");
    Finish(&target,0);Require(target.final==original+kAppend,"append must retain every original byte across reallocations");
    Require(g_pendingCount==0 && finished==1,"retire response before invalidating Finish");
    Response ordinary;bytes=static_cast<char*>(Space(&ordinary,original.size()));memcpy(bytes,original.data(),original.size());
    Optimized(&ordinary,nullptr);Finish(&ordinary,0);
    Require(ordinary.final==original && optimized==1,"unrelated resource and optimized cache unchanged");
    Response failed;Register(failed);bytes=static_cast<char*>(Space(&failed,original.size()));memcpy(bytes,original.data(),original.size());Finish(&failed,1);
    Require(failed.final==original && failed.status==1,"failed load unchanged");
    Response incremental;Register(incremental);bytes=static_cast<char*>(Space(&incremental,original.size()));memcpy(bytes,original.data(),original.size());
    Progress(&incremental);Space(&incremental,original.size());Finish(&incremental,0);
    Require(incremental.final==original && progress==1,"incremental response fails closed without partial-module patch");
    Response unknown;Register(unknown);bytes=static_cast<char*>(Space(&unknown,4));memcpy(bytes,"test",4);Finish(&unknown,0);
    Require(unknown.final=="test","changed module layout fails closed");
    const auto rejected=[](std::string_view query){return Apply(query).find("\"ok\":false")!=std::string::npos;};
    Require(rejected("key=Resolution&value=0"),"reject zero size");
    Require(rejected("key=Resolution&value=201"),"reject excessive size");
    Require(rejected("key=Resolution&value=100junk"),"reject malformed integer");
    Require(rejected("key=UIHeight&value=NaN"),"reject non-finite input");
    Require(rejected("key=CenterPreset&value=13") && rejected("key=D3D12LowerHook&value=1"),"no model/hook override path");
    Require(rejected("key=defaults&value=1"),"no reset command path");
    Require(Apply("key=Resolution&value=75&r=7")=="{\"ok\":true}","valid resolution request");
    Require(pc2vr::xr::testPercent==75 && GetPrivateProfileIntW(L"Resolution",L"ScalePercent",0,g_ini.c_str())==75,"resolution persisted and requested on owner thread");
    Require(StateJSON().find("\"ExpandedCoverage\":0")!=std::string::npos,"coverage defaults to original behavior");
    Require(rejected("key=ExpandedCoverage&value=2") && rejected("key=ExpandedCoverage&value=-1"),"coverage accepts only0/1");
    Require(Apply("key=ExpandedCoverage&value=1")=="{\"ok\":true}" && pc2vr::xr::testCoverage,"expanded policy requested");
    Require(GetPrivateProfileIntW(L"Resolution",L"ExpandedCoverage",0,g_ini.c_str())==1 && StateJSON().find("\"ExpandedCoverage\":1")!=std::string::npos,"coverage persisted and reported");
    Require(Apply("key=ExpandedCoverage&value=0")=="{\"ok\":true}" && !pc2vr::xr::testCoverage,"original policy requested");
    Require(GetPrivateProfileIntW(L"Resolution",L"ExpandedCoverage",1,g_ini.c_str())==0 && pc2vr::xr::testPercent==75,"off persisted without changing resolution");
    pc2vr::ui::testPanel.on=false;pc2vr::ui::testPanel.markers3d=false;
    Require(Apply("key=UIHorizontal&value=-30")=="{\"ok\":true}","signed position");
    Require(pc2vr::ui::testPanel.horizontalDeg==-30 && !pc2vr::ui::testPanel.on && !pc2vr::ui::testPanel.markers3d,"unrelated panel and marker settings preserved");
    WritePrivateProfileStringW(L"Rendering",L"FoveatedInnerDeg",L"27",g_ini.c_str());
    WritePrivateProfileStringW(L"Rendering",L"FoveatedMiddleDeg",L"43",g_ini.c_str());
    g_nvidia=false;
    Require(rejected("key=Foveated&value=2") && rejected("key=Foveated&value=-1"),"foveation accepts only boolean values");
    Require(Apply("key=Foveated&value=1")=="{\"ok\":true}" && pc2vr::vrs::Enabled(),"foveation uses existing atomic toggle without NVIDIA gating");
    Require(GetPrivateProfileIntW(L"Rendering",L"Foveated",0,g_ini.c_str())==1 && StateJSON().find("\"Foveated\":1")!=std::string::npos,"saved foveation and reported live state agree");
    Require(Apply("key=Foveated&value=0")=="{\"ok\":true}" && !pc2vr::vrs::Enabled() && GetPrivateProfileIntW(L"Rendering",L"Foveated",1,g_ini.c_str())==0,"disable foveation and persist off");
    Require(GetPrivateProfileIntW(L"Rendering",L"FoveatedInnerDeg",0,g_ini.c_str())==27 && GetPrivateProfileIntW(L"Rendering",L"FoveatedMiddleDeg",0,g_ini.c_str())==43,"toggle preserves existing foveation radii");
    Require(pc2vr::ui::testPanel.horizontalDeg==-30 && !pc2vr::ui::testPanel.on && !pc2vr::ui::testPanel.markers3d && pc2vr::xr::testPercent==75,"foveation leaves UI and resolution unchanged");
    g_nvidia=false;Require(rejected("key=CheekyEnabled&value=1"),"AMD/no capability rejects optional NVIDIA write");
    Require(RecalibrateCheeky().find("\"ok\":false")!=std::string::npos && commandCount==0,"AMD/no capability rejects recalibration");
    g_nvidia=true;g_snapshot=Snapshot;g_command=Command;
    Require(Apply("key=CheekyWidth&value=19").find("false")!=std::string::npos,"reject values below Cheeky's real center clamp");
    Require(Apply("key=CheekyWidth&value=25")=="{\"ok\":true}" && lastCommand.find("Width=0.25\n")!=std::string::npos,"exact fractional Cheeky command");
    Require(Apply("key=CheekyPeripheralScale&value=20")=="{\"ok\":true}" && lastCommand.find("PeripheralDlaaScale=0.20\n")!=std::string::npos,"Cheeky peripheral lower bound");
    Require(Apply("key=CheekyCenterSupersampling&value=125")=="{\"ok\":true}" && lastCommand.find("CenterSupersampling=1.25\n")!=std::string::npos,"supersampling command conversion");
    Require(Apply("key=CheekyCenterMode&value=1")=="{\"ok\":true}" && lastCommand.find("CenterMode=1\n")!=std::string::npos,"runtime gaze selector");
    Require(rejected("key=CheekyCenterMode&value=2"),"simulated gaze is not a gameplay setting");
    Require(Apply("key=CheekyAlignedHeight&value=-5")=="{\"ok\":true}" && lastCommand.find("AlignedHeightOffset=-0.05\n")!=std::string::npos,"negative sub-unit aligned height");
    Require(Apply("key=CheekyManualHeight&value=-100")=="{\"ok\":true}" && lastCommand.find("HeightOffset=-1.00\n")!=std::string::npos,"manual height lower endpoint");
    Require(Apply("key=CheekyAlignedHeight&value=100")=="{\"ok\":true}" && lastCommand.find("AlignedHeightOffset=1.00\n")!=std::string::npos,"positive aligned height endpoint");
    Require(rejected("key=CheekyAlignedHeight&value=-101") && rejected("key=CheekyManualHeight&value=101"),"bounded signed positions");
    Require(Apply("key=CheekyRoundness&value=100")=="{\"ok\":true}" && lastCommand.find("Roundness=1.00\n")!=std::string::npos,"roundness upper endpoint");
    Require(Apply("key=CheekyTransition&value=30")=="{\"ok\":true}" && lastCommand.find("TransitionWidth=0.30\n")!=std::string::npos,"transition upper endpoint");
    Require(rejected("key=CheekyTransition&value=31"),"transition rejects above upstream maximum");
    Require(Apply("key=CheekyBorder&value=1")=="{\"ok\":true}" && lastCommand.find("AlignmentBorder=1\n")!=std::string::npos,"explicit preview toggle");
    Require(Apply("key=CheekyGazeSmoothing&value=20")=="{\"ok\":true}" && lastCommand.find("GazeSmoothingMs=20\n")!=std::string::npos,"milliseconds are not converted to fractions");
    Require(Apply("key=CheekyGazeHold&value=1000")=="{\"ok\":true}" && lastCommand.find("GazeHoldMs=1000\n")!=std::string::npos,"hold upper endpoint");
    Require(rejected("key=CheekyGazeSmoothing&value=101") && rejected("key=CheekyGazeHold&value=-1"),"gaze timing bounds");
    Require(rejected("key=IndependentOpenXRGaze&value=1") && rejected("key=ManualStereoMapping&value=1"),"input ownership and mapping bypass are not exposed");
    const auto settingsBeforeAction=StateJSON();
    const auto commandsBeforeAction=commandCount;
    Require(RecalibrateCheeky()=="{\"ok\":true}" && commandCount==commandsBeforeAction+1 &&
        lastCommand.substr(lastCommand.find('\n',2)+1)=="calibration_recalibrate\n","one explicit recalibration command, not reset or forget");
    Require(StateJSON()==settingsBeforeAction,"recalibration bridge preserves mod settings");
    acceptCommand=false;
    Require(RecalibrateCheeky().find("\"ok\":false")!=std::string::npos,"propagate rejected recalibration");
    Require(rejected("key=CheekyCenterMode&value=0"),"propagate rejected setting transaction");
    acceptCommand=true;
    using namespace pc2vr::ui;
    PanelSettings panel;panel.heightDeg=0;
    const XrPosef anchor{{0,0,0,1},{1,2,3}};
    const float half=std::sqrt(.5f);
    const XrPosef head{{0,half,0,half},{5,6,7}};
    auto pose=PanelPose(panel,anchor,head,true);
    Require(Near(pose.position.x,1)&&Near(pose.position.y,2)&&Near(pose.position.z,1.5f),"anchored UI ignores head movement");
    panel.followHmd=true;pose=PanelPose(panel,anchor,head,true);
    Require(Near(pose.position.x,3.5f)&&Near(pose.position.y,6)&&Near(pose.position.z,7),"head-relative panel rotates and translates together");
    pose=PanelPose(panel,anchor,head,false);
    Require(Near(pose.position.x,-.5f)&&Near(pose.position.y,2)&&Near(pose.position.z,3),"orientation-only tracking keeps valid anchor position");
    panel.horizontalDeg=90;const XrPosef roll{{0,0,half,half},{0,0,0}};
    pose=PanelPose(panel,anchor,roll,true);
    Require(Near(pose.position.x,0)&&Near(pose.position.y,1.5f)&&Near(pose.position.z,0),"panel offsets compose in the head frame including roll");
    std::puts("PASS: response lifetimes/cache isolation, input validation/persistence, optional capability and non-identity panel poses");
}
