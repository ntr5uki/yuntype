#include "win_caret.h"
#include "win_utf8.h"

#include <oleacc.h>
#include <uiautomation.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace yuntype::win {

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    T *get() const { return ptr_; }
    T *operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    T **put() {
        reset();
        return &ptr_;
    }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T *ptr_ = nullptr;
};

bool isSaneRect(const RECT &r) {
    if (r.right < r.left || r.bottom < r.top) {
        return false;
    }
    if (r.left == 0 && r.top == 0 && r.right == 0 && r.bottom == 0) {
        return false;
    }
    const long w = r.right - r.left;
    const long h = r.bottom - r.top;
    if (w > 4000 || h > 2000) {
        return false;
    }
    POINT pt{r.left, r.top};
    return ::MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != nullptr;
}

bool rectFromSafeArray(SAFEARRAY *sa, RECT &out, bool useRightEdge) {
    if (!sa) {
        return false;
    }
    LONG lb = 0;
    LONG ub = -1;
    if (FAILED(::SafeArrayGetLBound(sa, 1, &lb)) || FAILED(::SafeArrayGetUBound(sa, 1, &ub))) {
        return false;
    }
    const LONG count = ub - lb + 1;
    if (count < 4) {
        return false;
    }
    double *data = nullptr;
    if (FAILED(::SafeArrayAccessData(sa, reinterpret_cast<void **>(&data))) || !data) {
        return false;
    }
    // Use the last bounding rect in the range (where the caret sits)
    const LONG base = ((count / 4) - 1) * 4;
    const double left = data[base + 0];
    const double top = data[base + 1];
    const double width = data[base + 2];
    const double height = data[base + 3];
    ::SafeArrayUnaccessData(sa);

    if (height <= 0.0 || height > 400.0) {
        return false;
    }
    const long x = static_cast<long>(std::lround(useRightEdge ? (left + width) : left));
    const long y = static_cast<long>(std::lround(top));
    const long h = std::max<long>(16, static_cast<long>(std::lround(height)));
    out = RECT{x, y, x + 2, y + h};
    return isSaneRect(out);
}

bool getCaretRangeFromElement(IUIAutomationElement *elem, ComPtr<IUIAutomationTextRange> &outRange) {
    if (!elem) {
        return false;
    }
    ComPtr<IUIAutomationTextPattern2> tp2;
    if (SUCCEEDED(elem->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(tp2.put()))) && tp2) {
        BOOL isActive = FALSE;
        if (SUCCEEDED(tp2->GetCaretRange(&isActive, outRange.put())) && outRange) {
            return true;
        }
    }
    ComPtr<IUIAutomationTextPattern> tp;
    if (SUCCEEDED(elem->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(tp.put()))) && tp) {
        ComPtr<IUIAutomationTextRangeArray> sel;
        if (SUCCEEDED(tp->GetSelection(sel.put())) && sel) {
            int len = 0;
            if (SUCCEEDED(sel->get_Length(&len)) && len > 0) {
                if (SUCCEEDED(sel->GetElement(0, outRange.put())) && outRange) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool getRectFromTextRange(IUIAutomationTextRange *range, RECT &out) {
    if (!range) {
        return false;
    }
    SAFEARRAY *sa = nullptr;
    if (SUCCEEDED(range->GetBoundingRectangles(&sa)) && sa) {
        const bool ok = rectFromSafeArray(sa, out, true);
        ::SafeArrayDestroy(sa);
        if (ok) {
            return true;
        }
    }
    // Collapsed caret at end of text often has empty bounding rect: inspect preceding char
    ComPtr<IUIAutomationTextRange> probe;
    if (SUCCEEDED(range->Clone(probe.put())) && probe) {
        int moved = 0;
        if (SUCCEEDED(probe->MoveEndpointByUnit(TextPatternRangeEndpoint_Start,
                                                TextUnit_Character, -1, &moved)) &&
            moved != 0) {
            SAFEARRAY *sa2 = nullptr;
            if (SUCCEEDED(probe->GetBoundingRectangles(&sa2)) && sa2) {
                const bool ok = rectFromSafeArray(sa2, out, true);
                ::SafeArrayDestroy(sa2);
                if (ok) {
                    return true;
                }
            }
        }
    }
    // Try expanding to enclosing character (e.g. start of empty/non-empty line)
    ComPtr<IUIAutomationTextRange> probe2;
    if (SUCCEEDED(range->Clone(probe2.put())) && probe2) {
        if (SUCCEEDED(probe2->ExpandToEnclosingUnit(TextUnit_Character))) {
            SAFEARRAY *sa3 = nullptr;
            if (SUCCEEDED(probe2->GetBoundingRectangles(&sa3)) && sa3) {
                const bool ok = rectFromSafeArray(sa3, out, false);
                ::SafeArrayDestroy(sa3);
                if (ok) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool queryMsaaCaret(HWND focusHwnd, RECT &out) {
    if (!focusHwnd) {
        return false;
    }
    ComPtr<IAccessible> acc;
    if (FAILED(::AccessibleObjectFromWindow(focusHwnd, OBJID_CARET, IID_IAccessible,
                                            reinterpret_cast<void **>(acc.put()))) ||
        !acc) {
        return false;
    }
    VARIANT varSelf{};
    varSelf.vt = VT_I4;
    varSelf.lVal = CHILDID_SELF;
    long x = 0, y = 0, w = 0, h = 0;
    if (FAILED(acc->accLocation(&x, &y, &w, &h, varSelf))) {
        return false;
    }
    if (h <= 0 || h > 400) {
        return false;
    }
    out = RECT{x, y, x + std::max<long>(w, 2), y + h};
    return isSaneRect(out);
}

bool queryGuiThreadCaret(HWND fgWnd, RECT &out, HWND *outFocusHwnd) {
    if (!fgWnd) {
        return false;
    }
    const DWORD tid = ::GetWindowThreadProcessId(fgWnd, nullptr);
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (!::GetGUIThreadInfo(tid, &gti)) {
        return false;
    }
    if (outFocusHwnd) {
        *outFocusHwnd = gti.hwndFocus ? gti.hwndFocus : fgWnd;
    }
    if (!gti.hwndCaret) {
        return false;
    }
    if (gti.rcCaret.left == 0 && gti.rcCaret.top == 0 &&
        gti.rcCaret.right == 0 && gti.rcCaret.bottom == 0) {
        return false;
    }
    POINT tl{gti.rcCaret.left, gti.rcCaret.top};
    POINT br{gti.rcCaret.right, gti.rcCaret.bottom};
    if (!::ClientToScreen(gti.hwndCaret, &tl) || !::ClientToScreen(gti.hwndCaret, &br)) {
        return false;
    }
    out = RECT{tl.x, tl.y, std::max(br.x, tl.x + 2), std::max(br.y, tl.y + 18)};
    return isSaneRect(out);
}

std::optional<yuntype::SurroundingSnapshot> readWin32EditSurrounding(HWND focusHwnd) {
    if (!focusHwnd) {
        return std::nullopt;
    }
    wchar_t cls[64]{};
    ::GetClassNameW(focusHwnd, cls, static_cast<int>(std::size(cls)));
    const std::wstring_view clsView(cls);
    const bool isEdit = (clsView == L"Edit" || clsView.find(L"RichEdit") != std::wstring_view::npos ||
                         clsView.find(L"RICHEDIT") != std::wstring_view::npos);
    if (!isEdit) {
        return std::nullopt;
    }
    DWORD_PTR lenRes = 0;
    if (!::SendMessageTimeoutW(focusHwnd, WM_GETTEXTLENGTH, 0, 0,
                               SMTO_ABORTIFHUNG, 50, &lenRes)) {
        return std::nullopt;
    }
    const std::size_t len = static_cast<std::size_t>(lenRes);
    if (len > 16384) {
        return std::nullopt;
    }
    DWORD selStart = 0;
    DWORD selEnd = 0;
    DWORD_PTR selRes = 0;
    if (!::SendMessageTimeoutW(focusHwnd, EM_GETSEL,
                               reinterpret_cast<WPARAM>(&selStart),
                               reinterpret_cast<LPARAM>(&selEnd),
                               SMTO_ABORTIFHUNG, 50, &selRes)) {
        return std::nullopt;
    }
    std::wstring buf(len + 1, L'\0');
    DWORD_PTR copied = 0;
    if (len > 0) {
        if (!::SendMessageTimeoutW(focusHwnd, WM_GETTEXT,
                                   static_cast<WPARAM>(buf.size()),
                                   reinterpret_cast<LPARAM>(buf.data()),
                                   SMTO_ABORTIFHUNG, 50, &copied)) {
            return std::nullopt;
        }
        buf.resize(std::min<std::size_t>(static_cast<std::size_t>(copied), len));
    } else {
        buf.clear();
    }
    const std::size_t s = std::min<std::size_t>(selStart, buf.size());
    const std::size_t e = std::min<std::size_t>(std::max<std::size_t>(selEnd, s), buf.size());
    const std::size_t winStart = s > 64 ? s - 64 : 0;
    const std::size_t winEnd = std::min<std::size_t>(buf.size(), e + 32);

    const std::string leftUtf8 = wideToUtf8(std::wstring_view(buf).substr(winStart, s - winStart));
    const std::string selUtf8 = wideToUtf8(std::wstring_view(buf).substr(s, e - s));
    const std::string rightUtf8 = wideToUtf8(std::wstring_view(buf).substr(e, winEnd - e));
    return yuntype::SurroundingSnapshot{
        leftUtf8 + selUtf8 + rightUtf8,
        leftUtf8.size(),
        leftUtf8.size() + selUtf8.size(),
    };
}

} // namespace

struct CaretTracker::Impl {
    bool comInitialized = false;
    ComPtr<IUIAutomation> uia;

    void ensureUia() {
        if (uia) {
            return;
        }
        if (!comInitialized) {
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(hr) || hr == S_FALSE) {
                comInitialized = true;
            }
        }
        ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(uia.put()));
    }
};

CaretTracker::CaretTracker() : impl_(new Impl()) {
    impl_->ensureUia();
}

CaretTracker::~CaretTracker() {
    if (impl_) {
        impl_->uia.reset();
        if (impl_->comInitialized) {
            ::CoUninitialize();
        }
        delete impl_;
        impl_ = nullptr;
    }
}

CaretAnchor CaretTracker::queryCaret(HWND fgWnd, std::optional<POINT> lastClickPt) {
    impl_->ensureUia();
    RECT rc{};

    // Level 1: UI Automation TextPattern2 / TextPattern
    ComPtr<IUIAutomationElement> focusedElem;
    if (impl_->uia && SUCCEEDED(impl_->uia->GetFocusedElement(focusedElem.put())) && focusedElem) {
        ComPtr<IUIAutomationTextRange> range;
        if (getCaretRangeFromElement(focusedElem.get(), range) &&
            getRectFromTextRange(range.get(), rc)) {
            return CaretAnchor{rc, true};
        }
    }

    // Level 2 & 3: GetGUIThreadInfo (also yields focusHwnd for MSAA)
    HWND focusHwnd = fgWnd;
    if (queryGuiThreadCaret(fgWnd, rc, &focusHwnd)) {
        return CaretAnchor{rc, true};
    }
    if (queryMsaaCaret(focusHwnd, rc)) {
        return CaretAnchor{rc, true};
    }
    if (focusHwnd != fgWnd && queryMsaaCaret(fgWnd, rc)) {
        return CaretAnchor{rc, true};
    }

    // Level 4a: Last mouse left-click position (usually right where the user clicked the text field)
    if (lastClickPt) {
        POINT pt = *lastClickPt;
        if (fgWnd) {
            RECT wr{};
            if (::GetWindowRect(fgWnd, &wr) && ::PtInRect(&wr, pt)) {
                return CaretAnchor{RECT{pt.x, pt.y, pt.x + 2, pt.y + 22}, false};
            }
        }
    }

    // Level 4b: Focused UIA control bounding rect (bottom-left)
    if (focusedElem) {
        RECT elemRc{};
        if (SUCCEEDED(focusedElem->get_CurrentBoundingRectangle(&elemRc)) && isSaneRect(elemRc)) {
            return CaretAnchor{
                RECT{elemRc.left + 8, elemRc.bottom - 24, elemRc.left + 10, elemRc.bottom}, false};
        }
    }

    // Level 4c: Foreground window bottom-center
    if (fgWnd) {
        RECT wr{};
        if (::GetWindowRect(fgWnd, &wr) && wr.right > wr.left && wr.bottom > wr.top) {
            const long cx = (wr.left + wr.right) / 2 - 120;
            const long cy = std::max(wr.top + 40, wr.bottom - 90);
            return CaretAnchor{RECT{cx, cy, cx + 2, cy + 22}, false};
        }
    }

    // Last resort: primary monitor bottom-center
    const int sw = ::GetSystemMetrics(SM_CXSCREEN);
    const int sh = ::GetSystemMetrics(SM_CYSCREEN);
    return CaretAnchor{RECT{sw / 2 - 120, sh - 160, sw / 2 - 118, sh - 138}, false};
}

std::optional<yuntype::SurroundingSnapshot> CaretTracker::readSurrounding(HWND fgWnd) {
    impl_->ensureUia();

    if (impl_->uia) {
        ComPtr<IUIAutomationElement> focusedElem;
        if (SUCCEEDED(impl_->uia->GetFocusedElement(focusedElem.put())) && focusedElem) {
            ComPtr<IUIAutomationTextRange> selRange;
            if (getCaretRangeFromElement(focusedElem.get(), selRange)) {
                BSTR selBstr = nullptr;
                std::string selUtf8;
                if (SUCCEEDED(selRange->GetText(256, &selBstr)) && selBstr) {
                    selUtf8 = wideToUtf8(std::wstring_view(selBstr, ::SysStringLen(selBstr)));
                    ::SysFreeString(selBstr);
                }

                std::string leftUtf8;
                ComPtr<IUIAutomationTextRange> leftRange;
                if (SUCCEEDED(selRange->Clone(leftRange.put())) && leftRange) {
                    leftRange->MoveEndpointByRange(TextPatternRangeEndpoint_End, selRange.get(),
                                                   TextPatternRangeEndpoint_Start);
                    int moved = 0;
                    leftRange->MoveEndpointByUnit(TextPatternRangeEndpoint_Start,
                                                  TextUnit_Character, -64, &moved);
                    BSTR leftBstr = nullptr;
                    if (SUCCEEDED(leftRange->GetText(256, &leftBstr)) && leftBstr) {
                        leftUtf8 = wideToUtf8(std::wstring_view(leftBstr, ::SysStringLen(leftBstr)));
                        ::SysFreeString(leftBstr);
                    }
                }

                std::string rightUtf8;
                ComPtr<IUIAutomationTextRange> rightRange;
                if (SUCCEEDED(selRange->Clone(rightRange.put())) && rightRange) {
                    rightRange->MoveEndpointByRange(TextPatternRangeEndpoint_Start, selRange.get(),
                                                    TextPatternRangeEndpoint_End);
                    int moved = 0;
                    rightRange->MoveEndpointByUnit(TextPatternRangeEndpoint_End,
                                                   TextUnit_Character, 32, &moved);
                    BSTR rightBstr = nullptr;
                    if (SUCCEEDED(rightRange->GetText(256, &rightBstr)) && rightBstr) {
                        rightUtf8 =
                            wideToUtf8(std::wstring_view(rightBstr, ::SysStringLen(rightBstr)));
                        ::SysFreeString(rightBstr);
                    }
                }

                return yuntype::SurroundingSnapshot{
                    leftUtf8 + selUtf8 + rightUtf8,
                    leftUtf8.size(),
                    leftUtf8.size() + selUtf8.size(),
                };
            }
        }
    }

    // Fallback to classic Win32 Edit/RichEdit
    if (fgWnd) {
        const DWORD tid = ::GetWindowThreadProcessId(fgWnd, nullptr);
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (::GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) {
            if (auto snap = readWin32EditSurrounding(gti.hwndFocus)) {
                return snap;
            }
        }
        if (auto snap = readWin32EditSurrounding(fgWnd)) {
            return snap;
        }
    }
    return std::nullopt;
}

} // namespace yuntype::win
