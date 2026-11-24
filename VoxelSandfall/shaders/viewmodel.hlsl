// First-person weapon model (AK-style rifle) and muzzle flash.
cbuffer VMCB : register(b2)
{
    row_major float4x4 mvp;
    row_major float4x4 modelView;
    float4 lightDirView;   // xyz = sun direction in view space, w = intensity
    float4 ambientCol;
    float4 flashCol;
};

struct VMIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float4 col : COLOR0;
};

struct VMOut
{
    float4 sv : SV_Position;
    float3 nrm : TEXCOORD0;
    float3 vpos : TEXCOORD1;
    float4 col : COLOR0;
};

VMOut VSView(VMIn i)
{
    VMOut o;
    o.sv = mul(mvp, float4(i.pos, 1));
    o.nrm = mul((float3x3)modelView, i.nrm);
    o.vpos = mul(modelView, float4(i.pos, 1)).xyz;
    o.col = i.col;
    return o;
}

float4 PSView(VMOut i) : SV_Target
{
    float3 n = normalize(i.nrm);
    float ndl = saturate(dot(n, lightDirView.xyz));
    float3 v = normalize(-i.vpos);
    float3 h = normalize(lightDirView.xyz + v);
    float spec = pow(saturate(dot(n, h)), 40.0) * 0.35;
    float rim = pow(1.0 - saturate(dot(n, v)), 3.0) * 0.12;
    float3 lit = i.col.rgb * (ambientCol.rgb + lightDirView.w * ndl) + spec + rim;
    lit += flashCol.rgb * 0.25 * flashCol.a;   // muzzle light spilling onto the weapon
    return float4(pow(saturate(lit), 1.0 / 2.2), 1);
}

float4 PSFlash(VMOut i) : SV_Target
{
    return float4(i.col.rgb * flashCol.a, 1);   // additive blend
}
