// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

//
#include <moonray/rendering/mcrt_common/Util.h>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>   // SymFromAddr - the backtrace_symbols() equivalent
#else
#include <execinfo.h>  // backtrace
#include <sys/syscall.h>
#endif

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace moonray {
namespace mcrt_common {

void
threadSleep()
{
#if defined(_WIN32)
    // The UCRT has no usleep(), and Sleep() only has millisecond granularity.
    std::this_thread::sleep_for(std::chrono::microseconds(500));
#else
    usleep(500);
#endif
}

void
threadYield()
{
    std::this_thread::yield();
}

void
debugPrintThreadID(const char *contextString)
{
    if (!contextString) contextString = "-- Thread ID = ";
#if defined(_WIN32)
    // GetCurrentThreadId() is the Win32 equivalent of gettid(): a system-wide
    // unique id for the calling thread.
    const unsigned long tid = GetCurrentThreadId();

    // This printing is thread safe.
    std::printf("%s%lu\n", contextString, tid);
#else
    pid_t tid = syscall(SYS_gettid);

    // This printing is thread safe.
    std::printf("%s%d\n", contextString, tid);
#endif
    std::fflush(stdout);
}

void
debugPrintCallstack(const char *contextString)
{
    static std::mutex mutex;

    mutex.lock();

    if (!contextString) contextString = "-- Callstack:\n";
    std::printf("%s\n", contextString);

    const int MAX_BACKTRACE_SYMBOLS = 64;
    void *pointers[MAX_BACKTRACE_SYMBOLS];

#if defined(_WIN32)

    // CaptureStackBackTrace() is the backtrace() equivalent; DbgHelp resolves
    // the addresses to names (the backtrace_symbols() equivalent). The symbol
    // handler is per-process and is initialized on first use; we already hold
    // _mutex_ here, and DbgHelp is not thread safe anyway.
    const USHORT size = CaptureStackBackTrace(0, MAX_BACKTRACE_SYMBOLS, pointers, nullptr);

    const HANDLE process = GetCurrentProcess();
    static bool symInitialized = false;
    if (!symInitialized) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        symInitialized = (SymInitialize(process, nullptr, TRUE) != FALSE);
    }

    alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(symbolBuffer);

    char indent[MAX_BACKTRACE_SYMBOLS + 1];
    memset(indent, ' ', sizeof(indent));

    for (USHORT i = 0; i < size; ++i) {
        // Indent backtrace.
        indent[i] = 0;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (symInitialized &&
            SymFromAddr(process, reinterpret_cast<DWORD64>(pointers[i]), &displacement, symbol)) {
            std::printf("%s%s\n", indent, symbol->Name);
        } else {
            // No .pdb for this module: the raw address is still useful.
            std::printf("%s%p\n", indent, pointers[i]);
        }
        indent[i] = ' ';
    }

#else

    size_t size = backtrace(pointers, MAX_BACKTRACE_SYMBOLS);
    char **strings = backtrace_symbols(pointers, size);

    char indent[MAX_BACKTRACE_SYMBOLS + 1];
    memset(indent, ' ', sizeof(indent));

    int wrapperIndex = -1;

    // Iterate to size - 1 since final may not be null terminated.
    for(size_t i = 0; i < size - 1; ++i) {
        // Indent backtrace.
        indent[i - wrapperIndex - 1] = 0;
        char *p = strrchr(strings[i], '/');
        std::printf("%s%s\n", indent, p ? (p + 1) : strings[i]);
        indent[i - wrapperIndex - 1] = ' ';
    }

#endif

    std::printf("\n");
    std::fflush(stdout);

#if !defined(_WIN32)
    free(strings);
#endif

    mutex.unlock();
}

// Functions exposed to ISPC:
extern "C"
{

void
CPP_debugPrintThreadID()
{
    debugPrintThreadID(nullptr);
}

void
CPP_debugPrintCallstack()
{
    debugPrintCallstack(nullptr);
}

}

} // namespace mcrt_common
} // namespace moonray

