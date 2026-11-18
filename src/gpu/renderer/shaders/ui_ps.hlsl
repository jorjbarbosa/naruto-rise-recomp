// The host UI's triangles (renderer/ui_pass.cpp): the vertex colour times the texture, both with
// premultiplied alpha. The game's block-compressed images have straight alpha and are premultiplied
// here.

cbuffer Constants : register(b0, space0)
{
    float4x4 g_transform; // read by the vertex shader
    float2 g_translation;
    uint g_straightAlpha;
};

Texture2D<float4> g_texture : register(t0, space0);
SamplerState g_sampler : register(s0, space1);

float4 main(float4 position : SV_Position, float4 colour : COLOR0, float2 uv : TEXCOORD0) : SV_Target
{
    float4 texel = g_texture.Sample(g_sampler, uv);
    if (g_straightAlpha != 0)
        texel.rgb *= texel.a;
    return colour * texel;
}
