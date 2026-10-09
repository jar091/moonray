// Copyright 2023-2025 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include "../ChangeWatcher.h"

#include <scene_rdl2/render/util/Files.h>
#include <scene_rdl2/common/platform/Platform.h>
#include <scene_rdl2/common/except/exceptions.h>
#include <scene_rdl2/render/util/Strings.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace moonray {

/*
 * The Linux implementation watches the *directory* a file lives in rather than
 * the file itself, because editors that use swap files rename the original away
 * and an inotify watch on the file would be dropped. Windows has the same
 * problem and the same answer: ReadDirectoryChangesW is directory-based by
 * design, so the structure below mirrors linux/ChangeWatcherImpl.cc exactly -
 * one watch per directory, a per-directory set of interesting file names, and a
 * filter on the name carried by each notification.
 *
 * The inotify events the Linux version subscribes to (IN_CLOSE_WRITE and
 * IN_MOVED_TO, i.e. "an editor finished writing this file" and "something was
 * renamed onto this file") map onto FILE_NOTIFY_CHANGE_LAST_WRITE and
 * FILE_NOTIFY_CHANGE_FILE_NAME.
 *
 * Non-blocking reads are expressed with overlapped I/O plus an event per
 * directory: hasChanged() polls the events, waitForChange() waits on them.
 */

namespace {

std::string
lastErrorStr(DWORD err)
{
    char* buffer = nullptr;
    const DWORD len =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, err, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string msg = (len && buffer) ? std::string(buffer, len) : std::to_string(err);
    if (buffer) LocalFree(buffer);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return msg;
}

std::string
toUtf8(const wchar_t* wide, std::size_t wideChars)
{
    if (wideChars == 0) return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(wideChars),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return std::string();
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(wideChars), &out[0], bytes,
                        nullptr, nullptr);
    return out;
}

std::wstring
toWide(const std::string& narrow)
{
    if (narrow.empty()) return std::wstring();
    const int chars = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                                          static_cast<int>(narrow.size()), nullptr, 0);
    if (chars <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), static_cast<int>(narrow.size()),
                        &out[0], chars);
    return out;
}

// Windows compares file names case-insensitively.
bool
sameFileName(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
        std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) ==
                   std::tolower(static_cast<unsigned char>(y));
        });
}

} // namespace

class ChangeWatcherWindows : public ChangeWatcher
{
public:
    ChangeWatcherWindows() = default;
    ~ChangeWatcherWindows() override;

    void watchFile(const std::string& filePath) override;
    bool hasChanged(std::set<std::string>* changedFiles = nullptr) override;
    void waitForChange() override;

private:
    struct Watch
    {
        std::string mDirName;
        std::vector<std::string> mFileNames; // the names we care about in mDirName
        HANDLE mDir {INVALID_HANDLE_VALUE};
        HANDLE mEvent {nullptr};
        OVERLAPPED mOverlapped {};
        // DWORD-aligned, as ReadDirectoryChangesW requires.
        std::vector<DWORD> mBuffer = std::vector<DWORD>(16 * 1024 / sizeof(DWORD));
        bool mPending {false};
    };

    void arm(Watch& watch);
    bool drain(Watch& watch, std::set<std::string>* changedFiles);

    std::vector<std::unique_ptr<Watch>> mWatches;
};

ChangeWatcher*
ChangeWatcher::CreateChangeWatcher()
{
    return new ChangeWatcherWindows();
}

ChangeWatcherWindows::~ChangeWatcherWindows()
{
    for (auto& watch : mWatches) {
        if (watch->mDir != INVALID_HANDLE_VALUE) {
            CancelIo(watch->mDir);
            CloseHandle(watch->mDir);
        }
        if (watch->mEvent) {
            CloseHandle(watch->mEvent);
        }
    }
}

void
ChangeWatcherWindows::arm(Watch& watch)
{
    std::memset(&watch.mOverlapped, 0, sizeof(watch.mOverlapped));
    watch.mOverlapped.hEvent = watch.mEvent;
    ResetEvent(watch.mEvent);

    // IN_CLOSE_WRITE  -> FILE_NOTIFY_CHANGE_LAST_WRITE
    // IN_MOVED_TO     -> FILE_NOTIFY_CHANGE_FILE_NAME
    if (!ReadDirectoryChangesW(watch.mDir,
                               watch.mBuffer.data(),
                               static_cast<DWORD>(watch.mBuffer.size() * sizeof(DWORD)),
                               FALSE, // this directory only, like an inotify watch
                               FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
                               nullptr,
                               &watch.mOverlapped,
                               nullptr)) {
        watch.mPending = false;
        throw scene_rdl2::except::RuntimeError(scene_rdl2::util::buildString(
                "Failed to read from file watcher for '", watch.mDirName, "': ",
                lastErrorStr(GetLastError())));
    }
    watch.mPending = true;
}

void
ChangeWatcherWindows::watchFile(const std::string& filePath)
{
    auto components = scene_rdl2::util::splitPath(filePath);
    const std::string& dirName = components.first;
    const std::string& fileName = components.second;

    for (auto& existing : mWatches) {
        if (sameFileName(existing->mDirName, dirName)) {
            if (std::none_of(existing->mFileNames.begin(), existing->mFileNames.end(),
                             [&fileName](const std::string& n) { return sameFileName(n, fileName); })) {
                existing->mFileNames.push_back(fileName);
            }
            return;
        }
    }

    auto watch = std::make_unique<Watch>();
    watch->mDirName = dirName;
    watch->mFileNames.push_back(fileName);

    // FILE_FLAG_BACKUP_SEMANTICS is what lets CreateFile open a directory;
    // FILE_FLAG_OVERLAPPED is what makes the reads non-blocking.
    watch->mDir = CreateFileW(toWide(dirName).c_str(),
                              FILE_LIST_DIRECTORY,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                              nullptr);
    if (watch->mDir == INVALID_HANDLE_VALUE) {
        throw scene_rdl2::except::RuntimeError(scene_rdl2::util::buildString(
                "Failed to add file watcher for '", filePath, "': ",
                lastErrorStr(GetLastError())));
    }

    watch->mEvent = CreateEventW(nullptr, TRUE /* manual reset */, FALSE, nullptr);
    if (!watch->mEvent) {
        const DWORD err = GetLastError();
        CloseHandle(watch->mDir);
        watch->mDir = INVALID_HANDLE_VALUE;
        throw scene_rdl2::except::RuntimeError(scene_rdl2::util::buildString(
                "Failed to initialize file watching for '", filePath, "': ",
                lastErrorStr(err)));
    }

    arm(*watch);
    mWatches.push_back(std::move(watch));
}

bool
ChangeWatcherWindows::drain(Watch& watch, std::set<std::string>* const changedFiles)
{
    if (!watch.mPending) return false;

    DWORD bytes = 0;
    if (!GetOverlappedResult(watch.mDir, &watch.mOverlapped, &bytes, FALSE)) {
        const DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            return false; // nothing yet; the read stays armed
        }
        watch.mPending = false;
        throw scene_rdl2::except::RuntimeError(scene_rdl2::util::buildString(
                "Failed to read from file watcher for '", watch.mDirName, "': ",
                lastErrorStr(err)));
    }
    watch.mPending = false;

    bool changeOccurred = false;
    if (bytes == 0) {
        // The buffer overflowed and the notifications were dropped. Grow it and
        // report a change: we cannot tell which file it was, and missing an edit
        // is worse than an extra reload (the Linux version grows its buffer for
        // the equivalent EINVAL).
        watch.mBuffer.resize(watch.mBuffer.size() * 2);
        changeOccurred = true;
    } else {
        const char* base = reinterpret_cast<const char*>(watch.mBuffer.data());
        DWORD offset = 0;
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
            const std::string name = toUtf8(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const auto it = std::find_if(watch.mFileNames.begin(), watch.mFileNames.end(),
                                         [&name](const std::string& n) { return sameFileName(n, name); });
            if (it != watch.mFileNames.end()) {
                if (changedFiles) {
                    changedFiles->insert(watch.mDirName + '/' + *it);
                }
                changeOccurred = true;
            }
            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
    }

    arm(watch);
    return changeOccurred;
}

bool
ChangeWatcherWindows::hasChanged(std::set<std::string>* const changedFiles)
{
    // We swallow every pending notification, but only report a change if one of
    // them named a watched file.
    bool changeOccurred = false;
    for (;;) {
        bool sawSomething = false;
        for (auto& watch : mWatches) {
            if (watch->mPending && WaitForSingleObject(watch->mEvent, 0) != WAIT_OBJECT_0) {
                continue;
            }
            const bool wasPending = watch->mPending;
            if (drain(*watch, changedFiles)) changeOccurred = true;
            if (wasPending) sawSomething = true;
        }
        if (!sawSomething) break;

        // Wait 0.1 second for any further notifications to appear, then read
        // again. Keep reading until no more notifications (as on Linux: one edit
        // produces a burst of events).
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return changeOccurred;
}

void
ChangeWatcherWindows::waitForChange()
{
    if (mWatches.empty()) return;

    std::vector<HANDLE> events;
    events.reserve(mWatches.size());
    for (auto& watch : mWatches) {
        events.push_back(watch->mEvent);
    }

    do {
        const DWORD rc = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(),
                                                FALSE, INFINITE);
        if (rc == WAIT_FAILED) {
            throw scene_rdl2::except::RuntimeError(scene_rdl2::util::buildString(
                    "Waiting on file watcher failed: ", lastErrorStr(GetLastError())));
        }
    } while (!hasChanged());
}

} // namespace moonray
