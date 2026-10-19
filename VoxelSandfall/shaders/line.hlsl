// Wireframe overlays (creator brush indicator, gun carve preview).
#include "common.hlsli"

float4 VSLine(float3 pos : POSITION) : SV_Position
{
    return mul(viewProj, mul(model, float4(pos, 1)));
}

float4 PSLine() : SV_Target
{
    return tint;
}
