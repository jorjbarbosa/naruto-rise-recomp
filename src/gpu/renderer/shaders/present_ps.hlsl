// The front buffer, scaled to the viewport with bilinear filtering.

Texture2D<float4> g_image : register(t0, space0);
SamplerState g_sampler : register(s0, space1);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(g_image.SampleLevel(g_sampler, uv, 0.0).rgb, 1.0);
}
