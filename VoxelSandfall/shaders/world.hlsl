// Voxel geometry: shadow pass, opaque lit pass (sun + shadow map + ray-traced sky) and water.
#include "common.hlsli"

struct VSIn
{
    float3 pos : POSITION;
    uint attr : ATTR;
    float2 off : CHUNKOFF;  // per instance: how far the chunk mesh has been moved by window shifts (x, z); 0 for everything else
};

struct VSOut
{
    float4 sv : SV_Position;
    centroid float3 wpos : TEXCOORD0;
    centroid float3 vpos : TEXCOORD1;
    centroid float3 nrm : TEXCOORD2;
    nointerpolation uint mat : TEXCOORD3;
    nointerpolation uint nidx : TEXCOORD4;
    centroid float ao : TEXCOORD5;
};

float4 VSShadow(VSIn i) : SV_Position
{
    // Water droplets do not cast shadows (they would show up as black spots on the bed of a pool): park them outside the clip volume.
    if (((i.attr >> 3) & 15) == 2) return float4(4, 4, 4, 1);
    float4 p = mul(model, float4(i.pos, 1));
    p.xz += i.off;
    return mul(lightViewProj, p);
}

VSOut VSWorld(VSIn i)
{
    VSOut o;
    uint n = i.attr & 7;
    uint m = (i.attr >> 3) & 15;
    uint ao = (i.attr >> 7) & 3;
    float4 wp = mul(model, float4(i.pos, 1));
    wp.xz += i.off;
    o.sv = mul(viewProj, wp);
    o.wpos = wp.xyz;
    o.vpos = i.pos;
    o.nrm = mul((float3x3)model, kNormals[n]);
    o.mat = m;
    o.nidx = n;
    o.ao = 0.38 + 0.62 * (ao / 3.0);
    return o;
}

float4 PSWorld(VSOut i) : SV_Target
{
    float3 n = normalize(i.nrm);
    float3 on = kNormals[i.nidx];
    // Voxel colour noise is keyed by the voxel's global position (the world window moves, the pattern must not).
    float3 cell = floor((objParams.x > 0.5 ? i.wpos + float3(fogB.y, 0, fogB.z) : i.vpos) - on * 0.5);
    float h1 = hash13(cell + float3(7.1, 3.3, 1.7) * i.mat);
    float h2 = hash13(cell.zxy + 19.19);
    float3 albedo = materialColor(i.mat, h1, h2, length(camPos.xyz - i.wpos)) * tint.rgb;

    float ndl = saturate(dot(n, sunDir.xyz));
    float sun = 0;
    if (ndl > 0)
    {
        // Shadow map for long range + a short voxel-exact ray toward the sun to close the contact gaps
        // (inner corners, wall/floor joints) that a 0.1-voxel shadow texel cannot resolve.
        float contact = qual.z > 0.5 ? traceSky(i.wpos + n * 0.02, sunDir.xyz, 7.0, 16) : 1.0;
        sun = shadowFactor(i.wpos, n) * contact * ndl;
    }
    float skyVis = skyVisibility(i.wpos + n * 0.03, n, h1);
    float aoTerm = objParams.x > 0.5 ? pixelAO(i.wpos, i.nidx) : i.ao;

    float3 hemi = lerp(groundCol.rgb, skyZenith.rgb, n.y * 0.5 + 0.5);
    float3 ambient = hemi * skyVis * aoTerm;
    float3 lit = albedo * (sunColor.rgb * sunDir.w * sun + ambient);
    if (i.mat == 2) lit += float3(0.05, 0.12, 0.2);  // splash droplets glow a little so they read as water
    lit += albedo * fireGlow(i.wpos, n);              // flames light what is near them
    lit += materialEmission(i.mat, h1, h2, cell);     // burning coal glows
    float3 col = toDisplay(lit);
    col = lerp(col, fogColorDisplay(i.wpos), fogAmount(i.wpos));
    return float4(col + ditherNoise(i.sv.xy) / 255.0, 1);
}

// ---- water ----------------------------------------------------------------------------------------------
// The surface is shaded from a smooth normal (smoothed voxel surface + ripple field + calm travelling waves), the
// colour is a Fresnel mix of the mirrored world (sky or the voxels around) and the water body, whose colour depends
// on how deep the water is (red light is absorbed first, so shallows are clear and the deep goes teal). Foam sits
// on splashes and along the shore. The pixel shader emits `add` and `mul`: result = add + background * mul.
// Slope of a calm water surface: travelling waves of different lengths whose phase is bent by slow noise, so the
// pattern never reads as a regular grid. Waves that would be smaller than a few pixels fade out (no shimmer).
float2 waveSlope(float2 p, float t, float footprint, int detail)
{
    static const float4 kWave[6] = {  // xy = direction, z = wave number (rad / voxel), w = slope amplitude
        float4(0.83, 0.56, 0.35, 0.034), float4(-0.31, 0.95, 0.58, 0.030), float4(0.62, -0.78, 0.97, 0.026),
        float4(-0.95, -0.30, 1.60, 0.021), float4(0.18, 0.98, 2.70, 0.016), float4(-0.71, 0.70, 4.30, 0.011)};
    float2 s = 0;
    const int count = detail >= 2 ? 6 : (detail == 1 ? 4 : 3);
    [unroll] for (int i = 0; i < 6; i++)
    {
        float4 w = kWave[i];
        float fade = 1.0 - smoothstep(0.5, 1.5, footprint * w.z);
        [branch] if (i < count && fade > 0.002)  // waves smaller than a pixel are skipped
        {
            float bend = detail >= 2 ? vnoise(p * 0.045 + i * 7.3) * 4.0 : (float)i * 1.7;
            float phase = dot(w.xy, p) * w.z + t * 0.45 * sqrt(700.0 * w.z) + bend;
            phase = 6.2831853 * frac(phase * 0.15915494);  // (p is a global coordinate, up to ~65000: keep cos() in its accurate range)
            s += w.xy * (cos(phase) * w.w * fade);
        }
    }
    return s;
}

// Caustics: the soft bright network that sunlight forms on the bed of shallow water. Animated Worley "cell borders"
// (where the nearest two feature points are equally far) at two scales, with the domain bent by slow noise so the
// cells are curvy rather than polygons.
float caustics(float2 p, float t, int octaves)
{
    const float2 warp = octaves >= 2 ? float2(vnoise(p * 0.05 + t * 0.07), vnoise(p.yx * 0.05 - t * 0.06)) - 0.5 : float2(0, 0);
    float c = 0;
    [loop] for (int o = 0; o < octaves; o++)
    {
        float2 q = (p + warp * (o == 0 ? 20.0 : 12.0)) * (0.085 + 0.06 * o) + float2(t * 0.045, -t * 0.036) * (1 + o);
        float2 ip = floor(q), fp = frac(q);
        float d1 = 9, d2 = 9;
        [unroll] for (int y = -1; y <= 1; y++)
        {
            [unroll] for (int x = -1; x <= 1; x++)
            {
                float2 g = float2(x, y);
                float2 h = float2(hash13(float3(ip + g, 3.1 + o)), hash13(float3(ip + g, 7.7 + o)));
                float2 pt = 0.5 + 0.45 * sin(t * (0.5 + 0.6 * h) + 6.2831 * h.yx);
                float d = length(g + pt - fp);
                if (d < d1)
                {
                    d2 = d1;
                    d1 = d;
                }
                else if (d < d2)
                {
                    d2 = d;
                }
            }
        }
        const float e = saturate(1.0 - (d2 - d1) * 2.0);
        c += e * e * (o == 0 ? 0.55 : 0.4);
    }
    return c * (octaves >= 2 ? 0.7 + 0.6 * vnoise(p * 0.03 + 11.0) : 1.0);  // some patches of the bed get more light than others
}

bool isWaterCell(int3 c)
{
    return c.y >= 0 && isLiquidMat(worldOcc.Load(int4(c, 0)) & 15);
}

struct WaterOut
{
    float4 add : SV_Target0;
    float4 mul : SV_Target1;
};

WaterOut PSWater(VSOut i)
{
    const bool oil = i.mat >= 9;       // 9 oil, 10 burning oil (the same pass as water, but thick, dark and shiny)
    const bool burning = i.mat == 10;
    const float3 on = kNormals[i.nidx];
    const int3 cell = int3(floor(i.wpos - on * 0.5));  // the water voxel that owns this face
    const float3 toEye = camPos.xyz - i.wpos;
    const float dist = length(toEye);
    const float3 v = toEye / dist;

    // The top faces, and the little steps and bumps of the staircase surface (a side face whose neighbour is air above
    // more water or above the bank the water ends on, up to 4 voxels down), are the same continuous surface: they get exactly the same smooth normal, so no
    // step edges show.
    const bool top = i.nidx == 2;
    bool step = false;
    if (i.nidx < 2 || i.nidx > 3)
    {
        const int3 outc = cell + int3(on);
        [loop] for (int k = 1; k <= 4; k++)
        {
            const uint m = worldOcc.Load(int4(outc + int3(0, -k, 0), 0)) & 15;
            if (m != 0)  // more water below (a step in the surface), or the bank / terrace the water ends on (a shore step)
            {
                step = true;
                break;
            }
        }
    }
    const bool surface = top || step;
    const int detail = (int)qual.x;  // 0 low, 1 medium, 2 high

    int depth = 0;  // water below this face (voxels)
    {
        int3 c = cell;
        const int maxDepth = detail >= 2 ? 48 : 24;
        [loop] for (int k = 0; k < maxDepth; k++)
        {
            if (!isWaterCell(c)) break;
            depth++;
            c.y--;
        }
    }

    const float4 fld = waterField.SampleLevel(linSamp, i.wpos.xz / gridDims.xz, 0);
    // Waves, caustics and foam are patterns of the world, not of the loaded window: they are keyed by the global position
    // (fogB.yz = the window's origin), so they stay put when the window slides.
    const float2 gp = i.wpos.xz + fogB.yz;
    const float footprint = dist * camUp.w * 0.00185;  // voxels per pixel at this distance
    const float calm = saturate((depth - 0.5) * 0.4);   // thin films do not ripple
    // A film a voxel or two deep (the water where it runs out over a bank or a stone terrace) is not a mirror: it only wets
    // what is under it. Its reflection, ripples and glints fade out with the depth.
    const float thin = smoothstep(0.5, 3.5, (float)depth);
    const float2 slope = (fld.xy * 1.8 + waveSlope(gp, camPos.w, footprint, detail) * calm) * (0.25 + 0.75 * thin) * (oil ? 0.30 : 1.0);
    const float3 n = surface ? normalize(float3(-slope.x, 1.0, -slope.y)) : on;

    // Fresnel (Schlick, water F0 = 0.02): grazing views are mirrors, looking straight down you see through.
    const float cosV = saturate(dot(n, v));
    const float F0 = oil ? 0.05 : 0.02;
    const float F = (F0 + (1.0 - F0) * pow(1.0 - cosV, 5.0)) * thin;

    // What the surface mirrors: the sky, or the voxels (walls, sand heaps) the reflected ray hits.
    float3 r = reflect(-v, n);
    if (surface) r.y = max(r.y, 0.02);
    r = normalize(r);
    float3 refl = skyGradient(r);
    float hitSky = 1.0;
    [branch] if (F > 0.05 && detail >= 1)
    {
        float3 hn;
        uint hm;
        const float th = traceHit(i.wpos + n * 0.06, r, detail >= 2 ? 100.0 : 60.0, detail >= 2 ? 64 : 32, hn, hm);
        if (th >= 0)
        {
            const float3 hp = i.wpos + r * th;
            const float ndl2 = saturate(dot(hn, sunDir.xyz));
            const float sh2 = ndl2 > 0 ? shadowFactor(hp, hn) : 0.0;
            const float3 hemi2 = lerp(groundCol.rgb, skyZenith.rgb, hn.y * 0.5 + 0.5);
            refl = materialColor(hm, 0.5, 0.5, 1000.0) * (sunColor.rgb * sunDir.w * ndl2 * sh2 + hemi2 * 0.9);
            hitSky = 0.0;
        }
    }

    // The water body: light that reaches it and is scattered back, dimmed by the depth it travelled through.
    const float sunVis = shadowFactor(i.wpos, float3(0, 1, 0));  // is the sun visible from here (= is its mirror image on the water)
    const float sunTop = saturate(sunDir.y) * sunVis;
    const float3 light = sunColor.rgb * sunDir.w * sunTop * 0.5 + lerp(groundCol.rgb, skyZenith.rgb, 0.85) * 0.9;
    const float cosR = sqrt(max(0.0, 1.0 - (1.0 - cosV * cosV) / 1.78));  // refracted view angle
    const float pathLen = depth / max(cosR, 0.3);
    const float3 T = exp(-(oil ? float3(0.34, 0.22, 0.12) : float3(0.075, 0.024, 0.012)) * pathLen);
    const float3 Tg = pow(T, 1.0 / 2.2);  // transmittance in display space (the frame buffer holds display colours)
    const float3 bodyCol = toDisplay((oil ? float3(0.030, 0.018, 0.006) : float3(0.012, 0.075, 0.095)) * light * 3.0);

    float3 add = F * toDisplay(refl) + (1.0 - F) * (1.0 - Tg) * bodyCol;
    // Sun glints: the sun's mirror image in the ripples. Tonemapped on its own (the sun is far brighter than anything the
    // convex mix above can carry) with the Fresnel weight applied to the radiance.
    if (hitSky > 0.5 && sunVis > 0.01)
    {
        const float sd = saturate(dot(r, sunDir.xyz));
        add += toDisplay(F * sunVis * sunColor.rgb * sunDir.w * (pow(sd, 700.0) * 9.0 + pow(sd, 70.0) * 0.12));
    }
    if (oil)  // a thin film of oil shimmers in rainbow colours
    {
        const float ph = cosV * 5.0 + vnoise(gp * 0.05) * 8.0 + camPos.w * 0.05;
        const float3 irid = 0.5 + 0.5 * cos(6.2831853 * (ph + float3(0.0, 0.33, 0.67)));
        add += irid * (F * 0.25 + 0.02);
    }
    add += toDisplay(fireGlow(i.wpos, n) * 0.08);  // flames light the liquid too
    float3 mul = (1.0 - F) * Tg * lerp(oil ? 0.50 : 0.82, 1.0, thin);  // wet stone is darker (oil stains it)

    // Sunlight focused by the rippled surface dances over the bed (only where the bed is lit and the water is neither
    // paper thin nor deep, and not so far away that it would shimmer).
    const float causticK = (oil ? 0.0 : 1.0) * smoothstep(1.5, 6.0, depth) * (1.0 - smoothstep(30.0, 80.0, depth)) * (1.0 - smoothstep(160.0, 420.0, dist));
    [branch] if (detail >= 1 && surface && sunTop > 0.01 && causticK > 0.02)  // the Worley pattern is the priciest part: only where it shows
    {
        const float3 rd = refract(-v, n, 1.0 / 1.33);
        const float2 bed = gp + rd.xz / max(-rd.y, 0.2) * depth;
        mul *= 1.0 + causticK * sunTop * 0.95 * (caustics(bed, camPos.w, detail >= 2 ? 2 : 1) - 0.3);
    }

    // Foam: lacy white on fresh splashes and in a thin line where the water meets land.
    [branch] if (!oil && surface && (fld.z > 0.02 || fld.w > 0.12))
    {
        const float lace = detail >= 2 ? 0.5 * vnoise(gp * 0.9 + camPos.w * 0.08) + 0.5 * vnoise(gp * 2.3 - camPos.w * 0.05)
                         : (detail == 1 ? vnoise(gp * 1.2 + camPos.w * 0.06) : 0.5);
        const float shoreBand = smoothstep(0.14, 0.40, fld.w) * smoothstep(0.30, 0.65, lace);
        const float splash = smoothstep(0.30, 1.05, fld.z * (0.25 + 1.3 * lace)) * 0.92;
        const float foamAmt = saturate(max(shoreBand * 0.35 * (0.4 + 0.6 * thin), splash));
        const float3 foamCol = toDisplay(light * float3(0.80, 0.85, 0.88));
        add = lerp(add, foamCol, foamAmt);
        mul *= 1.0 - foamAmt;
    }

    // Burning oil: the whole surface is aflame (flames themselves are particles above it).
    if (burning)
    {
        const float fl = 0.5 + 0.5 * vnoise(gp * 0.45 + camPos.w * 2.6);
        const float fl2 = vnoise(gp * 1.3 - camPos.w * 3.3);
        const float3 glow = float3(1.7, 0.50, 0.09) * (0.55 + 1.15 * fl * (0.6 + 0.8 * fl2));
        add = lerp(add, toDisplay(glow), 0.9);
        mul *= 0.12;
    }

    // chunk fog: the water fades into the fog like everything else (what is seen through it is already fogged)
    const float fog = fogAmount(i.wpos);
    add = lerp(add, fogColorDisplay(i.wpos), fog);
    mul *= 1.0 - fog;

    WaterOut o;
    o.add = float4(add + ditherNoise(i.sv.xy) / 255.0, 1.0);
    o.mul = float4(saturate(mul), 1.0);
    return o;
}
