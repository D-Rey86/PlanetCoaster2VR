// Runs the actual AFW pipeline on WARP, checking near/far parallax and sky.
#include "../../Loader/src/afw_warp.h"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace pc2vr::log {
void Write(const char* format, ...) { std::fprintf(stderr, "%s\n", format); }
}

namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT width=64, height=32;
constexpr uint32_t red=0xff0000ffu, blue=0xffff0000u, green=0xff00ff00u;

void Check(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",reason); std::exit(1); }
}

D3D12_RESOURCE_DESC Buffer(UINT64 size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width=size;
    d.Height=d.DepthOrArraySize=d.MipLevels=1;
    d.SampleDesc.Count=1;
    d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    list->ResourceBarrier(1,&b);
}

struct Uploaded { ComPtr<ID3D12Resource> texture, staging; };
Uploaded Upload(ID3D12Device* device, ID3D12GraphicsCommandList* list, DXGI_FORMAT format,
                const void* data, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=width; desc.Height=height;
    desc.DepthOrArraySize=desc.MipLevels=1;
    desc.Format=format;
    desc.SampleDesc.Count=1;
    Uploaded r;
    const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT}, cpu{D3D12_HEAP_TYPE_UPLOAD};
    Check(SUCCEEDED(device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr,IID_PPV_ARGS(&r.texture))),"create texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 bytes{};
    device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    const auto buffer=Buffer(bytes);
    Check(SUCCEEDED(device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr,IID_PPV_ARGS(&r.staging))),"create upload");
    void* mapped{};
    const D3D12_RANGE none{};
    Check(SUCCEEDED(r.staging->Map(0,&none,&mapped)),"map upload");
    for (UINT y=0; y<height; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped)+y*layout.Footprint.RowPitch,
                    static_cast<const uint8_t*>(data)+y*width*4,width*4);
    }
    r.staging->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{r.staging.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};
    src.PlacedFootprint=layout;
    D3D12_TEXTURE_COPY_LOCATION dst{r.texture.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_DEST) Transition(list,r.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,state);
    return r;
}
}

int main() {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
        SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"WARP device");
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    const D3D12_COMMAND_QUEUE_DESC queueDesc{};
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc))) &&
          SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list))) &&
          SUCCEEDED(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue))) &&
          SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"command objects");
    std::array<uint32_t,width*height> colors{}, motion{};
    std::array<float,width*height> depths{};
    for (UINT y=0; y<height; ++y) for (UINT x=0; x<width; ++x) {
        const bool foreground=x>=24 && x<40;
        const bool sky=y<8;
        depths[y*width+x]=sky ? 0.f : foreground ? .05f : .0125f;
        colors[y*width+x]=sky ? (x>=10 && x<20 ? green : blue) : foreground ? red : blue;
    }
    constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto color=Upload(device.Get(),list.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,colors.data(),D3D12_RESOURCE_STATE_COPY_DEST);
    const auto depth=Upload(device.Get(),list.Get(),DXGI_FORMAT_R32_FLOAT,depths.data(),read);
    const auto vectors=Upload(device.Get(),list.Get(),DXGI_FORMAT_R16G16_FLOAT,motion.data(),read);
    pc2vr::afw::Frame frame{{depth.texture.Get(),vectors.texture.Get(),DXGI_FORMAT_R32_FLOAT}, {}, {}, width,height};
    frame.source.basis={{1,0,0},{0,1,0},{0,0,1}};
    frame.source.fov={-1,1,1,-1};
    frame.source.nearZ=.1f;
    frame.target=frame.source;
    frame.target.pos.x=.25f;
    Check(pc2vr::afw::Prepare(device.Get(),color.texture->GetDesc(),frame,true),"prepare AFW pipeline");
    auto* output=pc2vr::afw::Record(list.Get(),0,0,1,frame,color.texture.Get(),nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(output!=nullptr,"record AFW pipeline");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 bytes{};
    const auto desc=output->GetDesc();
    device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    const auto buffer=Buffer(bytes);
    const D3D12_HEAP_PROPERTIES cpu{D3D12_HEAP_TYPE_READBACK};
    ComPtr<ID3D12Resource> readback;
    Check(SUCCEEDED(device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr,IID_PPV_ARGS(&readback))),"readback buffer");
    Transition(list.Get(),output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{output,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};
    dst.PlacedFootprint=layout;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Check(SUCCEEDED(list->Close()),"close AFW list");
    ID3D12CommandList* commandLists[]{list.Get()};
    queue->ExecuteCommandLists(1,commandLists);
    const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Check(event && SUCCEEDED(queue->Signal(fence.Get(),1)) && SUCCEEDED(fence->SetEventOnCompletion(1,event)) &&
          WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"AFW completes on WARP");
    CloseHandle(event);
    void* mapped{};
    const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
    Check(SUCCEEDED(readback->Map(0,&range,&mapped)),"map AFW result");
    const auto pixel=[&](UINT x,UINT y) {
        uint32_t rgba{};
        std::memcpy(&rgba,static_cast<uint8_t*>(mapped)+y*layout.Footprint.RowPitch+x*4,4);
        return rgba;
    };
    Check(pixel(22,16)==red && pixel(34,16)==red && pixel(38,16)==blue,"near geometry moves four pixels left");
    Check(pixel(11,3)==green && pixel(19,3)==green && pixel(21,3)==blue,"sky keeps zero disparity");
    readback->Unmap(0,nullptr);
    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info))) {
        for (UINT64 i=0; i<info->GetNumStoredMessages(); ++i) {
            SIZE_T size{};
            Check(SUCCEEDED(info->GetMessage(i,nullptr,&size)),"debug message size");
            std::vector<uint8_t> storage(size);
            auto* msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(SUCCEEDED(info->GetMessage(i,msg,&size)),"debug message");
            if (msg->Severity==D3D12_MESSAGE_SEVERITY_ERROR || msg->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                std::fprintf(stderr,"%s\n",msg->pDescription);
                Check(false,"D3D12 debug validation");
            }
        }
    }
    std::puts("PASS AFW WARP projection, zero sky disparity and D3D12 validation");
}
