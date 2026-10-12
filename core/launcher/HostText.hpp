// The settings launcher's text helpers for starting processes and reading the Linux folder pickers (s61),
// header-only so port_settings_tests covers them (Host.cpp uses them).
#pragma once

#include "prx/libc/include/PortSettings.hpp"

#include <string>
#include <string_view>

namespace Launcher {

// One argument for a Windows command line, as CommandLineToArgvW and the C runtime split it: quoted when it
// is empty or holds a space, tab or quote; a quote is escaped, and backslashes are doubled only before a
// quote (also before the closing one, so "D:\" stays D:\ and does not swallow the quote).
template <typename Text>
Text QuoteArgument(const Text& text) {
    using Char = typename Text::value_type;
    if (!text.empty() && text.find_first_of(Text{Char(' '), Char('\t'), Char('"')}) == Text::npos) return text;
    Text result(1, Char('"'));
    std::size_t backslashes = 0;
    for (const Char c : text) {
        if (c == Char('\\')) {
            ++backslashes;
            continue;
        }
        result.append(c == Char('"') ? backslashes * 2 + 1 : backslashes, Char('\\'));
        backslashes = 0;
        result += c;
    }
    result.append(backslashes * 2, Char('\\'));
    result += Char('"');
    return result;
}

// A file:// URI path with its %XX escapes decoded (the desktop portal's answer).
inline std::string PercentDecode(std::string_view text) {
    std::string result;
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size() && hex(text[index + 1]) >= 0 && hex(text[index + 2]) >= 0) {
            result += static_cast<char>(hex(text[index + 1]) * 16 + hex(text[index + 2]));
            index += 2;
            continue;
        }
        result += text[index];
    }
    return result;
}

enum class PortalAnswer { Unreadable, Picked, Cancelled };

// The xdg-desktop-portal FileChooser's Response signal line as gdbus monitor prints it: "Response (uint32 0,
// {'uris': <['file:///home/deck/Games/app0']>, ...})"; 0 = picked (with `folder`), 1 = cancelled.
inline PortalAnswer ParsePortalResponse(std::string_view line, std::string& folder) {
    const auto response = line.find("Response (uint32 ");
    if (response == std::string_view::npos || response + 17 >= line.size()) return PortalAnswer::Unreadable;
    const char status = line[response + 17];
    if (status == '1' || status == '2') return PortalAnswer::Cancelled;
    if (status != '0') return PortalAnswer::Unreadable;
    const auto uri = line.find("'file://", response);
    if (uri == std::string_view::npos) return PortalAnswer::Unreadable;
    const auto end = line.find('\'', uri + 1);
    if (end == std::string_view::npos) return PortalAnswer::Unreadable;
    folder = PercentDecode(line.substr(uri + 8, end - uri - 8));
    return folder.empty() ? PortalAnswer::Unreadable : PortalAnswer::Picked;
}

}
