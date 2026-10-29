cbuffer VertexConstants : register(b0, space4) { float4 offset; }
cbuffer PixelConstants : register(b1, space4) { float4 tint; }
cbuffer SharedConstants : register(b2, space4) { float4 multiplier; }
float4 vs_main(float3 position : POSITION) : SV_Position {
  return float4(position.xy + offset.xy, position.z, 1);
}
float4 ps_main() : SV_Target { return tint * multiplier; }
