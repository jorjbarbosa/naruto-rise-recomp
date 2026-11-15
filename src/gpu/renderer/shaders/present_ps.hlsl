// The front buffer through the title's gamma ramp, scaled to the viewport with bilinear filtering.
// The ramp maps each texel before the filter, as the console's display output maps the front buffer
// before scaling it, so the four texels of the footprint are gathered and filtered here.

Texture2D<float4> g_image : register(t0, space0);
SamplerState g_sampler : register(s0, space1);

// D3DGAMMARAMP as 10-bit values: x red, y green, z blue.
cbuffer Ramp : register(b0, space2)
{
    uint4 g_ramp[256];
};

float3 Ramp(float r, float g, float b)
{
    uint3 index = uint3(round(saturate(float3(r, g, b)) * 255.0));
    return float3(g_ramp[index.r].x, g_ramp[index.g].y, g_ramp[index.b].z) / 1023.0;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    uint width, height;
    g_image.GetDimensions(width, height);
    float2 size = float2(width, height);
    float2 texel = uv * size - 0.5;
    float2 base = floor(texel);
    float2 weight = texel - base;

    // Gathered at the shared corner of the footprint's four texels, so that the hardware's snapping
    // of the coordinate cannot pick another footprint than the one the weights belong to. Order: w
    // (left, top), z (right, top), x (left, bottom), y (right, bottom).
    float2 corner = (base + 1.0) / size;
    float4 r = g_image.GatherRed(g_sampler, corner);
    float4 g = g_image.GatherGreen(g_sampler, corner);
    float4 b = g_image.GatherBlue(g_sampler, corner);
    float3 top = lerp(Ramp(r.w, g.w, b.w), Ramp(r.z, g.z, b.z), weight.x);
    float3 bottom = lerp(Ramp(r.x, g.x, b.x), Ramp(r.y, g.y, b.y), weight.x);
    return float4(lerp(top, bottom, weight.y), 1.0);
}
