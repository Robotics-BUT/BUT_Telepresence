#pragma once

#include <string>
#include <memory>
#include <cstdarg>
#include <locale>
#include <algorithm>
#include <stdexcept>

// =============================================================================
// String Formatting Utilities
// =============================================================================

/**
 * Printf-style string formatting
 * @param fmt Format string
 * @param ... Format arguments
 * @return Formatted string
 */
inline std::string Fmt(const char* fmt, ...) {
    va_list vl;
    va_start(vl, fmt);
    int size = std::vsnprintf(nullptr, 0, fmt, vl);
    va_end(vl);

    if (size != -1) {
        std::unique_ptr<char[]> buffer(new char[size + 1]);

        va_start(vl, fmt);
        size = std::vsnprintf(buffer.get(), size + 1, fmt, vl);
        va_end(vl);

        if (size != -1) {
            return std::string(buffer.get(), size);
        }
    }

    throw std::runtime_error("Unexpected vsnprintf failure");
}

/**
 * Case-insensitive string comparison
 */
inline bool EqualsIgnoreCase(const std::string& s1, const std::string& s2,
                             const std::locale& loc = std::locale()) {
    const std::ctype<char>& ctype = std::use_facet<std::ctype<char>>(loc);
    const auto compareCharLower = [&](char c1, char c2) {
        return ctype.tolower(c1) == ctype.tolower(c2);
    };
    return s1.size() == s2.size() &&
           std::equal(s1.begin(), s1.end(), s2.begin(), compareCharLower);
}

/**
 * Convert boolean to string
 */
inline const char* BoolToString(bool b) {
    return b ? "true" : "false";
}

// =============================================================================
// Template Utilities
// =============================================================================

/**
 * Get size of C-style array at compile time
 */
template<typename T, size_t Size>
constexpr size_t ArraySize(const T (&)[Size]) noexcept {
    return Size;
}
