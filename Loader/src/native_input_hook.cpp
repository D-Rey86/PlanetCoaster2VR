#include "native_input_hook.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace pc2vr::pointer::native {
namespace {
std::once_flag verification;
std::mutex installation;
uint8_t* image{};
bool verified{};
struct RetailBuild {uint32_t timestamp,imageSize;uint8_t hash[32];size_t sites[10],landmarks[11];};
// Exact retail builds only. The second build's complete six functions have the
// same instructions/field offsets/branches after relocating calls and globals.
constexpr RetailBuild kBuilds[]={
    {0x6abfabee,0x130a4000,{0xb2,0x79,0x2e,0x2b,0xcd,0x57,0xc4,0x20,0x48,0x86,0xce,0x88,0x70,0x80,0x18,0x65,0x6e,0x64,0x14,0xef,0x88,0x10,0x74,0x55,0xf5,0x31,0x83,0xd7,0x8c,0x58,0x2e,0x9f},{0x16128a0,0x1612d50,0x19d270,0x1baff40,0x1863e90,0x315620,0x15b8af0},{0x35c1c0,0x1aac928,0x255ccc0,0x255ccd0,0x255ccf8,0x1baff40,0x1bb0010,0x1bb0180,0x1bb02f0}},
    {0x6ae11c9a,0x12187000,{0x9d,0xa9,0x0a,0xe2,0x61,0x1b,0xf4,0x1a,0xf5,0x32,0xa3,0x06,0x11,0x08,0x2b,0xd9,0x0c,0xdd,0xa6,0xb3,0x25,0xe5,0x70,0xac,0x63,0xa4,0x69,0x05,0x1e,0x73,0x6c,0x12},{0x1660bf0,0x16610a0,0x19f1e0,0x1bfe020,0x18b1fe0,0x317910,0x1606e40},{0x35e400,0x1afaa68,0x25aacc0,0x25aacd0,0x25aacf8,0x1bfe020,0x1bfe0f0,0x1bfe260,0x1bfe3d0}},
    {0x6ae8f335,0x12343000,{0x3e,0xf4,0xc5,0xf1,0xf1,0x15,0xfe,0x9f,0x9e,0x76,0xc9,0x50,0x3b,0xea,0xfd,0xa8,0x37,0x43,0x63,0x12,0x82,0x21,0x39,0x45,0xb9,0xd7,0x13,0x95,0x42,0x43,0xf2,0x65},{0x16616c0,0x1661b70,0x19f270,0x1bfeb90,0x18b2b50,0x3179a0,0x1607910,0x1e4f0c0,0x66c210,0x5c82a0},{0x35e490,0x1afb5d8,0x25abcc0,0x25abcd0,0x25abcf8,0x1bfeb90,0x1bfec60,0x1bfedd0,0x1bfef40,0x1e548f4,0x1a4a630}},
    {0x6aee41b0,0x12ee8000,{0x40,0x82,0xbd,0x57,0xee,0xcc,0xfd,0xa5,0x48,0x24,0x40,0x9e,0x9b,0xe3,0x2e,0x95,0x24,0x61,0xcf,0xb4,0x22,0xca,0xa2,0x01,0x66,0x2c,0x00,0xde,0xb5,0x42,0x17,0xf3},{0x1662a20,0x1662ed0,0x1a0120,0x1c019e0,0x18b3eb0,0x318850,0x1608c70,0x1e51f10,0x66d0c0,0x5c9150},{0x35f340,0x1afe428,0x25aecc0,0x25aecd0,0x25aecf8,0x1c019e0,0x1c01ab0,0x1c01c20,0x1c01d90,0x1e57744,0x1a4d5e0}},
};
const RetailBuild* selectedBuild{};
const RetailBuild* SelectBuild(uint32_t timestamp,uint32_t imageSize){
    for(const auto& build:kBuilds)if(build.timestamp==timestamp && build.imageSize==imageSize)return &build;
    return nullptr;
}
struct Entry { size_t rva; uint8_t bytes[16]; };
constexpr Entry kEntries[] = {
    {0x16128a0,{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57}},
    {0x1612d50,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57}},
    {0x19d270,{0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41}},
    {0x1baff40,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57}},
    {0x1863e90,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x4c}},
    {0x315620,{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57}},
    {0x15b8af0,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x4c}},
    {0x1e4f0c0,{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x44,0x8b,0x42,0x10,0x48,0x8b}},
    {0x66c210,{0x40,0x55,0x56,0x41,0x56,0x48,0x83,0xec,0x40,0x48,0x8b,0x41,0x20,0x48,0x8b,0xe9}},
    {0x5c82a0,{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x54,0x41,0x56}}
};
// Relocate five complete bytes, preserving bytes5..7 in the atomic patch.
// All entries start with a leaf MOV except DummyInSeat: its three PUSHes have
// explicit unwind metadata below. Its stack allocation remains at target+5.
bool HashFile(const wchar_t* path,const RetailBuild& build) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    uint8_t buffer[64*1024], digest[32]{}; DWORD size{};
    while (ok) {
        ok = ReadFile(file, buffer, sizeof(buffer), &size, nullptr) != 0;
        if (!ok || !size) break;
        ok = BCryptHashData(hash, buffer, size, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0 && !std::memcmp(digest, build.hash, sizeof(digest));
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}
void Verify() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return;
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    const auto* build=SelectBuild(pe->FileHeader.TimeDateStamp,pe->OptionalHeader.SizeOfImage);if(!build)return;
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path)/sizeof(path[0])));
    if (!length || length >= sizeof(path)/sizeof(path[0]) || !HashFile(path,*build)) return;
    for(size_t i=0;i<sizeof(kEntries)/sizeof(kEntries[0]);++i){
        const auto& entry=kEntries[i];const auto rva=build->sites[i];
        if (!rva) continue;
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(base+rva, &region, sizeof(region)) || region.AllocationBase != base ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD|PAGE_NOACCESS)) ||
            !(region.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) ||
            std::memcmp(base+rva, entry.bytes, sizeof(entry.bytes))) return;
    }
    image=base;selectedBuild=build;verified=true;
}
void Jump(uint8_t* code, const void* to) {
    code[0]=0xff; code[1]=0x25; std::memset(code+2,0,4); std::memcpy(code+6,&to,8);
    code[14]=code[15]=0x90;
}
void DummyTransfer(uint8_t* code, const void* to) {
    // MOV R11,imm64; JMP R11 (no REX.W on JMP). A RIP-indirect FF25
    // transfer is recognized as an epilogue and would skip the saved PUSHes.
    // R11 is volatile and this exact native function overwrites it at +0x10
    // before its first use. Other hook trampolines retain their leaf transfer.
    code[0]=0x49; code[1]=0xbb; std::memcpy(code+2,&to,8);
    code[10]=0x41; code[11]=0xff; code[12]=0xe3;
}
RUNTIME_FUNCTION* DummyUnwind(uint8_t* page) {
    // 40 55; 56; 41 56 = PUSH RBP (2 bytes), RSI, R14.
    // The unwind codes are in reverse instruction order; the odd code count
    // needs padding. The register transfer isn't an epilogue: all three
    // registers are still saved when execution continues in the native body.
    constexpr uint8_t unwind[] = {1,5,3,0,5,0xe0,3,0x60,2,0x50,0,0};
    std::memcpy(page+64,unwind,sizeof(unwind));
    auto* function=reinterpret_cast<RUNTIME_FUNCTION*>(page+80);
    *function={16,34,64};
    return function;
}
uint8_t* AllocateNear(uint8_t* target) {
    const auto origin = reinterpret_cast<uintptr_t>(target)&~uintptr_t(0xffff);
    for (uintptr_t delta=0x10000;delta<0x7ff00000;delta+=0x10000) {
        const uintptr_t choices[2]={origin+delta,origin>delta ? origin-delta : 0};
        for (const auto address : choices) if (address) {
            auto* page=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address),4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
            if (page) return page;
        }
    }
    return nullptr;
}
} // namespace
bool VerifyRetail() { std::call_once(verification, Verify); return verified; }
uintptr_t Address(Landmark landmark){
    const auto index=static_cast<size_t>(landmark);
    if (!VerifyRetail() || index>=std::size(selectedBuild->landmarks)) return 0;
    const auto offset=selectedBuild->landmarks[index];
    return offset ? reinterpret_cast<uintptr_t>(image)+offset : 0;
}
bool Install(Site site, void* replacement, Hook& hook) {
    if (!replacement || !VerifyRetail()) return false;
    const auto index = static_cast<size_t>(site);
    if (index >= sizeof(kEntries)/sizeof(kEntries[0])) return false;
    if (!selectedBuild->sites[index]) return false;
    std::lock_guard<std::mutex> lock(installation);
    if (hook.target || hook.original.load(std::memory_order_acquire)) return false;
    const auto& entry = kEntries[index];
    auto* target=image+selectedBuild->sites[index];
    if ((reinterpret_cast<uintptr_t>(target)&15) || std::memcmp(target,entry.bytes,16)) return false;
    auto* page = AllocateNear(target);
    if (!page) return false;
    Jump(page,replacement);
    auto* trampoline=page+16;
    std::memcpy(trampoline,entry.bytes,5);
    if (site==Site::DummyInSeat) DummyTransfer(trampoline+5,target+5);
    else Jump(trampoline+5,target+5);
    auto* unwind=site==Site::DummyInSeat ? DummyUnwind(page) : nullptr;
    DWORD protection{};
    if (!VirtualProtect(page,4096,PAGE_EXECUTE_READ,&protection)) {
        VirtualFree(page,0,MEM_RELEASE); return false;
    }
    FlushInstructionCache(GetCurrentProcess(),page,35);
    uint8_t patch[8]; std::memcpy(patch,entry.bytes,8); patch[0]=0xe9;
    const auto distance=reinterpret_cast<intptr_t>(page)-reinterpret_cast<intptr_t>(target+5);
    const auto relative=static_cast<int32_t>(distance);
    if (static_cast<intptr_t>(relative)!=distance) { VirtualFree(page,0,MEM_RELEASE); return false; }
    std::memcpy(patch+1,&relative,4);
    if (unwind && !RtlAddFunctionTable(unwind,1,reinterpret_cast<DWORD64>(page))) {
        VirtualFree(page,0,MEM_RELEASE); return false;
    }
    long long expected{}, desired{};
    std::memcpy(&expected,entry.bytes,8); std::memcpy(&desired,patch,8);
    if (!VirtualProtect(target,8,PAGE_EXECUTE_READWRITE,&protection)) {
        if (unwind) RtlDeleteFunctionTable(unwind);
        VirtualFree(page,0,MEM_RELEASE); return false;
    }
    // Publish the immutable trampoline before another thread can enter the hook.
    hook.original.store(trampoline,std::memory_order_release);
    const bool ok = InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(target),desired,expected)==expected;
    DWORD ignored{}; VirtualProtect(target,8,protection,&ignored);
    FlushInstructionCache(GetCurrentProcess(),target,8);
    if (!ok) {
        hook.original.store(nullptr,std::memory_order_release);
        if (unwind) RtlDeleteFunctionTable(unwind);
        VirtualFree(page,0,MEM_RELEASE); return false;
    }
    hook.target=target; // process-lifetime hook: never hot-unload the owning DLL
    return true;
}
} // namespace pc2vr::pointer::native
