#pragma once

// Engine matrices as the transforms and the pose read, compare and blend them (state interpolation
// plan, sections 8.6 and 8.7).
//
// A matrix is 16 floats (rows I, J, K, T, each with a fourth float) and a type word (addresses.h,
// jade::matrix). Its floats are kept as their guest bit patterns: compared exactly (a float the
// display wrote is found by its bits), converted only to blend and to measure. Pure functions; the
// loads read guest memory and are main thread only like their callers.

#include <array>
#include <cstdint>

namespace game::interpolation::matrices
{
    constexpr uint32_t kFloats = 16;

    using Bits = std::array<uint32_t, kFloats>;
    using Vector = std::array<float, 3>;

    float Float(uint32_t bits);
    uint32_t Bits32(float value);

    // The 16 floats at address.
    Bits LoadBits(uint32_t address);

    bool Finite(const uint32_t* bits, uint32_t count);

    float Length(const Vector& v);
    float Dot(const Vector& a, const Vector& b);
    float Distance(const Vector& a, const Vector& b);

    // Row r (0 to 3) of a matrix: its first three floats.
    Vector Row(const uint32_t* bits, uint32_t r);

    // The cosine between two rows; 1 when either is null.
    float Cosine(const Vector& a, const Vector& b);

    // The smallest cosine between the rotation rows of two matrices.
    float SmallestCosine(const uint32_t* a, const uint32_t* b);

    // The engine's matrix between a and b (the reference's blend, the engine's own in its double
    // rendering): each rotation row linear with its length interpolated, the fourth column (the
    // scale with the scale bit, else a value the renderer still reads for lighting) and the
    // translation row linear.
    void Blend(const uint32_t* a, const uint32_t* b, float t, uint32_t* out);

    // Sets the scale of each row (its fourth float) to 1: the end of a blend without the scale bit when
    // the other end has it (the engine's own blend, WOR_InterpoleMatrix_XX, which gives the result the
    // bit).
    void UnitScale(Bits& matrix);
}
