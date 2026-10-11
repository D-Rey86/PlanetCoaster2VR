#pragma once

#include <cstddef>
#include <d3d12.h>

namespace pc2vr::rtvreg { struct Info; }
namespace pc2vr::highlight {
void SetEnabled(bool enabled);
void OnPipelineCreated(ID3D12PipelineState* pipeline, const D3D12_SHADER_BYTECODE& pixelShader);
void OnPipelineCreated(ID3D12PipelineState* pipeline, const D3D12_PIPELINE_STATE_STREAM_DESC& stream);
void OnListReset(ID3D12GraphicsCommandList* list, ID3D12PipelineState* initialPipeline);
void OnPipelineBound(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline);
void BeforeIndirectDraw(ID3D12GraphicsCommandList* list);
void OnTargetsBound(ID3D12GraphicsCommandList* list, unsigned count, bool hasDepth,
                    D3D12_CPU_DESCRIPTOR_HANDLE handle, const rtvreg::Info* target);
}
