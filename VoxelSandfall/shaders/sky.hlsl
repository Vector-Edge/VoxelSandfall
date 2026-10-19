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
    float up = dir.y;
    float3 col = lerp(skyHorizon.rgb, skyZenith.rgb, pow(saturate(up), 0.45));
    float3 below = lerp(skyHorizon.rgb, float3(0.035, 0.04, 0.055), saturate(-up * 5.0));
    col = up >= 0 ? col : below;

    float sd = saturate(dot(dir, sunDir.xyz));
    col += sunColor.rgb * (pow(sd, 900.0) * 14.0 + pow(sd, 32.0) * 0.28);
    return float4(toDisplay(col) + ditherNoise(i.pos.xy) / 255.0, 1);
}
