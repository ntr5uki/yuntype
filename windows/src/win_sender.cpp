#include "win_sender.h"
#include "win_utf8.h"

#include <algorithm>
#include <vector>

namespace yuntype::win {

namespace {

// Release held modifier keys briefly if Alt/Ctrl/Win is still physically down when committing,
// so injected BackSpace / Unicode characters don't trigger app shortcuts (e.g. Alt+BackSpace).
void appendModifierRelease(std::vector<INPUT> &inputs, std::vector<WORD> &released) {
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
            released.push_back(vk);
        }
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
    const std::wstring wide = utf8ToWide(utf8);
    if (wide.empty()) {
        return;
    }
    std::vector<INPUT> inputs;
    inputs.reserve(wide.size() * 2 + 8);
    std::vector<WORD> released;
    appendModifierRelease(inputs, released);

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
    sendInputBatched(inputs);
}

void sendBackspaces(std::size_t count) {
    if (count == 0) {
        return;
    }
    std::vector<INPUT> inputs;
    inputs.reserve(count * 2 + 8);
    std::vector<WORD> released;
    appendModifierRelease(inputs, released);

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
    sendInputBatched(inputs);
}

} // namespace yuntype::win
