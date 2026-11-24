// Scene depth of a single-sample target (see depth_ms.hlsl).
Texture2D<float> depth1 : register(t7);

float PSDepth1(float4 pos : SV_Position) : SV_Target
{
    return depth1.Load(int3(pos.xy, 0));
}
