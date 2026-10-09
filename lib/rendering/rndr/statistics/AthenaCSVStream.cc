// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include <moonray/rendering/rndr/statistics/AthenaCSVStream.h>
#include <moonray/rendering/rndr/Error.h>
#include <scene_rdl2/render/util/GetEnv.h>

#include <array>
#include <cerrno>
#include <climits> // HOST_NAME_MAX
#include <cstring> //strerror_r
#include <ctime> //strftime & localtime_r
#include <iostream>
#include <sstream>
#ifdef _WIN32
// Winsock2 is the Windows spelling of the same BSD socket API. initSocketLayer()
// is the WSAStartup() wrapper this module already owns.
#include <moonray/rendering/rndr/statistics/SocketStream.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h> // GetComputerNameA
#else
#include <arpa/inet.h>
#include <netdb.h> //getservbyname
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h> //gethostname
#endif

#ifndef HOST_NAME_MAX
# ifdef _POSIX_HOST_NAME_MAX
#  define HOST_NAME_MAX _POSIX_HOST_NAME_MAX
# else
#  define HOST_NAME_MAX 255
# endif
#endif

#ifdef _WIN32
static_assert(sizeof(SOCKET) == sizeof(std::uintptr_t),
              "AthenaCSVStreamBuf::mSocket stands in for a Winsock SOCKET");
// Winsock has no SOCK_NONBLOCK socket() flag; non-blocking mode is a separate
// ioctlsocket(FIONBIO) call after the socket exists (see open() below).
#else
#ifndef SOCK_NONBLOCK
#include <fcntl.h>
#define SOCK_NONBLOCK O_NONBLOCK
#endif
#endif

namespace {
constexpr unsigned int LOG_USER = 1;
constexpr unsigned int LOG_INFO = 6;
constexpr unsigned int SYSLOG_PRIO = (LOG_USER << 3) | LOG_INFO;
}

namespace moonray {
namespace stats {

AthenaCSVStreamBuf::AthenaCSVStreamBuf(const scene_rdl2::util::GUID& guid, bool debug) :
        mGUID(guid.asString()),
        mHostname(getLocalHostName()),
        mStorage(),
        mDwaGacId{scene_rdl2::util::getenv<std::string>("DWA_GACID")},
#ifdef _WIN32
        mSocket{static_cast<std::uintptr_t>(INVALID_SOCKET)},
#else
        mSocket{-1},
#endif
        mOpened(false)
{
    open(debug);
}

AthenaCSVStreamBuf::~AthenaCSVStreamBuf()
{
    if (mOpened) {
#ifdef _WIN32
        ::shutdown(static_cast<SOCKET>(mSocket), SD_BOTH);
        // Unlike a POSIX fd, a Winsock handle is not reclaimed by shutdown().
        ::closesocket(static_cast<SOCKET>(mSocket));
#else
        ::shutdown(mSocket, SHUT_RDWR);
#endif
    }
}

void
AthenaCSVStreamBuf::open(bool debug)
{
    // From the Athena wiki:
    // The "athena." identifier followed by your producer or application name,
    // e.g.: "athena.moonray.stats"
    const char* athena_development = scene_rdl2::util::getenv<const char*>("MNRY_ATHENA_STATS_DEV");
    if (debug || athena_development != nullptr) {
        mIdent = std::string("dev-athena.moonray_stats");
    } else {
        mIdent = std::string("prod-athena.moonray_stats");
    }

#ifdef _WIN32
    initSocketLayer();
#endif

    const auto serviceEntry = ::getservbyname("syslog", "udp");
#ifndef _WIN32
    ::endservent(); // no Winsock counterpart: getservbyname() keeps no open handle
#endif

    if (serviceEntry == nullptr) {
        std::cerr << "Failed to lookup syslog from services database" << std::endl;
        return;
    }

#ifdef _WIN32
    const SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Failed to create socket due to: " << rndr::getErrorDescription() << std::endl;
        return;
    }
    u_long nonBlocking = 1;
    if (::ioctlsocket(sock, FIONBIO, &nonBlocking) != 0) { // == SOCK_NONBLOCK above
        std::cerr << "Failed to set socket non-blocking due to: "
                  << rndr::getErrorDescription() << std::endl;
        ::closesocket(sock);
        return;
    }
    mSocket = static_cast<std::uintptr_t>(sock);
#else
    mSocket = ::socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK, 0);
    if (mSocket == -1) {
        std::cerr << "Failed to create socket due to: " << rndr::getErrorDescription() << std::endl;
        return;
    }
#endif

    struct sockaddr_in addr;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_family = AF_INET;
    addr.sin_port = htons(serviceEntry->s_port);
#ifdef _WIN32
    if (::connect(static_cast<SOCKET>(mSocket), (struct sockaddr*)&addr, sizeof(addr)) == 0) {
#else
    if (::connect(mSocket, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
#endif
        mOpened = true;
    } else {
        std::cerr << "Failed to connect to socket due to: " << rndr::getErrorDescription() << std::endl;
    }
}

std::streambuf::int_type 
AthenaCSVStreamBuf::overflow(int_type ch)
{
    if (!traits_type::eq_int_type(ch, traits_type::eof())) {
        if (ch != traits_type::to_int_type('\n')) {
            mStorage.push_back(traits_type::to_char_type(ch));
        } else {
            if (!mStorage.empty()) {
                appendBuffer(mHostname.c_str(), mHostname.length());
                appendBuffer(',');
                appendBuffer(mGUID.c_str(), mGUID.length());
                appendBuffer(',');
                appendTimestamp();
                appendBuffer(',');
                appendBuffer(mStorage.data(), mStorage.size());
                appendBuffer('\0');  // string must be null terminated
                flushBuffer();
                mStorage.clear();
            }
        }
        return traits_type::not_eof(ch);
    }
    return traits_type::eof();
}

std::chrono::milliseconds::rep 
AthenaCSVStreamBuf::msSinceEpoch()
{
    const auto p = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(p.time_since_epoch()).count();
}

void
AthenaCSVStreamBuf::appendTimestamp()
{
    // digits10 is rounded down, because it represents the
    // number of digits that can be stored without loss of
    // information, so add one.
    constexpr auto digits = std::numeric_limits<MSType>::digits10 + 1;

    // Add one for terminator.
    char timestampString[digits + 1] = { 0 };

    const auto ms = msSinceEpoch();
#ifdef _WIN32
    // long is 32-bit on LLP64, so the millisecond count needs the long long
    // conversion here (same class of fix as scene_rdl2 change 7).
    static_assert(sizeof(MSType) == sizeof(long long),
                    "We're using the long long conversion for sprintf");
    std::snprintf(timestampString, digits + 1, "%lld", static_cast<long long>(ms));
#else
    static_assert(sizeof(MSType) == sizeof(long),
                    "We're using the long conversion for sprintf");
    std::snprintf(timestampString, digits + 1, "%ld", ms);
#endif
    appendBuffer(timestampString, std::strlen(timestampString));
}

void
AthenaCSVStreamBuf::flushBuffer()
{
    if (mSyslogBuffer.size() <= 0) return;

    // TIMESTAMP
    char timestamp[256];
    time_t now = time(0);
    struct tm tmnow;
#ifdef _WIN32
    localtime_s(&tmnow, &now); // same call, arguments the other way round
#else
    localtime_r(&now, &tmnow);
#endif
    // Note timestamp must be in this old format, ISO standard format isn't accepted
    //rfc5424: strftime(timestamp, 256, "%FT%T%z", &tmnow);
    strftime(timestamp, 256, "%b %e %H:%M:%S", &tmnow);
    
    std::ostringstream msg;
    msg << "<" << SYSLOG_PRIO << "> "
        << timestamp << " "
        << mHostname << " "
        << mIdent << ": "
        << mDwaGacId << "," << mSyslogBuffer.data();

    std::string s = msg.str();
#ifdef _WIN32
    if (::send(static_cast<SOCKET>(mSocket), s.c_str(), static_cast<int>(s.size()), 0) == SOCKET_ERROR) {
#else
    if (::send(mSocket, s.c_str(), s.size(), 0) == -1) {
#endif
        std::cerr << "Failed to send log message to Athena due to: "
                  << rndr::getErrorDescription()
                  << " : " << s << std::endl;
    }

    mSyslogBuffer.clear();
}

std::string 
AthenaCSVStreamBuf::getLocalHostName()
{
#ifdef _WIN32
    // The Win32 computer name is the same machine identifier and, unlike
    // Winsock's gethostname(), needs no WSAStartup() to have happened first.
    char hostname[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD hostnameSize = static_cast<DWORD>(sizeof(hostname));
    if (!GetComputerNameA(hostname, &hostnameSize)) return std::string();
    return std::string(hostname, hostnameSize);
#else
    char hostname[HOST_NAME_MAX];
    gethostname(hostname, HOST_NAME_MAX);
    return std::string(hostname);
#endif
}

} // namespace stats
} // namespace moonray

