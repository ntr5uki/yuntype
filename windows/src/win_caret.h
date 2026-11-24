#ifndef YUNTYPE_WIN_CARET_H
#define YUNTYPE_WIN_CARET_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../../core/sentence_history.h"

#include <optional>
#include <string_view>

namespace yuntype::win {

struct CaretAnchor {
    RECT rect{};
    bool precise = false;
};

class CaretTracker {
public:
    CaretTracker();
    ~CaretTracker();

    CaretTracker(const CaretTracker &) = delete;
    CaretTracker &operator=(const CaretTracker &) = delete;

    // 4-level caret position query executed off the hook/UI thread with a short timeout.
    CaretAnchor queryCaret(HWND fgWnd, std::optional<POINT> lastClickPt);

    // Read surrounding text (and active selection up to 2048 chars) with timeout protection.
    std::optional<yuntype::SurroundingSnapshot> readSurrounding(HWND fgWnd);

    // Select `expectedSuffix` immediately before the caret via UIA or Win32 Edit so the caller
    // can overwrite it in-place with zero BackSpaces. Returns false if unsupported or mismatched.
    bool selectSuffixBeforeCaret(HWND fgWnd, std::string_view expectedSuffix);

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

} // namespace yuntype::win

#endif
