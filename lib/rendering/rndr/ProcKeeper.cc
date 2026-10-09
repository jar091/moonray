// Copyright 2023-2025 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0
#include <moonray/rendering/rndr/ImageWriteCache.h>
#include <moonray/rendering/rndr/ProcKeeper.h>

#include <moonray/common/mcrt_macros/moonray_static_check.h>
#include <moonray/rendering/mcrt_common/Util.h>
#ifdef _WIN32
// Put back the empty `far` / `near` macros scene_rdl2's DebugLog.h #undefs:
// <winsock2.h>, reached through TimeUtil.h, still spells its pointers `FAR *`.
#define far
#define near
#endif
#include <scene_rdl2/render/util/TimeUtil.h>

#include <iostream>
#include <sstream>
#include <thread>      // sleep_for

#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <filesystem>  // temp_directory_path()
#include <io.h>        // _open(), _close(), _write(), _commit(), _unlink()
#include <process.h>   // _getpid()
#else
#include <sys/types.h> // getpid()
#include <unistd.h>    // usleep(), getpid()
#endif

// Enable debug messages regarding thread boot and write progress file information.
//#define DEBUG_MSG

// Enable a test code for SceneContext dump action by the signal for MOONRAY-4417.
// Keep this code until we mainly start working for MOONRAY-4417.
// There is similar definition in side CheckpointSigIntHandler.h. Please check them.
//#define TEST_SCENE_CONTEXT_DUMP

// Definition of the filename prefix used for writing action progress update.
// Windows has no /tmp; the per-user temporary directory is its equivalent and is
// resolved at run time (see setupImageWriteProgressFilename()).
#define WRITE_PROGRESS_UPDATE_FILE_NAME "moonray_write."
#ifdef _WIN32
#define WRITE_PROGRESS_UPDATE_FILE_DIR (std::filesystem::temp_directory_path().string() + "/")
#else
#define WRITE_PROGRESS_UPDATE_FILE_DIR "/tmp/"
#endif

#ifdef _WIN32
// The Windows spellings of the POSIX libc calls used below. _commit() is the
// _open()-descriptor form of fsync(): it flushes the file's buffers to disk.
#define open   _open
#define close  _close
#define write  _write
#define unlink _unlink
#define fsync  _commit
#endif

namespace moonray {
namespace rndr {
    
ProcKeeper::ProcKeeper() :
    mThreadState(ThreadState::INIT),
    mRunState(RunState::WAIT),
    mThreadShutdown(false),
    mImageWriteProgress(false),
    mImageWriteCache(nullptr),
    mImageWriteProgressFd(-1),
    mSceneContextDump(false)
{
    setupImageWriteProgressFilename();

    // We have to build thread after finish mMutex and mCvBoot initialization completed
    mThread = std::move(std::thread(threadMain, this));

    // Wait until main thread is booted
    //
    // Looks like this condition wait version does not work on tin machine occasionally somehow.
    // We can not repro same trouble on pearl so far. Probably, this issue is related to threadMain stops
    // with condition wait immediately after boot. I changed to a naive version which is just checking
    // atomic variable with short thread sleep interval instead.
    // This code will be refactored when working regarding MOONRAY-4417. (Toshi Mar/25/2022)
    /*
    std::unique_lock<std::mutex> uqLock(mMutexBoot);
    mCvBoot.wait(uqLock,
                 [&]{
                     return (mThreadState != ThreadState::INIT); // Not wait if already non INIT condition
                 });
    */
    while (mThreadState.load() == ThreadState::INIT) {
        mcrt_common::threadSleep();
    }
}

ProcKeeper::~ProcKeeper()
{
    mThreadShutdown = true; // This is the only place mThreadShutdown is set to true

    mRunState = RunState::START;
    mCvRun.notify_one(); // notify to ProcKeeper threadMain loop

    if (mThread.joinable()) {
        mThread.join();
    }

    std::string msg;
    closeWriteProgressFile(msg);
    if (!msg.empty()) scene_rdl2::logging::Logger::info(msg);
}

bool
ProcKeeper::openWriteProgressFile()
{
    mImageWriteProgress = openWriteProgressFileMain();
    return mImageWriteProgress;
}

bool
ProcKeeper::closeWriteProgressFile(std::string& msg)
{
    msg.clear();
    
    if (mImageWriteProgressFd == -1) return true;

    if (::close(mImageWriteProgressFd) == -1) return false;
    mImageWriteProgressFd = -1;

    if (::unlink(mImageWriteProgressFilename.c_str()) == -1) {
        return false;
    }

    msg = mImageWriteProgressFilename + " was unlinked";

    return true;
}

void
ProcKeeper::startImageWrite(const ImageWriteCache *imageWriteCache)
{
    if (!mImageWriteProgress) return;
    
    mImageWriteCache = imageWriteCache;
    mMainTaskCancel = false;

    mRunState = RunState::START;
    mCvRun.notify_one(); // notify to ProcKeeper threadMain loop
}

void
ProcKeeper::finishImageWrite()
// busy wait blocking function
{
    if (mImageWriteProgress) {
        mMainTaskCancel = true;

        while (!mThreadShutdown && mRunState != RunState::WAIT) {
            std::this_thread::sleep_for(std::chrono::microseconds(500)); // 0.5ms
        }
    }
    mImageWriteCache = nullptr;
}

void
ProcKeeper::signalActionSceneContextDump()
// Request to execute sceneContext dump
{
    if (mSceneContextDump) {
        return; // We already start dumping sceneContext and avoid duplicate execution
    }
    mSceneContextDump = true;

    mRunState = RunState::START;
    mCvRun.notify_one(); // notify to ProcKeeper threadMain loop
}

//------------------------------------------------------------------------------------------

// static function
void
ProcKeeper::threadMain(ProcKeeper *keeper)
//
// ProcKeeper main thread. Basic idea is that this thread is booted at beginning of moonray
// process and keeps watching the entire moonray execution.
// At this moment, the main task of this thread is updating write-progress file but we plan
// to implement sceneContext dump by signal action near future.
//
// This thread is basically slept by condition wait until 1 of the following 2 situations happens.
// So performance-wise, it is pretty small impact to the CPU resources.
//    1) write-progress update action
//    2) sceneContext dump action.
// In the case of #1, after finishing the close write-progress file, the thread is back to sleep condition.
// However, case #2 always terminate the process and never returns.
//
{
    // First, change keeper's threadState condition and do notify_one to caller.
    keeper->mThreadState = ThreadState::IDLE;

    // This code will be refactored when working regarding MOONRAY-4417.
    // (MOONRAY-4417: Investigation of sceneContext logger for the crash of mcrt_computation.
    // This is a crash dump of current sceneContext when crash).
    // See comment of ProcKeeper constructor comments.
    // keeper->mCvBoot.notify_one(); // notify to ProcKeeper's constructor.

#   ifdef DEBUG_MSG
    std::cerr << ">> ProcKeeper.cc threadMain() booted\n";
#   endif // end DEBUG_MSG

    while (true) {
        {
            std::unique_lock<std::mutex> uqLock(keeper->mMutexRun);
            keeper->mCvRun.wait(uqLock, [&]{
                    return (keeper->mRunState == RunState::START); // Not wait if state is START
                });
        }

        if (keeper->mThreadShutdown) { // before exit test
            break;
        }

        keeper->mThreadState = ThreadState::BUSY;
        keeper->main(); // main function of procKeeper thread
        keeper->mThreadState = ThreadState::IDLE;
        keeper->mRunState = RunState::WAIT;

        if (keeper->mThreadShutdown) { // after exit test
            break;
        }
    }

    keeper->mThreadState = ThreadState::DONE;

    // This code will be refactored when working regarding MOONRAY-4417.
    // See comment of ProcKeeper constructor comments.
    // keeper->mCvBoot.notify_one();

#   ifdef DEBUG_MSG
    std::cerr << ">> ProcKeeper.cc threadMain() shutdown\n";
#   endif // end DEBUG_MSG
}

//------------------------------------------------------------------------------------------

void
ProcKeeper::main()
//
// main logic when the thread is awaking
//    
{
#   ifdef DEBUG_MSG
    std::cerr << ">> ProcKeeper.cc main() imageWriteCache:0x" << std::hex << (uintptr_t)mImageWriteCache
              << " start\n";
#   endif // end DEBUG_MSG

    if (mSceneContextDump) {
        signalActionSceneContextDumpMain(); // this function never return
    }

    std::ostringstream ostr;
    ostr << scene_rdl2::time_util::currentTimeStr() << " image write start >";
    updateWriteProgressFile(ostr.str());

    unsigned prevCounter = mImageWriteCache->getProgressCounter();
    scene_rdl2::rec_time::RecTime updateTime, wholeTime;
    wholeTime.start();
    updateTime.start();

    // If the ImageWriteCache progress counter is not changed more than maxIntervalSec interval,
    // we assume the write action hang-up and stop updating the write-progress file.
    // 30 sec silence may be reasonably working to detect the problem but might be updated in the future.
    static constexpr float maxIntervalSec = 30.0f; // sec

    // We guarantee the write-progress file is updated every sleepIntervalSec interval as long as
    // we seem to write action is properly ongoing.
    static constexpr float sleepIntervalSec = 0.25f; // sec

    // This is the main loop of the write-progress-file update action until close write-progress file.
    // And also handles sceneContext dump requests.
    while (true) {
        if (mThreadShutdown || mMainTaskCancel) {
            break; // thread is shut down or the write-progress file was closed.
        }
        if (mSceneContextDump) {
            signalActionSceneContextDumpMain(); // this function never return
        }

        unsigned currCounter = mImageWriteCache->getProgressCounter();
        if (prevCounter != currCounter) {
            prevCounter = currCounter;
            updateTime.start();
        }

        if (updateTime.end() < maxIntervalSec) {
            updateWriteProgressFile(progressSymbol(mImageWriteCache));
        }

        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<int>(sleepIntervalSec * 1000000.0f)));
    }

    ostr.str("");
    ostr << "< done " << scene_rdl2::time_util::currentTimeStr() << " " << wholeTime.end() << " sec\n";
    updateWriteProgressFile(ostr.str());
}

void
ProcKeeper::setupImageWriteProgressFilename()
{
    std::ostringstream ostr;
#ifdef _WIN32
    const int myPid = _getpid();
#else
    const pid_t myPid = getpid();
#endif
    ostr << WRITE_PROGRESS_UPDATE_FILE_DIR << WRITE_PROGRESS_UPDATE_FILE_NAME
         << static_cast<unsigned>(myPid) << ".log";

    mImageWriteProgressFilename = ostr.str();

#   ifdef DEBUG_MSG
    std::cerr << ">> ProcKeeper.cc mImageWriteProgressFilename:" << mImageWriteProgressFilename << '\n';
#   endif // end DEBUG_MSG
}

bool
ProcKeeper::openWriteProgressFileMain()
{
    auto logMessage = [](const std::string& filename) {
        std::ostringstream ostr;
        ostr << "imageWriteProgressFile:" << filename << " was created";
        scene_rdl2::logging::Logger::info(ostr.str());
    };

#ifdef _WIN32
    mImageWriteProgressFd = ::open(mImageWriteProgressFilename.c_str(),
                                   _O_WRONLY | _O_CREAT, _S_IREAD);
#else
    mImageWriteProgressFd = ::open(mImageWriteProgressFilename.c_str(), O_WRONLY | O_CREAT, S_IREAD);
#endif
    if (mImageWriteProgressFd == -1) {
        return false;
    }

    logMessage(mImageWriteProgressFilename);

    std::ostringstream ostr;
    ostr << "procWatcher start " << scene_rdl2::time_util::currentTimeStr();

    return updateWriteProgressFile(ostr.str() + '\n');
}

bool
ProcKeeper::updateWriteProgressFile(const std::string &str) const
{
    if (mImageWriteProgressFd == -1) return true;

    if (::write(mImageWriteProgressFd, static_cast<const void *>(str.c_str()), str.size()) == -1) {
        return false;
    }
    if (::fsync(mImageWriteProgressFd) == -1) return false;
    return true;
}

// static function
std::string
ProcKeeper::runStateStr(const RunState &state)
{
    switch (state) {
    case RunState::WAIT : return "WAIT";
    case RunState::START : return "START";
    default : return "?";
    }
}

// static function
std::string
ProcKeeper::progressSymbol(const ImageWriteCache *cache)
{
    switch (cache->getProgressStage()) {
    case ImageWriteCache::ProgressStage::INIT  : return "I";
    case ImageWriteCache::ProgressStage::WRITE : return "w";
    case ImageWriteCache::ProgressStage::FILE  : return "f";
    case ImageWriteCache::ProgressStage::IMAGE : return ".";
    case ImageWriteCache::ProgressStage::BUFF0 : return "+";
    case ImageWriteCache::ProgressStage::BUFF1 : return "0";
    case ImageWriteCache::ProgressStage::BUFF2 : {
        int i = cache->getProgressBuffFraction();
        if (i < 10) return std::to_string(i);
        return "*";
    }
    default : return "?";
    }
}

//------------------------------------------------------------------------------------------
    
void
ProcKeeper::signalActionSceneContextDumpMain()
{
    // We plan to implement sceneContext dump action main logic here. (MOONRAY-4417)

#ifdef TEST_SCENE_CONTEXT_DUMP
    std::cerr << ">> ProcKeeper.cc signalActionSceneContextDumpMain()\n";
    sleep(1);
#endif // end TEST_SCENE_CONTEXT_DUMP

    _exit(EXIT_SUCCESS);
}

//==========================================================================================

ProcKeeper::ProcKeeperShPtr gProcKeeper;

// static function
void
ProcKeeper::init()
{
    MOONRAY_THREADSAFE_STATIC_WRITE(gProcKeeper.reset(new ProcKeeper));
}

// static function
ProcKeeper::ProcKeeperShPtr
ProcKeeper::get()
{
    return gProcKeeper;
}

} // namespace rndr
} // namespace moonray

