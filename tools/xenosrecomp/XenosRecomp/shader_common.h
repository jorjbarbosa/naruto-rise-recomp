#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#ifdef NARUTO_RECOMP
    // Alpha to coverage, the bit of the UNLEASHED_RECOMP path.
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272)
#ifdef NARUTO_RECOMP
#define g_InverseResolutionScale   vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 280)
#define g_VertexFetch(i)           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 320 + (i) * 4)
#define g_ClipPlane(i)             vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 384 + (i) * 16)
#endif

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#ifdef NARUTO_RECOMP
// c17.zw holds 1 / the internal resolution scale of the native renderer, which
// brings the pixel position of ps_param_gen back to guest pixels;
// c18 and c19 the descriptor indices of the vertex textures (declared by the
// cache builder); c20..c23 the vertex fetch descriptors (vfetchNaruto);
// c24..c29 the user clip planes PA_CL_UCP_0..5 (zero when disabled).
#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    float2 g_InverseResolutionScale : packoffset(c17.z); \
    uint4 g_VertexFetchArray[4] : packoffset(c20); \
    float4 g_ClipPlaneArray[6] : packoffset(c24);

#define g_VertexFetch(i) (g_VertexFetchArray[(i) / 4][(i) % 4])
#define g_ClipPlane(i) (g_ClipPlaneArray[i])
#else
#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x);
#endif

uint g_SpecConstants();

#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

#ifdef NARUTO_RECOMP
// A 1D texture (the bone matrices of the skinned meshes, the toon ramps) is
// bound as a 2D texture one texel high and sampled at level 0 (a vertex shader
// has no derivatives). The instruction's texel offset (offsetX * 0.5, as the 2D
// fetch) selects the row of a bone matrix: the Xenos adds it in texels to the
// normalized coordinate; with unnormalized coordinates (denorm) the coordinate
// itself is in texels.
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float offset, uint denorm)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    float width = getTexture2DDimensions(texture).x;
    float u = denorm != 0 ? (texCoord + offset) / width : texCoord + offset / width;
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], float2(u, 0.5), 0);
}

// A 2D fetch with unnormalized coordinates (texels).
float4 tfetch2DDenorm(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], (texCoord + offset) / getTexture2DDimensions(texture));
}

// getGradients: the screen-space derivatives of two source components,
// X and Z = ddx(source.xy), Y and W = ddy(source.xy), coarse because the
// texture unit computes them per quad. No texture is read. The cel shaders take
// the width of their light step from them.
float4 getGradients(float2 value)
{
    float2 dx = ddx_coarse(value);
    float2 dy = ddy_coarse(value);
    return float4(dx.x, dy.x, dx.y, dy.y);
}
#endif

#ifdef NARUTO_RECOMP
// The vertex fetch decoded in the shader. Every element arrives raw,
// R32[G32[B32[A32]]]_UINT, its words in host order after the endian swap of the
// stream, and is decoded by its descriptor, one per element in fetch address
// order: bits 0..5 the Xenos vertex format, 6 signed, 7 integer, 8 the DEC3N
// rule of 10_11_11 (as tfetchR11G11B10), and 9..20 the source of
// each output component (0..3 a component in hardware order, 4 zero, 5 one),
// where the host combines the runtime-patched fetch with the template's
// destination swizzle. Normalized: unsigned divided by 2^n - 1, signed by
// 2^(n-1) - 1 and clamped at -1.
float vfetchComponent(uint raw, uint shift, uint bits, uint descriptor)
{
    uint value = (raw >> shift) & ((1u << bits) - 1u);
    bool isSigned = (descriptor & 0x40u) != 0;
    int signedValue = isSigned ? (int(value << (32u - bits)) >> (32u - bits)) : int(value);
    if ((descriptor & 0x80u) != 0)
        return float(signedValue);
    if (isSigned)
        return max(float(signedValue) / float((1u << (bits - 1u)) - 1u), -1.0);
    return float(value) / float((1u << bits) - 1u);
}

float4 vfetchNaruto(uint4 raw, uint descriptor)
{
    float4 c = float4(0.0, 0.0, 0.0, 0.0);
    switch (descriptor & 0x3Fu)
    {
    case 6: // 8_8_8_8
        c = float4(vfetchComponent(raw.x, 0, 8, descriptor), vfetchComponent(raw.x, 8, 8, descriptor),
            vfetchComponent(raw.x, 16, 8, descriptor), vfetchComponent(raw.x, 24, 8, descriptor));
        break;
    case 7: // 2_10_10_10
        c = float4(vfetchComponent(raw.x, 0, 10, descriptor), vfetchComponent(raw.x, 10, 10, descriptor),
            vfetchComponent(raw.x, 20, 10, descriptor), vfetchComponent(raw.x, 30, 2, descriptor));
        break;
    case 16: // 10_11_11
        if ((descriptor & 0x100u) != 0)
        {
            c = float4(
                (raw.x & 0x00000400 ? -1.0 : 0.0) + ((raw.x & 0x3FF) / 1024.0),
                (raw.x & 0x00200000 ? -1.0 : 0.0) + (((raw.x >> 11) & 0x3FF) / 1024.0),
                (raw.x & 0x80000000 ? -1.0 : 0.0) + (((raw.x >> 22) & 0x1FF) / 512.0),
                0.0);
        }
        else
        {
            c = float4(vfetchComponent(raw.x, 0, 11, descriptor), vfetchComponent(raw.x, 11, 11, descriptor),
                vfetchComponent(raw.x, 22, 10, descriptor), 0.0);
        }
        break;
    case 25: // 16_16
        c = float4(vfetchComponent(raw.x, 0, 16, descriptor), vfetchComponent(raw.x, 16, 16, descriptor), 0.0, 0.0);
        break;
    case 26: // 16_16_16_16
        c = float4(vfetchComponent(raw.x, 0, 16, descriptor), vfetchComponent(raw.x, 16, 16, descriptor),
            vfetchComponent(raw.y, 0, 16, descriptor), vfetchComponent(raw.y, 16, 16, descriptor));
        break;
    case 31: // 16_16_FLOAT
        c = float4(f16tof32(raw.x), f16tof32(raw.x >> 16), 0.0, 0.0);
        break;
    case 32: // 16_16_16_16_FLOAT
        c = float4(f16tof32(raw.x), f16tof32(raw.x >> 16), f16tof32(raw.y), f16tof32(raw.y >> 16));
        break;
    case 36: // 32_FLOAT
        c.x = asfloat(raw.x);
        break;
    case 37: // 32_32_FLOAT
        c.xy = asfloat(raw.xy);
        break;
    case 57: // 32_32_32_FLOAT
        c.xyz = asfloat(raw.xyz);
        break;
    case 38: // 32_32_32_32_FLOAT
        c = asfloat(raw);
        break;
    }
    float source[8] = { c.x, c.y, c.z, c.w, 0.0, 1.0, 0.0, 0.0 };
    return float4(source[(descriptor >> 9) & 7], source[(descriptor >> 12) & 7], source[(descriptor >> 15) & 7],
        source[(descriptor >> 18) & 7]);
}
#endif

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
