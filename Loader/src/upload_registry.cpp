#include "upload_registry.h"
#include "upload_table.h"

#include <atomic>
#include <d3d12.h>
#include <new>
#include <windows.h>

#include "log.h"

namespace pc2vr::uploads
{
namespace
{
// Resources own the observer, not the reverse. No upload heap is retained.
constexpr GUID kUploadLifetime{0xe618836a, 0x60cd, 0x4b97, {0xa7, 0xfa, 0x25, 0x3e, 0x68, 0xdb, 0x8f, 0xce}};
detail::UploadTable* Table()
{
    // Resource callbacks can run after CRT teardown. This small CPU-only state
    // lasts until process exit so its mutex cannot disappear under a callback.
    static auto* table = new (std::nothrow) detail::UploadTable;
    return table;
}
class Lifetime final : public IUnknown
{
    std::atomic<ULONG> references{1};
    detail::UploadTable* table;
    uint64_t id;

  public:
    Lifetime(detail::UploadTable* owner, uint64_t token) : table(owner), id(token)
    {
    }
    ~Lifetime()
    {
        if (id)
            table->Remove(id);
    }
    void RetainRegistration()
    {
        id = 0;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown)
            return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return references.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = references.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!count)
            delete this;
        return count;
    }
};
bool PinModule()
{
    static const bool pinned = [] {
        HMODULE module = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCWSTR>(&OnBufferCreated), &module) != FALSE;
    }();
    return pinned;
}
void LifetimeUnavailable()
{
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
        LOG("upload lifetime notifications unavailable; retaining registrations as compatibility fallback");
}
} // namespace
void OnBufferCreated(ID3D12Resource* resource, uint64_t size)
{
    if (!resource || size < kMinSize)
        return;
    void* cpu = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(resource->Map(0, &noRead, &cpu)) || !cpu)
        return;
    const uint64_t begin = resource->GetGPUVirtualAddress();
    bool writeCombined = false;
    MEMORY_BASIC_INFORMATION mapping{};
    if (VirtualQuery(cpu, &mapping, sizeof(mapping)) == sizeof(mapping))
    {
        const auto offset = reinterpret_cast<uintptr_t>(cpu) - reinterpret_cast<uintptr_t>(mapping.BaseAddress);
        writeCombined = (mapping.Protect & PAGE_WRITECOMBINE) != 0 && offset <= mapping.RegionSize &&
                        size <= mapping.RegionSize - offset;
    }
    auto* table = Table();
    if (!table)
        return;
    uint64_t id = 0;
    try
    {
        id = table->Add(begin, size, static_cast<const uint8_t*>(cpu), writeCombined);
    }
    catch (const std::bad_alloc&)
    {
        return;
    }
    if (!id)
        return;
    // Re-registering can release a previous observer. Do not hold the table's
    // writer lock while calling into the resource's private-data store.
    auto* lifetime = PinModule() ? new (std::nothrow) Lifetime(table, id) : nullptr;
    if (!lifetime)
        LifetimeUnavailable();
    else
    {
        if (FAILED(resource->SetPrivateDataInterface(kUploadLifetime, lifetime)))
        {
            lifetime->RetainRegistration();
            LifetimeUnavailable();
        }
        lifetime->Release();
    }
    static std::atomic<unsigned> registrations{0};
    if (registrations.fetch_add(1, std::memory_order_relaxed) < 8)
        LOG("upload mapping: gpu %llx..%llx (%llu KB) cpu %p", begin, begin + size, size >> 10, cpu);
}
const uint8_t* Translate(uint64_t gpuVa, uint64_t bytes, bool* writeCombined)
{
    auto* table = Table();
    if (!table)
    {
        if (writeCombined)
            *writeCombined = false;
        return nullptr;
    }
    static thread_local detail::TranslationCache cache;
    return table->Translate(gpuVa, bytes, writeCombined, cache);
}
uint32_t Count()
{
    const auto* table = Table();
    return table ? table->Count() : 0;
}
} // namespace pc2vr::uploads
