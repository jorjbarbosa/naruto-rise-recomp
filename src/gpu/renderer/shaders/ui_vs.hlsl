// The host UI's triangles (renderer/ui_pass.cpp): a vertex in frame pixels, moved by the draw's
// translation, then by its transform, which also holds the projection to clip space.

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
    output.position = mul(g_constants.transform, float4(input.position + g_constants.translation, 0.0, 1.0));
    output.colour = input.colour;
    output.uv = input.uv;
    return output;
}
