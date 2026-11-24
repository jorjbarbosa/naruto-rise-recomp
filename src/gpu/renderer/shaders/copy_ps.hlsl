// A bilinear copy of the source over the viewport, with a constant alpha the pipeline blends with:
// the halving of the backdrop and its composition over the sharp image (renderer/backdrop.cpp).

// Shared by the copy and the blur (FilterConstants in renderer/passes.cpp): root constants on D3D12,
// push constants on Vulkan. Scalars, so that both lay them out at the same offsets.
struct Constants
{
    float alpha;     // the copy's
    int directionX;  // the blur's: (1, 0) or (0, 1)
    int directionY;
};
#ifdef __spirv__
[[vk::push_constant]] Constants g_constants;
#else
ConstantBuffer<Constants> g_constants : register(b0, space0);
#endif

Texture2D<float4> g_source : register(t0, space0);
SamplerState g_sampler : register(s0, space1);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(g_source.Sample(g_sampler, uv).rgb, g_constants.alpha);
}
