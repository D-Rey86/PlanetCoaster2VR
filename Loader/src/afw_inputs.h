#pragma once

#include <d3d12.h>

namespace pc2vr::rtvreg { struct Info; }

namespace pc2vr::afw {
struct Guides {
    ID3D12Resource* depth{};
    ID3D12Resource* motion{};
    DXGI_FORMAT depthView{};
};

void SetEnabled(bool enabled);
bool Enabled();
void MarkVelocityPipeline(ID3D12PipelineState* pipeline, const D3D12_SHADER_BYTECODE& shader);
void OnReset(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline);
void OnPipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline);
void OnTargets(ID3D12GraphicsCommandList* list, const rtvreg::Info* target, const D3D12_CPU_DESCRIPTOR_HANDLE* depth);
void OnDraw(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances);
void OnBarriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers);
Guides FindGuides(UINT count, ID3D12CommandList* const* lists, UINT width, UINT height);
}
