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
#include <memory>
#include <new>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
#include <VideoStreamExtensions.h>
}

namespace ThreadPriority {

constexpr int kHighPriorityNice = -10;
constexpr rlim_t kRequiredNiceLimit = static_cast<rlim_t>(20 - kHighPriorityNice); // 30 permits nice -10

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

inline PriorityRequestState g_VideoReceiveRequest;
inline PriorityRequestState g_DecoderRequest;
inline PriorityRequestState g_RenderRequest;

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

inline bool readNiceLimit(struct rlimit& limit)
{
    return getrlimit(RLIMIT_NICE, &limit) == 0;
}

inline bool ensureNiceAllowance(const char* threadName)
{
    struct rlimit limit;
    if (!readNiceLimit(limit)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority: getrlimit(RLIMIT_NICE) failed: %s",
                    threadName, std::strerror(errno));
        return false;
    }

    if (limit.rlim_cur >= kRequiredNiceLimit || limit.rlim_cur == RLIM_INFINITY) {
        return true;
    }

    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < kRequiredNiceLimit) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority: RLIMIT_NICE hard limit too low (soft=%llu hard=%llu need=%llu)",
                    threadName,
                    static_cast<unsigned long long>(limit.rlim_cur),
                    static_cast<unsigned long long>(limit.rlim_max),
                    static_cast<unsigned long long>(kRequiredNiceLimit));
        return false;
    }

    struct rlimit raised = limit;
    raised.rlim_cur = kRequiredNiceLimit;
    if (setrlimit(RLIMIT_NICE, &raised) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s priority: unable to raise RLIMIT_NICE soft limit: %s",
                    threadName, std::strerror(errno));
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "%s priority: raised RLIMIT_NICE soft limit to %llu",
                threadName,
                static_cast<unsigned long long>(kRequiredNiceLimit));
    return true;
}

inline int requestPriorityForTid(pid_t tid, ThreadRole role, const char* threadName)
{
    PriorityRequestState& state = requestState(role);

    bool requestSucceeded = false;
    if (ensureNiceAllowance(threadName)) {
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

    // PacerRender is already running by the time FFDecoder starts. Apply the
    // same direct nice request to that specific TID, without changing the rest
    // of Moonlight's threads.
    elevateNamedThread(ThreadRole::Render, "PacerRender");

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

inline void formatLimitValue(rlim_t value, char* output, std::size_t length)
{
    if (value == RLIM_INFINITY) {
        std::snprintf(output, length, "inf");
    }
    else {
        std::snprintf(output, length, "%llu",
                      static_cast<unsigned long long>(value));
    }
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
    char softLimit[24] = "?";
    char hardLimit[24] = "?";

    struct rlimit niceLimit;
    if (readNiceLimit(niceLimit)) {
        formatLimitValue(niceLimit.rlim_cur, softLimit, sizeof(softLimit));
        formatLimitValue(niceLimit.rlim_max, hardLimit, sizeof(hardLimit));
    }

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
                  "RLIMIT_NICE: s=%s h=%s\n"
                  "VideoRecv: %s\n"
                  "FFDecoder: %s\n"
                  "PacerRender: %s",
                  softLimit, hardLimit,
                  videoLine, decoderLine, renderLine);
}

} // namespace ThreadPriority

#endif // Q_OS_LINUX
