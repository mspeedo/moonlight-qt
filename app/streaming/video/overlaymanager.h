#pragma once

#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayImageAdjustments,
    OverlayMax
};

#if defined(HAVE_LIBPLACEBO_VULKAN) && defined(Q_OS_LINUX)
// Graph surfaces are recycled only after the asynchronous Vulkan upload has
// finished. recycleDebugGraphSurface() returns false for ordinary SDL surfaces.
SDL_Surface* acquireDebugGraphSurface(int width, int height);
bool recycleDebugGraphSurface(SDL_Surface* surface);
#endif

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;

#if defined(HAVE_LIBPLACEBO_VULKAN) && defined(Q_OS_LINUX)
    virtual bool supportsDebugGraph() const { return false; }
    // Supported renderers consume both surfaces, including on failure. Null text
    // means graph-only update; both null means hide. Publish a pair atomically.
    virtual bool updateDebugOverlay(SDL_Surface*, SDL_Surface*) { return false; }
#endif
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);
    void publishOverlaySurface(OverlayType type, SDL_Surface* newSurface);
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);

#if defined(HAVE_LIBPLACEBO_VULKAN) && defined(Q_OS_LINUX)
    static int debugOverlayThreadEntry(void* opaque);
    void debugOverlayThreadProc();
    bool ensureDebugOverlayWorker();
    void stopDebugOverlayWorker();
    void setDebugOverlayWorkerEnabled(bool enabled);
    bool queueDebugOverlayUpdate();
    static void telemetryStateChanged(void* opaque);
    void appendDebugTelemetry(char* text, std::size_t length, std::uint64_t nowUs);
    bool publishDebugOverlaySurfaces(SDL_Surface* text, SDL_Surface* graph,
                                     std::uint64_t generation, std::uint64_t revision);
#endif

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[4096];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
    std::mutex m_RendererMutex;

#if defined(HAVE_LIBPLACEBO_VULKAN) && defined(Q_OS_LINUX)
    SDL_Thread* m_DebugOverlayThread = nullptr;
    TTF_Font* m_DebugOverlayFont = nullptr;
    std::mutex m_DebugOverlayMutex;
    std::condition_variable m_DebugOverlayCondition;
    bool m_DebugOverlayStop = false;
    bool m_DebugOverlayPending = false;
    bool m_DebugOverlayEnabled = false;
    bool m_DebugOverlayAttached = false;
    bool m_DebugOverlaySplit = false;
    bool m_DebugOverlayReady = false;
    bool m_DebugOverlayStatePending = false;
    std::uint64_t m_DebugOverlayGeneration = 0;
    char m_DebugOverlayPendingText[4096] = {};
#endif
};

}
