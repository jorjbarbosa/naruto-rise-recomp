struct Push { uint64_t vs; uint64_t ps; uint64_t common; };
[[vk::push_constant]] ConstantBuffer<Push> constants;
[[vk::binding(0, 0)]] Texture2D<float4> textures[];
[[vk::binding(0, 2)]] TextureCube<float4> cubes[];
[[vk::binding(0, 3)]] SamplerState samplers[];
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs_main(uint id : SV_VertexID) {
  Vertex v;
  v.uv = float2((id << 1) & 2, id & 2);
  v.position = float4(v.uv * float2(2, -2) + float2(-1, 1) +
                     vk::RawBufferLoad<float4>(constants.vs).xy, 0, 1);
  return v;
}
float4 ps_main(Vertex v) : SV_Target {
  uint reg = vk::RawBufferLoad<uint>(constants.ps + 16);
  uint forced_slot = vk::RawBufferLoad<uint>(constants.ps + 20);
  uint cube = vk::RawBufferLoad<uint>(constants.ps + 24);
  uint image_offset = reg < 16 ? (cube != 0 ? 128 : 0) + reg * 4 : 288 + (reg - 16) * 4;
  uint sampler_offset = reg < 16 ? 192 + reg * 4 : 304 + (reg - 16) * 4;
  uint image = forced_slot != 0 ? forced_slot : vk::RawBufferLoad<uint>(constants.common + image_offset);
  uint sampler_index = vk::RawBufferLoad<uint>(constants.common + sampler_offset);
  float4 value = cube != 0 ? cubes[image].SampleLevel(samplers[sampler_index], float3(1, 0, 0), 0) :
                             textures[image].SampleLevel(samplers[sampler_index], v.uv, 0);
  return value * vk::RawBufferLoad<float4>(constants.ps);
}
