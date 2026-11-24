#include "win_sender.h"
#include "win_utf8.h"

#include <algorithm>
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
    constexpr std::size_t kChunk = 64;
    for (std::size_t i = 0; i < inputs.size(); i += kChunk) {
        const std::size_t n = std::min(kChunk, inputs.size() - i);
        ::SendInput(static_cast<UINT>(n), const_cast<INPUT *>(inputs.data() + i), sizeof(INPUT));
        if (i + n < inputs.size()) {
            ::Sleep(1);
        }
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

} // namespace yuntype::win
