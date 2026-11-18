// A bilinear copy of the source over the viewport, with a constant alpha the pipeline blends with:
// the halving of the backdrop and its composition over the sharp image (renderer/backdrop.cpp).

// Shared by the copy and the blur (FilterConstants in renderer/passes.cpp).
cbuffer Constants : register(b0, space0)
{
    float g_alpha;    // the copy's
    int2 g_direction; // the blur's: (1, 0) or (0, 1)
};

Texture2D<float4> g_source : register(t0, space0);
SamplerState g_sampler : register(s0, space1);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(g_source.Sample(g_sampler, uv).rgb, g_alpha);
}
