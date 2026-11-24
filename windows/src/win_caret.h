#ifndef YUNTYPE_WIN_CARET_H
#define YUNTYPE_WIN_CARET_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../../core/sentence_history.h"

#include <optional>

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

    // 4-level caret position query: UIA -> MSAA -> GetGUIThreadInfo -> Fallback.
    CaretAnchor queryCaret(HWND fgWnd, std::optional<POINT> lastClickPt);

    // Read surrounding text around caret via UIA TextPattern2/TextPattern or Win32 Edit messages.
    std::optional<yuntype::SurroundingSnapshot> readSurrounding(HWND fgWnd);

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

} // namespace yuntype::win

#endif
