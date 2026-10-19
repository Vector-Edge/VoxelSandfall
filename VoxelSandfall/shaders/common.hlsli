// Shared constants + lighting helpers for the voxel renderer.
#pragma pack_matrix(row_major)

cbuffer FrameCB : register(b0)
{
    float4x4 viewProj;
    float4x4 lightViewProj;
    float4 camPos;      // xyz, w = time
    float4 sunDir;      // xyz = direction towards the sun, w = intensity
    float4 sunColor;
    float4 skyZenith;
    float4 skyHorizon;
    float4 groundCol;
    float4 params;      // x = shadow texel, y = sky ray count, z = sky ray length, w = max DDA steps
    float4 gridDims;    // xyz = world size in voxels
    float4 camRight;    // xyz, w = aspect
    float4 camUp;       // xyz, w = tan(fov/2)
    float4 camFwd;      // xyz
};

cbuffer ObjectCB : register(b1)
{
    float4x4 model;
    float4 tint;        // rgb = multiplier, a = water opacity
    float4 objParams;
};

Texture2D<float> shadowMap : register(t0);
Texture3D<uint> worldOcc : register(t1);
Texture3D<uint> brickOcc : register(t2);   // 8x8x8-voxel bricks: 1 = contains something opaque
SamplerComparisonState shadowSamp : register(s0);

static const float3 kNormals[6] = {
    float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)
};

float hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float3 acesTonemap(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// Interleaved-gradient-noise dither: removes 8-bit banding in smooth gradients (sky, soft shadows).
float ditherNoise(float2 pix)
{
    return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715)))) - 0.5;
}

float3 toDisplay(float3 linearColor)
{
    return pow(acesTonemap(linearColor * 0.85), 1.0 / 2.2);
}

// 3x3 PCF sun shadow. The map is a 4096^2 orthographic depth map fitted to the arena.
float shadowFactor(float3 wpos, float3 n)
{
    float3 p = wpos + n * 0.2;
    float4 lp = mul(lightViewProj, float4(p, 1));
    float3 ndc = lp.xyz / lp.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1 || ndc.z > 1) return 1;
    float t = params.x;
    float s = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
            s += shadowMap.SampleCmpLevelZero(shadowSamp, uv + float2(x, y) * t, ndc.z - 0.00003);
    return s / 9.0;
}

// Ray march through the world occupancy volume: 1 = the ray escapes to the sky, 0 = blocked.
// Empty 8x8x8 bricks are skipped in one step; occupied ones are walked voxel by voxel.
float traceSky(float3 o, float3 d, float maxDist, int maxSteps)
{
    int3 dims = int3(gridDims.xyz);
    float3 inv = 1.0 / (d + (step(0.0, d) * 2.0 - 1.0) * 1e-6);
    float t = 0;
    [loop] for (int i = 0; i < maxSteps; i++)
    {
        float3 p = o + d * (t + 1e-3);
        int3 cell = int3(floor(p));
        if (cell.y >= dims.y) return 1;
        if (cell.y < 0) return 0;
        if (cell.x < 0 || cell.z < 0 || cell.x >= dims.x || cell.z >= dims.z) return 1;
        int3 brick = cell >> 3;
        float3 lo, hi;
        if (brickOcc.Load(int4(brick, 0)) == 0)
        {
            lo = float3(brick * 8);
            hi = lo + 8.0;
        }
        else
        {
            uint m = worldOcc.Load(int4(cell, 0)) & 15;
            if (m == 1 || m >= 3) return 0;
            lo = float3(cell);
            hi = lo + 1.0;
        }
        float3 tb = (lerp(lo, hi, step(0.0, d)) - o) * inv;
        t = min(tb.x, min(tb.y, tb.z));
        if (t > maxDist) return 1;
    }
    return 1;
}

// Ray-traced sky visibility (cosine-weighted hemisphere rays, rotated per voxel).
float skyVisibility(float3 p, float3 n, float seed)
{
    int rayCount = (int)params.y;
    if (rayCount <= 0) return 1;
    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    float rot = seed * 6.2831853;
    float vis = 0;
    [loop] for (int i = 0; i < rayCount; i++)
    {
        float u = (i + 0.5) / rayCount;
        float r = sqrt(u);
        float phi = i * 2.39996323 + rot;
        float3 d = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(1.0 - u);
        vis += traceSky(p, d, params.z, (int)params.w);
    }
    return vis / rayCount;
}

// Per-voxel albedo with a little random variation that fades out once voxels become sub-pixel.
float3 materialColor(uint m, float h1, float h2, float dist)
{
    float fade = saturate((520.0 - dist) / 380.0);
    float v = 1.0 + (h1 - 0.5) * 0.22 * fade;
    if (m == 1) return float3(0.52, 0.42, 0.23) * v;                                          // sand
    if (m == 2) return float3(0.10, 0.36, 0.62) * v;                                          // water (debris)
    if (m == 4) return float3(0.042, 0.045, 0.056) * (1.0 + (h1 - 0.5) * 0.35 * fade);          // permanent stone
    return float3(0.25, 0.248, 0.245) * v * float3(1.0, 1.0 + (h2 - 0.5) * 0.05 * fade, 1.0);   // normal stone
}

// ---- per-pixel ambient occlusion from the occupancy volume (chunk meshes carry no vertex AO) ----------
static const int3 kFaceN[6] = {int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0), int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)};
static const int3 kFaceU[6] = {int3(0, 1, 0), int3(0, 0, 1), int3(0, 0, 1), int3(1, 0, 0), int3(1, 0, 0), int3(0, 1, 0)};
static const int3 kFaceV[6] = {int3(0, 0, 1), int3(0, 1, 0), int3(1, 0, 0), int3(0, 0, 1), int3(0, 1, 0), int3(1, 0, 0)};

bool occOpaque(int3 c)
{
    int3 dims = int3(gridDims.xyz);
    if (c.y < 0) return true;
    if (c.y >= dims.y || c.x < 0 || c.z < 0 || c.x >= dims.x || c.z >= dims.z) return false;
    uint m = worldOcc.Load(int4(c, 0)) & 15;
    return m == 1 || m >= 3;
}

float aoLevel(bool s1, bool s2, bool c)
{
    return (s1 && s2) ? 0.0 : 3.0 - ((s1 ? 1.0 : 0.0) + (s2 ? 1.0 : 0.0) + (c ? 1.0 : 0.0));
}

float pixelAO(float3 wpos, uint face)
{
    int3 n = kFaceN[face], u = kFaceU[face], v = kFaceV[face];
    int3 cell = int3(floor(wpos - 0.5 * float3(n)));
    int3 f = cell + n;
    float3 local = wpos - float3(cell);
    float fu = saturate(dot(local, float3(u))), fv = saturate(dot(local, float3(v)));
    bool pu = occOpaque(f + u), mu = occOpaque(f - u), pv = occOpaque(f + v), mv = occOpaque(f - v);
    float a00 = aoLevel(mu, mv, occOpaque(f - u - v));
    float a10 = aoLevel(pu, mv, occOpaque(f + u - v));
    float a01 = aoLevel(mu, pv, occOpaque(f - u + v));
    float a11 = aoLevel(pu, pv, occOpaque(f + u + v));
    float ao = lerp(lerp(a00, a10, fu), lerp(a01, a11, fu), fv);
    return 0.38 + 0.62 * (ao / 3.0);
}
