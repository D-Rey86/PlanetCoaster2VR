// Offline checks against user-supplied retail files; never hooks or launches them.
#include "../../Loader/src/native_input_hook.cpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
namespace {
void Check(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
void File(const wchar_t* path){
    using namespace pc2vr::pointer::native;
    std::ifstream stream(path,std::ios::binary);const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),{});
    Check(bytes.size()>sizeof(IMAGE_DOS_HEADER),"retail file missing");
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    Check(dos->e_magic==IMAGE_DOS_SIGNATURE && dos->e_lfanew>0 && static_cast<size_t>(dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS64)<bytes.size(),"PE bounds");
    const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data()+dos->e_lfanew);
    const auto* build=SelectBuild(pe->FileHeader.TimeDateStamp,pe->OptionalHeader.SizeOfImage);
    Check(build!=nullptr,"exact PE build selection");Check(HashFile(path,*build),"exact SHA256");
    Check(!HashFile(path,build==&kBuilds[0] ? kBuilds[1] : kBuilds[0]),"other build hash rejected");
    const auto* sections=IMAGE_FIRST_SECTION(pe);
    const auto raw=[&](size_t rva,size_t size)->const char*{
        for(unsigned i=0;i<pe->FileHeader.NumberOfSections;++i){const auto& section=sections[i];
            if(rva>=section.VirtualAddress && rva+size<=section.VirtualAddress+section.SizeOfRawData){
                const auto offset=section.PointerToRawData+rva-section.VirtualAddress;Check(offset+size<=bytes.size(),"raw file bounds");return bytes.data()+offset;
            }
        }
        Check(false,"unmapped RVA");return nullptr;
    };
    for(size_t i=0;i<std::size(kEntries);++i)if(build->sites[i])
        Check(!std::memcmp(raw(build->sites[i],16),kEntries[i].bytes,16),"complete allowed hook bytes");
    const auto directory=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto* descriptors=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(raw(directory.VirtualAddress,directory.Size));
    bool found[3]{};const char* names[]={"GetRawInputData","GetRawInputDeviceInfoA","GetRawInputDeviceList"};
    for(size_t i=0;i<directory.Size/sizeof(*descriptors) && descriptors[i].Name;++i){const auto& descriptor=descriptors[i];
        if(_stricmp(raw(descriptor.Name,11),"USER32.dll"))continue;
        for(size_t n=0;;++n){const auto* thunk=reinterpret_cast<const IMAGE_THUNK_DATA64*>(raw(descriptor.OriginalFirstThunk+n*8,8));if(!thunk->u1.AddressOfData)break;
            if(IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal))continue;const char* name=raw(static_cast<size_t>(thunk->u1.AddressOfData)+2,1);
            for(size_t j=0;j<3;++j)if(!std::strcmp(name,names[j])){Check(descriptor.FirstThunk+n*8==build->landmarks[j+2],"import name/address contract");found[j]=true;}
        }
    }
    Check(found[0]&&found[1]&&found[2],"all native raw imports present");
}
void Unwind(){
    using namespace pc2vr::pointer::native;
    auto* page=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Check(page!=nullptr,"unwind allocation");
    std::memcpy(page+16,kEntries[static_cast<size_t>(Site::DummyInSeat)].bytes,5);
    DummyTransfer(page+21,page+256);
    const uint8_t continuation[]={0x41,0x5e,0x5e,0x5d,0xb8,42,0,0,0,0xc3};
    std::memcpy(page+256,continuation,sizeof(continuation));
    auto* function=DummyUnwind(page);
    DWORD old{};Check(VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old)!=0,"RX trampoline");
    Check(RtlAddFunctionTable(function,1,reinterpret_cast<DWORD64>(page))!=0,"register unwind");
    FlushInstructionCache(GetCurrentProcess(),page,4096);
    Check(reinterpret_cast<int(*)()>(page+16)()==42,"execute relocated pushes and native continuation");
    // Every reachable instruction boundary, including the transfer jump.
    const unsigned offsets[]={0,2,3,5,15};
    for(const auto offset:offsets){
        DWORD64 stack[64]{};stack[32]=0x123456789abcull;
        stack[31]=0x11111111;stack[30]=0x22222222;stack[29]=0x33333333;
        CONTEXT context{};context.Rbp=stack[31];context.Rsi=stack[30];context.R14=stack[29];
        const unsigned slots=(offset>=2)+(offset>=3)+(offset>=5);
        context.Rsp=reinterpret_cast<DWORD64>(stack+32-slots);
        context.Rip=reinterpret_cast<DWORD64>(page+16+offset);
        DWORD64 base{};auto* entry=RtlLookupFunctionEntry(context.Rip,&base,nullptr);
        Check(entry==function,"registered trampoline lookup");
        void* data{};DWORD64 establisher{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,context.Rip,entry,&context,&data,&establisher,nullptr);
        if(context.Rip!=stack[32] || context.Rsp!=reinterpret_cast<DWORD64>(stack+33))
            std::fprintf(stderr,"unwind failed at instruction offset %u\n",offset);
        Check(context.Rip==stack[32] && context.Rsp==reinterpret_cast<DWORD64>(stack+33),"unwound caller and stack");
        Check(context.Rbp==stack[31] && context.Rsi==stack[30] && context.R14==stack[29],"unwound nonvolatile registers");
    }
    Check(RtlDeleteFunctionTable(function)!=0,"unregister unwind");VirtualFree(page,0,MEM_RELEASE);
}
}
int wmain(int argc,wchar_t** argv){
    using namespace pc2vr::pointer::native;Check(argc>=2,"supply retail executable paths");
    Check(!SelectBuild(0,0) && !SelectBuild(kBuilds[0].timestamp,kBuilds[1].imageSize),"unknown/mixed PE identity rejected");
    for(int i=1;i<argc;++i)File(argv[i]);
    for(size_t i=0;i<2;++i)Check(!kBuilds[i].sites[static_cast<size_t>(Site::DummyInSeat)] &&
        !kBuilds[i].sites[static_cast<size_t>(Site::PeepHeadHidden)] &&
        !kBuilds[i].landmarks[static_cast<size_t>(Landmark::SceneViewFlags)],"older builds retain original rider visibility");
    Unwind();
    Check(!VerifyRetail() && Address(Landmark::MouseRayCaller)==0,"test process cannot acquire retail hook addresses");
    std::puts("PASS exact retail identities, all hook bytes, imports, trampoline unwind, older-build fallback and unknown-process rejection");
}
