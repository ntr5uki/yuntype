#include "win_caret.h"
#include "win_utf8.h"

#include "../../core/utf8_util.h"

#include <oleacc.h>
#include <uiautomation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace yuntype::win {

namespace {

constexpr auto kUiaTaskTimeout = std::chrono::milliseconds(120);

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

bool getSelectionRangeFromElement(IUIAutomationElement *elem,
                                  ComPtr<IUIAutomationTextRange> &outRange) {
    if (!elem) {
        return false;
    }
    // Prefer active TextPattern::GetSelection first so non-empty selections (e.g. Ctrl+A)
    // are returned intact instead of a collapsed caret range.
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
    return getCaretRangeFromElement(elem, outRange);
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

bool isClassicWin32Edit(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    wchar_t cls[64]{};
    ::GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    const std::wstring_view clsView(cls);
    return clsView == L"Edit" || clsView.find(L"RichEdit") != std::wstring_view::npos ||
           clsView.find(L"RICHEDIT") != std::wstring_view::npos;
}

HWND findWin32EditTarget(HWND fgWnd) {
    if (!fgWnd) {
        return nullptr;
    }
    const DWORD tid = ::GetWindowThreadProcessId(fgWnd, nullptr);
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (::GetGUIThreadInfo(tid, &gti) && gti.hwndFocus && isClassicWin32Edit(gti.hwndFocus)) {
        return gti.hwndFocus;
    }
    if (isClassicWin32Edit(fgWnd)) {
        return fgWnd;
    }
    return nullptr;
}

std::optional<yuntype::SurroundingSnapshot> readWin32EditSurrounding(HWND editHwnd) {
    if (!editHwnd) {
        return std::nullopt;
    }
    DWORD_PTR lenRes = 0;
    if (!::SendMessageTimeoutW(editHwnd, WM_GETTEXTLENGTH, 0, 0,
                               SMTO_ABORTIFHUNG, 50, &lenRes)) {
        return std::nullopt;
    }
    const std::size_t len = static_cast<std::size_t>(lenRes);
    if (len > 32768) {
        return std::nullopt;
    }
    DWORD selStart = 0;
    DWORD selEnd = 0;
    DWORD_PTR selRes = 0;
    if (!::SendMessageTimeoutW(editHwnd, EM_GETSEL,
                               reinterpret_cast<WPARAM>(&selStart),
                               reinterpret_cast<LPARAM>(&selEnd),
                               SMTO_ABORTIFHUNG, 50, &selRes)) {
        return std::nullopt;
    }
    std::wstring buf(len + 1, L'\0');
    DWORD_PTR copied = 0;
    if (len > 0) {
        if (!::SendMessageTimeoutW(editHwnd, WM_GETTEXT,
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
    const std::size_t winStart = s > 96 ? s - 96 : 0;
    const std::size_t selCapEnd = std::min<std::size_t>(e, s + 2048);
    const std::size_t winEnd = std::min<std::size_t>(buf.size(), selCapEnd + 32);

    const std::string leftUtf8 = wideToUtf8(std::wstring_view(buf).substr(winStart, s - winStart));
    const std::string selUtf8 = wideToUtf8(std::wstring_view(buf).substr(s, selCapEnd - s));
    const std::string rightUtf8 =
        wideToUtf8(std::wstring_view(buf).substr(selCapEnd, winEnd - selCapEnd));
    return yuntype::SurroundingSnapshot{
        leftUtf8 + selUtf8 + rightUtf8,
        leftUtf8.size(),
        leftUtf8.size() + selUtf8.size(),
    };
}

bool selectWin32EditSuffix(HWND editHwnd, std::string_view expectedSuffix) {
    if (!editHwnd || expectedSuffix.empty()) {
        return false;
    }
    const std::wstring expectedW = utf8ToWide(expectedSuffix);
    if (expectedW.empty()) {
        return false;
    }
    DWORD selStart = 0;
    DWORD selEnd = 0;
    DWORD_PTR selRes = 0;
    if (!::SendMessageTimeoutW(editHwnd, EM_GETSEL,
                               reinterpret_cast<WPARAM>(&selStart),
                               reinterpret_cast<LPARAM>(&selEnd),
                               SMTO_ABORTIFHUNG, 50, &selRes)) {
        return false;
    }
    if (selStart != selEnd || selStart < expectedW.size()) {
        return false;
    }
    DWORD_PTR lenRes = 0;
    if (!::SendMessageTimeoutW(editHwnd, WM_GETTEXTLENGTH, 0, 0,
                               SMTO_ABORTIFHUNG, 50, &lenRes)) {
        return false;
    }
    const std::size_t len = static_cast<std::size_t>(lenRes);
    if (len > 32768 || selStart > len) {
        return false;
    }
    std::wstring buf(len + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!::SendMessageTimeoutW(editHwnd, WM_GETTEXT,
                               static_cast<WPARAM>(buf.size()),
                               reinterpret_cast<LPARAM>(buf.data()),
                               SMTO_ABORTIFHUNG, 50, &copied)) {
        return false;
    }
    buf.resize(std::min<std::size_t>(static_cast<std::size_t>(copied), len));
    if (selStart > buf.size()) {
        return false;
    }
    const std::size_t startIdx = selStart - expectedW.size();
    if (std::wstring_view(buf).substr(startIdx, expectedW.size()) != expectedW) {
        return false;
    }
    DWORD_PTR setRes = 0;
    return ::SendMessageTimeoutW(editHwnd, EM_SETSEL,
                                 static_cast<WPARAM>(startIdx),
                                 static_cast<LPARAM>(selStart),
                                 SMTO_ABORTIFHUNG, 50, &setRes) != 0;
}

CaretAnchor fallbackCaretAnchor(HWND fgWnd, std::optional<POINT> lastClickPt) {
    RECT rc{};
    HWND focusHwnd = fgWnd;
    if (queryGuiThreadCaret(fgWnd, rc, &focusHwnd)) {
        return CaretAnchor{rc, true};
    }
    if (lastClickPt && fgWnd) {
        RECT wr{};
        if (::GetWindowRect(fgWnd, &wr) && ::PtInRect(&wr, *lastClickPt)) {
            return CaretAnchor{
                RECT{lastClickPt->x, lastClickPt->y, lastClickPt->x + 2, lastClickPt->y + 22},
                false};
        }
    }
    if (fgWnd) {
        RECT wr{};
        if (::GetWindowRect(fgWnd, &wr) && wr.right > wr.left && wr.bottom > wr.top) {
            const long cx = (wr.left + wr.right) / 2 - 120;
            const long cy = std::max(wr.top + 40, wr.bottom - 90);
            return CaretAnchor{RECT{cx, cy, cx + 2, cy + 22}, false};
        }
    }
    const int sw = ::GetSystemMetrics(SM_CXSCREEN);
    const int sh = ::GetSystemMetrics(SM_CYSCREEN);
    return CaretAnchor{RECT{sw / 2 - 120, sh - 160, sw / 2 - 118, sh - 138}, false};
}

} // namespace

struct CaretTracker::Impl {
    struct PendingTask {
        std::function<void(IUIAutomation *)> fn;
        std::mutex doneMutex;
        std::condition_variable doneCv;
        bool done = false;
    };

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::shared_ptr<PendingTask> nextTask;
    bool busy = false;
    bool stop = false;
    std::thread worker;

    Impl() {
        worker = std::thread([this]() { workerLoop(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stop = true;
            nextTask.reset();
        }
        queueCv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Run a COM/UIA task on the dedicated STA thread with a hard timeout so the caller
    // (and WH_KEYBOARD_LL hook thread) NEVER stalls waiting on a hung app's accessibility tree.
    bool runWithTimeout(std::function<void(IUIAutomation *)> fn,
                        std::chrono::milliseconds timeout = kUiaTaskTimeout) {
        auto task = std::make_shared<PendingTask>();
        task->fn = std::move(fn);
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (stop || busy) {
                return false; // Worker still busy on a slow app: degrade immediately
            }
            nextTask = task;
        }
        queueCv.notify_one();

        std::unique_lock<std::mutex> dlock(task->doneMutex);
        return task->doneCv.wait_for(dlock, timeout, [&]() { return task->done; });
    }

    void workerLoop() {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comInit = SUCCEEDED(hr) || hr == S_FALSE;
        ComPtr<IUIAutomation> uia;
        ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(uia.put()));

        while (true) {
            std::shared_ptr<PendingTask> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [&]() { return stop || nextTask != nullptr; });
                if (stop) {
                    break;
                }
                task = std::move(nextTask);
                busy = true;
            }

            if (task && task->fn) {
                if (!uia) {
                    ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(uia.put()));
                }
                task->fn(uia.get());
            }

            {
                std::lock_guard<std::mutex> lock(queueMutex);
                busy = false;
            }
            if (task) {
                {
                    std::lock_guard<std::mutex> dlock(task->doneMutex);
                    task->done = true;
                }
                task->doneCv.notify_all();
            }
        }

        uia.reset();
        if (comInit) {
            ::CoUninitialize();
        }
    }
};

CaretTracker::CaretTracker() : impl_(new Impl()) {}

CaretTracker::~CaretTracker() {
    delete impl_;
    impl_ = nullptr;
}

CaretAnchor CaretTracker::queryCaret(HWND fgWnd, std::optional<POINT> lastClickPt) {
    auto result = std::make_shared<std::optional<CaretAnchor>>();
    const bool finished = impl_->runWithTimeout([fgWnd, lastClickPt, result](IUIAutomation *uia) {
        RECT rc{};
        ComPtr<IUIAutomationElement> focusedElem;
        if (uia && SUCCEEDED(uia->GetFocusedElement(focusedElem.put())) && focusedElem) {
            ComPtr<IUIAutomationTextRange> range;
            if (getSelectionRangeFromElement(focusedElem.get(), range) &&
                getRectFromTextRange(range.get(), rc)) {
                *result = CaretAnchor{rc, true};
                return;
            }
        }
        HWND focusHwnd = fgWnd;
        if (queryGuiThreadCaret(fgWnd, rc, &focusHwnd)) {
            *result = CaretAnchor{rc, true};
            return;
        }
        if (queryMsaaCaret(focusHwnd, rc)) {
            *result = CaretAnchor{rc, true};
            return;
        }
        if (focusHwnd != fgWnd && queryMsaaCaret(fgWnd, rc)) {
            *result = CaretAnchor{rc, true};
            return;
        }
        if (lastClickPt && fgWnd) {
            RECT wr{};
            if (::GetWindowRect(fgWnd, &wr) && ::PtInRect(&wr, *lastClickPt)) {
                *result = CaretAnchor{
                    RECT{lastClickPt->x, lastClickPt->y, lastClickPt->x + 2, lastClickPt->y + 22},
                    false};
                return;
            }
        }
        if (focusedElem) {
            RECT elemRc{};
            if (SUCCEEDED(focusedElem->get_CurrentBoundingRectangle(&elemRc)) &&
                isSaneRect(elemRc)) {
                *result = CaretAnchor{
                    RECT{elemRc.left + 8, elemRc.bottom - 24, elemRc.left + 10, elemRc.bottom},
                    false};
                return;
            }
        }
    });

    if (finished && *result) {
        return **result;
    }
    return fallbackCaretAnchor(fgWnd, lastClickPt);
}

std::optional<yuntype::SurroundingSnapshot> CaretTracker::readSurrounding(HWND fgWnd) {
    auto result = std::make_shared<std::optional<yuntype::SurroundingSnapshot>>();
    const bool finished = impl_->runWithTimeout([fgWnd, result](IUIAutomation *uia) {
        if (uia) {
            ComPtr<IUIAutomationElement> focusedElem;
            if (SUCCEEDED(uia->GetFocusedElement(focusedElem.put())) && focusedElem) {
                ComPtr<IUIAutomationTextRange> selRange;
                if (getSelectionRangeFromElement(focusedElem.get(), selRange)) {
                    BSTR selBstr = nullptr;
                    std::string selUtf8;
                    if (SUCCEEDED(selRange->GetText(2048, &selBstr)) && selBstr) {
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
                                                      TextUnit_Character, -96, &moved);
                        BSTR leftBstr = nullptr;
                        if (SUCCEEDED(leftRange->GetText(512, &leftBstr)) && leftBstr) {
                            leftUtf8 =
                                wideToUtf8(std::wstring_view(leftBstr, ::SysStringLen(leftBstr)));
                            ::SysFreeString(leftBstr);
                        }
                    }

                    std::string rightUtf8;
                    ComPtr<IUIAutomationTextRange> rightRange;
                    if (SUCCEEDED(selRange->Clone(rightRange.put())) && rightRange) {
                        rightRange->MoveEndpointByRange(TextPatternRangeEndpoint_Start,
                                                        selRange.get(),
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

                    *result = yuntype::SurroundingSnapshot{
                        leftUtf8 + selUtf8 + rightUtf8,
                        leftUtf8.size(),
                        leftUtf8.size() + selUtf8.size(),
                    };
                    return;
                }
                // Fallback for editable controls without TextPattern (e.g. WeChat / Qt inputs):
                // inspect writable UIA ValuePattern when reasonably short.
                ComPtr<IUIAutomationValuePattern> vp;
                if (SUCCEEDED(focusedElem->GetCurrentPatternAs(
                        UIA_ValuePatternId, IID_PPV_ARGS(vp.put()))) &&
                    vp) {
                    BOOL isReadOnly = FALSE;
                    if (SUCCEEDED(vp->get_CurrentIsReadOnly(&isReadOnly)) && !isReadOnly) {
                        BSTR valBstr = nullptr;
                        if (SUCCEEDED(vp->get_CurrentValue(&valBstr)) && valBstr) {
                            const UINT wlen = ::SysStringLen(valBstr);
                            std::string valUtf8;
                            if (wlen > 0 && wlen <= 2048) {
                                valUtf8 = wideToUtf8(std::wstring_view(valBstr, wlen));
                            }
                            ::SysFreeString(valBstr);
                            if (!valUtf8.empty()) {
                                const std::size_t endPos = valUtf8.size();
                                *result = yuntype::SurroundingSnapshot{
                                    std::move(valUtf8),
                                    endPos,
                                    endPos,
                                };
                                return;
                            }
                        }
                    }
                }
            }
        }
        if (HWND editHwnd = findWin32EditTarget(fgWnd)) {
            *result = readWin32EditSurrounding(editHwnd);
        }
    });

    if (finished && *result) {
        return *result;
    }
    if (HWND editHwnd = findWin32EditTarget(fgWnd)) {
        return readWin32EditSurrounding(editHwnd);
    }
    return std::nullopt;
}

bool CaretTracker::selectSuffixBeforeCaret(HWND fgWnd, std::string_view expectedSuffix) {
    if (expectedSuffix.empty()) {
        return false;
    }
    const std::string expected(expectedSuffix);
    const int charCount = static_cast<int>(yuntype::utf8Length(expected));
    if (charCount <= 0 || charCount > 1024) {
        return false;
    }

    auto selected = std::make_shared<bool>(false);
    const bool finished = impl_->runWithTimeout([fgWnd, expected, charCount, selected](IUIAutomation *uia) {
        if (uia) {
            ComPtr<IUIAutomationElement> focusedElem;
            if (SUCCEEDED(uia->GetFocusedElement(focusedElem.put())) && focusedElem) {
                ComPtr<IUIAutomationTextRange> caretRange;
                if (getSelectionRangeFromElement(focusedElem.get(), caretRange)) {
                    ComPtr<IUIAutomationTextRange> targetRange;
                    if (SUCCEEDED(caretRange->Clone(targetRange.put())) && targetRange) {
                        // Collapse to caret start, then extend start backward by charCount chars
                        targetRange->MoveEndpointByRange(TextPatternRangeEndpoint_End,
                                                         caretRange.get(),
                                                         TextPatternRangeEndpoint_Start);
                        int moved = 0;
                        if (SUCCEEDED(targetRange->MoveEndpointByUnit(
                                TextPatternRangeEndpoint_Start, TextUnit_Character, -charCount,
                                &moved)) &&
                            moved == -charCount) {
                            BSTR bstr = nullptr;
                            if (SUCCEEDED(targetRange->GetText(2048, &bstr)) && bstr) {
                                const std::string actual =
                                    wideToUtf8(std::wstring_view(bstr, ::SysStringLen(bstr)));
                                ::SysFreeString(bstr);
                                if (actual == expected && SUCCEEDED(targetRange->Select())) {
                                    *selected = true;
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (HWND editHwnd = findWin32EditTarget(fgWnd)) {
            if (selectWin32EditSuffix(editHwnd, expected)) {
                *selected = true;
            }
        }
    });

    return finished && *selected;
}

} // namespace yuntype::win
