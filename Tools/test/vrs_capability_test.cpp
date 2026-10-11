// Executes production VRS initialization/uploads on a real D3D12 queue. The
// forced2x2 phase tests fallback buffers, not physical unsupported-GPU behavior.
#include "vrs.cpp"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>

namespace pc2vr::log {void Write(const char*,...) {}}
namespace {
void Require(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
void Check(HRESULT result,const char* why) {if(FAILED(result)){std::fprintf(stderr,"HRESULT 0x%08lx\n",static_cast<unsigned long>(result));Require(false,why);}}
void Wait(ID3D12CommandQueue* queue,ID3D12Fence* fence,UINT64 value) {
    Check(queue->Signal(fence,value),"signal test fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"test fence event");
    Check(fence->SetEventOnCompletion(value,event),"arm test fence");
    const auto result=WaitForSingleObject(event,10000);CloseHandle(event);Require(result==WAIT_OBJECT_0,"GPU completion timeout");
}
std::vector<uint8_t> ReadPattern(ID3D12Device* device,ID3D12CommandQueue* queue) {
    using namespace pc2vr::vrs;
    const auto description=g.image->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 size{};
    device->GetCopyableFootprints(&description,0,1,0,&footprint,nullptr,nullptr,&size);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=size;
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};ComPtr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback buffer");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"test allocator");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"test command list");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={g.image.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION source{g.image.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
    D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};destination.PlacedFootprint=footprint;
    list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
    Check(list->Close(),"close test copy");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"readback fence");Wait(queue,fence.Get(),1);
    uint8_t* bytes{};const D3D12_RANGE range{0,static_cast<SIZE_T>(size)};Check(readback->Map(0,&range,reinterpret_cast<void**>(&bytes)),"map GPU result");
    std::vector<uint8_t> result(static_cast<size_t>(g.tw)*g.th);
    for(uint32_t row=0;row<g.th;++row)memcpy(result.data()+static_cast<size_t>(row)*g.tw,bytes+footprint.Offset+static_cast<size_t>(row)*footprint.Footprint.RowPitch,g.tw);
    const D3D12_RANGE none{0,0};readback->Unmap(0,&none);return result;
}
void DebugErrors(ID3D12InfoQueue* info) {
    if(!info)return;
    const auto count=info->GetNumStoredMessagesAllowedByRetrievalFilter();
    for(UINT64 i=0;i<count;++i) {
        SIZE_T size{};Check(info->GetMessage(i,nullptr,&size),"message size");std::vector<uint8_t> storage(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(i,message,&size),"message body");
        if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            std::fprintf(stderr,"D3D12: %s\n",message->pDescription);Require(false,"D3D12 validation error");
        }
    }
}
}
int main(int argc,char** argv) {
    using namespace pc2vr::vrs;
    Require(argc==2,"supply test-owned binary output path");FILE* output{};Require(fopen_s(&output,argv[1],"wb")==0,"open test output");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"create hardware device");
    ComPtr<ID3D12InfoQueue> info;device.As(&info);
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&options,sizeof(options)),"query actual VRS caps");
    std::printf("Hardware: Tier%d,additional rates%d,tile%u,debug layer%d\n",static_cast<int>(options.VariableShadingRateTier),options.AdditionalShadingRatesSupported,options.ShadingRateImageTileSize,debug?1:0);
    Require(options.VariableShadingRateTier>=D3D12_VARIABLE_SHADING_RATE_TIER_2,"this GPU cannot execute the Tier2 test");
    Require(CheckSupport(device.Get()),"production capability selection");
#ifndef PC2VR_VRS_REFERENCE
    Require(g.rate4x4==(options.AdditionalShadingRatesSupported!=FALSE),"production4x4 flag must equal GPU capability");
#else
    Require(options.AdditionalShadingRatesSupported!=FALSE,"baseline oracle requires supported4x4 hardware");
#endif
    const bool native4x4=g.rate4x4;D3D12_COMMAND_QUEUE_DESC queueDesc{};ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"create direct queue");
    Configure(true,25,40);
    const float frusta[2][4]{{-2.f,1.7f,1.5f,-1.5f},{-1.7f,2.f,1.5f,-1.5f}};
    const float centers[2][2]{{-.15f,.1f},{.2f,-.05f}};
    unsigned samples{},outer4{},outer2{},full{};
    for(int mode=0;mode<2;++mode) {
        g.rate4x4=mode==0?native4x4:false;memset(g.uploadValid,0,sizeof(g.uploadValid));
        for(int fullRate=0;fullRate<2;++fullRate)for(int frame=0;frame<4;++frame)for(int eye=0;eye<2;++eye) {
            constexpr uint32_t width=1001,height=769;
            PrepareEye(queue.Get(),eye,width,height,frusta[eye],centers[eye],fullRate!=0);
            Require(g.image && g_image.load()==g.image.Get(),"production image published");
            auto pattern=ReadPattern(device.Get(),queue.Get());
            const double inner=std::tan(25.*3.14159265358979323846/180.),middle=std::tan(40.*3.14159265358979323846/180.);
            for(uint32_t y=0;y<g.th;++y)for(uint32_t x=0;x<g.tw;++x) {
                const double tx=frusta[eye][0]+(frusta[eye][1]-frusta[eye][0])*((x+.5)*g.tile/width);
                const double ty=frusta[eye][2]+(frusta[eye][3]-frusta[eye][2])*((y+.5)*g.tile/height);
                const double dx=tx-centers[eye][0],dy=ty-centers[eye][1],distance=std::sqrt(dx*dx+dy*dy);
                const uint8_t actual=pattern[static_cast<size_t>(y)*g.tw+x];
                if(std::fabs(distance-inner)>1e-5 && std::fabs(distance-middle)>1e-5) {
                    const auto expected=fullRate || distance<inner?D3D12_SHADING_RATE_1X1:
                        distance<middle || !g.rate4x4?D3D12_SHADING_RATE_2X2:D3D12_SHADING_RATE_4X4;
                    Require(actual==expected,"GPU pattern disagrees with independent radial oracle");
                }
                Require(g.rate4x4 || actual!=D3D12_SHADING_RATE_4X4,"fallback never publishes unsupported4x4");
                outer4+=actual==D3D12_SHADING_RATE_4X4;outer2+=actual==D3D12_SHADING_RATE_2X2;full+=actual==D3D12_SHADING_RATE_1X1;
            }
            Require(fwrite(pattern.data(),pattern.size(),1,output)==1,"write GPU pattern records");++samples;
        }
    }
    Require(outer2 && full,"fallback and full-rate regions exercised");Require(!native4x4 || outer4,"normal4x4 region exercised");
    SetEnabled(false);const auto before=g.fenceValue;PrepareEye(queue.Get(),0,1001,769,frusta[0],centers[0],false);
    Require(g.fenceValue==before,"Off records no additional upload");
    DebugErrors(info.Get());Require(fclose(output)==0,"close test output");
    // Actual WARP adapter checks the no-Tier2 disabling path independently.
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> warpDevice;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP adapter");
    Check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&warpDevice)),"WARP device");
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 warpCaps{};Check(warpDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,&warpCaps,sizeof(warpCaps)),"WARP caps");
    if(warpCaps.VariableShadingRateTier<D3D12_VARIABLE_SHADING_RATE_TIER_2) {
        g.checked=false;g.supported=false;Require(!CheckSupport(warpDevice.Get()),"actual unsupported device disables VRS");
    }
    std::printf("PASS: %u real GPU uploads/readbacks,4x4=%u,2x2=%u,1x1=%u,cache/Off and WARP Tier%d\n",samples,outer4,outer2,full,static_cast<int>(warpCaps.VariableShadingRateTier));
}
