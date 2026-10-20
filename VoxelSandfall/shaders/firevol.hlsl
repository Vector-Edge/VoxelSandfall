// Fire as a volume: every pixel marches a ray through a coarse "heat" field (the flames' fuel: burning coal and oil, painted flames
// - see Renderer::buildFireHeat) and turns the heat into flames with scrolling, turbulent noise: the field is eroded by noise that
// rises, so tongues of flame lick up, split and die away; what is left is lit by a black-body colour ramp and added to the picture.
// The ray stops where the scene is (the depth buffer, resolved to one sample per pixel), so rocks and walls hide flames behind them.
#include "common.hlsli"

Texture3D<float> fireHeat : register(t5);    // R8: 0..1, one texel per fireBoxA.w voxels, covering the whole loaded window
Texture3D<float> fireNoise : register(t6);   // R8: tileable 3D value noise, 64^3
Texture2D<float> sceneDepth : register(t7);  // scene depth, one value per pixel (1 = nothing there)
SamplerState wrapSamp : register(s2);

struct SkyOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

// Colour of fire at a "temperature" 0..1: dull red, orange, yellow, white-yellow (display space).
float3 fireRamp(float t)
{
    float3 c = lerp(float3(0.34, 0.012, 0.0), float3(1.0, 0.24, 0.015), saturate(t / 0.42));
    c = lerp(c, float3(1.0, 0.50, 0.06), saturate((t - 0.42) / 0.30));
    c = lerp(c, float3(1.0, 0.76, 0.24), saturate((t - 0.72) / 0.28));
    return c;
}

float4 PSFireVol(SkyOut i) : SV_Target
{
    if (fireBoxB.w < 0.5) discard;
    const float3 dir = normalize(camFwd.xyz + camRight.xyz * (i.ndc.x * camUp.w * camRight.w) + camUp.xyz * (i.ndc.y * camUp.w));
    const float3 o = camPos.xyz;

    // the part of the ray inside the box that holds all the fire
    const float3 inv = 1.0 / (dir + (step(0.0, dir) * 2.0 - 1.0) * 1e-6);
    const float3 t0 = (fireBoxA.xyz - o) * inv, t1 = (fireBoxB.xyz - o) * inv;
    const float3 tmn = min(t0, t1), tmx = max(t0, t1);
    const float tN = max(max(tmn.x, tmn.y), max(tmn.z, 0.0));
    float tF = min(min(tmx.x, tmx.y), tmx.z);
    if (tF <= tN) discard;

    // ... and in front of the scene
    const float z = sceneDepth.Load(int3(i.pos.xy, 0));
    if (z < 1.0)
    {
        const float n = depthInfo.x, f = depthInfo.y;
        const float lin = n * f / (f - z * (f - n));
        tF = min(tF, lin / max(dot(dir, camFwd.xyz), 1e-3) + 0.6);  // (a flame touching a surface still shows right up to it)
    }
    if (tF <= tN) discard;

    const float3 gridSize = fireGrid.xyz * fireBoxA.w;  // voxels covered by the heat texture
    const int maxSteps = (int)fireGrid.w;
    const float time = camPos.w;
    const float dt = max(1.35, (tF - tN) / (float)maxSteps);
    float t = tN + dt * frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))) + time * 7.13);  // (dithered start, different every frame: no banding)
    float3 acc = 0;
    float trans = 1.0;

    [loop] for (int s = 0; s < maxSteps && t < tF && trans > 0.04; s++)
    {
        const float3 p = o + dir * t;
        const float h0 = fireHeat.SampleLevel(linSamp, p / gridSize, 0);
        if (h0 < 0.012)
        {
            t += dt * 1.9;  // empty space: leap
            continue;
        }
        // moving noise: it scrolls upwards, so the flames rise; two scales for large and small tongues
        const float3 q1 = p * float3(0.046, 0.019, 0.046) + float3(0.0, -time * 0.50, 0.0);          // (tall features: tongues, not blobs)
        const float3 q2 = p * float3(0.115, 0.050, 0.115) + float3(0.31, -time * 0.92, 0.17);
        const float n1 = fireNoise.SampleLevel(wrapSamp, q1, 0);
        const float n2 = fireNoise.SampleLevel(wrapSamp, q2, 0);
        // the flames sway: look the heat up a little to one side
        const float3 pd = p + float3(n1 - 0.5, 0.0, n2 - 0.5) * (5.5 + 5.0 * h0);
        const float h = fireHeat.SampleLevel(linSamp, pd / gridSize, 0);
        const float erosion = saturate((n1 * 0.58 + n2 * 0.42 - 0.5) * 1.7 + 0.5);
        float d = saturate((h * 1.75 - erosion * 1.05) * 1.4);
        d = d * d * (3.0 - 2.0 * d);
        if (d > 0.004)
        {
            const float temp = saturate(d * 1.0 + h * 0.22);
            const float3 emit = fireRamp(temp) * (0.7 + 1.5 * temp);
            acc += trans * emit * (d * dt * 0.105);
            trans *= exp(-d * dt * 0.10);
        }
        t += dt;
    }
    if (trans > 0.995) discard;

    const float3 mid = o + dir * (0.5 * (tN + tF));
    const float visible = 1.0 - fogAmount(mid);
    const float flicker = 0.94 + 0.06 * sin(time * 17.0) * sin(time * 11.3 + 1.7);
    acc /= 1.0 + 0.30 * max(acc.r, max(acc.g, acc.b));  // (a big fire keeps its colour instead of burning out to white)
    return float4(acc * visible * flicker, (1.0 - trans) * visible);
}
