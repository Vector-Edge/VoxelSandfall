// 2D overlay: crosshair, text, panels. Positions are in pixels (origin top-left).
cbuffer UICB : register(b0)
{
    float2 screenSize;
    float2 pad;
};

Texture2D uiTex : register(t0);
SamplerState uiSamp : register(s0);

struct UIIn
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

struct UIOut
{
    float4 sv : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

UIOut VSUI(UIIn i)
{
    UIOut o;
    o.sv = float4(i.pos.x / screenSize.x * 2.0 - 1.0, 1.0 - i.pos.y / screenSize.y * 2.0, 0, 1);
    o.uv = i.uv;
    o.col = i.col;
    return o;
}

float4 PSUI(UIOut i) : SV_Target
{
    return i.col * uiTex.Sample(uiSamp, i.uv);
}
