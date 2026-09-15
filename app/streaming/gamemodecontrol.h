#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX)

#include "../SDL_compat.h"
#include "threadpriority.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QVariant>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <sys/syscall.h>
#include <unistd.h>

namespace GameModeControl {

enum class State {
    NotRequested,
    ActiveNative,
    ActiveNativePreRegistered,
    RequestedNativeUnconfirmed,
    Failed,
    Released,
};

inline std::atomic<State> g_State {State::NotRequested};
inline std::mutex g_Mutex;
inline int g_Users = 0;
inline bool g_RegisteredByUs = false;

inline QDBusInterface createNativeGameMode()
{
    return QDBusInterface(QStringLiteral("com.feralinteractive.GameMode"),
                          QStringLiteral("/com/feralinteractive/GameMode"),
                          QStringLiteral("com.feralinteractive.GameMode"),
                          QDBusConnection::sessionBus());
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
#if defined(SYS_pidfd_open)
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

inline int callNativePidfd(QDBusInterface& native,
                           const char* method,
                           bool& valid)
{
    valid = false;

    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!(bus.connectionCapabilities() & QDBusConnection::UnixFileDescriptorPassing)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native pidfd unavailable: D-Bus FD passing is unsupported");
        return -1;
    }

    SelfPidfds pidfds;
    if (!pidfds.open()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native pidfd_open failed: %s",
                    std::strerror(errno));
        return -1;
    }

    const QDBusUnixFileDescriptor gameFd(pidfds.game);
    const QDBusUnixFileDescriptor requesterFd(pidfds.requester);
    if (!gameFd.isValid() || !requesterFd.isValid()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native pidfd wrapping failed");
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

inline bool acquire()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_Users++ != 0) {
        return true;
    }

    g_RegisteredByUs = false;

    QDBusInterface native = createNativeGameMode();
    if (!native.isValid()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Native GameMode D-Bus service is unavailable");
        g_State.store(State::Failed, std::memory_order_release);
        return false;
    }

    bool queryValid = false;
    const int initialStatus = callNativePidfd(native, "QueryStatusByPIDFd", queryValid);
    if (queryValid && initialStatus == 2) {
        g_State.store(State::ActiveNativePreRegistered, std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode already ACTIVE(native) through pidfd API");
        return true;
    }

    bool registerValid = false;
    const int registerStatus = callNativePidfd(native,
                                               "RegisterGameByPIDFd",
                                               registerValid);
    if (!registerValid || registerStatus != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native pidfd registration failed (status=%d)",
                    registerStatus);
        g_State.store(State::Failed, std::memory_order_release);
        return false;
    }

    g_RegisteredByUs = true;

    bool verifyValid = false;
    const int verifiedStatus = callNativePidfd(native,
                                               "QueryStatusByPIDFd",
                                               verifyValid);
    if (verifyValid && verifiedStatus == 2) {
        g_State.store(State::ActiveNative, std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode ACTIVE(native) through pidfd API");
    }
    else {
        g_State.store(State::RequestedNativeUnconfirmed, std::memory_order_release);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameMode native registration succeeded but active state is unconfirmed");
    }

    return true;
}

inline void release()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_Users == 0 || --g_Users != 0) {
        return;
    }

    if (g_RegisteredByUs) {
        QDBusInterface native = createNativeGameMode();
        if (native.isValid()) {
            bool valid = false;
            const int status = callNativePidfd(native,
                                               "UnregisterGameByPIDFd",
                                               valid);
            if (!valid || status != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "GameMode native unregister failed (status=%d)",
                            status);
            }
        }
    }

    g_RegisteredByUs = false;
    g_State.store(State::Released, std::memory_order_release);
}

inline const char* stateText()
{
    if (!ThreadPriority::isEnabledFast()) {
        return "OFF";
    }

    switch (g_State.load(std::memory_order_acquire)) {
    case State::NotRequested:
        return "OFF";
    case State::ActiveNative:
        return "ACTIVE(native)";
    case State::ActiveNativePreRegistered:
        return "ACTIVE(native,pre)";
    case State::RequestedNativeUnconfirmed:
        return "REQUESTED(native)";
    case State::Failed:
        return "FAILED";
    case State::Released:
        return "RELEASED";
    }
    return "?";
}

struct ThreadStartContext
{
    SDL_ThreadFunction function;
    void* data;
    const char* name;
};

inline int threadStartThunk(void* opaque)
{
    std::unique_ptr<ThreadStartContext> context(
        static_cast<ThreadStartContext*>(opaque));
    SDL_ThreadFunction function = context->function;
    void* data = context->data;
    const char* name = context->name;

    ThreadPriority::requestElevatedNormalPriority(ThreadPriority::ThreadRole::Decoder, name);
    ThreadPriority::elevateNamedThread(ThreadPriority::ThreadRole::Render, "PacerRender");

    const int result = function(data);
    release();
    return result;
}

inline SDL_Thread* createStreamingThread(SDL_ThreadFunction function,
                                         const char* name,
                                         void* data)
{
    if (!ThreadPriority::isEnabledFast()) {
        return SDL_CreateThread(function, name, data);
    }

    auto* context = new (std::nothrow) ThreadStartContext{function, data, name};
    if (context == nullptr) {
        SDL_OutOfMemory();
        return nullptr;
    }

    acquire();

    SDL_Thread* thread = SDL_CreateThread(threadStartThunk, name, context);
    if (thread == nullptr) {
        release();
        delete context;
    }
    return thread;
}

} // namespace GameModeControl

#endif // Q_OS_LINUX
