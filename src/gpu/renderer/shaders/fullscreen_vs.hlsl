// A triangle that covers the viewport, drawn with three vertices and no vertex buffer.

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Output main(uint id : SV_VertexID)
{
    Output output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
