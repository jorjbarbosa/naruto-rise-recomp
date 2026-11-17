// A resolve that a copy cannot do: one sample of a multisampled color surface, or the depth of a
// depth-stencil surface. The viewport is the destination region; each pixel reads the source texel
// at its position plus the offset.
//
// DEPTH: the source is the depth plane of a D32S8 surface, written to an R32_FLOAT destination.
// MULTISAMPLE: the source is multisampled and the sample is selected.

// Root constants on D3D12, push constants on Vulkan (where a constant buffer in register b0 would
// take the binding of the source).
struct Constants
{
    int2 offset;
    uint sample;
};
#ifdef __spirv__
[[vk::push_constant]] Constants g_constants;
#else
ConstantBuffer<Constants> g_constants : register(b0, space0);
#endif

#if DEPTH
#define Value float
#else
#define Value float4
#endif

#if MULTISAMPLE
Texture2DMS<Value> g_source : register(t0, space0);
#else
Texture2D<Value> g_source : register(t0, space0);
#endif

Value main(float4 position : SV_Position) : SV_Target
{
    int2 texel = int2(position.xy) + g_constants.offset;
#if MULTISAMPLE
    return g_source.Load(texel, g_constants.sample);
#else
    return g_source.Load(int3(texel, 0));
#endif
}
