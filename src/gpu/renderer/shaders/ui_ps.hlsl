// The host UI's triangles (renderer/ui_pass.cpp): the vertex colour times the texture, both with
// premultiplied alpha. The game's block-compressed images have straight alpha and are premultiplied
// here.

// The draw's constants (Constants in renderer/ui_pass.cpp): root constants on D3D12, push constants
// on Vulkan, laid out at the same offsets by both.
struct Constants
{
    float4x4 transform; // column-major, as the pass writes it
    float2 translation;
    uint straightAlpha; // read by the pixel shader
};
#ifdef __spirv__
[[vk::push_constant]] Constants g_constants;
#else
ConstantBuffer<Constants> g_constants : register(b0, space0);
#endif

Texture2D<float4> g_texture : register(t0, space0);
SamplerState g_sampler : register(s0, space1);

float4 main(float4 position : SV_Position, float4 colour : COLOR0, float2 uv : TEXCOORD0) : SV_Target
{
    float4 texel = g_texture.Sample(g_sampler, uv);
    if (g_constants.straightAlpha != 0)
        texel.rgb *= texel.a;
    return colour * texel;
}
