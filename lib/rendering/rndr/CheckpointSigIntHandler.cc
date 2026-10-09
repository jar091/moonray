// Copyright 2023-2025 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0
#include <moonray/rendering/rndr/CheckpointSigIntHandler.h>
#include <moonray/rendering/rndr/ImageWriteDriver.h>
#include <moonray/rendering/rndr/ProcKeeper.h>

#include <scene_rdl2/render/logging/logging.h>

#include <atomic>
#include <cstdlib> // EXIT_FAILURE
#include <cstring> // memset
#include <signal.h>
#include <string> // to_string
#ifdef _WIN32
#include <windows.h> // SetConsoleCtrlHandler
#endif

namespace moonray {
namespace rndr {

#ifdef _WIN32

//
// Windows has no sigaction()/SIGINT-with-siginfo. Its equivalent of "the queue
// system asked this process to stop" is a console control event, delivered by
// the OS on a dedicated thread to the handler registered with
// SetConsoleCtrlHandler(). CTRL_C_EVENT is the direct counterpart of SIGINT;
// the close/logoff/shutdown events are included because they are the other ways
// a batch render is asked to wind up, and they are the cases where writing the
// checkpoint before dying matters most.
//
static
BOOL WINAPI
checkpointConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        break;
    default:
        return FALSE; // not ours; pass it on to the next handler
    }

    ImageWriteDriver::SigInfo info;
    info.si_signo = static_cast<int>(ctrlType);
    ImageWriteDriver::get()->interruptBySignal(info);

    // Returning TRUE suppresses the default terminate action, which is what the
    // POSIX side achieves by installing a handler for SIGINT: the
    // ImageWriteDriver thread dumps the snapshot and then _exit()s itself.
    return TRUE;
}

#else // else of _WIN32

static
void
checkpointSigActionFunction(int /*sig*/, siginfo_t *info, void */*ctx*/)
{
    ImageWriteDriver::get()->interruptBySignal(*info);
}

#endif // end of not _WIN32

#ifdef TEST_SCENE_CONTEXT_DUMP
static
void
sceneContextDumpSigActionFunction(int sig, siginfo_t */*info*/, void */*ctx*/)
{
    auto sigStr = [](int sig) -> std::string {
        switch (sig) {
        case SIGABRT : return "SIGABRT";
        case SIGSEGV : return "SIGSEGV";
        case SIGILL : return "SIGILL";
        case SIGFPE : return "SIGFPE";
        default : return "?";
        }
    };

    std::cerr << ">> CheckpointSigIntHandler.cc sceneContextDumpSigActionFunction() sig:" << sigStr(sig) << '\n';

    ProcKeeper::get()->signalActionSceneContextDump();

    std::cerr << ">> CheckpointSigIntHandler.cc startSceneContextDump() end\n";
}
#endif // end TEST_SCENE_CONTEXT_DUMP

//-----------------------------------------------------------------------------------------

static std::atomic<bool> gCheckpointSigIntHandlerActionStarted(false);

#ifdef _WIN32
//
// The two APIs do not have the same contract, and the difference matters here.
// sigaction(SIGINT, SIG_DFL) is an idempotent "reset to the default action": it
// succeeds whether or not a handler was ever installed. SetConsoleCtrlHandler(h,
// FALSE) instead removes one specific entry from the process handler list and
// fails with ERROR_INVALID_PARAMETER when h is not in it.
//
// RenderContext::startFrame() depends on the POSIX behaviour - it calls
// disable() unconditionally on entry to render prep, with no matching enable()
// (the only enable() is in RenderFrame.cc, and only for PROGRESS_CHECKPOINT with
// memory snapshots active). Tracking the registration restores the idempotent
// contract, and also keeps a repeated enable() from stacking duplicate entries,
// which Windows would otherwise require a matching number of removals to undo.
//
static std::atomic<bool> gConsoleCtrlHandlerInstalled(false);
#endif // end of _WIN32

// static function
void
CheckpointSigIntHandler::enable()
//
// setup handler for SIGINT
//
{
#ifdef _WIN32
    bool notInstalled = false;
    if (gConsoleCtrlHandlerInstalled.compare_exchange_strong(notInstalled, true)) {
        if (!SetConsoleCtrlHandler(checkpointConsoleCtrlHandler, TRUE)) {
            const DWORD err = GetLastError();
            gConsoleCtrlHandlerInstalled.store(false);
            scene_rdl2::logging::Logger::fatal("setup new console control handler for checkpoint-dump "
                                               "failed. (GetLastError:" + std::to_string(err) + ')');
            exit(EXIT_FAILURE);
        }
    }
#else // else of _WIN32
    struct sigaction newSigIntAction;
    std::memset(&newSigIntAction, 0, sizeof(newSigIntAction));

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);

    newSigIntAction.sa_mask = block;
    newSigIntAction.sa_sigaction = checkpointSigActionFunction;
    newSigIntAction.sa_flags = SA_SIGINFO | SA_RESTART;

    if (sigaction(SIGINT, &newSigIntAction, NULL) < 0) {
        scene_rdl2::logging::Logger::fatal("setup new SIGINT handler for checkpoint-dump failed.");
        exit(EXIT_FAILURE);
    }
#endif // end of not _WIN32

    // We need tmp directory projection for signal-based checkpointing.
    // We have to create DO_NOT_TMP_CLEAR file in the tmp directory with the proper expiration timestamp.
    constexpr float maxDeltaTime = 15.0f; // Longest expiration duration to set for DO_NOT_TMP_CLEAR.
                                          // (unit is minutes).
    constexpr float minDeltaTime = 5.0f;  // Increase expiration duration if the current remaining is
                                          // less than this value. (unit is minutes)
    // constexpr float maxDeltaTime = 3.0f; // for test
    // constexpr float minDeltaTime = 1.5f; // for test
    // std::cerr << ">> CheckpointSigIntHandler.cc maxDeltaTime:" << maxDeltaTime << " minDeltaTime:" << minDeltaTime << '\n';
    if (!ImageWriteDriver::get()->openTmpDirControlFile(maxDeltaTime, minDeltaTime)) {
        scene_rdl2::logging::Logger::fatal("tmpDirControlFile open failed.");        
    }

    if (!ProcKeeper::get()->openWriteProgressFile()) {
        scene_rdl2::logging::Logger::fatal("writeProgressFile open failed.");
    }
}

// static function
void
CheckpointSigIntHandler::disable()
//
// fallback to default signal handler for SIGINT
//
{
    if (gCheckpointSigIntHandlerActionStarted.load(std::memory_order_relaxed)) {
        // checkpointSigIntHandler action has been started already.
        // We skip disabling.
        return;
    }

    std::string msg;
    if (!ProcKeeper::get()->closeWriteProgressFile(msg)) {
        scene_rdl2::logging::Logger::fatal("writeProgressFile close failed.");
    }
    if (!msg.empty()) scene_rdl2::logging::Logger::info(msg + '\n');

#ifdef _WIN32
    // Removing our handler restores the default console control behaviour
    // (terminate the process), which is the SIG_DFL equivalent here. Nothing to
    // remove when we never registered - see gConsoleCtrlHandlerInstalled above.
    bool installed = true;
    if (gConsoleCtrlHandlerInstalled.compare_exchange_strong(installed, false)) {
        if (!SetConsoleCtrlHandler(checkpointConsoleCtrlHandler, FALSE)) {
            const DWORD err = GetLastError();
            scene_rdl2::logging::Logger::fatal("fall back to default console control handler "
                                               "failed. (GetLastError:" + std::to_string(err) + ')');
            exit(EXIT_FAILURE);
        }
    }
#else // else of _WIN32
    struct sigaction newSigIntAction;
    std::memset(&newSigIntAction, 0, sizeof(newSigIntAction));

    newSigIntAction.sa_sigaction = reinterpret_cast<void (*)(int, siginfo_t *, void *)>(SIG_DFL);
    newSigIntAction.sa_flags = SA_SIGINFO;

    if (sigaction(SIGINT, &newSigIntAction, NULL) < 0) {
        scene_rdl2::logging::Logger::fatal("fall back to default SIGINT handler failed.");
        exit(EXIT_FAILURE);
    }
#endif // end of not _WIN32
}

// static function
void
CheckpointSigIntHandler::handlerActionStarted()
{
    gCheckpointSigIntHandlerActionStarted.store(true, std::memory_order_relaxed);
}

//------------------------------------------------------------------------------------------

#ifdef TEST_SCENE_CONTEXT_DUMP

static std::atomic<bool> gSceneContextDumpSigHandlerActionStarted = false;

// static function
void
SceneContextDumpSigHandler::enable()
//
// setup handler for SIGABRT, SIGSEGV, SIGILL, and SIGFPE
//
{
    std::cerr << ">> CheckpointSigIntHandler.cc SceneContextDumpSigHandler::enable() start\n";

    struct sigaction newSigIntAction;
    std::memset(&newSigIntAction, 0, sizeof(newSigIntAction));

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGABRT);
    sigaddset(&block, SIGSEGV);
    sigaddset(&block, SIGILL);
    sigaddset(&block, SIGFPE);

    newSigIntAction.sa_mask = block;
    newSigIntAction.sa_sigaction = sceneContextDumpSigActionFunction;
    newSigIntAction.sa_flags = SA_SIGINFO | SA_RESTART;

    if (sigaction(SIGABRT, &newSigIntAction, NULL) < 0 ||
        sigaction(SIGSEGV, &newSigIntAction, NULL) < 0 ||
        sigaction(SIGILL, &newSigIntAction, NULL) < 0 ||
        sigaction(SIGFPE, &newSigIntAction, NULL) < 0) {
        scene_rdl2::logging::Logger::fatal("setup new SIGABRT,SIGSEGV,SIGILL, and SIGFPE handler "
                                        "for sceneContext-dump failed.");
        exit(EXIT_FAILURE);
    }
    std::cerr << ">> CheckpointSigIntHandler.cc SceneContextDumpSigHandler::enable() done\n";
}

// static function
void
SceneContextDumpSigHandler::disable()
//
// fallback to default signal handler for SIGABRT, SIGSEGV, SIGILL, and SIGFPE
//
{
    std::cerr << ">> CheckpointSigIntHandler.cc SceneContextDumpSigHandler::disable() start\n";

    if (gSceneContextDumpSigHandlerActionStarted.load(std::memory_order_relaxed)) {
        // sceneContextDumpSigHandler action has been started already.
        // We skip disabling.
        return;
    }

    struct sigaction newSigIntAction;
    std::memset(&newSigIntAction, 0, sizeof(newSigIntAction));

    newSigIntAction.sa_sigaction = reinterpret_cast<void (*)(int, siginfo_t *, void *)>(SIG_DFL);
    newSigIntAction.sa_flags = SA_SIGINFO;
    
    if (sigaction(SIGABRT, &newSigIntAction, NULL) < 0 ||
        sigaction(SIGSEGV, &newSigIntAction, NULL) < 0 ||
        sigaction(SIGILL, &newSigIntAction, NULL) < 0 ||
        sigaction(SIGFPE, &newSigIntAction, NULL) < 0) {
        scene_rdl2::logging::Logger::fatal("fall back to default handler failed for "
                                        "SIGABRT, SIGSEGV, SIGILL, and SIGFPE.");
        exit(EXIT_FAILURE);
    }

    std::cerr << ">> CheckpointSigIntHandler.cc SceneContextDumpSigHandler::disable() done\n";
}

// static function
void
SceneContextDumpSigHandler::handlerActionStarted()
{
    gSceneContextDumpSigHandlerActionStarted.store(true, std::memory_order_relaxed);
}

#endif // end TEST_SCENE_CONTEXT_DUMP

} // namespace rndr
} // namespace moonray

