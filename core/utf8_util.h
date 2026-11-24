#ifndef YUNTYPE_CORE_UTF8_UTIL_H
#define YUNTYPE_CORE_UTF8_UTIL_H

#include <cstddef>
#include <string_view>
#include <vector>

namespace yuntype {

inline std::size_t utf8CharLen(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

inline std::size_t utf8NextChar(std::string_view s, std::size_t pos) {
    if (pos >= s.size()) {
        return s.size();
    }
    const std::size_t step = utf8CharLen(static_cast<unsigned char>(s[pos]));
    std::size_t next = pos + 1;
    while (next < s.size() && next < pos + step &&
           (static_cast<unsigned char>(s[next]) & 0xC0) == 0x80) {
        ++next;
    }
    return next;
}

inline std::size_t utf8PrevChar(std::string_view s, std::size_t pos) {
    if (pos == 0) {
        return 0;
    }
    if (pos > s.size()) {
        pos = s.size();
    }
    --pos;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

inline std::size_t utf8Length(std::string_view s) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < s.size()) {
        pos = utf8NextChar(s, pos);
        ++count;
    }
    return count;
}

inline std::vector<std::string_view> utf8SplitChars(std::string_view s) {
    std::vector<std::string_view> chars;
    chars.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        const std::size_t next = utf8NextChar(s, pos);
        chars.push_back(s.substr(pos, next - pos));
        pos = next;
    }
    return chars;
}

inline std::size_t utf8ByteOffsetForChars(std::string_view s, std::size_t charIndex) {
    std::size_t pos = 0;
    std::size_t count = 0;
    while (pos < s.size() && count < charIndex) {
        pos = utf8NextChar(s, pos);
        ++count;
    }
    return pos;
}

} // namespace yuntype

#endif
