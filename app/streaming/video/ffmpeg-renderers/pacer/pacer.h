#pragma once

#include "../../decoder.h"
#include "../renderer.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>

// The maximum number of frames pacer will ever hold is:
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
#define PACER_MAX_OUTSTANDING_FRAMES (3 + 1 + 1)

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual bool waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
        return false;
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats, bool diagnosticMode = false);

    // Used only by the isolated local renderer test; never set for streaming.
    void setDiagnosticAcquireLate(bool enabled) { m_DiagnosticAcquireLate.store(enabled, std::memory_order_relaxed); }
    // Set before initialize(), on the standalone diagnostic instance only.
    void setDiagnosticQueueLatest(bool latest) { m_DiagnosticQueueLatest = latest; }

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, bool enableVsync);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;
    bool m_RenderLatestFrame;
    bool m_DiagnosticMode;
    bool m_DiagnosticQueueLatest = true;
    std::atomic<bool> m_DiagnosticAcquireLate { false };

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;
};
