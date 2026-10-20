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
    float4 qual;        // x = water detail (0 low .. 2 high), y = 1: 9-tap shadow filter (else 4), z = 1: contact shadow ray, w = 1: stable sky rays
    float4 fogA;        // x = distance fog on, y = fog start distance (horizontal, from the eye), z = distance where it is total
    float4 fogB;        // yz = hash origin (global voxel x, z mod 65536), w = 1: sky below the horizon is fog coloured
    float4 skyView;     // rgb = zenith colour of the visible sky, w = cloud cover (0 = none)
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
Texture2D<float4> waterField : register(t3);   // xy = water surface slope (smoothed voxel surface + ripples), z = foam, w = shore
SamplerComparisonState shadowSamp : register(s0);
SamplerState linSamp : register(s1);

static const float3 kNormals[6] = {
    float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)
};

float hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float vnoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash13(float3(i, 1.0)), b = hash13(float3(i + float2(1, 0), 1.0));
    float c = hash13(float3(i + float2(0, 1), 1.0)), d = hash13(float3(i + float2(1, 1), 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
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

// The procedural sky gradient (linear radiance) in a direction, without the sun.
float3 skyGradient(float3 dir)
{
    float up = dir.y;
    float3 col = lerp(skyHorizon.rgb, skyView.rgb, pow(saturate(up), 0.45));
    // Below the horizon: a dark void in the arena; in the endless world the ground goes on for ever in haze, a touch darker the
    // further down you look (the fog on distant voxels uses the very same colour, so where the loaded world ends nothing shows).
    float3 voidCol = lerp(skyHorizon.rgb, float3(0.035, 0.04, 0.055), saturate(-up * 5.0));
    float3 hazeCol = lerp(skyHorizon.rgb, float3(0.17, 0.20, 0.28), 0.80 * smoothstep(0.0, 0.55, -up));
    return up >= 0 ? col : lerp(voidCol, hazeCol, fogB.w);
}

// Distance haze for the endless world: nothing for the first stretch, then a soft, slowly thickening veil that has swallowed
// everything just before the edge of the loaded window (so the unloaded world is never seen, and chunks never pop in or out
// where you can see them). Round: the distance from the eye (height counts half), so what you see is a dome around you, never the
// square of the window - the window is wider than the dome in every direction, corners included.
float fogAmount(float3 wpos)
{
    if (fogA.x < 0.5) return 0.0;
    const float3 d = wpos - camPos.xyz;
    const float dist = sqrt(d.x * d.x + d.z * d.z + 0.25 * d.y * d.y);
    float t = saturate((dist - fogA.y) / (fogA.z - fogA.y));
    // Thin for a long way, then easing into total with neither slope nor curvature at the end (either would read as an edge:
    // far ground is squeezed into a few pixel rows, so any change of rate there shows up as a line).
    const float g = t * t * (0.30 + 0.70 * t);
    const float x = saturate((t - 0.35) / 0.65);
    return lerp(g, 1.0, x * x * x * (x * (x * 6.0 - 15.0) + 10.0));
}

// How much of the sky in a direction is cloud (0..1): soft, slowly drifting, thinning out towards the horizon.
float cloudDensity(float3 dir)
{
    if (skyView.w <= 0.0 || dir.y < 0.012) return 0.0;
    float2 p = dir.xz / (dir.y + 0.16) * 2.0 + float2(camPos.w * 0.010, camPos.w * 0.004);
    float n = 0.5 * vnoise(p) + 0.3 * vnoise(p * 2.3 + 7.1) + 0.2 * vnoise(p * 5.1 + 3.3);
    return smoothstep(0.50, 0.80, n) * skyView.w * smoothstep(0.012, 0.20, dir.y);
}

// Sky plus clouds, sun glow and sun disc.
float3 skyRadiance(float3 dir)
{
    float sd = saturate(dot(dir, sunDir.xyz));
    float3 col = skyGradient(dir);
    float cd = cloudDensity(dir);
    float3 cloudCol = lerp(float3(0.58, 0.62, 0.72), float3(1.0, 0.96, 0.88), saturate(sd * 2.5 + 0.25)) * (0.70 + 0.30 * saturate(sunDir.y * 2.0));
    col = lerp(col, cloudCol, cd * 0.9);
    return col + sunColor.rgb * (pow(sd, 900.0) * 14.0 * (1.0 - cd) + pow(sd, 32.0) * 0.28);
}

// What distant things fade into: the sky in the direction you look at them (without clouds), so the edge of the loaded world and
// the sky / haze beyond it are the same colour.
float3 fogColorDisplay(float3 wpos)
{
    const float3 dir = normalize(wpos - camPos.xyz + float3(0.0001, 0.0, 0.0001));
    const float sd = saturate(dot(dir, sunDir.xyz));
    return toDisplay(skyGradient(dir) + sunColor.rgb * (pow(sd, 32.0) * 0.28));
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
    [branch] if (qual.y > 0.5)
    {
        [unroll] for (int y = -1; y <= 1; y++)
            [unroll] for (int x = -1; x <= 1; x++)
                s += shadowMap.SampleCmpLevelZero(shadowSamp, uv + float2(x, y) * t, ndc.z - 0.00003);
        return s / 9.0;
    }
    // cheap filter: four taps, each already a 2x2 hardware comparison
    [unroll] for (int k = 0; k < 4; k++)
        s += shadowMap.SampleCmpLevelZero(shadowSamp, uv + float2((k & 1) ? 0.7 : -0.7, (k & 2) ? 0.7 : -0.7) * t, ndc.z - 0.00003);
    return s * 0.25;
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

// Like traceSky, but reports what the ray hit: returns the hit distance (or -1 when it escapes), the face normal of
// the entered voxel and its material. Only sand / stone / bedrock count; water is transparent.
float traceHit(float3 o, float3 d, float maxDist, int maxSteps, out float3 hn, out uint hm)
{
    float3 nrm = float3(0, 1, 0);
    uint mat = 0;
    float result = -1;
    int3 dims = int3(gridDims.xyz);
    float3 inv = 1.0 / (d + (step(0.0, d) * 2.0 - 1.0) * 1e-6);
    float t = 0;
    float3 axisN = float3(0, 1, 0);  // unit vector of the axis crossed last
    float axisD = d.y;
    [loop] for (int i = 0; i < maxSteps; i++)
    {
        float3 p = o + d * (t + 1e-3);
        int3 cell = int3(floor(p));
        if (cell.y >= dims.y) break;
        if (cell.y < 0)
        {
            mat = 4;
            result = t;
            break;
        }
        if (cell.x < 0 || cell.z < 0 || cell.x >= dims.x || cell.z >= dims.z) break;
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
            if (m == 1 || m >= 3)
            {
                mat = m;
                nrm = axisN * (axisD > 0 ? -1.0 : 1.0);
                result = t;
                break;
            }
            lo = float3(cell);
            hi = lo + 1.0;
        }
        float3 tb = (lerp(lo, hi, step(0.0, d)) - o) * inv;
        t = min(tb.x, min(tb.y, tb.z));
        if (tb.x <= tb.y && tb.x <= tb.z)
        {
            axisN = float3(1, 0, 0);
            axisD = d.x;
        }
        else if (tb.y <= tb.z)
        {
            axisN = float3(0, 1, 0);
            axisD = d.y;
        }
        else
        {
            axisN = float3(0, 0, 1);
            axisD = d.z;
        }
        if (t > maxDist) break;
    }
    hn = nrm;
    hm = mat;
    return result;
}

// Ray-traced sky visibility (cosine-weighted hemisphere rays, rotated per voxel).
float skyVisibility(float3 p, float3 n, float seed)
{
    int rayCount = (int)params.y;
    if (rayCount <= 0) return 1;
    // Only on the low quality tiers: far surfaces get fewer rays (their voxels are smaller than a pixel).
    if (qual.w > 0.5)
    {
        const float camDist = length(p - camPos.xyz);
        if (camDist > 420.0) rayCount = max(1, rayCount >> 2);
        else if (camDist > 200.0) rayCount = max(1, rayCount >> 1);
    }
    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    // With few rays, per-voxel random directions show up as grain; fixed directions give a smooth (if banded) result.
    float rot = (qual.w > 0.5 ? 0.37 : seed) * 6.2831853;
    float vis = 0;
    [loop] for (int i = 0; i < rayCount; i++)
    {
        float u = (i + 0.5) / rayCount;
        float r = sqrt(u);
        float phi = i * 2.39996323 + rot;
        float3 d = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(1.0 - u);
        vis += traceSky(p, d, params.z, (int)params.w);
    }
    // few rays: count one unoccluded ray as a prior so fully blocked pixels are dim rather than pitch black
    return qual.w > 0.5 ? (vis + 1.0) / (rayCount + 1.0) : vis / rayCount;
}

// Per-voxel albedo with a little random variation that fades out once voxels become sub-pixel.
float3 materialColor(uint m, float h1, float h2, float dist)
{
    float fade = saturate((520.0 - dist) / 380.0);
    float v = 1.0 + (h1 - 0.5) * 0.22 * fade;
    if (m == 1) return float3(0.52, 0.42, 0.23) * v;                                          // sand
    if (m == 2) return float3(0.22, 0.55, 0.90) * v;                                          // water (droplets)
    if (m == 4) return float3(0.042, 0.045, 0.056) * (1.0 + (h1 - 0.5) * 0.35 * fade);          // permanent stone
    if (m == 5)  // dirt: brown earth with grit (stone) and sandy flecks, a mixture of the two
    {
        float3 c = float3(0.150, 0.092, 0.052);
        c = lerp(c, float3(0.20, 0.195, 0.19), step(0.88, h2) * 0.85);
        c = lerp(c, float3(0.33, 0.25, 0.14), step(0.93, h1) * 0.8);
        return c * v;
    }
    if (m == 6)  // grass
    {
        float3 c = lerp(float3(0.040, 0.125, 0.025), float3(0.095, 0.20, 0.040), h2);
        return c * (0.8 + 0.4 * h1);
    }
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
