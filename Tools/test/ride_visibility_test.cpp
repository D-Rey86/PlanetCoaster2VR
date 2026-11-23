// Regression contracts for dummy provenance and restoration; no game launch.
#include "../../Loader/src/ride_visibility.cpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
namespace pc2vr::log { void Write(const char*,...) {} }
namespace pc2vr::pointer::native {
uintptr_t Address(Landmark){return 0;}
bool Install(Site,void*,Hook&){return false;}
}
namespace {
using namespace pc2vr::ridecam;
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
unsigned originalCalls{},flagCalls{};uint64_t flagEntity{};void* flagScene{};bool flagHidden{};
int Original(void*){++originalCalls;return 7;}
void Flags(void* scene,uint64_t id,uint32_t mask,bool hidden,uint8_t group,uint32_t scope){
 Check(originalCalls>flagCalls,"native head helper runs first");
 Check(mask==512 && group==2 && scope==2,"only native first-person category");
 ++flagCalls;flagEntity=id;flagScene=scene;flagHidden=hidden;
}
struct Fixture {
 alignas(8) uint8_t lua[48]{},slots[120]{},userdata[64]{},api[512]{},meshes[1024]{};
 uint8_t* bottom=slots;
 Fixture(){
  *reinterpret_cast<uint8_t**>(lua+16)=slots+120;
  *reinterpret_cast<uint8_t***>(lua+32)=&bottom;
  *reinterpret_cast<uint32_t*>(slots+40)=0x47;
  *reinterpret_cast<uint32_t*>(slots+64)=0x13;
  *reinterpret_cast<uint32_t*>(slots+88)=1;
  *reinterpret_cast<uint32_t*>(slots+112)=1;
  *reinterpret_cast<uint8_t**>(slots+24)=userdata;
  *reinterpret_cast<uint8_t**>(userdata+48)=api;
  *reinterpret_cast<uint8_t**>(api+0x160)=meshes;
  *reinterpret_cast<void**>(meshes+0x3e8)=this;
 }
 void Head(uint64_t id,bool hide,bool local=true){
  *reinterpret_cast<uint64_t*>(slots+48)=id;
  *reinterpret_cast<uint64_t*>(slots+72)=hide;
  *reinterpret_cast<uint64_t*>(slots+96)=local;
  Check(PeepHeadHidden(lua)==7,"preserve native return value");
 }
 void Dummy(uint64_t id){g_lastDummy={lua,id};}
};
}
int main(){
 g_headHook.original.store(reinterpret_cast<void*>(&Original));g_flags=&Flags;g_ready=true;
 Fixture first;
 first.Head(100,true);Check(flagCalls==0,"ordinary guest unchanged");
 first.Dummy(101);first.Head(100,true);Check(flagCalls==0,"different entity unchanged");
 first.Dummy(100);first.Head(100,true,false);Check(flagCalls==0,"nonlocal hide unchanged");
 first.Dummy(100);first.Head(100,true);Check(flagCalls==1 && flagEntity==100 && flagHidden && flagScene==&first,"selected dummy hidden");
 first.Dummy(100);first.Head(100,true);Check(flagCalls==1,"duplicate hide does not repeat");
 first.Dummy(999);first.Head(100,false);Check(flagCalls==2 && !flagHidden && !g_hidden.entity,"restore despite unrelated last dummy query");
 first.Dummy(101);first.Head(101,true);Check(flagCalls==3 && flagEntity==101,"next seat hidden");
 Fixture second;second.Dummy(200);second.Head(200,true);
 Check(flagCalls==4 && flagEntity==200 && flagScene==&second,"new live scene invalidates old scene without dereference");
 second.Head(200,false);Check(flagCalls==5 && !g_hidden.entity,"new scene restore");
 second.Dummy(300);*reinterpret_cast<uint32_t*>(second.slots+40)=0;
 second.Head(300,true);Check(flagCalls==5,"malformed receiver unchanged");
 g_ready=false;first.Dummy(400);first.Head(400,true);Check(flagCalls==5,"unavailable hooks retain native behavior");
 Check(originalCalls==11,"all native head calls preserved");
 g_ready=true;first.Dummy(500);first.Head(500,true);
 std::thread worker([&]{first.Head(500,false);});worker.join();
 Check(flagCalls==7 && !flagHidden && !g_hidden.entity,"restore on a different script worker");
 std::puts("PASS guest exclusion, local-only provenance, duplicate/seat restoration, scene invalidation and native behavior preservation");
}
