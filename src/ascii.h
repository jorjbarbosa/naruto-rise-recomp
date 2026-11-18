#pragma once

// ASCII case folding and comparison that do not depend on the C library's locale or its non-standard
// case-insensitive calls.

#include <cstddef>
#include <string_view>

namespace ascii
{
    // c with A-Z folded to a-z; every other byte unchanged.
    constexpr char ToLower(char c)
    {
        return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    }

    // Compares at most length bytes of a and b, stopping after a terminator, with A-Z folded to a-z:
    // the difference of the folded unsigned bytes where they first differ, or 0. This is _strnicmp
    // (and strncasecmp) in the C locale.
    inline int CompareNoCase(const char* a, const char* b, size_t length)
    {
        for (size_t i = 0; i < length; i++)
        {
            int left = static_cast<unsigned char>(ToLower(a[i]));
            int right = static_cast<unsigned char>(ToLower(b[i]));
            if (left != right || left == 0)
                return left - right;
        }
        return 0;
    }

    // True when a and b have the same length and the same bytes with A-Z folded to a-z.
    inline bool EqualNoCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
        {
            if (ToLower(a[i]) != ToLower(b[i]))
                return false;
        }
        return true;
    }
}
