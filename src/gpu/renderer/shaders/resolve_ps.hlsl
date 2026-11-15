// A resolve that a copy cannot do: one sample of a multisampled color surface, or the depth of a
// depth-stencil surface. The viewport is the destination region; each pixel reads the source texel
// at its position plus the offset.
//
// DEPTH: the source is the depth plane of a D32S8 surface, written to an R32_FLOAT destination.
// MULTISAMPLE: the source is multisampled and the sample is selected.

cbuffer Constants : register(b0, space0)
{
    int2 g_offset;
    uint g_sample;
};

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
    int2 texel = int2(position.xy) + g_offset;
#if MULTISAMPLE
    return g_source.Load(texel, g_sample);
#else
    return g_source.Load(int3(texel, 0));
#endif
}
