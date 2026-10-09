// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

//

#include <moonray/common/mcrt_util/ProcessStats.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>    // GetProcessMemoryInfo
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#endif

namespace moonray {

namespace util {

int64
ProcessUtilization::getUserSeconds(const ProcessUtilization &start) const
{
#ifdef __APPLE__
    return (this->userTime - start.userTime) / 1E9; // Nano to Sec
#elif defined(_WIN32)
    return (this->userTime - start.userTime) / 10000000; // 100ns ticks to Sec
#else
    return (this->userTime - start.userTime) / sysconf(_SC_CLK_TCK);
#endif
}

int64
ProcessUtilization::getSystemSeconds(const ProcessUtilization &start) const
{
#ifdef __APPLE__
    return (this->systemTime - start.systemTime) / 1E9; // Nano to Sec
#elif defined(_WIN32)
    return (this->systemTime - start.systemTime) / 10000000; // 100ns ticks to Sec
#else
    return (this->systemTime - start.systemTime) / sysconf(_SC_CLK_TCK);
#endif
}

ProcessStats::ProcessStats()
{
#if defined(__APPLE__) || defined(_WIN32)
    // Don't need to try to open /proc
#else
    mStatmFile.open("/proc/self/statm");
    mIOStatFile.open("/proc/self/io");
    mProcStatFile.open("/proc/self/stat");
#endif
}


int64
ProcessStats::getBytesRead() const
{
    std::string inStr;
    int64  bytesRead = 0;

#ifdef __APPLE__

    // TODO: figure a way to gets the process bytes read

#elif defined(_WIN32)

    // IO_COUNTERS::ReadTransferCount is the number of bytes transferred by
    // this process' read operations, which is what /proc/self/io's first
    // field (rchar) reports.
    mReadIOMutex.lock();
    IO_COUNTERS io;
    if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
        bytesRead = static_cast<int64>(io.ReadTransferCount);
    }
    mReadIOMutex.unlock();

#else
    mReadIOMutex.lock();
    if(mIOStatFile.good()) {
        mIOStatFile >> inStr >> bytesRead;
        //reset file pointer for later reads
        mIOStatFile.seekg(std::ios_base::beg);
    } else {
        //  sometimes we lose the file handle so
        //  close it and try to reopen for the next query.
        mIOStatFile.close();
        mIOStatFile.open("/proc/self/io");
    }
    mReadIOMutex.unlock();
#endif

    return bytesRead;

}

int64
ProcessStats::getProcessMemory() const
{
    int64 currentMemoryUsage = 0;

#ifdef __APPLE__

    pid_t pid;
    struct proc_taskinfo info;
    int size;

    mMemoryReadMutex.lock();
    pid = getpid();

    size = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info));
    if (size == PROC_PIDTASKINFO_SIZE) {
        currentMemoryUsage = info.pti_resident_size; // Already in bytes
    }
    mMemoryReadMutex.unlock();

#elif defined(_WIN32)

    // WorkingSetSize is the resident set of the process, already in bytes,
    // i.e. the equivalent of statm's resident pages * _SC_PAGE_SIZE.
    mMemoryReadMutex.lock();
    PROCESS_MEMORY_COUNTERS pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        currentMemoryUsage = static_cast<int64>(pmc.WorkingSetSize);
    }
    mMemoryReadMutex.unlock();

#else

    mMemoryReadMutex.lock();
    if(mStatmFile.good()) {
        int64 memorySize = 0;
        int64 memoryResident = 0;
        int64 memoryShared = 0;
        //sometimes the OS is writing to the file
        try {
            mStatmFile >> memorySize >> memoryResident >> memoryShared;
        } catch( std::ios_base::failure f ) {
            scene_rdl2::logging::Logger::error(f.what());
        }
        currentMemoryUsage = memoryResident * sysconf(_SC_PAGE_SIZE);
        //reset file pointer for later reads
        mStatmFile.seekg(std::ios_base::beg);
    } else {
        //  sometimes we lose the file handle so
        //  close it and try to reopen for the next query.
        mStatmFile.close();
        mStatmFile.open("/proc/self/statm");
    }
    mMemoryReadMutex.unlock();

#endif
    return currentMemoryUsage;
}


ProcessUtilization
ProcessStats::getProcessUtilization() const
{

    ProcessUtilization result;
    std::string inStr;

#ifdef __APPLE__

    pid_t pid;
    struct proc_taskinfo info;
    int size;
    mach_timebase_info_data_t timebase_info = {0};
        
    mach_timebase_info(&timebase_info);

    mSystemUtilMutex.lock();
    pid = getpid();

    size = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info));
    if (size == PROC_PIDTASKINFO_SIZE) {
        result.userTime = (info.pti_total_user) * timebase_info.numer / timebase_info.denom; // Nanosecs
        result.systemTime = (info.pti_total_system) * timebase_info.numer / timebase_info.denom; // Nanosecs
    }
    mSystemUtilMutex.unlock();

#elif defined(_WIN32)

    // GetProcessTimes() reports kernel/user time as FILETIMEs holding a count
    // of 100-nanosecond ticks (not a date), which is the Windows analogue of
    // /proc/self/stat fields 14 and 15 (clock ticks). getUserSeconds() /
    // getSystemSeconds() divide by the matching 1e7 instead of _SC_CLK_TCK.
    result.userTime = 0;
    result.systemTime = 0;

    mSystemUtilMutex.lock();
    FILETIME creationTime, exitTime, kernelTime, userTime;
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)) {
        ULARGE_INTEGER u, s;
        u.LowPart  = userTime.dwLowDateTime;
        u.HighPart = userTime.dwHighDateTime;
        s.LowPart  = kernelTime.dwLowDateTime;
        s.HighPart = kernelTime.dwHighDateTime;
        result.userTime   = static_cast<int64>(u.QuadPart);
        result.systemTime = static_cast<int64>(s.QuadPart);
    }
    mSystemUtilMutex.unlock();

#else

    mSystemUtilMutex.lock();
    if(mProcStatFile.good()) {
        mProcStatFile >> inStr;  //1
        mProcStatFile >> inStr;  //2
        mProcStatFile >> inStr;  //3
        mProcStatFile >> inStr;  //4
        mProcStatFile >> inStr;  //5
        mProcStatFile >> inStr;  //6
        mProcStatFile >> inStr;  //7
        mProcStatFile >> inStr;  //8
        mProcStatFile >> inStr;  //9
        mProcStatFile >> inStr;  //10
        mProcStatFile >> inStr;  //11
        mProcStatFile >> inStr;  //12
        mProcStatFile >> inStr;  //13
        mProcStatFile >> result.userTime;  //14
        mProcStatFile >> result.systemTime;  //15

        //reset file pointer for later reads
        mProcStatFile.seekg(std::ios_base::beg);
    } else {
        //  sometimes we lose the file handle so
        //  close it and try to reopen for the next query.
        mProcStatFile.close();
        mProcStatFile.open("/proc/self/stat");
    }
    mSystemUtilMutex.unlock();
#endif

    return result;

}


} //util
} //moonray


