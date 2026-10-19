// Voxel geometry: shadow pass, opaque lit pass (sun + shadow map + ray-traced sky) and water.
#include "common.hlsli"

struct VSIn
{
    float3 pos : POSITION;
    uint attr : ATTR;
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
    return mul(lightViewProj, mul(model, float4(i.pos, 1)));
}

VSOut VSWorld(VSIn i)
{
    VSOut o;
    uint n = i.attr & 7;
    uint m = (i.attr >> 3) & 7;
    uint ao = (i.attr >> 6) & 3;
    float4 wp = mul(model, float4(i.pos, 1));
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
    float3 cell = floor(i.vpos - on * 0.5);
    float h1 = hash13(cell + float3(7.1, 3.3, 1.7) * i.mat);
    float h2 = hash13(cell.zxy + 19.19);
    float3 albedo = materialColor(i.mat, h1, h2, length(camPos.xyz - i.wpos)) * tint.rgb;

    float ndl = saturate(dot(n, sunDir.xyz));
    float sun = 0;
    if (ndl > 0)
    {
        // Shadow map for long range + a short voxel-exact ray toward the sun to close the contact gaps
        // (inner corners, wall/floor joints) that a 0.1-voxel shadow texel cannot resolve.
        float contact = traceSky(i.wpos + n * 0.02, sunDir.xyz, 7.0, 16);
        sun = shadowFactor(i.wpos, n) * contact * ndl;
    }
    float skyVis = skyVisibility(i.wpos + n * 0.03, n, h1);
    float aoTerm = objParams.x > 0.5 ? pixelAO(i.wpos, i.nidx) : i.ao;

    float3 hemi = lerp(groundCol.rgb, skyZenith.rgb, n.y * 0.5 + 0.5);
    float3 ambient = hemi * skyVis * aoTerm;
    float3 lit = albedo * (sunColor.rgb * sunDir.w * sun + ambient);
    return float4(toDisplay(lit) + ditherNoise(i.sv.xy) / 255.0, 1);
}

float4 PSWater(VSOut i) : SV_Target
{
    float3 n = normalize(i.nrm);
    float3 v = normalize(camPos.xyz - i.wpos);
    float3 on = kNormals[i.nidx];
    float3 cell = floor(i.vpos - on * 0.5);
    float h1 = hash13(cell + 3.0);

    float ndl = saturate(dot(n, sunDir.xyz));
    float sun = ndl > 0 ? shadowFactor(i.wpos, n) * ndl : 0;
    float3 hemi = lerp(groundCol.rgb, skyZenith.rgb, n.y * 0.5 + 0.5);
    float3 albedo = float3(0.02, 0.12, 0.26) * (0.9 + 0.2 * h1);
    float3 lit = albedo * (sunColor.rgb * sunDir.w * sun + hemi * 0.9);

    float3 h = normalize(sunDir.xyz + v);
    float spec = pow(saturate(dot(n, h)), 120.0) * sun * 2.5;
    float fres = pow(1.0 - saturate(dot(n, v)), 4.0);
    lit += spec * sunColor.rgb + fres * skyHorizon.rgb * 0.5;
    float alpha = saturate(tint.a * (0.72 + 0.28 * fres));
    return float4(toDisplay(lit), alpha);
}
