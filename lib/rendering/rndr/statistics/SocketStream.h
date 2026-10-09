// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

///
///

#pragma once

#include <array>
#include <chrono>
#include <iosfwd>
#include <istream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <scene_rdl2/common/platform/Platform.h> // ssize_t
// scene_rdl2's common/platform/DebugLog.h (reached through Platform.h above)
// #undefs the legacy `far` / `near` macros directly after including
// <windows.h>, and <minwindef.h>'s include guard stops them from ever being
// defined again. Winsock spells its pointer parameters `FAR *`, i.e. `far *`,
// including inside the FD_ZERO()/FD_SET() macros expanded further down, so put
// the two empty macros back - exactly what <minwindef.h> would do if it were
// re-read.
#define far
#define near
#else
#include <netinet/in.h>
#endif

namespace moonray {
namespace stats {

// Winsock's socket handle is an unsigned UINT_PTR with its own invalid value,
// not a small non-negative file descriptor, so the handle type and the "no
// socket" value are named rather than assumed. Everything else about the BSD
// socket API is spelled identically on both platforms.
#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

// Winsock requires a per-process WSAStartup() before any socket call; this is
// the idempotent, thread-safe wrapper for it. Nothing to do elsewhere.
void initSocketLayer();

class SocketException : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class ListeningSocket
{
public:
    friend class Socket;

    explicit ListeningSocket(int port);
    ListeningSocket(); // Automatic port

    ListeningSocket(const ListeningSocket&) = delete;
    ListeningSocket(ListeningSocket&& other) noexcept;
    ListeningSocket& operator=(const ListeningSocket&) = delete;
    ListeningSocket& operator=(ListeningSocket&& other) noexcept;

    ~ListeningSocket();

    explicit operator bool() const noexcept;
    int getPortNumber() const;

    void swap(ListeningSocket& other) noexcept;

private:
    SocketHandle mFD;
};

class Socket
{
public:
    // These calls will block as necessary.
    static Socket as_server(const ListeningSocket& ls);
    static Socket as_client(const std::string& hostname, int port);

    Socket() noexcept;
    Socket(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(const Socket&) = delete;
    Socket& operator=(Socket&& other) noexcept;

    ~Socket();

    explicit operator bool() const noexcept;

    ssize_t write(const char* buf, ssize_t count);
    ssize_t read(char* buf, ssize_t count);

    void swap(Socket& other) noexcept;

private:
    SocketHandle mFD;
};

class SocketStreambuf : public std::streambuf
{
public:
    SocketStreambuf();
    explicit SocketStreambuf(Socket&& socket);

    SocketStreambuf(const SocketStreambuf&) = delete;
    SocketStreambuf operator=(const SocketStreambuf&) = delete;
    ~SocketStreambuf();

    bool is_open() const noexcept;
    void open(const std::string& filename, int port);

private:
    bool write_to_socket();
    int_type overflow(int_type ch) override;
    int sync() override;
    int_type underflow() override;

    static constexpr std::size_t sPutBack = 8;
    std::array<char, 2048> mOutputBuffer;
    std::array<char, 2048> mInputBuffer;

    Socket mSock;
};

class SocketStream : public std::iostream
{
public:
    explicit SocketStream(Socket&& socket) :
            std::iostream(nullptr),
            mBuf(std::move(socket))
    {
        this->init(&mBuf);
    }

    bool is_open() const noexcept
    {
        return mBuf.is_open();
    }

private:
    SocketStreambuf mBuf;
};

} // namespace stats
} // namespace moonray

