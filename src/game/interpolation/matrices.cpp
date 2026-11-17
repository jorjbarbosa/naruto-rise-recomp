// Engine matrices as guest bit patterns: loads, measures and the blend (matrices.h).

#include "game/interpolation/matrices.h"

#include "memory.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace game::interpolation::matrices
{
    float Float(uint32_t bits)
    {
        return std::bit_cast<float>(bits);
    }

    uint32_t Bits32(float value)
    {
        return std::bit_cast<uint32_t>(value);
    }

    Bits LoadBits(uint32_t address)
    {
        Bits bits;
        for (uint32_t i = 0; i < kFloats; i++)
            bits[i] = mem::Load32(address + 4 * i);
        return bits;
    }

    bool Finite(const uint32_t* bits, uint32_t count)
    {
        return std::all_of(bits, bits + count, [](uint32_t b) { return std::isfinite(Float(b)); });
    }

    float Length(const Vector& v)
    {
        return std::sqrt(Dot(v, v));
    }

    float Dot(const Vector& a, const Vector& b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    float Distance(const Vector& a, const Vector& b)
    {
        return Length({ a[0] - b[0], a[1] - b[1], a[2] - b[2] });
    }

    Vector Row(const uint32_t* bits, uint32_t r)
    {
        return { Float(bits[4 * r]), Float(bits[4 * r + 1]), Float(bits[4 * r + 2]) };
    }

    float Cosine(const Vector& a, const Vector& b)
    {
        float la = Length(a), lb = Length(b);
        if (la <= 1e-6f || lb <= 1e-6f)
            return 1.0f;
        return Dot(a, b) / (la * lb);
    }

    float SmallestCosine(const uint32_t* a, const uint32_t* b)
    {
        float smallest = 1.0f;
        for (uint32_t r = 0; r < 3; r++)
            smallest = std::min(smallest, Cosine(Row(a, r), Row(b, r)));
        return smallest;
    }

    void Blend(const uint32_t* a, const uint32_t* b, float t, uint32_t* out)
    {
        for (uint32_t r = 0; r < 3; r++)
        {
            Vector row;
            for (uint32_t c = 0; c < 3; c++)
            {
                float x = Float(a[4 * r + c]);
                row[c] = x + (Float(b[4 * r + c]) - x) * t;
            }
            float la = Length(Row(a, r)), lb = Length(Row(b, r));
            float length = Length(row);
            float scale = length > 1e-6f ? (la + (lb - la) * t) / length : 1.0f;
            for (uint32_t c = 0; c < 3; c++)
                out[4 * r + c] = Bits32(row[c] * scale);
            float w = Float(a[4 * r + 3]);
            out[4 * r + 3] = Bits32(w + (Float(b[4 * r + 3]) - w) * t);
        }
        for (uint32_t i = 12; i < kFloats; i++)
        {
            float x = Float(a[i]);
            out[i] = Bits32(x + (Float(b[i]) - x) * t);
        }
    }

    void UnitScale(Bits& matrix)
    {
        for (uint32_t r = 0; r < 3; r++)
            matrix[4 * r + 3] = Bits32(1.0f);
    }
}
