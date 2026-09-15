#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include "../SDL_compat.h"

#include <cerrno>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
#include <VideoStreamExtensions.h>
}

namespace ThreadPriority {

inline void requestElevatedNormalPriority(const char* threadName)
{
    const int priorityResult = SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    errno = 0;
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    const int niceValue = getpriority(PRIO_PROCESS, tid);
    const int niceError = errno;
    const int scheduler = sched_getscheduler(0);

    if (priorityResult == 0) {
        if (niceError == 0 && scheduler >= 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "%s priority request succeeded (nice=%d, scheduler=%d)",
                        threadName, niceValue, scheduler);
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
                        "%s priority request denied: %s (nice=%d, scheduler=%d)",
                        threadName, SDL_GetError(), niceValue, scheduler);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s priority request denied: %s",
                        threadName, SDL_GetError());
        }
    }
}

inline void videoReceiveThreadInit()
{
    // SDL_THREAD_PRIORITY_HIGH maps to an elevated SCHED_OTHER nice value on
    // Linux. SDL's Linux backend falls back to the XDG Realtime portal/RTKit
    // when direct setpriority() is denied, which works from Flatpak sandboxes.
    requestElevatedNormalPriority("VideoRecv");
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

} // namespace ThreadPriority

#endif // Q_OS_LINUX
