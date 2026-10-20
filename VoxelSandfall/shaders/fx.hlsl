// Flames, sparks and smoke: camera-facing sprites, one per instance (see Renderer::buildFx).
//   PSFire  - flame tongues and sparks, blended "premultiplied over" (they glow, and pile up to their own colour, not to white)
//   PSSmoke - alpha blended, lit by the sky and sun and by the flames' own glow
// Sprites overlap a lot (a big fire is thousands of them), so the pixel shaders are kept to a few ALU operations: lighting and
// fog are worked out per corner in the vertex shader, and the "noise" is a couple of sines.
#include "common.hlsli"

struct FxIn
{
    float4 posSize : POSITION;   // xyz = centre, w = half size
    float4 col : COLOR;          // rgb, a (display space)
    float2 misc : TEXCOORD0;     // x = rotation, y = kind (0 flame, 1 spark, 2 smoke)
    uint vid : SV_VertexID;
};

struct FxOut
{
    float4 sv : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR;          // (smoke: already lit)
    float4 misc : TEXCOORD1;     // x = kind, y = seed, z = fade (fog and near-camera), w = fog amount
    float3 fogCol : TEXCOORD2;
};

float cheapNoise(float2 p, float s)
{
    return 0.5 + 0.28 * sin(p.x * 2.7 + p.y * 1.3 + s) + 0.22 * sin(p.x * -1.9 + p.y * 3.3 + s * 1.7 + 1.3);
}

FxOut VSFx(FxIn i)
{
    FxOut o;
    const float2 c = float2((i.vid & 1) ? 1.0 : -1.0, (i.vid & 2) ? 1.0 : -1.0);
    const float flame = i.misc.y < 0.5 ? 1.0 : 0.0;
    // a flame tongue stands upright on the screen (a little lean) and is taller than wide; the rest turns
    const float rot = flame > 0.5 ? (frac(i.misc.x * 0.1591549) - 0.5) * 0.35 : i.misc.x;
    const float cs = cos(rot), sn = sin(rot);
    float2 r = float2(c.x * cs - c.y * sn, c.x * sn + c.y * cs);
    r.y *= flame > 0.5 ? 1.75 : 1.0;
    const float3 centre = i.posSize.xyz + camUp.xyz * (flame > 0.5 ? i.posSize.w * 0.55 : 0.0);  // (the tongue's foot is at the particle)
    const float3 wp = centre + (camRight.xyz * r.x + camUp.xyz * r.y) * i.posSize.w;
    o.sv = mul(viewProj, float4(wp, 1.0));
    o.uv = c;
    o.col = i.col;
    const float fog = fogAmount(wp);
    float fade = saturate((length(camPos.xyz - wp) - 2.0) / 10.0);   // no hard cut when one drifts through the camera
    if (i.misc.y > 1.5)  // smoke: lit from the sun side (screen space) and warmed from below by the flames
    {
        const float sunSide = 0.5 + 0.5 * (c.x * dot(sunDir.xyz, camRight.xyz) + c.y * dot(sunDir.xyz, camUp.xyz));
        float3 rgb = i.col.rgb * (0.62 + 0.55 * sunSide) * (0.55 + 0.45 * saturate(sunDir.y + 0.35));
        rgb += i.col.rgb * toDisplay(fireGlow(wp, float3(0, 1, 0))) * 0.55;
        o.col = float4(rgb, i.col.a * saturate((length(camPos.xyz - wp) - 4.0) / 16.0) * (1.0 - 0.5 * fog));
        fade = 1.0;
    }
    else
    {
        fade *= 1.0 - fog;
    }
    o.misc = float4(i.misc.y, frac(i.misc.x * 0.1591549 + i.posSize.x * 0.013 + i.posSize.z * 0.017), fade, fog);
    o.fogCol = fogColorDisplay(wp);
    return o;
}

float4 PSFire(FxOut i) : SV_Target
{
    float a, hot;
    if (i.misc.x > 0.5)  // spark: a hot point
    {
        const float d2 = dot(i.uv, i.uv);
        if (d2 >= 1.0) discard;
        a = saturate(1.0 - d2);
        a = a * a * a;
        hot = a;
    }
    else                 // a tongue of flame: narrow at the tip, torn sideways by moving noise, hottest low and in the middle
    {
        const float h = saturate(i.uv.y * 0.5 + 0.5);   // 0 at the foot .. 1 at the tip
        const float n = cheapNoise(float2(i.uv.x * 1.6, h * 3.1 - camPos.w * 3.4), i.misc.y * 41.0);
        const float x = i.uv.x + (n - 0.5) * 1.7 * h;
        const float width = pow(saturate(1.0 - h), 0.85) * (0.28 + 0.72 * smoothstep(0.0, 0.22, h));
        const float e = abs(x) / max(width, 0.03);
        a = saturate(1.0 - e * e * 1.25) * saturate(1.0 - h * h) * smoothstep(0.0, 0.22, h);   // (no flat cut at the foot)
        a *= a * (0.7 + 0.6 * n);
        if (a <= 0.01) discard;
        hot = a * saturate(1.0 - h * 1.5) * saturate(1.0 - e * 1.3);
    }
    const float cover = a * i.col.a;
    const float3 rgb = (i.col.rgb * a + float3(1.0, 0.80, 0.42) * hot * 0.55) * i.col.a;
    return float4(rgb * i.misc.z, cover * 0.8 * i.misc.z);
}

float4 PSSmoke(FxOut i) : SV_Target
{
    const float d2 = dot(i.uv, i.uv);
    if (d2 >= 1.0) discard;
    const float n = cheapNoise(i.uv * 1.9, i.misc.y * 47.0 + camPos.w * 0.10);
    const float n2 = cheapNoise(i.uv * 4.3, i.misc.y * 23.0 - camPos.w * 0.17);
    float a = saturate(1.0 - d2);
    a = a * a * (3.0 - 2.0 * a);
    a *= 0.35 + 1.05 * n * (0.55 + 0.9 * n2);
    a *= i.col.a;
    if (a <= 0.004) discard;
    return float4(lerp(i.col.rgb, i.fogCol, i.misc.w), saturate(a));
}
