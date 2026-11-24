// Procedural sky dome drawn as a full-screen triangle behind everything.
#include "common.hlsli"

struct SkyOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

SkyOut VSSky(uint id : SV_VertexID)
{
    SkyOut o;
    float2 p = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    o.pos = float4(p, 1, 1);
    o.ndc = p;
    return o;
}

float4 PSSky(SkyOut i) : SV_Target
{
    float3 dir = normalize(camFwd.xyz + camRight.xyz * (i.ndc.x * camUp.w * camRight.w) + camUp.xyz * (i.ndc.y * camUp.w));
    float3 col = skyRadiance(dir);
    return float4(toDisplay(col) + ditherNoise(i.pos.xy) / 255.0, 1);
}

// Drawn over the finished scene while the camera is under water: a deep blue-green veil, lighter towards the surface.
float4 PSUnderwater(SkyOut i) : SV_Target
{
    float3 deep = float3(0.010, 0.085, 0.130), shallow = float3(0.050, 0.300, 0.360);
    return float4(lerp(deep, shallow, saturate(i.ndc.y * 0.5 + 0.55)), 0.55);
}

// Upscales the (lower resolution) scene to the back buffer: bilinear plus a light sharpening (the scene was shaded at
// `render scale` < 1 on low quality settings).
Texture2D<float4> blitSrc : register(t4);

float4 PSBlit(SkyOut i) : SV_Target
{
    float2 uv = float2(i.ndc.x * 0.5 + 0.5, 0.5 - i.ndc.y * 0.5);
    float2 sz;
    blitSrc.GetDimensions(sz.x, sz.y);
    float2 px = 1.0 / sz;
    float3 c = blitSrc.SampleLevel(linSamp, uv, 0).rgb;
    float3 blur = 0.25 * (blitSrc.SampleLevel(linSamp, uv + float2(px.x, 0), 0).rgb + blitSrc.SampleLevel(linSamp, uv - float2(px.x, 0), 0).rgb +
                          blitSrc.SampleLevel(linSamp, uv + float2(0, px.y), 0).rgb + blitSrc.SampleLevel(linSamp, uv - float2(0, px.y), 0).rgb);
    return float4(saturate(c + (c - blur) * 0.45), 1);
}
