#pragma once

namespace pc2vr::afw {
inline constexpr char kShader[] = R"(
struct View {
    float4 posNear;
    float4 rightK;
    float4 upJx;
    float4 forwardJy;
    float4 tanFov;  // left, right, up, down
};
cbuffer Frame : register(b0) { View source, target, previous; uint history; };
Texture2D<float> Depth : register(t0);
Texture2D<float2> Motion : register(t1);
Texture2D<float4> Color : register(t2);
Texture2D<float4> HistoryColor : register(t3);
Texture2D<uint> HistoryDepth : register(t4);
Texture2D<uint> FreshGrid : register(t5);
Texture2D<uint> HistoryGrid : register(t6);
RWTexture2D<uint> CurrentDepth : register(u0);
RWTexture2D<uint> FreshSplat : register(u1);
RWTexture2D<uint> HistorySplat : register(u2);
RWTexture2D<float4> Output : register(u3);
SamplerState LinearClamp : register(s0);

static const uint Sky = 1u;
static const uint Moving = 0x80000000u;

float2 Jitter(View v) { return float2(v.upJx.w, -v.forwardJy.w) * .5; }
float3 Ray(View v, float2 uv) {
    return v.forwardJy.xyz + v.rightK.xyz * lerp(v.tanFov.x, v.tanFov.y, uv.x)
                           + v.upJx.xyz * lerp(v.tanFov.z, v.tanFov.w, uv.y);
}
float2 Project(View v, float3 relative, float z) {
    float2 t = float2(dot(relative, v.rightK.xyz), dot(relative, v.upJx.xyz)) / z;
    return (t - v.tanFov.xz) / (v.tanFov.yw - v.tanFov.xz);
}
bool InImage(float2 uv) { return all(uv >= 0) && all(uv < 1); }

uint2 DepthTexel(uint2 cell, uint2 rawSize, uint2 gridSize) {
    return min(uint2((float2(cell) + .5) * float2(rawSize) / float2(gridSize)), rawSize-1);
}
float2 DepthUv(uint2 cell, uint2 rawSize, uint2 gridSize, View view) {
    return (float2(DepthTexel(cell, rawSize, gridSize)) + .5) / float2(rawSize) - Jitter(view);
}
float InverseZ(float raw, View v) {
    return raw > 0 ? max(0, (raw - v.rightK.w) / v.posNear.w) : 0;
}
float3 Relative(View from, View to, float2 uv, uint depth) {
    float3 ray = Ray(from, uv);
    return depth == Sky ? ray : ray / asfloat(depth) + (from.posNear.xyz - to.posNear.xyz);
}

[numthreads(8,8,1)]
void Clear(uint3 thread : SV_DispatchThreadID) {
    uint2 size;
    FreshSplat.GetDimensions(size.x, size.y);
    if (any(thread.xy >= size)) return;
    FreshSplat[thread.xy] = 0;
    HistorySplat[thread.xy] = 0;
}

void Splat(View view, float2 uv, uint depth, uint2 size, bool old) {
    float3 rel = Relative(view, target, uv, depth);
    float z = dot(rel, target.forwardJy.xyz);
    if (z <= 1e-5) return;
    float2 dst = Project(target, rel, z);
    if (any(dst < -.01) || any(dst > 1.01)) return;
    uint key = depth == Sky ? Sky : asuint(rcp(z));
    int2 base = int2(floor(dst * size - .5));
    [unroll] for (uint i=0; i<4; ++i) {
        int2 p = base + int2(i & 1, i >> 1);
        if (all(p >= 0) && all(p < int2(size))) {
            uint ignored;
            if (old) InterlockedMax(HistorySplat[p], key, ignored);
            else InterlockedMax(FreshSplat[p], key, ignored);
        }
    }
}

[numthreads(8,8,1)]
void Build(uint3 thread : SV_DispatchThreadID) {
    uint2 grid, raw;
    FreshSplat.GetDimensions(grid.x, grid.y);
    Depth.GetDimensions(raw.x, raw.y);
    uint2 p = thread.xy;
    if (any(p >= grid)) return;
    uint2 texel = DepthTexel(p, raw, grid);
    float2 uv = (float2(texel)+.5)/raw-Jitter(source);
    float inverse = InverseZ(Depth[texel], source);
    uint key = inverse > 0 ? asuint(inverse) : Sky;
    // Guide motion is current to previous in native render pixels. Compare
    // against the motion of a static surface under the actual eye cameras.
    // Only history agrees to fill occlusions. Fresh geometry uses its own depth.
    bool moving = key != Sky;
    if (history && key != Sky) {
        float3 old = Relative(source, previous, uv, key);
        float z = dot(old, previous.forwardJy.xyz);
        if (z > 1e-5) {
            float2 stationary = (Project(previous, old, z)-uv)*raw;
            float2 error = abs(Motion[texel]-stationary);
            moving = !all(error < 1.5);
        }
    }
    CurrentDepth[p] = key | (moving ? Moving : 0);
    Splat(source, uv, key, grid, false);
    if (history) {
        uint oldDepth = HistoryDepth[p];
        if (oldDepth && !(oldDepth & Moving)) Splat(previous, DepthUv(p,raw,grid,previous), oldDepth, grid, true);
    }
}

bool Agree(float a, float b) { return abs(a-b) <= max(a,b)*.035; }

bool SampleFresh(float2 uv, uint key, out float3 result) {
    result = 0;
    if (!key) return false;
    float3 relative = Relative(target, source, uv, key);
    float z = dot(relative, source.forwardJy.xyz);
    if (z <= 1e-5) return false;
    float2 from = Project(source, relative, z);
    if (!InImage(from)) return false;
    uint2 raw;
    Depth.GetDimensions(raw.x, raw.y);
    float2 depthUv = from + Jitter(source);
    if (!InImage(depthUv)) return false;
    float expected = key == Sky ? 0 : rcp(z);
    float measured = InverseZ(Depth[min(uint2(depthUv*raw),raw-1)], source);
    if (!Agree(measured,expected)) return false;
    result = Color.SampleLevel(LinearClamp, from, 0).rgb;
    return true;
}

bool SampleHistory(float2 uv, uint key, uint2 grid, out float3 result) {
    result = 0;
    if (!history || !key) return false;
    float3 relative = Relative(target, previous, uv, key);
    float z = dot(relative, previous.forwardJy.xyz);
    if (z <= 1e-5) return false;
    float2 from = Project(previous, relative, z);
    if (!InImage(from)) return false;
    float2 depthUv = from + Jitter(previous);
    if (!InImage(depthUv)) return false;
    uint prior = HistoryDepth[min(uint2(depthUv*grid),grid-1)];
    if (!prior || (prior & Moving)) return false;
    if (!Agree(prior == Sky ? 0 : asfloat(prior), key == Sky ? 0 : rcp(z))) return false;
    // Reject an old surface when the fresh eye can now see behind it.
    // A surface hidden by a nearer object can still fill the other eye.
    float3 check = Relative(target,source,uv,key);
    float checkZ = dot(check,source.forwardJy.xyz);
    if (key != Sky && checkZ > 1e-5) {
        float2 at = Project(source,check,checkZ)+Jitter(source);
        if (InImage(at)) {
            uint2 raw;
            Depth.GetDimensions(raw.x,raw.y);
            float fresh = InverseZ(Depth[min(uint2(at*raw),raw-1)],source);
            if (fresh < rcp(checkZ)*.965) return false;
        }
    }
    result = HistoryColor.SampleLevel(LinearClamp,from,0).rgb;
    return true;
}

[numthreads(8,8,1)]
void Resolve(uint3 thread : SV_DispatchThreadID) {
    uint2 size, grid;
    Output.GetDimensions(size.x,size.y);
    if (any(thread.xy>=size)) return;
    FreshGrid.GetDimensions(grid.x,grid.y);
    float2 uv = (float2(thread.xy)+.5)/size;
    int2 cell = int2(uv*grid);
    float3 result;
    if (SampleFresh(uv,FreshGrid[cell],result)) {
        Output[thread.xy] = float4(result,1);
        return;
    }
    if (SampleHistory(uv,HistoryGrid[cell],grid,result)) {
        Output[thread.xy] = float4(result,1);
        return;
    }
    // Unseen pixels have no exact colour. Extend the farther of the nearby
    // visible surfaces, avoiding a stretched strip from a near seat or rail.
    [loop] for (int step=1; step<=32; step*=2) {
        int2 left = max(cell-int2(step,0),0);
        int2 right = min(cell+int2(step,0),int2(grid)-1);
        uint l = FreshGrid[left], r = FreshGrid[right];
        int2 fill = l && (!r || l<=r) ? left : right;
        if (SampleFresh((float2(fill)+.5)/grid,FreshGrid[fill],result)) {
            Output[thread.xy] = float4(result,1);
            return;
        }
    }
    Output[thread.xy] = float4(Color.SampleLevel(LinearClamp,uv,0).rgb,1);
}
)";
}
