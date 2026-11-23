// The front buffer through the title's gamma ramp, fitted to the viewport. Each window pixel is the
// average of the image's area under it, so that an image larger than the window (the internal
// resolution scale) is reduced without skipping texels; enlarged, the area is one texel wide and the
// weights are bilinear. The ramp maps each texel before the filter, as the console's display output
// maps the front buffer before scaling it.

Texture2D<float4> g_image : register(t0, space0);

// D3DGAMMARAMP as 10-bit values: x red, y green, z blue. D3D12 binds it as a root constant buffer;
// Vulkan has none, so its device address comes in a push constant.
#ifdef __spirv__
struct RampAddress
{
    uint64_t address;
};
[[vk::push_constant]] RampAddress g_ramp;

uint4 RampEntry(uint index)
{
    return vk::RawBufferLoad<uint4>(g_ramp.address + index * 16, 16);
}
#else
cbuffer Ramp : register(b0, space1)
{
    uint4 g_ramp[256];
};

uint4 RampEntry(uint index)
{
    return g_ramp[index];
}
#endif

// The most texels a window pixel averages along an axis: an image up to 32 times the window's size.
static const int kMaxTaps = 33;

float3 Ramp(float3 color)
{
    uint3 index = uint3(round(saturate(color) * 255.0));
    return float3(RampEntry(index.r).x, RampEntry(index.g).y, RampEntry(index.b).z) / 1023.0;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    uint width, height;
    g_image.GetDimensions(width, height);
    float2 size = float2(width, height);
    // Texels per window pixel: uv is linear over the viewport, so its change between neighboring
    // pixels is the same everywhere.
    float2 ratio = abs(float2(ddx(uv.x), ddy(uv.y))) * size;
    // The area under the pixel, in texels, at least one texel wide. A texel weighs the part of it
    // inside; texels past the edge are the edge's (clamp).
    float2 reach = max(ratio, 1.0) * 0.5;
    float2 low = uv * size - reach;
    float2 high = uv * size + reach;
    int2 first = int2(floor(low));
    int2 last = min(int2(ceil(high)) - 1, first + kMaxTaps - 1);
    int2 edge = int2(width, height) - 1;

    float3 sum = 0.0;
    float total = 0.0;
    [loop]
    for (int y = first.y; y <= last.y; y++)
    {
        float weightY = min(y + 1.0, high.y) - max(float(y), low.y);
        [loop]
        for (int x = first.x; x <= last.x; x++)
        {
            float weight = weightY * (min(x + 1.0, high.x) - max(float(x), low.x));
            int2 texel = clamp(int2(x, y), 0, edge);
            sum += weight * Ramp(g_image.Load(int3(texel, 0)).rgb);
            total += weight;
        }
    }
    return float4(sum / total, 1.0);
}
