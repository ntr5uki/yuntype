#include "win_sender.h"
#include "win_utf8.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace yuntype::win {

namespace {

constexpr WORD kMaskUnassignedVk = 0xE8;

// Release held modifier keys cleanly (masking Alt first so menu bar is not activated).
void appendModifierRelease(std::vector<INPUT> &inputs) {
    const bool altDown = ((::GetAsyncKeyState(VK_LMENU) & 0x8000) != 0) ||
                         ((::GetAsyncKeyState(VK_RMENU) & 0x8000) != 0);
    if (altDown) {
        INPUT maskDown{};
        maskDown.type = INPUT_KEYBOARD;
        maskDown.ki.wVk = kMaskUnassignedVk;
        maskDown.ki.dwExtraInfo = kYunTypeInjectedTag;
        INPUT maskUp = maskDown;
        maskUp.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(maskDown);
        inputs.push_back(maskUp);
    }
    static constexpr WORD kMods[] = {
        VK_LMENU, VK_RMENU, VK_LCONTROL, VK_RCONTROL, VK_LWIN, VK_RWIN,
    };
    for (WORD vk : kMods) {
        if ((::GetAsyncKeyState(vk) & 0x8000) != 0) {
            INPUT up{};
            up.type = INPUT_KEYBOARD;
            up.ki.wVk = vk;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
            up.ki.dwExtraInfo = kYunTypeInjectedTag;
            inputs.push_back(up);
        }
    }
}

void appendBackspaces(std::vector<INPUT> &inputs, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = VK_BACK;
        down.ki.dwExtraInfo = kYunTypeInjectedTag;
        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(down);
        inputs.push_back(up);
    }
}

void appendUnicode(std::vector<INPUT> &inputs, std::string_view utf8) {
    const std::wstring wide = utf8ToWide(utf8);
    for (wchar_t ch : wide) {
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wVk = VK_RETURN;
            down.ki.dwExtraInfo = kYunTypeInjectedTag;
            INPUT up = down;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
            inputs.push_back(down);
            inputs.push_back(up);
            continue;
        }
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = static_cast<WORD>(ch);
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        down.ki.dwExtraInfo = kYunTypeInjectedTag;

        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(down);
        inputs.push_back(up);
    }
}

void sendInputBatched(const std::vector<INPUT> &inputs) {
    if (inputs.empty()) {
        return;
    }
    constexpr std::size_t kChunk = 512;
    for (std::size_t i = 0; i < inputs.size(); i += kChunk) {
        const std::size_t n = std::min(kChunk, inputs.size() - i);
        ::SendInput(static_cast<UINT>(n), const_cast<INPUT *>(inputs.data() + i), sizeof(INPUT));
    }
}

} // namespace

void sendUnicodeText(std::string_view utf8) {
    if (utf8.empty()) {
        return;
    }
    std::vector<INPUT> inputs;
    inputs.reserve(utf8.size() * 2 + 10);
    appendModifierRelease(inputs);
    appendUnicode(inputs, utf8);
    sendInputBatched(inputs);
}

void sendBackspaces(std::size_t count) {
    if (count == 0) {
        return;
    }
    std::vector<INPUT> inputs;
    inputs.reserve(count * 2 + 10);
    appendModifierRelease(inputs);
    appendBackspaces(inputs, count);
    sendInputBatched(inputs);
}

void sendReplaceText(std::size_t eraseChars, std::string_view utf8) {
    if (eraseChars == 0 && utf8.empty()) {
        return;
    }
    std::vector<INPUT> inputs;
    inputs.reserve(eraseChars * 2 + utf8.size() * 2 + 10);
    appendModifierRelease(inputs);
    appendBackspaces(inputs, eraseChars);
    appendUnicode(inputs, utf8);
    sendInputBatched(inputs);
}

void suppressAltMenuActivation() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = kMaskUnassignedVk;
    inputs[0].ki.dwExtraInfo = kYunTypeInjectedTag;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, inputs, sizeof(INPUT));
}

bool isTerminalWindow(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    wchar_t cls[128]{};
    const int n = ::GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    if (n <= 0) {
        return false;
    }
    const std::wstring_view v(cls, static_cast<std::size_t>(n));
    return v == L"CASCADIA_HOSTING_WINDOW_CLASS" ||
           v == L"ConsoleWindowClass" ||
           v == L"mintty" ||
           v == L"Alacritty" ||
           v == L"PuTTY" ||
           v == L"VirtualConsoleClass" ||
           v.find(L"wezterm") != std::wstring_view::npos ||
           v.find(L"WezTerm") != std::wstring_view::npos ||
           v.find(L"Terminal") != std::wstring_view::npos;
}

namespace {

bool openClipboardRetry(int maxTries = 6) {
    for (int i = 0; i < maxTries; ++i) {
        if (::OpenClipboard(nullptr)) {
            return true;
        }
        ::Sleep(5);
    }
    return false;
}

void setClipboardDwordFormat(const wchar_t *formatName, DWORD value) {
    const UINT fmt = ::RegisterClipboardFormatW(formatName);
    if (fmt == 0) {
        return;
    }
    HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!hg) {
        return;
    }
    if (void *p = ::GlobalLock(hg)) {
        std::memcpy(p, &value, sizeof(DWORD));
        ::GlobalUnlock(hg);
        if (!::SetClipboardData(fmt, hg)) {
            ::GlobalFree(hg);
        }
    } else {
        ::GlobalFree(hg);
    }
}

void markClipboardSilent() {
    setClipboardDwordFormat(L"ExcludeClipboardContentFromMonitorProcessing", 1);
    setClipboardDwordFormat(L"CanIncludeInClipboardHistory", 0);
    setClipboardDwordFormat(L"CanUploadToCloudClipboard", 0);
}

bool readClipboardUnicodeText(std::wstring &out) {
    out.clear();
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return false;
    }
    if (!openClipboardRetry()) {
        return false;
    }
    bool ok = false;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t *ptr = static_cast<const wchar_t *>(::GlobalLock(h))) {
            const std::size_t maxChars = ::GlobalSize(h) / sizeof(wchar_t);
            std::size_t len = 0;
            while (len < maxChars && len < 65536 && ptr[len] != L'\0') {
                ++len;
            }
            out.assign(ptr, len);
            ::GlobalUnlock(h);
            ok = true;
        }
    }
    ::CloseClipboard();
    return ok;
}

void restoreClipboardUnicodeText(const std::wstring &text, bool hadText) {
    if (!openClipboardRetry()) {
        return;
    }
    ::EmptyClipboard();
    if (hadText) {
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hg) {
            if (void *p = ::GlobalLock(hg)) {
                std::memcpy(p, text.c_str(), bytes);
                ::GlobalUnlock(hg);
                if (!::SetClipboardData(CF_UNICODETEXT, hg)) {
                    ::GlobalFree(hg);
                }
            } else {
                ::GlobalFree(hg);
            }
        }
    }
    markClipboardSilent();
    ::CloseClipboard();
}

} // namespace

std::optional<std::string> readSelectionViaClipboard(HWND fgWnd) {
    if (!fgWnd || isTerminalWindow(fgWnd)) {
        return std::nullopt;
    }

    std::wstring origText;
    const bool hadOrigText = readClipboardUnicodeText(origText);
    const DWORD seqBefore = ::GetClipboardSequenceNumber();

    std::vector<INPUT> inputs;
    inputs.reserve(20);
    appendModifierRelease(inputs);
    for (WORD vk : {VK_LSHIFT, VK_RSHIFT}) {
        if ((::GetAsyncKeyState(vk) & 0x8000) != 0) {
            INPUT up{};
            up.type = INPUT_KEYBOARD;
            up.ki.wVk = vk;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
            up.ki.dwExtraInfo = kYunTypeInjectedTag;
            inputs.push_back(up);
        }
    }

    INPUT ctrlDown{};
    ctrlDown.type = INPUT_KEYBOARD;
    ctrlDown.ki.wVk = VK_CONTROL;
    ctrlDown.ki.dwExtraInfo = kYunTypeInjectedTag;

    INPUT cDown{};
    cDown.type = INPUT_KEYBOARD;
    cDown.ki.wVk = 'C';
    cDown.ki.dwExtraInfo = kYunTypeInjectedTag;

    INPUT cUp = cDown;
    cUp.ki.dwFlags = KEYEVENTF_KEYUP;

    INPUT ctrlUp = ctrlDown;
    ctrlUp.ki.dwFlags = KEYEVENTF_KEYUP;

    inputs.push_back(ctrlDown);
    inputs.push_back(cDown);
    inputs.push_back(cUp);
    inputs.push_back(ctrlUp);
    sendInputBatched(inputs);

    bool changed = false;
    for (int i = 0; i < 16; ++i) {
        ::Sleep(5);
        if (::GetClipboardSequenceNumber() != seqBefore) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        return std::nullopt;
    }

    std::wstring copiedW;
    const bool gotText = readClipboardUnicodeText(copiedW);
    restoreClipboardUnicodeText(origText, hadOrigText);

    if (!gotText || copiedW.empty() || copiedW.size() > 2048) {
        return std::nullopt;
    }
    std::string utf8 = wideToUtf8(copiedW);
    if (utf8.empty()) {
        return std::nullopt;
    }
    return utf8;
}

} // namespace yuntype::win
