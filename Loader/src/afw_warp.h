#pragma once

#include <cstdint>

#include "afw_inputs.h"
#include "cb_math.h"

namespace pc2vr::afw {
struct Frame {
    Guides guides;
    cbmath::ParsedView source, target;
    UINT width{}, height{};
};

// The capture allocator and fences also protect AFW descriptors and output.
bool Prepare(ID3D12Device* device, const D3D12_RESOURCE_DESC& color, const Frame& frame, bool idle);
ID3D12Resource* Record(ID3D12GraphicsCommandList* list, unsigned slot, int eye, uint64_t serial,
                       const Frame& frame, ID3D12Resource* color, ID3D12Resource* priorColor, DXGI_FORMAT colorFormat);
void InvalidateHistory();
}
