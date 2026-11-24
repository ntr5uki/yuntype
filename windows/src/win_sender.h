#ifndef YUNTYPE_WIN_SENDER_H
#define YUNTYPE_WIN_SENDER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace yuntype::win {

// Send UTF-8 text into the active window via SendInput(KEYEVENTF_UNICODE).
void sendUnicodeText(std::string_view utf8);

// Send N BackSpace keystrokes into the active window.
void sendBackspaces(std::size_t count);

// Atomically erase N characters before the caret and type UTF-8 replacement text.
void sendReplaceText(std::size_t eraseChars, std::string_view utf8);

// Inject a harmless unassigned key so releasing Alt does not activate the app menu bar.
void suppressAltMenuActivation();

// Detect common terminal emulators where Ctrl+C would send SIGINT.
bool isTerminalWindow(HWND hwnd);

// Read an active text selection via silent Ctrl+C (backing up and restoring original clipboard
// text with Win11 history-exclusion flags). Skips terminals. Returns std::nullopt if unchanged.
std::optional<std::string> readSelectionViaClipboard(HWND fgWnd);

} // namespace yuntype::win

#endif
