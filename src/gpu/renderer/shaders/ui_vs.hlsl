// The host UI's triangles (renderer/ui_pass.cpp): a vertex in frame pixels, moved by the draw's
// translation, then by its transform, which also holds the projection to clip space.

cbuffer Constants : register(b0, space0)
{
    float4x4 g_transform; // column-major, as the pass writes it
    float2 g_translation;
    uint g_straightAlpha; // read by the pixel shader
};

struct Input
{
    float2 position : POSITION;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

struct Output
{
    float4 position : SV_Position;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

Output main(Input input)
{
    Output output;
    output.position = mul(g_transform, float4(input.position + g_translation, 0.0, 1.0));
    output.colour = input.colour;
    output.uv = input.uv;
    return output;
}
