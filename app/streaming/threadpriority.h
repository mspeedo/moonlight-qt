#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include "../SDL_compat.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QVariant>

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
    ActivePortal,
    ActivePortalPreRegistered,
    ActiveNative,
    ActiveNativePreRegistered,
    RequestedPortalUnconfirmed,
    RequestedNativeUnconfirmed,
    Failed,
    Released,
};

enum class GameModeTransport {
    None,
    Portal,
    Native,
};

inline std::atomic<GameModeState> g_GameModeState {GameModeState::NotRequested};
inline std::mutex g_GameModeMutex;
inline int g_GameModeUsers = 0;
inline bool g_GameModeRegisteredByUs = false;
inline GameModeTransport g_GameModeTransport = GameModeTransport::None;

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

inline QDBusInterface createNativeGameMode()
{
    return QDBusInterface(QStringLiteral("com.feralinteractive.GameMode"),
                          QStringLiteral("/com/feralinteractive/GameMode"),
                          QStringLiteral("com.feralinteractive.GameMode"),
                          QDBusConnection::sessionBus());
}

inline int queryPortalGameMode(QDBusInterface& portal, bool& valid)
{
    QDBusReply<int> reply = portal.call(QStringLiteral("QueryStatus"),
                                        static_cast<int>(getpid()));
    valid = reply.isValid();
    return valid ? reply.value() : -1;
}

struct SelfPidfds
{
    int game = -1;
    int requester = -1;

    ~SelfPidfds()
    {
        if (game >= 0) {
            close(game);
        }
        if (requester >= 0) {
            close(requester);
        }
    }

    bool open()
    {
#ifdef SYS_pidfd_open
        game = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
        if (game < 0) {
            return false;
        }

        requester = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
        if (requester < 0) {
            close(game);
            game = -1;
            return false;
        }
        return true;
#else
        errno = ENOSYS;
        return false;
#endif
    }
};

inline int callNativeGameModePidfd(QDBusInterface& native,
                                   const char* method,
                                   bool& valid)
{
    valid = false;

    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!(bus.connectionCapabilities() & QDBusConnection::UnixFileDescriptorPassing)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native fallback unavailable: D-Bus FD passing is not supported");
        return -1;
    }

    SelfPidfds pidfds;
    if (!pidfds.open()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native fallback unable to open self pidfds: %s",
                    std::strerror(errno));
        return -1;
    }

    const QDBusUnixFileDescriptor gameFd(pidfds.game);
    const QDBusUnixFileDescriptor requesterFd(pidfds.requester);
    if (!gameFd.isValid() || !requesterFd.isValid()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native fallback unable to wrap self pidfds for D-Bus");
        return -1;
    }

    QDBusReply<int> reply = native.call(QString::fromLatin1(method),
                                        QVariant::fromValue(gameFd),
                                        QVariant::fromValue(requesterFd));
    valid = reply.isValid();
    if (!valid) {
        const QByteArray error = reply.error().message().toUtf8();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native %s failed: %s",
                    method,
                    error.constData());
        return -1;
    }

    return reply.value();
}

inline bool tryAcquireGameModePortal()
{
    QDBusInterface portal = createGameModePortal();
    if (!portal.isValid()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode portal is unavailable; trying native pidfd fallback");
        return false;
    }

    bool queryValid = false;
    const int initialStatus = queryPortalGameMode(portal, queryValid);
    if (queryValid && initialStatus == 2) {
        g_GameModeRegisteredByUs = false;
        g_GameModeTransport = GameModeTransport::Portal;
        g_GameModeState.store(GameModeState::ActivePortalPreRegistered,
                              std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode is already active through the portal");
        return true;
    }

    QDBusReply<int> registerReply = portal.call(QStringLiteral("RegisterGame"),
                                                static_cast<int>(getpid()));
    if (!registerReply.isValid()) {
        const QByteArray error = registerReply.error().message().toUtf8();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode portal registration unavailable (%s); trying native pidfd fallback",
                    error.constData());
        return false;
    }

    if (registerReply.value() != 0) {
        bool retryValid = false;
        const int retryStatus = queryPortalGameMode(portal, retryValid);
        if (retryValid && retryStatus == 2) {
            g_GameModeRegisteredByUs = false;
            g_GameModeTransport = GameModeTransport::Portal;
            g_GameModeState.store(GameModeState::ActivePortalPreRegistered,
                                  std::memory_order_release);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "GameMode is already active through the portal");
            return true;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode portal registration was rejected; trying native pidfd fallback");
        return false;
    }

    g_GameModeRegisteredByUs = true;
    g_GameModeTransport = GameModeTransport::Portal;

    bool verifyValid = false;
    const int verifiedStatus = queryPortalGameMode(portal, verifyValid);
    if (verifyValid && verifiedStatus == 2) {
        g_GameModeState.store(GameModeState::ActivePortal, std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode registration succeeded through the portal");
    }
    else {
        g_GameModeState.store(GameModeState::RequestedPortalUnconfirmed,
                              std::memory_order_release);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode portal registration succeeded but active state could not be confirmed");
    }

    return true;
}

inline bool tryAcquireGameModeNative()
{
    QDBusInterface native = createNativeGameMode();
    if (!native.isValid()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native GameMode D-Bus service is unavailable");
        return false;
    }

    bool queryValid = false;
    const int initialStatus = callNativeGameModePidfd(native,
                                                      "QueryStatusByPIDFd",
                                                      queryValid);
    if (queryValid && initialStatus == 2) {
        g_GameModeRegisteredByUs = false;
        g_GameModeTransport = GameModeTransport::Native;
        g_GameModeState.store(GameModeState::ActiveNativePreRegistered,
                              std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode is already active through native pidfd API");
        return true;
    }

    bool registerValid = false;
    const int registerStatus = callNativeGameModePidfd(native,
                                                       "RegisterGameByPIDFd",
                                                       registerValid);
    if (!registerValid || registerStatus != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native GameMode pidfd registration failed (status=%d)",
                    registerStatus);
        return false;
    }

    g_GameModeRegisteredByUs = true;
    g_GameModeTransport = GameModeTransport::Native;

    bool verifyValid = false;
    const int verifiedStatus = callNativeGameModePidfd(native,
                                                       "QueryStatusByPIDFd",
                                                       verifyValid);
    if (verifyValid && verifiedStatus == 2) {
        g_GameModeState.store(GameModeState::ActiveNative, std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode registration succeeded through native pidfd API");
    }
    else {
        g_GameModeState.store(GameModeState::RequestedNativeUnconfirmed,
                              std::memory_order_release);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native GameMode registration succeeded but active state could not be confirmed");
    }

    return true;
}

inline void acquireGameMode()
{
    std::lock_guard<std::mutex> lock(g_GameModeMutex);
    if (g_GameModeUsers++ != 0) {
        return;
    }

    g_GameModeRegisteredByUs = false;
    g_GameModeTransport = GameModeTransport::None;

    if (tryAcquireGameModePortal()) {
        return;
    }

    if (tryAcquireGameModeNative()) {
        return;
    }

    g_GameModeState.store(GameModeState::Failed, std::memory_order_release);
}

inline void releaseGameMode()
{
    std::lock_guard<std::mutex> lock(g_GameModeMutex);
    if (g_GameModeUsers == 0 || --g_GameModeUsers != 0) {
        return;
    }

    if (g_GameModeRegisteredByUs) {
        if (g_GameModeTransport == GameModeTransport::Portal) {
            QDBusInterface portal = createGameModePortal();
            if (portal.isValid()) {
                QDBusReply<int> reply = portal.call(QStringLiteral("UnregisterGame"),
                                                    static_cast<int>(getpid()));
                if (!reply.isValid() || reply.value() != 0) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "GameMode portal unregister request failed");
                }
            }
        }
        else if (g_GameModeTransport == GameModeTransport::Native) {
            QDBusInterface native = createNativeGameMode();
            if (native.isValid()) {
                bool valid = false;
                const int status = callNativeGameModePidfd(native,
                                                           "UnregisterGameByPIDFd",
                                                           valid);
                if (!valid || status != 0) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Native GameMode unregister request failed (status=%d)",
                                status);
                }
            }
        }
    }

    g_GameModeRegisteredByUs = false;
    g_GameModeTransport = GameModeTransport::None;
    g_GameModeState.store(GameModeState::Released, std::memory_order_release);
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
    case GameModeState::ActivePortal:
        return "ACTIVE(portal)";
    case GameModeState::ActivePortalPreRegistered:
        return "ACTIVE(portal-pre)";
    case GameModeState::ActiveNative:
        return "ACTIVE(native)";
    case GameModeState::ActiveNativePreRegistered:
        return "ACTIVE(native-pre)";
    case GameModeState::RequestedPortalUnconfirmed:
        return "UNCONFIRMED(portal)";
    case GameModeState::RequestedNativeUnconfirmed:
        return "UNCONFIRMED(native)";
    case GameModeState::Failed:
        return "FAILED";
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
