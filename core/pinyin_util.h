#ifndef YUNTYPE_CORE_PINYIN_UTIL_H
#define YUNTYPE_CORE_PINYIN_UTIL_H

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace yuntype {

inline bool hasAsciiAlpha(std::string_view s) {
    for (unsigned char c : s) {
        if (std::isalpha(c)) {
            return true;
        }
    }
    return false;
}

inline bool isAllAsciiGraphicOrSpace(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

// Extract the trailing replaceable ASCII pinyin suffix from a typed ASCII buffer.
// Stops after unmatched shell/command delimiters like ", ', `, (, [, {, =, |, >, <, $.
inline std::string extractReplaceablePinyinSuffix(std::string_view buffer) {
    if (buffer.empty()) {
        return {};
    }
    constexpr std::string_view kBoundaries = "\"'`([{=|><$&\\/";
    std::size_t start = 0;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(buffer[i]);
        if (c < 0x20 || c >= 0x80 ||
            kBoundaries.find(static_cast<char>(c)) != std::string_view::npos) {
            start = i + 1;
        }
    }
    while (start > 0 && start < buffer.size() && buffer[start] == ' ') {
        ++start;
    }
    std::string_view candidate = buffer.substr(start);
    if (!hasAsciiAlpha(candidate)) {
        return {};
    }
    return std::string(candidate);
}

} // namespace yuntype

#endif
