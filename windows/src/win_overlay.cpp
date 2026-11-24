#include "win_overlay.h"
#include "win_utf8.h"

#include <dwmapi.h>

#include <algorithm>

namespace yuntype::win {

namespace {

constexpr wchar_t kOverlayClassName[] = L"YunTypeCandidateOverlayWnd";

bool isSystemDarkMode() {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD val = 1;
    DWORD sz = sizeof(val);
    ::RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                       reinterpret_cast<LPBYTE>(&val), &sz);
    ::RegCloseKey(hKey);
    return val == 0;
}

int scaleDpi(int px, int dpi) {
    return ::MulDiv(px, dpi, 96);
}

int monitorDpiForRect(const RECT &rc) {
    POINT pt{rc.left, rc.bottom};
    HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon) {
        return 96;
    }
    using GetDpiForMonitorFn = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
    static const auto fn = []() -> GetDpiForMonitorFn {
        HMODULE mod = ::LoadLibraryW(L"shcore.dll");
        return mod ? reinterpret_cast<GetDpiForMonitorFn>(
                         ::GetProcAddress(mod, "GetDpiForMonitor"))
                   : nullptr;
    }();
    if (fn) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) && dx >= 72) {
            return static_cast<int>(dx);
        }
    }
    return 96;
}

HFONT createUiFont(int ptSize, int dpi, bool bold) {
    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(ptSize, dpi, 72);
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    ::wcsncpy(lf.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE - 1);
    return ::CreateFontIndirectW(&lf);
}

std::wstring candidateLabelPrefix(int idx) {
    const int num = (idx == 9) ? 0 : (idx + 1);
    return std::to_wstring(num) + L". ";
}

} // namespace

CandidateOverlay::~CandidateOverlay() {
    destroy();
}

bool CandidateOverlay::create(HINSTANCE hInst, std::function<void(int)> onCandidateClick) {
    onCandidateClick_ = std::move(onCandidateClick);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = &CandidateOverlay::wndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClassName;
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kOverlayClassName, L"", WS_POPUP,
        0, 0, 200, 48, nullptr, nullptr, hInst, this);
    if (!hwnd_) {
        return false;
    }

    // Request Windows 11 rounded corners (silently ignored on older Windows)
    constexpr DWORD kDwmwaWindowCornerPreference = 33;
    constexpr DWORD kDwmwcpRound = 2;
    ::DwmSetWindowAttribute(hwnd_, kDwmwaWindowCornerPreference, &kDwmwcpRound, sizeof(kDwmwcpRound));
    return true;
}

void CandidateOverlay::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
}

void CandidateOverlay::update(const OverlayModel &model, const RECT &anchorRect) {
    model_ = model;
    lastAnchor_ = anchorRect;
    hoverIndex_ = -1;
    if (!hwnd_) {
        return;
    }
    if (!model_.visible) {
        hide();
        return;
    }
    recomputeLayoutAndMove(anchorRect);
    if (!visible_) {
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void CandidateOverlay::hide() {
    if (hwnd_ && visible_) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
    hoverIndex_ = -1;
    itemRects_.clear();
}

LRESULT CALLBACK CandidateOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CandidateOverlay *self = nullptr;
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        self = reinterpret_cast<CandidateOverlay *>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<CandidateOverlay *>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->handleMessage(msg, wp, lp);
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CandidateOverlay::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = ::BeginPaint(hwnd_, &ps);
        paint(hdc);
        ::EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        const int hit = hitTestCandidate(pt);
        if (hit != hoverIndex_) {
            hoverIndex_ = hit;
            ::InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        const int hit = hitTestCandidate(pt);
        if (hit >= 0 && onCandidateClick_) {
            onCandidateClick_(hit);
        }
        return 0;
    }
    default:
        return ::DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

int CandidateOverlay::hitTestCandidate(POINT pt) const {
    for (std::size_t i = 0; i < itemRects_.size(); ++i) {
        if (::PtInRect(&itemRects_[i], pt)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void CandidateOverlay::recomputeLayoutAndMove(const RECT &anchorRect) {
    dpi_ = monitorDpiForRect(anchorRect);
    const int padX = scaleDpi(12, dpi_);
    const int padY = scaleDpi(8, dpi_);
    const int gapY = scaleDpi(6, dpi_);
    const int itemPadX = scaleDpi(8, dpi_);
    const int itemPadY = scaleDpi(4, dpi_);
    const int itemGapX = scaleDpi(6, dpi_);

    HDC hdc = ::GetDC(hwnd_);
    HFONT preeditFont = createUiFont(10, dpi_, false);
    HFONT candFont = createUiFont(11, dpi_, false);
    HFONT smallFont = createUiFont(9, dpi_, false);

    auto measure = [&](HFONT f, const std::wstring &s) -> SIZE {
        HGDIOBJ old = ::SelectObject(hdc, f);
        SIZE sz{};
        if (!s.empty()) {
            ::GetTextExtentPoint32W(hdc, s.c_str(), static_cast<int>(s.size()), &sz);
        } else {
            ::GetTextExtentPoint32W(hdc, L"Ag", 2, &sz);
            sz.cx = 0;
        }
        ::SelectObject(hdc, old);
        return sz;
    };

    const std::wstring preeditLine = utf8ToWide(model_.preedit + model_.statusSuffix);
    const SIZE preeditSz = measure(preeditFont, preeditLine);
    int contentW = std::max(scaleDpi(120, dpi_), static_cast<int>(preeditSz.cx));
    int curY = padY + std::max<int>(scaleDpi(20, dpi_), preeditSz.cy);

    itemRects_.clear();
    if (!model_.pageCandidates.empty()) {
        curY += gapY;
        // Horizontal candidate bar when items are short; vertical list if wide
        int horizW = 0;
        int maxItemH = scaleDpi(24, dpi_);
        std::vector<SIZE> itemSizes;
        itemSizes.reserve(model_.pageCandidates.size());
        for (std::size_t i = 0; i < model_.pageCandidates.size(); ++i) {
            const std::wstring label =
                candidateLabelPrefix(static_cast<int>(i)) + utf8ToWide(model_.pageCandidates[i]);
            SIZE sz = measure(candFont, label);
            sz.cx += itemPadX * 2;
            sz.cy = std::max(maxItemH, static_cast<int>(sz.cy) + itemPadY * 2);
            itemSizes.push_back(sz);
            horizW += sz.cx + (i > 0 ? itemGapX : 0);
            maxItemH = std::max(maxItemH, static_cast<int>(sz.cy));
        }

        std::wstring pageTag;
        if (model_.totalPages > 1 || model_.hasMoreAction) {
            pageTag = std::to_wstring(model_.currentPage + 1) + L"/" +
                      std::to_wstring(std::max(1, model_.totalPages));
            if (model_.hasMoreAction) {
                pageTag += L"+";
            }
        }
        const SIZE tagSz = pageTag.empty() ? SIZE{0, 0} : measure(smallFont, pageTag);
        const int maxHorizW = scaleDpi(620, dpi_);
        const bool vertical = (horizW + (pageTag.empty() ? 0 : tagSz.cx + scaleDpi(12, dpi_)) > maxHorizW);

        if (!vertical) {
            int x = padX;
            for (const auto &sz : itemSizes) {
                itemRects_.push_back(RECT{x, curY, x + sz.cx, curY + maxItemH});
                x += sz.cx + itemGapX;
            }
            const int rowW = horizW + (pageTag.empty() ? 0 : tagSz.cx + scaleDpi(14, dpi_));
            contentW = std::max(contentW, rowW);
            curY += maxItemH;
        } else {
            int maxRowW = contentW;
            for (const auto &sz : itemSizes) {
                maxRowW = std::max(maxRowW, static_cast<int>(sz.cx));
            }
            maxRowW = std::min(maxRowW, scaleDpi(540, dpi_));
            contentW = std::max(contentW, maxRowW);
            for (const auto &sz : itemSizes) {
                itemRects_.push_back(RECT{padX, curY, padX + contentW, curY + sz.cy});
                curY += sz.cy + scaleDpi(3, dpi_);
            }
            if (!pageTag.empty()) {
                curY += scaleDpi(2, dpi_) + tagSz.cy;
            }
        }
    }

    if (!model_.auxText.empty()) {
        const SIZE auxSz = measure(smallFont, utf8ToWide(model_.auxText));
        contentW = std::max(contentW, std::min<int>(auxSz.cx, scaleDpi(540, dpi_)));
        curY += gapY + auxSz.cy;
    }

    ::DeleteObject(preeditFont);
    ::DeleteObject(candFont);
    ::DeleteObject(smallFont);
    ::ReleaseDC(hwnd_, hdc);

    width_ = std::min(contentW + padX * 2, scaleDpi(680, dpi_));
    height_ = curY + padY;

    // Position right below the caret; flip above if near bottom work area edge
    POINT pt{anchorRect.left, anchorRect.bottom};
    HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT work{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
    if (mon && ::GetMonitorInfoW(mon, &mi)) {
        work = mi.rcWork;
    }

    const int margin = scaleDpi(6, dpi_);
    int x = anchorRect.left;
    int y = anchorRect.bottom + margin;
    if (y + height_ > work.bottom - margin) {
        y = anchorRect.top - height_ - margin;
    }
    if (y < work.top + margin) {
        y = work.top + margin;
    }
    if (x + width_ > work.right - margin) {
        x = work.right - width_ - margin;
    }
    if (x < work.left + margin) {
        x = work.left + margin;
    }

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void CandidateOverlay::paint(HDC hdc) {
    const bool dark = isSystemDarkMode();
    const COLORREF bgCol = dark ? RGB(32, 33, 36) : RGB(252, 252, 253);
    const COLORREF borderCol = dark ? RGB(66, 70, 78) : RGB(214, 218, 224);
    const COLORREF preeditCol = dark ? RGB(175, 198, 255) : RGB(30, 88, 188);
    const COLORREF textCol = dark ? RGB(236, 238, 242) : RGB(28, 30, 34);
    const COLORREF mutedCol = dark ? RGB(148, 154, 166) : RGB(118, 124, 136);
    const COLORREF activeBg = dark ? RGB(52, 92, 168) : RGB(37, 99, 235);
    const COLORREF activeText = RGB(255, 255, 255);
    const COLORREF hoverBg = dark ? RGB(48, 52, 60) : RGB(236, 240, 248);
    const COLORREF sepCol = dark ? RGB(52, 56, 64) : RGB(232, 235, 240);

    HDC memDc = ::CreateCompatibleDC(hdc);
    HBITMAP bmp = ::CreateCompatibleBitmap(hdc, width_, height_);
    HGDIOBJ oldBmp = ::SelectObject(memDc, bmp);

    RECT full{0, 0, width_, height_};
    HBRUSH bgBrush = ::CreateSolidBrush(bgCol);
    ::FillRect(memDc, &full, bgBrush);
    ::DeleteObject(bgBrush);

    HPEN borderPen = ::CreatePen(PS_SOLID, 1, borderCol);
    HGDIOBJ oldPen = ::SelectObject(memDc, borderPen);
    HGDIOBJ oldBrush = ::SelectObject(memDc, ::GetStockObject(HOLLOW_BRUSH));
    ::Rectangle(memDc, 0, 0, width_, height_);
    ::SelectObject(memDc, oldBrush);
    ::SelectObject(memDc, oldPen);
    ::DeleteObject(borderPen);

    ::SetBkMode(memDc, TRANSPARENT);
    const int padX = scaleDpi(12, dpi_);
    const int padY = scaleDpi(8, dpi_);

    HFONT preeditFont = createUiFont(10, dpi_, false);
    HFONT candFont = createUiFont(11, dpi_, false);
    HFONT smallFont = createUiFont(9, dpi_, false);

    // 1. Preedit line
    {
        HGDIOBJ oldFont = ::SelectObject(memDc, preeditFont);
        ::SetTextColor(memDc, preeditCol);
        const std::wstring preText = utf8ToWide(model_.preedit + model_.statusSuffix);
        RECT preRc{padX, padY, width_ - padX, padY + scaleDpi(24, dpi_)};
        ::DrawTextW(memDc, preText.c_str(), static_cast<int>(preText.size()), &preRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (model_.showDraftCaret) {
            const std::size_t clamped = std::min(model_.preeditCursorByte, model_.preedit.size());
            const std::wstring before = utf8ToWide(model_.preedit.substr(0, clamped));
            SIZE bsz{};
            if (!before.empty()) {
                ::GetTextExtentPoint32W(memDc, before.c_str(), static_cast<int>(before.size()), &bsz);
            }
            const int cx = std::min(width_ - padX - 2, padX + static_cast<int>(bsz.cx));
            HPEN caretPen = ::CreatePen(PS_SOLID, std::max(1, scaleDpi(2, dpi_)), preeditCol);
            HGDIOBJ op = ::SelectObject(memDc, caretPen);
            ::MoveToEx(memDc, cx, padY + scaleDpi(2, dpi_), nullptr);
            ::LineTo(memDc, cx, padY + scaleDpi(20, dpi_));
            ::SelectObject(memDc, op);
            ::DeleteObject(caretPen);
        }
        ::SelectObject(memDc, oldFont);
    }

    // 2. Candidate items
    if (!itemRects_.empty()) {
        const int sepY = itemRects_.front().top - scaleDpi(3, dpi_);
        HPEN sepPen = ::CreatePen(PS_SOLID, 1, sepCol);
        HGDIOBJ op = ::SelectObject(memDc, sepPen);
        ::MoveToEx(memDc, padX, sepY, nullptr);
        ::LineTo(memDc, width_ - padX, sepY);
        ::SelectObject(memDc, op);
        ::DeleteObject(sepPen);

        HGDIOBJ oldFont = ::SelectObject(memDc, candFont);
        const int radius = scaleDpi(6, dpi_);
        for (std::size_t i = 0; i < itemRects_.size() && i < model_.pageCandidates.size(); ++i) {
            const bool active = (static_cast<int>(i) == model_.pageCursor);
            const bool hovered = (!active && static_cast<int>(i) == hoverIndex_);
            const RECT &ir = itemRects_[i];
            if (active || hovered) {
                HBRUSH br = ::CreateSolidBrush(active ? activeBg : hoverBg);
                HPEN pn = ::CreatePen(PS_SOLID, 1, active ? activeBg : hoverBg);
                HGDIOBJ ob = ::SelectObject(memDc, br);
                HGDIOBJ op2 = ::SelectObject(memDc, pn);
                ::RoundRect(memDc, ir.left, ir.top, ir.right, ir.bottom, radius, radius);
                ::SelectObject(memDc, op2);
                ::SelectObject(memDc, ob);
                ::DeleteObject(pn);
                ::DeleteObject(br);
            }
            ::SetTextColor(memDc, active ? activeText : textCol);
            RECT tr = ir;
            tr.left += scaleDpi(8, dpi_);
            tr.right -= scaleDpi(6, dpi_);
            const std::wstring label =
                candidateLabelPrefix(static_cast<int>(i)) + utf8ToWide(model_.pageCandidates[i]);
            ::DrawTextW(memDc, label.c_str(), static_cast<int>(label.size()), &tr,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        ::SelectObject(memDc, oldFont);

        if (model_.totalPages > 1 || model_.hasMoreAction) {
            HGDIOBJ sf = ::SelectObject(memDc, smallFont);
            ::SetTextColor(memDc, mutedCol);
            std::wstring tag = std::to_wstring(model_.currentPage + 1) + L"/" +
                               std::to_wstring(std::max(1, model_.totalPages));
            if (model_.hasMoreAction) {
                tag += L"+";
            }
            RECT tagRc{padX, itemRects_.back().top, width_ - padX, height_ - padY};
            ::DrawTextW(memDc, tag.c_str(), static_cast<int>(tag.size()), &tagRc,
                        DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectObject(memDc, sf);
        }
    }

    // 3. Aux error/hint line
    if (!model_.auxText.empty()) {
        HGDIOBJ sf = ::SelectObject(memDc, smallFont);
        ::SetTextColor(memDc, mutedCol);
        const std::wstring auxW = utf8ToWide(model_.auxText);
        RECT auxRc{padX, height_ - padY - scaleDpi(18, dpi_), width_ - padX, height_ - padY};
        ::DrawTextW(memDc, auxW.c_str(), static_cast<int>(auxW.size()), &auxRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(memDc, sf);
    }

    ::BitBlt(hdc, 0, 0, width_, height_, memDc, 0, 0, SRCCOPY);

    ::DeleteObject(preeditFont);
    ::DeleteObject(candFont);
    ::DeleteObject(smallFont);
    ::SelectObject(memDc, oldBmp);
    ::DeleteObject(bmp);
    ::DeleteDC(memDc);
}

} // namespace yuntype::win
