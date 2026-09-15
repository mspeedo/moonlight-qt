#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include "../SDL_compat.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <new>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
#include <VideoStreamExtensions.h>
}

namespace ThreadPriority {

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

enum class GameModeState {
    NotRequested,
    ActiveRegistered,
    ActivePreRegistered,
    RequestedUnconfirmed,
    Rejected,
    PortalUnavailable,
    Released,
};

inline std::atomic<GameModeState> g_GameModeState {GameModeState::NotRequested};
inline std::mutex g_GameModeMutex;
inline int g_GameModeUsers = 0;
inline bool g_GameModeRegisteredByUs = false;

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

inline int requestElevatedNormalPriority(ThreadRole role, const char* threadName)
{
    PriorityRequestState& state = requestState(role);
    const int priorityResult = SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    state.result.store(priorityResult, std::memory_order_relaxed);
    state.attempted.store(1, std::memory_order_release);

    errno = 0;
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    const int niceValue = getpriority(PRIO_PROCESS, tid);
    const int niceError = errno;
    const int scheduler = sched_getscheduler(0);

    if (priorityResult == 0) {
        if (niceError == 0 && scheduler >= 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "%s priority request succeeded (nice=%d, scheduler=%s)",
                        threadName, niceValue, schedulerName(scheduler));
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "%s priority request succeeded (priority verification unavailable)",
                        threadName);
        }
    }
    else {
        if (niceError == 0 && scheduler >= 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s priority request denied: %s (nice=%d, scheduler=%s)",
                        threadName, SDL_GetError(), niceValue, schedulerName(scheduler));
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s priority request denied: %s",
                        threadName, SDL_GetError());
        }
    }

    return priorityResult;
}

inline QDBusInterface createGameModePortal()
{
    return QDBusInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
                          QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.GameMode"),
                          QDBusConnection::sessionBus());
}

inline int queryGameMode(QDBusInterface& portal, bool& valid)
{
    QDBusReply<int> reply = portal.call(QStringLiteral("QueryStatus"),
                                        static_cast<int>(getpid()));
    valid = reply.isValid();
    return valid ? reply.value() : -1;
}

inline void acquireGameMode()
{
    std::lock_guard<std::mutex> lock(g_GameModeMutex);
    if (g_GameModeUsers++ != 0) {
        return;
    }

    QDBusInterface portal = createGameModePortal();
    if (!portal.isValid()) {
        g_GameModeState.store(GameModeState::PortalUnavailable, std::memory_order_release);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode portal is unavailable");
        return;
    }

    bool queryValid = false;
    const int initialStatus = queryGameMode(portal, queryValid);
    if (queryValid && initialStatus == 2) {
        g_GameModeRegisteredByUs = false;
        g_GameModeState.store(GameModeState::ActivePreRegistered, std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode is already active for Moonlight");
        return;
    }

    QDBusReply<int> registerReply = portal.call(QStringLiteral("RegisterGame"),
                                                static_cast<int>(getpid()));
    if (!registerReply.isValid()) {
        g_GameModeState.store(GameModeState::PortalUnavailable, std::memory_order_release);
        const QByteArray error = registerReply.error().message().toUtf8();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode registration failed: %s",
                    error.constData());
        return;
    }

    if (registerReply.value() != 0) {
        bool retryValid = false;
        const int retryStatus = queryGameMode(portal, retryValid);
        if (retryValid && retryStatus == 2) {
            g_GameModeRegisteredByUs = false;
            g_GameModeState.store(GameModeState::ActivePreRegistered, std::memory_order_release);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "GameMode is already active for Moonlight");
        }
        else {
            g_GameModeRegisteredByUs = false;
            g_GameModeState.store(GameModeState::Rejected, std::memory_order_release);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GameMode registration was rejected");
        }
        return;
    }

    g_GameModeRegisteredByUs = true;
    bool verifyValid = false;
    const int verifiedStatus = queryGameMode(portal, verifyValid);
    if (verifyValid && verifiedStatus == 2) {
        g_GameModeState.store(GameModeState::ActiveRegistered, std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode registration succeeded and is active");
    }
    else {
        g_GameModeState.store(GameModeState::RequestedUnconfirmed, std::memory_order_release);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode registration succeeded but active state could not be confirmed");
    }
}

inline void releaseGameMode()
{
    std::lock_guard<std::mutex> lock(g_GameModeMutex);
    if (g_GameModeUsers == 0 || --g_GameModeUsers != 0) {
        return;
    }

    if (g_GameModeRegisteredByUs) {
        QDBusInterface portal = createGameModePortal();
        if (portal.isValid()) {
            QDBusReply<int> reply = portal.call(QStringLiteral("UnregisterGame"),
                                                static_cast<int>(getpid()));
            if (!reply.isValid() || reply.value() != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "GameMode unregister request failed");
            }
        }
        g_GameModeRegisteredByUs = false;
    }

    g_GameModeState.store(GameModeState::Released, std::memory_order_release);
}

inline void videoReceiveThreadInit()
{
    // SDL_THREAD_PRIORITY_HIGH maps to elevated Linux scheduling where allowed.
    // SDL can use the Linux priority helpers/RTKit path when direct adjustment
    // is not permitted, keeping this compatible with Flatpak.
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
    const int result = function(data);
    releaseGameMode();
    return result;
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

    // This wrapper is currently used only for FFDecoder. Register the Moonlight
    // process with GameMode for the lifetime of the real streaming decoder.
    acquireGameMode();

    SDL_Thread* thread = SDL_CreateThread(elevatedThreadStartThunk, name, context);
    if (thread == nullptr) {
        releaseGameMode();
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

        errno = 0;
        const int niceValue = getpriority(PRIO_PROCESS, static_cast<id_t>(parsedTid));
        if (errno != 0) {
            continue;
        }
        const int scheduler = sched_getscheduler(static_cast<pid_t>(parsedTid));
        if (scheduler < 0) {
            continue;
        }

        target->found = true;
        target->niceValue = niceValue;
        target->scheduler = scheduler;
    }

    closedir(tasks);
}

inline const char* gameModeStateText()
{
    switch (g_GameModeState.load(std::memory_order_acquire)) {
    case GameModeState::NotRequested:
        return "WAIT";
    case GameModeState::ActiveRegistered:
        return "ACTIVE";
    case GameModeState::ActivePreRegistered:
        return "ACTIVE(pre)";
    case GameModeState::RequestedUnconfirmed:
        return "UNCONFIRMED";
    case GameModeState::Rejected:
        return "FAILED";
    case GameModeState::PortalUnavailable:
        return "UNAVAILABLE";
    case GameModeState::Released:
        return "RELEASED";
    default:
        return "?";
    }
}

inline const char* effectiveStateText(const EffectivePriority& effective,
                                      const PriorityRequestState* request)
{
    if (!effective.found) {
        return "WAIT";
    }

    if (request != nullptr &&
        request->attempted.load(std::memory_order_acquire) != 0 &&
        request->result.load(std::memory_order_relaxed) != 0) {
        return "FAIL";
    }

    return isEffectivelyElevated(effective.niceValue, effective.scheduler) ? "OK" : "NORMAL";
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

    if (videoReceive.found && decoder.found && render.found) {
        std::snprintf(output,
                      length,
                      "GameMode: %s\n"
                      "VideoRecv: %s n=%d %s\n"
                      "FFDecoder: %s n=%d %s\n"
                      "PacerRender: %s n=%d %s",
                      gameModeStateText(),
                      effectiveStateText(videoReceive, &g_VideoReceiveRequest),
                      videoReceive.niceValue,
                      schedulerName(videoReceive.scheduler),
                      effectiveStateText(decoder, &g_DecoderRequest),
                      decoder.niceValue,
                      schedulerName(decoder.scheduler),
                      effectiveStateText(render, nullptr),
                      render.niceValue,
                      schedulerName(render.scheduler));
        return;
    }

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
                  "GameMode: %s\n"
                  "VideoRecv: %s\n"
                  "FFDecoder: %s\n"
                  "PacerRender: %s",
                  gameModeStateText(), videoLine, decoderLine, renderLine);
}

} // namespace ThreadPriority

#endif // Q_OS_LINUX
