#ifndef YUNTYPE_WIN_SENDER_H
#define YUNTYPE_WIN_SENDER_H

#include <cstddef>
#include <string_view>

namespace yuntype::win {

// Send UTF-8 text into the active window via SendInput(KEYEVENTF_UNICODE).
void sendUnicodeText(std::string_view utf8);

// Send N BackSpace keystrokes into the active window.
void sendBackspaces(std::size_t count);

} // namespace yuntype::win

#endif
