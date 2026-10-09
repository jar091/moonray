// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

///
///

#include "Util.h"

#if defined(_WIN32)
#include <cstdio>     // stdout, _fileno
#include <io.h>       // _isatty
#include <windows.h>  // GetConsoleScreenBufferInfo
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace moonray_stats {

/// Split a line at a space (literally ' ') nearest the center of the string.
std::pair<std::string, std::string> splitMiddle(const std::string& s)
{
    if (s.empty()) {
        return std::make_pair(std::string(), std::string());
    }

    const auto center = s.size() / 2;
    const auto posl = s.rfind(' ', center);
    const auto posr = s.find(' ', center);

    if (posl == std::string::npos && posr == std::string::npos) {
        // No spaces found.
        return std::make_pair(s, std::string());
    } else if (posl == std::string::npos) {
        return std::make_pair(s.substr(0, posr), s.substr(posr + 1));
    } else if (posr == std::string::npos) {
        return std::make_pair(s.substr(0, posl), s.substr(posl + 1));
    } else {
        const auto distl = center - posl;
        const auto distr = posr - center;
        const auto pos = (distl < distr) ? posl : posr;
        return std::make_pair(s.substr(0, pos), s.substr(pos + 1));
    }
}

int computeWindowWidth()
{
#if defined(_WIN32)
    // If attached to a terminal...
    if (_isatty(_fileno(stdout))) {
        // Get the console window width. srWindow is the visible portion of the
        // screen buffer, which is the analogue of ws_col (dwSize.X is the
        // scrollback buffer width, which can be wider).
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            return csbi.srWindow.Right - csbi.srWindow.Left + 1;
        }
    }
    return 72;
#else
    // If attached to a terminal...
    if (isatty(STDOUT_FILENO)) {
        // Get the terminal width.
        winsize win;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &win);
        return win.ws_col;
    } else {
        return 72;
    }
#endif
}

std::string createDashTitle(std::string s)
{
    s = ' ' + s + ' ';

    const std::size_t preLength = 10;
    const std::size_t overallLength = 58;
    const std::size_t maxStringRoom = overallLength - preLength;

    if (s.length() >= maxStringRoom) {
        return std::string(preLength, '-') + s;
    } else {
        return std::string(preLength, '-') +
               s +
               std::string(maxStringRoom - s.length(), '-');
    }
}

std::string createArrowTitle(std::string s)
{
    s = ' ' + s + ' ';

    const std::size_t numDashes = 10;
    const std::string dashString(numDashes, '-');
    return '<' + dashString + s + dashString + '>';
}

} // namespace moonray_stats

