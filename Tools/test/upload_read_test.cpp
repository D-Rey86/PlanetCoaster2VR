// Exercise the actual snapshot reader and registry against D3D12 upload memory,
// ordinary cached memory, guard pages, and fresh writes from a different thread.
#include <windows.h>
#include <d3d12.h>
#include <intrin.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include "../../Loader/src/upload_read.h"
#include "../../Loader/src/upload_registry.h"

using Microsoft::WRL::ComPtr;
using pc2vr::uploads::ReadSnapshot;

int main() {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = pc2vr::uploads::kMinSize;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)))) return 2;
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(buffer->Map(0, &noRead, &mapped))) return 3;
    pc2vr::uploads::OnBufferCreated(buffer.Get(), desc.Width);
    const uint64_t va = buffer->GetGPUVirtualAddress();
    bool wc = false;
    if (pc2vr::uploads::Translate(va + 256, 1700, &wc) != static_cast<uint8_t*>(mapped) + 256) return 4;
    bool invalidWc = true;
    if (pc2vr::uploads::Translate(va + desc.Width - 16, 32, &invalidWc) || invalidWc) return 5;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(mapped, &mbi, sizeof(mbi))) return 6;
    if (wc != ((mbi.Protect & PAGE_WRITECOMBINE) != 0)) return 7;
    int feature[4]; __cpuid(feature, 1);
    printf("D3D12 upload: WC=%d SSE4.1=%d\n", wc, (feature[2] & (1 << 19)) != 0);

    alignas(16) std::array<uint8_t, 4096> expected{}, out{};
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(i * 13 + 7);
    std::memcpy(mapped, expected.data(), expected.size()); _mm_sfence();
    for (bool streamingAllowed : {false, wc}) {
        for (size_t offset : {size_t(0), size_t(1), size_t(15), size_t(16), size_t(272)}) {
            for (size_t size : {size_t(1), size_t(15), size_t(16), size_t(17), size_t(1696)}) {
                if (!ReadSnapshot(out.data() + 1, static_cast<uint8_t*>(mapped) + offset, size, streamingAllowed) ||
                    std::memcmp(out.data() + 1, expected.data() + offset, size)) return 8;
            }
        }
    }
    if (!ReadSnapshot(out.data(), expected.data(), 1696, false) || std::memcmp(out.data(), expected.data(), 1696)) return 9;

    std::atomic<int> written{0}, consumed{0};
    bool fresh = true;
    std::thread producer([&] {
        for (int generation = 1; generation <= 2000; ++generation) {
            while (consumed.load(std::memory_order_acquire) != generation - 1) std::this_thread::yield();
            std::memset(mapped, generation & 255, 1696); _mm_sfence();
            written.store(generation, std::memory_order_release);
        }
    });
    for (int generation = 1; generation <= 2000; ++generation) {
        while (written.load(std::memory_order_acquire) != generation) std::this_thread::yield();
        if (!ReadSnapshot(out.data() + 1, mapped, 1696, wc)) fresh = false;
        for (size_t i = 1; i <= 1696; ++i) if (out[i] != static_cast<uint8_t>(generation & 255)) fresh = false;
        consumed.store(generation, std::memory_order_release);
    }
    producer.join();
    if (!fresh) return 10;

    void* denied = VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD oldProtection;
    if (!denied || !VirtualProtect(static_cast<uint8_t*>(denied) + 4096, 4096, PAGE_NOACCESS, &oldProtection)) return 11;
    // The read ending exactly at the page boundary succeeds; no extra SIMD tail read.
    if (!ReadSnapshot(out.data(), static_cast<uint8_t*>(denied) + 4096 - 1696, 1696, true)) return 12;
    for (bool allow : {false, true}) {
        if (ReadSnapshot(out.data(), static_cast<uint8_t*>(denied) + 4096, 16, allow) ||
            ReadSnapshot(out.data(), static_cast<uint8_t*>(denied) + 4096 - 16, 32, allow)) return 13;
    }
    VirtualFree(denied, 0, MEM_RELEASE);
    buffer->Unmap(0, &noRead);
    puts("PASS: registry bounds/properties, exact bytes, fallback/alignment/lengths, cached memory, 2000 cross-thread generations, guard pages");
    return 0;
}
