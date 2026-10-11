// Public-contract tests: aliases, invalidation, range boundaries, concurrent
// publication, and real D3D12 resource ownership. No rendering is fabricated.
#include "../../Loader/src/upload_registry.h"
#include "../../Loader/src/upload_table.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <d3d12.h>
#include <limits>
#include <random>
#include <thread>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using pc2vr::uploads::detail::TranslationCache;
using pc2vr::uploads::detail::UploadTable;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                                                \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)
int main()
{
    std::vector<uint8_t> arena(1024 * 1024);
    UploadTable t;
    TranslationCache cache;
    bool wc = false;
    const auto a = t.Add(1000, 1000, arena.data(), false);
    CHECK(t.Translate(1600, 100, &wc, cache) == arena.data() + 600 && !wc);
    const auto b = t.Add(1200, 300, arena.data() + 4096, true);
    CHECK(t.Translate(1600, 100, &wc, cache) == arena.data() + 600 && !wc); // seed older range
    CHECK(t.Translate(1250, 100, &wc, cache) == arena.data() + 4096 + 50 && wc);
    CHECK(t.Translate(1250, 400, &wc, cache) == arena.data() + 250 && !wc); // spans beyond newer
    CHECK(t.Translate(1260, 100, &wc, cache) == arena.data() + 4096 + 60 && wc);
    CHECK(t.Translate(1500, 0, &wc, cache) == arena.data() + 4096 + 300 && wc);
    CHECK(t.Translate(1500, 1, &wc, cache) == arena.data() + 500 && !wc);
    t.Remove(b);
    CHECK(t.Translate(1250, 100, &wc, cache) == arena.data() + 250 && !wc);
    CHECK(!t.Translate(5000, 100, &wc, cache) && !wc); // negative gap
    const auto c = t.Add(4900, 500, arena.data() + 8192, true);
    CHECK(t.Translate(5000, 100, &wc, cache) == arena.data() + 8192 + 100 && wc);
    t.Remove(c);
    CHECK(!t.Translate(5000, 100, &wc, cache) && !wc);
    t.Remove(a);
    CHECK(!t.Translate(1250, 100, &wc, cache) && !wc && t.Count() == 0);
    CHECK(!t.Translate(std::numeric_limits<uint64_t>::max() - 4, 16, &wc, cache) && !wc);
    CHECK(!t.Add(std::numeric_limits<uint64_t>::max() - 4, 16, arena.data(), false));

    // Compare caching with an independent newest-live-range oracle, using partial
    // overlaps and destructive/reconstructive changes, rather than uniform ranges.
    struct Model
    {
        uint64_t id, begin, size;
        const uint8_t* cpu;
        bool wc;
    };
    std::vector<Model> live;
    std::mt19937 rng(9137);
    for (unsigned step = 0; step < 30000; ++step)
    {
        if (step % 7 == 0 && live.size() < 100)
        {
            uint64_t begin = rng() % 12000, size = 1 + rng() % 1800;
            const uint8_t* cpu = arena.data() + (step % 200) * 4096;
            bool property = (step & 1) != 0;
            auto id = t.Add(begin, size, cpu, property);
            live.push_back({id, begin, size, cpu, property});
        }
        else if (step % 11 == 0 && !live.empty())
        {
            size_t index = rng() % live.size();
            t.Remove(live[index].id);
            live.erase(live.begin() + index);
        }
        uint64_t va = rng() % 16000, bytes = rng() % 2000;
        const uint8_t* expected = nullptr;
        bool expectedWc = false;
        for (auto i = live.rbegin(); i != live.rend(); ++i)
            if (va >= i->begin && va + bytes <= i->begin + i->size)
            {
                expected = i->cpu + (va - i->begin);
                expectedWc = i->wc;
                break;
            }
        CHECK(t.Translate(va, bytes, &wc, cache) == expected && wc == expectedWc);
    }
    for (auto& e : live)
        t.Remove(e.id);
    CHECK(t.Count() == 0);

    // Readers check only samples taken within one publication generation. Writer
    // changes ownership at the same VA, which must invalidate both cache kinds.
    std::atomic<bool> finished{false};
    std::atomic<unsigned> mismatches{0};
    std::atomic<uint64_t> checks{0}, attempts{0};
    std::atomic<unsigned> ready{0};
    const auto initial = t.Add(20000, 1024, arena.data(), false);
    std::vector<std::thread> readers;
    for (unsigned n = 0; n < 4; ++n)
        readers.emplace_back([&] {
            TranslationCache local;
            ++ready;
            while (!finished.load())
            {
                const auto before = t.Generation();
                bool p = false;
                const auto* result = t.Translate(20128, 100, &p, local);
                const auto after = t.Generation();
                ++attempts;
                // Every writer stage is either unmapped or mapped to one of two valid spans.
                if (result && result != arena.data() + 128 && result != arena.data() + 4096 + 128)
                    ++mismatches;
                if ((result == arena.data() + 128 && p) ||
                    (result == arena.data() + 4096 + 128 && !p) || (!result && p))
                    ++mismatches;
                if (before == after)
                    ++checks;
            }
        });
    while (ready.load() < 4 || checks.load() < 100)
        std::this_thread::yield();
    uint64_t token = initial;
    for (unsigned i = 0; i < 12000; ++i)
    {
        t.Remove(token);
        token = t.Add(20000, 1024, arena.data() + ((i & 1) ? 4096 : 0), (i & 1) != 0);
    }
    const auto finalChecks = checks.load();
    while (checks.load() < finalChecks + 100)
        std::this_thread::yield();
    finished.store(true);
    for (auto& reader : readers)
        reader.join();
    t.Remove(token);
    printf("concurrency mismatches=%u checks=%llu remaining=%u attempts=%llu\n", mismatches.load(), checks.load(),
           t.Count(), attempts.load());
    CHECK(mismatches == 0 && checks > 0 && t.Count() == 0);

    ComPtr<ID3D12Device> device;
    CHECK(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = pc2vr::uploads::kMinSize;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    struct Buffer
    {
        ComPtr<ID3D12Resource> resource;
        void* cpu = nullptr;
        uint64_t va = 0;
    };
    std::vector<Buffer> buffers;
    for (unsigned i = 0; i < 320; ++i)
    {
        Buffer v;
        CHECK(SUCCEEDED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&v.resource))));
        const D3D12_RANGE noRead{0, 0};
        CHECK(SUCCEEDED(v.resource->Map(0, &noRead, &v.cpu)));
        v.va = v.resource->GetGPUVirtualAddress();
        pc2vr::uploads::OnBufferCreated(v.resource.Get(), desc.Width);
        buffers.push_back(std::move(v));
    }
    CHECK(pc2vr::uploads::Count() == 320);
    for (auto& v : buffers)
        CHECK(pc2vr::uploads::Translate(v.va + 256, 1700) == static_cast<uint8_t*>(v.cpu) + 256);
    // Installing another observer on the same COM object removes only its old token.
    pc2vr::uploads::OnBufferCreated(buffers[0].resource.Get(), desc.Width);
    CHECK(pc2vr::uploads::Count() == 320);
    ComPtr<IUnknown> lease;
    CHECK(SUCCEEDED(buffers[0].resource.As(&lease)));
    const auto leasedVa = buffers[0].va;
    auto* leasedCpu = static_cast<uint8_t*>(buffers[0].cpu);
    buffers.clear();
    CHECK(pc2vr::uploads::Count() == 1);
    CHECK(pc2vr::uploads::Translate(leasedVa + 256, 1700) == leasedCpu + 256);
    lease.Reset();
    CHECK(pc2vr::uploads::Count() == 0);
    CHECK(!pc2vr::uploads::Translate(leasedVa + 256, 1700));
    printf("PASS: aliases, gaps, range overflow, 30000 oracle cases, %llu concurrent stable samples, 320 live D3D12 "
           "buffers, actual COM lifetime and re-registration\n",
           checks.load());
    return 0;
}
