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
#include <memory>
#include <new>
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

enum class ThreadRole {
    VideoReceive,
    Decoder,
};

struct PriorityRequestState
{
    std::atomic<int> attempted {0};
    std::atomic<int> result {0};
};

inline PriorityRequestState g_VideoReceiveRequest;
inline PriorityRequestState g_DecoderRequest;

inline PriorityRequestState& requestState(ThreadRole role)
{
    return role == ThreadRole::VideoReceive ? g_VideoReceiveRequest : g_DecoderRequest;
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

// SteamOS Gaming Mode does not expose the XDG Realtime portal that normally
// translates Flatpak PID/TID namespace values for RealtimeKit. Direct RTKit
// calls from inside the sandbox therefore use unusable namespace-local IDs.
//
// For the three latency-sensitive stream threads, pass process/thread pidfds
// to a short-lived host command. Reading pidfd fdinfo in the host PID namespace
// yields the real host PID/TID, which can then be handed to RealtimeKit's
// MakeThreadHighPriorityWithPID(). The host command exits immediately after
// the single request; there is no persistent helper and no GameMode tuning.
inline bool requestFlatpakHostPriority(pid_t tid, const char* threadName)
{
#if defined(SYS_pidfd_open)
    const int processPidfd = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
    if (processPidfd < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority fallback: pidfd_open(process) failed: %s",
                    threadName, std::strerror(errno));
        return false;
    }

    // PIDFD_THREAD was added in Linux 6.9 and is defined as O_EXCL. SteamOS
    // kernels used by the current handheld builds are newer than that, while
    // using O_EXCL here also avoids depending on newer libc headers.
    const int threadPidfd = static_cast<int>(syscall(SYS_pidfd_open, tid, O_EXCL));
    if (threadPidfd < 0) {
        const int savedErrno = errno;
        close(processPidfd);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority fallback: pidfd_open(thread) failed: %s",
                    threadName, std::strerror(savedErrno));
        return false;
    }

    // pidfd_open() returns CLOEXEC descriptors. flatpak-spawn needs the
    // descriptors to survive exec so duplicate them with CLOEXEC cleared.
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
                    "%s host priority fallback: unable to duplicate pidfds: %s",
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

    // fdinfo's Pid field is interpreted in the procfs reader's PID namespace.
    // Because this script runs on the host, the two forwarded pidfds resolve to
    // the host process PID and host kernel TID without any numeric PID guessing.
    static const char hostScript[] =
        "pid_from_fd() { "
        "while read -r key value rest; do "
        "if [ \"$key\" = \"Pid:\" ]; then printf '%s\\n' \"$value\"; return 0; fi; "
        "done < \"/proc/self/fdinfo/$1\"; "
        "return 1; "
        "}; "
        "process=$(pid_from_fd \"$1\") || exit 20; "
        "thread=$(pid_from_fd \"$2\") || exit 21; "
        "[ \"$process\" -gt 0 ] 2>/dev/null || exit 22; "
        "[ \"$thread\" -gt 0 ] 2>/dev/null || exit 23; "
        "exec /usr/bin/busctl --system --timeout=2s call "
        "org.freedesktop.RealtimeKit1 /org/freedesktop/RealtimeKit1 "
        "org.freedesktop.RealtimeKit1 MakeThreadHighPriorityWithPID "
        "tti \"$process\" \"$thread\" -10 >/dev/null 2>&1";

    char* const argv[] = {
        const_cast<char*>("flatpak-spawn"),
        const_cast<char*>("--host"),
        const_cast<char*>("--watch-bus"),
        processForwardArg,
        threadForwardArg,
        const_cast<char*>("/bin/sh"),
        const_cast<char*>("-c"),
        const_cast<char*>(hostScript),
        const_cast<char*>("moonlight-priority"),
        processFdArg,
        threadFdArg,
        nullptr,
    };

    pid_t childPid = -1;
    const int spawnResult = posix_spawnp(&childPid,
                                         "flatpak-spawn",
                                         nullptr,
                                         nullptr,
                                         argv,
                                         environ);

    // posix_spawnp() has copied the descriptors into the child by this point.
    close(processForwardFd);
    close(threadForwardFd);

    if (spawnResult != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority fallback: flatpak-spawn failed: %s",
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
                    "%s host priority fallback: waitpid failed: %s",
                    threadName, std::strerror(errno));
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority fallback failed (exit=%d)",
                    threadName, exitCode);
        return false;
    }

    return true;
#else
    (void)tid;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "%s host priority fallback unavailable: pidfd_open syscall missing",
                threadName);
    return false;
#endif
}

inline int requestElevatedNormalPriority(ThreadRole role, const char* threadName)
{
    PriorityRequestState& state = requestState(role);
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));

    int requestResult = -1;
    if (runningInFlatpak()) {
        // Skip SDL's native RTKit fallback in Flatpak. Without the XDG
        // Realtime portal it sends sandbox-local numeric PID/TID values and
        // cannot target this host thread correctly.
        requestResult = requestFlatpakHostPriority(tid, threadName) ? 0 : -1;
    }
    else {
        requestResult = SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
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
                    threadName, requestResult, niceValue, schedulerName(scheduler));
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority request failed and effective priority could not be verified",
                    threadName);
    }
    return -1;
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

inline void elevateNamedFlatpakThread(const char* threadName)
{
    if (!runningInFlatpak()) {
        return;
    }

    const pid_t tid = findThreadTidByName(threadName);
    if (tid <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s host priority fallback: thread not found",
                    threadName);
        return;
    }

    int niceValue = 0;
    int scheduler = -1;
    if (readEffectivePriority(tid, niceValue, scheduler) &&
        isEffectivelyElevated(niceValue, scheduler)) {
        return;
    }

    requestFlatpakHostPriority(tid, threadName);

    if (readEffectivePriority(tid, niceValue, scheduler) &&
        isEffectivelyElevated(niceValue, scheduler)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority elevated (nice=%d, scheduler=%s)",
                    threadName, niceValue, schedulerName(scheduler));
    }
    else if (scheduler >= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority remains normal (nice=%d, scheduler=%s)",
                    threadName, niceValue, schedulerName(scheduler));
    }
}

inline void videoReceiveThreadInit()
{
    requestElevatedNormalPriority(ThreadRole::VideoReceive, "VideoRecv");
}

class VideoReceiveThreadPriorityRegistration
{
public:
    VideoReceiveThreadPriorityRegistration()
    {
        LiSetVideoReceiveThreadInitCallback(videoReceiveThreadInit);
    }
};

inline VideoReceiveThreadPriorityRegistration videoReceiveThreadPriorityRegistration;

struct ElevatedThreadStartContext
{
    SDL_ThreadFunction function;
    void* data;
    const char* name;
};

inline int elevatedThreadStartThunk(void* opaque)
{
    std::unique_ptr<ElevatedThreadStartContext> context(
        static_cast<ElevatedThreadStartContext*>(opaque));
    SDL_ThreadFunction function = context->function;
    void* data = context->data;
    const char* name = context->name;

    requestElevatedNormalPriority(ThreadRole::Decoder, name);

    // Pacer::initialize() creates PacerRender before FFDecoder. Its stock SDL
    // HIGH request cannot cross the Flatpak PID namespace when the Realtime
    // portal is missing, so elevate that already-running thread here too.
    elevateNamedFlatpakThread("PacerRender");

    return function(data);
}

inline SDL_Thread* createElevatedNormalPriorityThread(SDL_ThreadFunction function,
                                                       const char* name,
                                                       void* data)
{
    auto* context = new (std::nothrow) ElevatedThreadStartContext{function, data, name};
    if (context == nullptr) {
        SDL_OutOfMemory();
        return nullptr;
    }

    SDL_Thread* thread = SDL_CreateThread(elevatedThreadStartThunk, name, context);
    if (thread == nullptr) {
        delete context;
    }
    return thread;
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

    // Effective state wins over the stored request result. This matters for
    // PacerRender and for any case where a later host RTKit fallback succeeds
    // after an earlier in-sandbox request failed.
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
                      effectiveStateText(render, nullptr),
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
