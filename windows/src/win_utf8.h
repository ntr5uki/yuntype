#ifndef YUNTYPE_WIN_UTF8_H
#define YUNTYPE_WIN_UTF8_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace yuntype::win {

inline constexpr ULONG_PTR kYunTypeInjectedTag = 0x59554E54ULL; // "YUNT"

inline std::wstring utf8ToWide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

inline std::string wideToUtf8(std::wstring_view ws) {
    if (ws.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

inline std::uint64_t monotonicNowUsec() {
    static const LARGE_INTEGER freq = []() {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    if (freq.QuadPart <= 0) {
        return static_cast<std::uint64_t>(::GetTickCount64()) * 1000ULL;
    }
    return static_cast<std::uint64_t>(
        (counter.QuadPart * 1000000ULL) / static_cast<std::uint64_t>(freq.QuadPart));
}

inline std::string currentExePathUtf8() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) {
        return {};
    }
    return wideToUtf8(std::wstring_view(buf, len));
}

} // namespace yuntype::win

#endif
