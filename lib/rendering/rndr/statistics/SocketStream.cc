// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

///
///

#include <moonray/rendering/rndr/statistics/SocketStream.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#ifdef _WIN32
// Winsock2 is the Windows spelling of the same BSD socket API: the calls used
// here (getaddrinfo/socket/bind/listen/accept/connect/getsockname/select) have
// identical signatures. What differs is spelled out below.
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace moonray {
namespace stats {

#ifdef _WIN32

void initSocketLayer()
{
    // WSAStartup() is reference counted, but doing it once behind a call_once
    // keeps it off the hot path. There is no matching WSACleanup(): the sockets
    // outlive any scope it could be tied to, and the OS reclaims at exit.
    static std::once_flag sOnce;
    std::call_once(sOnce, [] {
        WSADATA wsaData;
        const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (rc != 0) {
            throw SocketException("Unable to initialize Winsock: " + std::to_string(rc));
        }
    });
}

namespace {
// Winsock reports errors through WSAGetLastError() rather than errno, and
// read()/write() are not defined for sockets there - recv()/send() are.
inline int socketErrno() { return WSAGetLastError(); }
constexpr int kSocketEIntr       = WSAEINTR;
constexpr int kSocketEWouldBlock = WSAEWOULDBLOCK;
constexpr int kSocketEAgain      = WSAEWOULDBLOCK;
inline void closeSocket(SocketHandle fd) { ::closesocket(fd); }
inline ssize_t sendSocket(SocketHandle fd, const char* buf, ssize_t count)
{
    return ::send(fd, buf, static_cast<int>(count), 0);
}
inline ssize_t recvSocket(SocketHandle fd, char* buf, ssize_t count)
{
    return ::recv(fd, buf, static_cast<int>(count), 0);
}
} // namespace

#else // else of _WIN32

void initSocketLayer() {}

namespace {
inline int socketErrno() { return errno; }
constexpr int kSocketEIntr       = EINTR;
constexpr int kSocketEWouldBlock = EWOULDBLOCK;
constexpr int kSocketEAgain      = EAGAIN;
inline void closeSocket(SocketHandle fd) { ::close(fd); }
inline ssize_t sendSocket(SocketHandle fd, const char* buf, ssize_t count)
{
    return ::write(fd, buf, count);
}
inline ssize_t recvSocket(SocketHandle fd, char* buf, ssize_t count)
{
    return ::read(fd, buf, count);
}
} // namespace

#endif // end of not _WIN32

ListeningSocket::ListeningSocket(int port) :
    mFD(kInvalidSocket)
{
    initSocketLayer();

    addrinfo hints;
    memset(&hints, 0, sizeof(addrinfo));
    hints.ai_family    = AF_UNSPEC;  // Allow IPv4 or IPv6
    hints.ai_socktype  = SOCK_STREAM;
    hints.ai_flags     = AI_PASSIVE; // For wildcard IP address
    hints.ai_protocol  = 0;          // Any protocol
    hints.ai_canonname = nullptr;
    hints.ai_addr      = nullptr;
    hints.ai_next      = nullptr;

    constexpr int portdigits = std::numeric_limits<decltype(port)>::digits10;
    char portstring[portdigits + 1];
    snprintf(portstring, portdigits + 1, "%i", port);

    addrinfo* result;
    const int gairesult = getaddrinfo(nullptr, portstring, &hints, &result);
    if (gairesult != 0) {
        throw SocketException(std::string("Unable to get server information: ") +
                              gai_strerror(gairesult));
    }

    addrinfo* rp;
    SocketHandle sfd = kInvalidSocket;
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == kInvalidSocket) {
            continue;
        }

        if (bind(sfd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
            // success!
            break;
        }

        closeSocket(sfd);
    }

    freeaddrinfo(result);

    if (rp == nullptr) {
        throw SocketException("Could not bind");
    }

    mFD = sfd;

    if (listen(mFD, 5) < 0) {
        throw SocketException("Error on listen: " + std::to_string(socketErrno()));
    }
}

ListeningSocket::ListeningSocket() :
    ListeningSocket(0)
{
}

ListeningSocket::ListeningSocket(ListeningSocket&& other) noexcept :
    mFD(other.mFD)
{
    other.mFD = kInvalidSocket;
}

ListeningSocket& ListeningSocket::operator=(ListeningSocket&& other) noexcept
{
    this->swap(other);
    return *this;
}

ListeningSocket::~ListeningSocket()
{
    if (mFD != kInvalidSocket) {
        closeSocket(mFD);
    }
}

ListeningSocket::operator bool() const noexcept
{
    return mFD != kInvalidSocket;
}

int ListeningSocket::getPortNumber() const
{
    sockaddr_in addr;
    socklen_t addrlen = static_cast<socklen_t>(sizeof(addr));
    std::memset(&addr, 0, addrlen);
    if (getsockname(mFD, reinterpret_cast<sockaddr*>(&addr), &addrlen) < 0) {
        throw SocketException("Error in getsockname");
    }
    return ntohs(addr.sin_port);
}

void ListeningSocket::swap(ListeningSocket& other) noexcept
{
    std::swap(other.mFD, mFD);
}

Socket::Socket() noexcept :
    mFD(kInvalidSocket)
{
}

Socket::~Socket()
{
    if (mFD != kInvalidSocket) {
        closeSocket(mFD);
    }
}

Socket Socket::as_client(const std::string& hostname, int port)
{
    initSocketLayer();

    addrinfo hints;
    memset(&hints, 0, sizeof(addrinfo));
    hints.ai_family   = AF_UNSPEC;  // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = 0;
    hints.ai_protocol = 0;          // Any protocol

    constexpr int portdigits = std::numeric_limits<decltype(port)>::digits10;
    char portstring[portdigits + 1];
    snprintf(portstring, portdigits + 1, "%i", port);

    addrinfo* result;
    const int gairesult = getaddrinfo(hostname.c_str(), portstring, &hints, &result);
    if (gairesult != 0) {
        throw SocketException(std::string("Unable to get host address: ") +
                              gai_strerror(gairesult));
    }

    addrinfo* rp;
    SocketHandle sfd = kInvalidSocket;
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == kInvalidSocket) {
            continue;
        }

        if (connect(sfd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) != -1) {
            // success!
            break;
        }

        closeSocket(sfd);
    }

    freeaddrinfo(result);

    if (rp == nullptr) {
        throw SocketException("Could not connect to host");
    }

    Socket ret;
    ret.mFD = sfd;
    return ret;
}

Socket Socket::as_server(const ListeningSocket& ls)
{
    Socket ret;
    sockaddr_in cli_addr;
    socklen_t clilen = static_cast<socklen_t>(sizeof(cli_addr));
    ret.mFD = accept(ls.mFD, reinterpret_cast<sockaddr*>(&cli_addr), &clilen);
    if (ret.mFD == kInvalidSocket) {
        throw SocketException("Error on accept: " + std::to_string(socketErrno()));
    }
    return ret;
}

Socket::Socket(Socket&& other) noexcept :
    mFD(other.mFD)
{
    other.mFD = kInvalidSocket;
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    this->swap(other);
    return *this;
}

Socket::operator bool() const noexcept
{
    return mFD != kInvalidSocket;
}

ssize_t Socket::write(const char* buf, ssize_t count)
{
    ssize_t n = 0;
    while (n < count) {
        const ssize_t ret = sendSocket(mFD, buf + n, count - n);
        if (ret < 0) {
            const int err = socketErrno();
            if (err == kSocketEIntr) {
                // Signal interruption; try again.
                continue;
            } else if (err == kSocketEWouldBlock || err == kSocketEAgain) {
                // Blocking socket. Wait for it to free up.
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(mFD, &writefds);

                // TODO: set timeout? What do we do if we timeout?
                if (select(static_cast<int>(mFD) + 1, nullptr, &writefds, nullptr, nullptr) < 0) {
                    if (socketErrno() == kSocketEIntr) {
                        continue;
                    }
                    return n;
                }
                assert(FD_ISSET(mFD, &writefds));
            } else {
                return n;
            }
        } else {
            n += ret;
        }
    }
    return n;
}

ssize_t Socket::read(char* buf, ssize_t count)
{
    ssize_t rc = 0;
    for ( ; ; ) {
        const ssize_t ret = recvSocket(mFD, buf + rc, count - rc);
        if (ret >= 0) {
            return ret;
        } else {
            const int err = socketErrno();
            if (err == kSocketEIntr) {
                // Signal interruption; try again.
                continue;
            } else if (err == kSocketEWouldBlock || err == kSocketEAgain) {
                // Blocking socket. Wait for it to free up.
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(mFD, &readfds);

                // TODO: set timeout? What do we do if we timeout?
                if (select(static_cast<int>(mFD) + 1, &readfds, nullptr, nullptr, nullptr) < 0) {
                    if (socketErrno() == kSocketEIntr) {
                        continue;
                    }
                    return rc;
                }
                assert(FD_ISSET(mFD, &readfds));
            } else {
                return rc;
            }
        }
    }
}

void Socket::swap(Socket& other) noexcept
{
    std::swap(other.mFD, mFD);
}

SocketStreambuf::SocketStreambuf() :
    SocketStreambuf(Socket())
{
}

SocketStreambuf::SocketStreambuf(Socket&& socket) :
    mSock(std::move(socket))
{
#if 0 // Ugh. We need C++14 libraries for this to work.
    static_assert(sPutBack < mInputBuffer.size(),
                  "We need enough size for putback and writing.");
#endif

    char *const outBase = mOutputBuffer.data();
    setp(outBase, outBase + mOutputBuffer.size());

    char *const inEnd = mInputBuffer.data() + mInputBuffer.size();
    setg(inEnd, inEnd, inEnd);
}

SocketStreambuf::~SocketStreambuf()
{
    pubsync();
}

bool SocketStreambuf::is_open() const noexcept
{
    return static_cast<bool>(mSock);
}

void SocketStreambuf::open(const std::string& filename, int port)
{
    mSock = Socket::as_client(filename, port);
}

bool SocketStreambuf::write_to_socket()
{
    const std::ptrdiff_t count = pptr() - pbase();
    const ssize_t n = mSock.write(pbase(), count);
    if (n != count) {
        return false;
    }
    pbump(-n);
    return true;
}

SocketStreambuf::int_type SocketStreambuf::overflow(int_type ch)
{
    if (mSock && !traits_type::eq_int_type(ch, traits_type::eof())) {
        if (!write_to_socket()) {
            return traits_type::eof();
        }
        assert(std::less<char*>()(pptr(), epptr()));
        traits_type::assign(*pptr(), traits_type::to_char_type(ch));
        pbump(1);
        return traits_type::not_eof(ch);
    }
    return traits_type::eof();
}

int SocketStreambuf::sync()
{
    return write_to_socket() ? 0 : 1;
}

SocketStreambuf::int_type SocketStreambuf::underflow()
{
    if (gptr() < egptr()) { // Buffer not exhausted.
        return traits_type::not_eof(*gptr());
    }

    if (!mSock) {
        return traits_type::eof();
    }

    char* const base = mInputBuffer.data();
    char* start = base;

    if (eback() == base) { // Not our first fill
        // Move the putback characters to the beginning of the buffer.
        std::memmove(base, egptr() - sPutBack, sPutBack);
        start += sPutBack;
    }

    const std::size_t count = mInputBuffer.size() - (start - base);
    const ssize_t rc = mSock.read(start, static_cast<ssize_t>(count));
    if (rc <= 0) {
        return traits_type::eof();
    }
    setg(base, start, start + rc);
    return traits_type::not_eof(*gptr());
}

} // namespace stats
} // namespace moonray

