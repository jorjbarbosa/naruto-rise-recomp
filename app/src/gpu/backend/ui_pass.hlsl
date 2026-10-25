// The UI layer of the Present (UiFrame, contracts.h): textured triangles over
// the image in the back buffer, with straight alpha. The vertices are in the
// frame's coordinate space, stretched over the whole back buffer; the color is
// R8G8B8A8_UNORM and multiplies the texture (a white texture for untextured
// triangles).
// Compiled offline into backend/ui_pass.h (dxc -T vs_6_0 or ps_6_0 -E <entry>):
// vs_main as kUiVs and ps_main as kUiPs. The SPIR-V of the Vulkan backend comes
// from the build (tools/native-renderer/build-pass-spirv.py): the texture and
// the linear clamp sampler in set 0, the inverse coordinate space (root
// constants in D3D12) as push constants of the vertex stage.
[[vk::binding(0, 0)]] Texture2D<float4> image : register(t0);
[[vk::binding(1, 0)]] SamplerState linear_clamp : register(s0);
#ifdef __spirv__
struct PassConstants {
  float2 inverse_space;
};
[[vk::push_constant]] PassConstants pass_constants;
#define inverse_space pass_constants.inverse_space
#else
cbuffer Pass : register(b0) {
  float2 inverse_space;  // 1 / the frame's coordinate space.
};
#endif

struct Vertex {
  float2 position : POSITION;
  float2 uv : TEXCOORD;
  float4 color : COLOR;
};
struct Fragment {
  float4 position : SV_Position;
  float2 uv : TEXCOORD;
  float4 color : COLOR;
};

Fragment vs_main(Vertex v) {
  Fragment f;
  f.position = float4(v.position * inverse_space * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  f.uv = v.uv;
  f.color = v.color;
  return f;
}

float4 ps_main(Fragment f) : SV_Target {
  return image.Sample(linear_clamp, f.uv) * f.color;
}
