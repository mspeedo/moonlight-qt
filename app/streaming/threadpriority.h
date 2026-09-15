#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include "../SDL_compat.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include <VideoStreamExtensions.h>
}

extern char** environ;

namespace ThreadPriority {

constexpr int kHighPriorityNice = -10;
constexpr const char* kHostHelperPath = "/opt/moonlight-priority/moonlight-priority-helper";

enum class ThreadRole {
    VideoReceive,
    Decoder,
    Render,
};

struct PriorityRequestState
{
    std::atomic<int> attempted {0};
    std::atomic<int> result {0};
};

inline std::atomic<bool> g_Enabled {false};
inline PriorityRequestState g_VideoReceiveRequest;
inline PriorityRequestState g_DecoderRequest;
inline PriorityRequestState g_RenderRequest;

inline void videoReceiveThreadInit();

inline void setEnabled(bool enabled)
{
    g_Enabled.store(enabled, std::memory_order_release);
    LiSetVideoReceiveThreadInitCallback(enabled ? videoReceiveThreadInit : nullptr);
}

inline bool isEnabledFast()
{
    return g_Enabled.load(std::memory_order_relaxed);
}

inline PriorityRequestState& requestState(ThreadRole role)
{
    switch (role) {
    case ThreadRole::VideoReceive:
        return g_VideoReceiveRequest;
    case ThreadRole::Decoder:
        return g_DecoderRequest;
    case ThreadRole::Render:
        return g_RenderRequest;
    }

    return g_VideoReceiveRequest;
}

inline const char* schedulerName(int scheduler)
{
    switch (scheduler) {
    case SCHED_OTHER:
        return "OTHER";
#ifdef SCHED_BATCH
    case SCHED_BATCH:
        return "BATCH";
#endif
#ifdef SCHED_IDLE
    case SCHED_IDLE:
        return "IDLE";
#endif
    case SCHED_FIFO:
        return "FIFO";
    case SCHED_RR:
        return "RR";
#ifdef SCHED_DEADLINE
    case SCHED_DEADLINE:
        return "DEADLINE";
#endif
    default:
        return "?";
    }
}

inline bool isEffectivelyElevated(int niceValue, int scheduler)
{
    if (scheduler == SCHED_FIFO || scheduler == SCHED_RR) {
        return true;
    }
#ifdef SCHED_DEADLINE
    if (scheduler == SCHED_DEADLINE) {
        return true;
    }
#endif
    return niceValue < 0;
}

inline bool readEffectivePriority(pid_t tid, int& niceValue, int& scheduler)
{
    errno = 0;
    niceValue = getpriority(PRIO_PROCESS, static_cast<id_t>(tid));
    if (errno != 0) {
        return false;
    }

    scheduler = sched_getscheduler(tid);
    return scheduler >= 0;
}

inline bool runningInFlatpak()
{
    const char* flatpakId = std::getenv("FLATPAK_ID");
    return flatpakId != nullptr && flatpakId[0] != '\0';
}

inline bool requestFlatpakHostPriority(pid_t tid, const char* threadName)
{
#if defined(SYS_pidfd_open)
    const int processPidfd = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
    if (processPidfd < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority: pidfd_open(process) failed: %s",
                    threadName, std::strerror(errno));
        return false;
    }

    // Linux 6.9+ interprets O_EXCL as PIDFD_THREAD for pidfd_open(). This lets
    // the host helper resolve the exact kernel TID rather than only the process.
    const int threadPidfd = static_cast<int>(syscall(SYS_pidfd_open, tid, O_EXCL));
    if (threadPidfd < 0) {
        const int savedErrno = errno;
        close(processPidfd);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority: pidfd_open(thread) failed: %s",
                    threadName, std::strerror(savedErrno));
        return false;
    }

    // pidfd_open() returns CLOEXEC descriptors. Duplicate them without CLOEXEC
    // so flatpak-spawn can forward the pidfds into the host helper process.
    const int processForwardFd = fcntl(processPidfd, F_DUPFD, 64);
    const int threadForwardFd = fcntl(threadPidfd, F_DUPFD, 65);
    close(processPidfd);
    close(threadPidfd);

    if (processForwardFd < 0 || threadForwardFd < 0) {
        const int savedErrno = errno;
        if (processForwardFd >= 0) {
            close(processForwardFd);
        }
        if (threadForwardFd >= 0) {
            close(threadForwardFd);
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority: unable to duplicate pidfds: %s",
                    threadName, std::strerror(savedErrno));
        return false;
    }

    char processForwardArg[48];
    char threadForwardArg[48];
    char processFdArg[24];
    char threadFdArg[24];
    std::snprintf(processForwardArg, sizeof(processForwardArg),
                  "--forward-fd=%d", processForwardFd);
    std::snprintf(threadForwardArg, sizeof(threadForwardArg),
                  "--forward-fd=%d", threadForwardFd);
    std::snprintf(processFdArg, sizeof(processFdArg), "%d", processForwardFd);
    std::snprintf(threadFdArg, sizeof(threadFdArg), "%d", threadForwardFd);

    char* const argv[] = {
        const_cast<char*>("flatpak-spawn"),
        const_cast<char*>("--host"),
        const_cast<char*>("--watch-bus"),
        processForwardArg,
        threadForwardArg,
        const_cast<char*>(kHostHelperPath),
        processFdArg,
        threadFdArg,
        const_cast<char*>(threadName),
        nullptr,
    };

    pid_t childPid = -1;
    const int spawnResult = posix_spawnp(&childPid,
                                         "flatpak-spawn",
                                         nullptr,
                                         nullptr,
                                         argv,
                                         environ);

    close(processForwardFd);
    close(threadForwardFd);

    if (spawnResult != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority: flatpak-spawn failed: %s",
                    threadName, std::strerror(spawnResult));
        return false;
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(childPid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority: waitpid failed: %s",
                    threadName, std::strerror(errno));
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority helper failed (exit=%d)",
                    threadName, exitCode);
        return false;
    }

    return true;
#else
    (void)tid;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "%s host priority unavailable: pidfd_open syscall missing",
                threadName);
    return false;
#endif
}

inline int requestPriorityForTid(pid_t tid, ThreadRole role, const char* threadName)
{
    if (!isEnabledFast()) {
        return 0;
    }

    PriorityRequestState& state = requestState(role);

    bool requestSucceeded = false;
    if (runningInFlatpak()) {
        requestSucceeded = requestFlatpakHostPriority(tid, threadName);
    }
    else {
        errno = 0;
        requestSucceeded = setpriority(PRIO_PROCESS,
                                       static_cast<id_t>(tid),
                                       kHighPriorityNice) == 0;
        if (!requestSucceeded) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s priority: setpriority(%d) failed: %s",
                        threadName, kHighPriorityNice, std::strerror(errno));
        }
    }

    int niceValue = 0;
    int scheduler = -1;
    const bool verified = readEffectivePriority(tid, niceValue, scheduler);
    const bool elevated = verified && isEffectivelyElevated(niceValue, scheduler);

    state.result.store(elevated ? 0 : -1, std::memory_order_relaxed);
    state.attempted.store(1, std::memory_order_release);

    if (elevated) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority elevated (nice=%d, scheduler=%s)",
                    threadName, niceValue, schedulerName(scheduler));
        return 0;
    }

    if (verified) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority request failed (request=%d, nice=%d, scheduler=%s)",
                    threadName, requestSucceeded ? 0 : -1,
                    niceValue, schedulerName(scheduler));
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority request failed and effective priority could not be verified",
                    threadName);
    }

    return -1;
}

inline int requestElevatedNormalPriority(ThreadRole role, const char* threadName)
{
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    return requestPriorityForTid(tid, role, threadName);
}

inline pid_t findThreadTidByName(const char* threadName)
{
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr) {
        return -1;
    }

    pid_t result = -1;
    while (dirent* entry = readdir(tasks)) {
        char* end = nullptr;
        const long parsedTid = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || parsedTid <= 0) {
            continue;
        }

        char path[96];
        std::snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", parsedTid);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            continue;
        }

        char name[32] = {};
        const bool readName = std::fgets(name, sizeof(name), file) != nullptr;
        std::fclose(file);
        if (!readName) {
            continue;
        }

        name[std::strcspn(name, "\r\n")] = '\0';
        if (std::strcmp(name, threadName) == 0) {
            result = static_cast<pid_t>(parsedTid);
            break;
        }
    }

    closedir(tasks);
    return result;
}

inline void elevateNamedThread(ThreadRole role, const char* threadName)
{
    if (!isEnabledFast()) {
        return;
    }

    const pid_t tid = findThreadTidByName(threadName);
    if (tid <= 0) {
        PriorityRequestState& state = requestState(role);
        state.result.store(-1, std::memory_order_relaxed);
        state.attempted.store(1, std::memory_order_release);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority: thread not found",
                    threadName);
        return;
    }

    int niceValue = 0;
    int scheduler = -1;
    if (readEffectivePriority(tid, niceValue, scheduler) &&
        isEffectivelyElevated(niceValue, scheduler)) {
        PriorityRequestState& state = requestState(role);
        state.result.store(0, std::memory_order_relaxed);
        state.attempted.store(1, std::memory_order_release);
        return;
    }

    requestPriorityForTid(tid, role, threadName);
}

inline void videoReceiveThreadInit()
{
    requestElevatedNormalPriority(ThreadRole::VideoReceive, "VideoRecv");
}

struct EffectivePriority
{
    bool found = false;
    int niceValue = 0;
    int scheduler = -1;
};

inline void collectEffectivePriorities(EffectivePriority& videoReceive,
                                       EffectivePriority& decoder,
                                       EffectivePriority& render)
{
    DIR* tasks = opendir("/proc/self/task");
    if (tasks == nullptr) {
        return;
    }

    while (dirent* entry = readdir(tasks)) {
        char* end = nullptr;
        const long parsedTid = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || parsedTid <= 0) {
            continue;
        }

        char path[96];
        std::snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", parsedTid);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            continue;
        }

        char name[32] = {};
        const bool readName = std::fgets(name, sizeof(name), file) != nullptr;
        std::fclose(file);
        if (!readName) {
            continue;
        }

        name[std::strcspn(name, "\r\n")] = '\0';
        EffectivePriority* target = nullptr;
        if (std::strcmp(name, "VideoRecv") == 0) {
            target = &videoReceive;
        }
        else if (std::strcmp(name, "FFDecoder") == 0) {
            target = &decoder;
        }
        else if (std::strcmp(name, "PacerRender") == 0) {
            target = &render;
        }

        if (target == nullptr) {
            continue;
        }

        int niceValue = 0;
        int scheduler = -1;
        if (!readEffectivePriority(static_cast<pid_t>(parsedTid), niceValue, scheduler)) {
            continue;
        }

        target->found = true;
        target->niceValue = niceValue;
        target->scheduler = scheduler;
    }

    closedir(tasks);
}

inline const char* effectiveStateText(const EffectivePriority& effective,
                                      const PriorityRequestState* request)
{
    if (!effective.found) {
        return "WAIT";
    }

    if (isEffectivelyElevated(effective.niceValue, effective.scheduler)) {
        return "OK";
    }

    if (request != nullptr &&
        request->attempted.load(std::memory_order_acquire) != 0 &&
        request->result.load(std::memory_order_relaxed) != 0) {
        return "FAIL";
    }

    return "NORMAL";
}

inline void formatOverlayLines(char* output, std::size_t length)
{
    if (output == nullptr || length == 0) {
        return;
    }

    if (!isEnabledFast()) {
        std::snprintf(output,
                      length,
                      "VideoRecv: OFF\n"
                      "FFDecoder: OFF\n"
                      "PacerRender: OFF");
        return;
    }

    EffectivePriority videoReceive;
    EffectivePriority decoder;
    EffectivePriority render;
    collectEffectivePriorities(videoReceive, decoder, render);

    char videoLine[64];
    char decoderLine[64];
    char renderLine[64];

    if (videoReceive.found) {
        std::snprintf(videoLine, sizeof(videoLine), "%s n=%d %s",
                      effectiveStateText(videoReceive, &g_VideoReceiveRequest),
                      videoReceive.niceValue, schedulerName(videoReceive.scheduler));
    }
    else {
        std::snprintf(videoLine, sizeof(videoLine), "WAIT");
    }

    if (decoder.found) {
        std::snprintf(decoderLine, sizeof(decoderLine), "%s n=%d %s",
                      effectiveStateText(decoder, &g_DecoderRequest),
                      decoder.niceValue, schedulerName(decoder.scheduler));
    }
    else {
        std::snprintf(decoderLine, sizeof(decoderLine), "WAIT");
    }

    if (render.found) {
        std::snprintf(renderLine, sizeof(renderLine), "%s n=%d %s",
                      effectiveStateText(render, &g_RenderRequest),
                      render.niceValue, schedulerName(render.scheduler));
    }
    else {
        std::snprintf(renderLine, sizeof(renderLine), "WAIT");
    }

    std::snprintf(output,
                  length,
                  "VideoRecv: %s\n"
                  "FFDecoder: %s\n"
                  "PacerRender: %s",
                  videoLine, decoderLine, renderLine);
}

} // namespace ThreadPriority

#endif // Q_OS_LINUX
