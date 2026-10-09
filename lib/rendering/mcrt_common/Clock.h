// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

/// @file Clock.h
#pragma once

#include <time.h>

#if defined(_WIN32)
#include <cstdint>
#include <windows.h>
#endif

namespace moonray {
namespace mcrt_common {

#if defined(_WIN32)

// The UCRT declares struct timespec but none of POSIX's clock ids, and it has
// no clock_gettime(). These are namespace-local, so they neither collide with
// nor leak into anything else; ordinary lookup from Clock below finds them on
// Windows and the POSIX ones everywhere else.
//
// Each id has an exact Win32 equivalent: thread and process CPU time are
// GetThreadTimes()/GetProcessTimes() (kernel + user, in 100 ns ticks, which is
// what CLOCK_*_CPUTIME_ID measures), and monotonic time is
// QueryPerformanceCounter().
using clockid_t = int;

enum : clockid_t
{
    CLOCK_REALTIME           = 0,
    CLOCK_MONOTONIC          = 1,
    CLOCK_PROCESS_CPUTIME_ID = 2,
    CLOCK_THREAD_CPUTIME_ID  = 3
};

inline int clock_gettime(clockid_t clkId, struct timespec* ts)
{
    FILETIME creation, exit, kernel, user;

    switch (clkId) {
    case CLOCK_THREAD_CPUTIME_ID:
        if (!::GetThreadTimes(::GetCurrentThread(), &creation, &exit, &kernel, &user)) {
            return -1;
        }
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
        if (!::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
            return -1;
        }
        break;
    default: {
        LARGE_INTEGER frequency, counter;
        if (!::QueryPerformanceFrequency(&frequency) || !::QueryPerformanceCounter(&counter)) {
            return -1;
        }
        ts->tv_sec  = static_cast<time_t>(counter.QuadPart / frequency.QuadPart);
        ts->tv_nsec = static_cast<long>(((counter.QuadPart % frequency.QuadPart) * 1000000000LL) /
                                        frequency.QuadPart);
        return 0;
    }
    }

    const std::uint64_t kernel100ns =
        (static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    const std::uint64_t user100ns =
        (static_cast<std::uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    const std::uint64_t total100ns = kernel100ns + user100ns;

    ts->tv_sec  = static_cast<time_t>(total100ns / 10000000ull);
    ts->tv_nsec = static_cast<long>((total100ns % 10000000ull) * 100ull);
    return 0;
}

#endif // _WIN32

/// @brief an RAII interface to unix clock_gettime()
/// Usage:
/// \code
///    int64_t nsec = 0;
///    {
///        Clock(&nsec);
///        ...
///    }
///    double secs = Clock::seconds(nsec);
/// \endcode

class Clock
{
public:
    // Clock counts number of nanoseconds, convert to seconds.
    __forceinline static double seconds(int64_t nsecs)
    {
        return static_cast<double>(nsecs) / NSPERSEC;
    }

    // Seconds convert to clock counts number of nanoseconds
    __forceinline static int64_t nanoseconds(double sec)
    {
        return static_cast<int64_t>(sec * NSPERSEC);
    }

    __forceinline Clock(int64_t *stat,
                        bool startNow = true,
                        clockid_t clkId = CLOCK_THREAD_CPUTIME_ID):
        mStat(stat),
        mClkId(clkId),
        mStopped(true)
    {
        if (startNow) {
            start();
        }
    }

    __forceinline ~Clock()
    {
        stop();
    }

    __forceinline void start()
    {
        if (mStat && mStopped) {
            clock_gettime(mClkId, &mStart);
            mStopped = false;
        }
    }

    __forceinline void stop()
    {
        if (mStat && !mStopped) {
            struct timespec end;
            clock_gettime(mClkId, &end);
            const int64_t sdiff = end.tv_sec - mStart.tv_sec;
            const int64_t nsdiff = end.tv_nsec - mStart.tv_nsec;
            *mStat += sdiff * NSPERSEC + nsdiff;
            mStopped = true;
        }
    }

private:
    static const int64_t NSPERSEC = 1000000000;
    int64_t *mStat; // inactive if null
    clockid_t mClkId;
    struct timespec mStart;
    bool mStopped;
};

#define MCRT_COMMON_CLOCK_OPEN(STAT) { mcrt_common::Clock clock((STAT));

#define MCRT_COMMON_CLOCK_CLOSE() }
        
} // namespace mcrt_common
} // namespace moonray

