// One direction of a separable Gaussian blur (renderer/backdrop.cpp): each pixel of the viewport,
// the size of the source, takes the weighted texels along the direction, clamped at the edges.

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

// A sigma of 2.5 texels over 9 taps, normalized.
static const float kWeights[5] = { 0.1716, 0.1584, 0.1246, 0.0835, 0.0477 };

float4 main(float4 position : SV_Position) : SV_Target
{
    uint width, height;
    g_source.GetDimensions(width, height);
    int2 last = int2(width, height) - 1;
    int2 texel = int2(position.xy);
    int2 direction = int2(g_constants.directionX, g_constants.directionY);
    float3 sum = g_source.Load(int3(texel, 0)).rgb * kWeights[0];
    for (int i = 1; i < 5; i++)
    {
        sum += g_source.Load(int3(clamp(texel + direction * i, 0, last), 0)).rgb * kWeights[i];
        sum += g_source.Load(int3(clamp(texel - direction * i, 0, last), 0)).rgb * kWeights[i];
    }
    return float4(sum, 1.0);
}
