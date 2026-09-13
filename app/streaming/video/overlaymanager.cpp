#include "overlaymanager.h"
#include "path.h"

#if defined(HAVE_LIBPLACEBO_VULKAN) && defined(Q_OS_LINUX)
#include "streaming/latencybenchmarkcontrol.h"
#include "streaming/latencyprobe.h"
#include "streaming/streamhealthtelemetry.h"
#include "streaming/streampipelinetelemetry.h"
#include "streaming/session.h"
#define HAVE_LATENCY_PROBE 1
#endif

using namespace Overlay;

#ifdef HAVE_LATENCY_PROBE
namespace {

constexpr int kTelemetryGraphPlotHeight = 44;
constexpr int kTelemetryGraphRowGap = 8;
constexpr int kTelemetryGraphPanelPadding = 8;
constexpr int kTelemetryGraphSurfaceGap = 24;
constexpr float kTelemetryGraphMaxMs = 25.0f;

void blitGraphLabel(SDL_Surface* destination,
                    TTF_Font* font,
                    const char* text,
                    int x,
                    int y,
                    SDL_Color color)
{
    SDL_Surface* shadow = TTF_RenderUTF8_Blended(font, text, {0, 0, 0, 255});
    SDL_Surface* label = TTF_RenderUTF8_Blended(font, text, color);

    if (shadow != nullptr) {
        SDL_Rect dst = {x + 1, y + 1, shadow->w, shadow->h};
        SDL_BlitSurface(shadow, nullptr, destination, &dst);
        SDL_FreeSurface(shadow);
    }
    if (label != nullptr) {
        SDL_Rect dst = {x, y, label->w, label->h};
        SDL_BlitSurface(label, nullptr, destination, &dst);
        SDL_FreeSurface(label);
    }
}

int graphValueToY(float valueMs, int plotY)
{
    if (valueMs < 0.0f) {
        valueMs = 0.0f;
    }
    if (valueMs > kTelemetryGraphMaxMs) {
        valueMs = kTelemetryGraphMaxMs;
    }

    const int usableHeight = kTelemetryGraphPlotHeight - 3;
    const int scaled = static_cast<int>(
            (valueMs / kTelemetryGraphMaxMs) * usableHeight + 0.5f);
    return plotY + kTelemetryGraphPlotHeight - 2 - scaled;
}

void drawTelemetryGraph(SDL_Surface* surface,
                        int plotX,
                        int plotY,
                        const StreamPipelineTelemetry::GraphSeries& series,
                        double framePeriodMs,
                        bool drawFramePeriod,
                        SDL_Color color)
{
    const Uint32 background = SDL_MapRGBA(surface->format, 0, 0, 0, 0x70);
    const Uint32 grid = SDL_MapRGBA(surface->format,
                                    color.r / 3,
                                    color.g / 3,
                                    color.b / 3,
                                    0x70);
    const Uint32 border = SDL_MapRGBA(surface->format,
                                      color.r / 2,
                                      color.g / 2,
                                      color.b / 2,
                                      0xB0);
    const Uint32 reference = SDL_MapRGBA(surface->format,
                                         color.r,
                                         color.g,
                                         color.b,
                                         0x70);
    const Uint32 trace = SDL_MapRGBA(surface->format,
                                     color.r,
                                     color.g,
                                     color.b,
                                     0xD0);

    SDL_Rect plot = {plotX,
                     plotY,
                     static_cast<int>(StreamPipelineTelemetry::kGraphColumns),
                     kTelemetryGraphPlotHeight};
    SDL_FillRect(surface, &plot, background);

    for (int ms = 5; ms < static_cast<int>(kTelemetryGraphMaxMs); ms += 5) {
        const int y = graphValueToY(static_cast<float>(ms), plotY);
        SDL_Rect line = {plotX, y,
                         static_cast<int>(StreamPipelineTelemetry::kGraphColumns), 1};
        SDL_FillRect(surface, &line, grid);
    }

    if (drawFramePeriod && framePeriodMs > 0.0 &&
            framePeriodMs < kTelemetryGraphMaxMs) {
        const int y = graphValueToY(static_cast<float>(framePeriodMs), plotY);
        SDL_Rect line = {plotX, y,
                         static_cast<int>(StreamPipelineTelemetry::kGraphColumns), 1};
        SDL_FillRect(surface, &line, reference);
    }

    const int bottom = plotY + kTelemetryGraphPlotHeight - 2;
    for (std::size_t i = 0; i < StreamPipelineTelemetry::kGraphColumns; ++i) {
        if (!series.valid[i]) {
            continue;
        }

        const int y = graphValueToY(series.maximumMs[i], plotY);
        SDL_Rect bar = {plotX + static_cast<int>(i),
                        y,
                        1,
                        bottom - y + 1};
        SDL_FillRect(surface, &bar, trace);
    }

    SDL_Rect top = {plotX, plotY,
                    static_cast<int>(StreamPipelineTelemetry::kGraphColumns), 1};
    SDL_Rect bottomLine = {plotX, plotY + kTelemetryGraphPlotHeight - 1,
                           static_cast<int>(StreamPipelineTelemetry::kGraphColumns), 1};
    SDL_Rect left = {plotX, plotY, 1, kTelemetryGraphPlotHeight};
    SDL_Rect right = {plotX + static_cast<int>(StreamPipelineTelemetry::kGraphColumns) - 1,
                      plotY, 1, kTelemetryGraphPlotHeight};
    SDL_FillRect(surface, &top, border);
    SDL_FillRect(surface, &bottomLine, border);
    SDL_FillRect(surface, &left, border);
    SDL_FillRect(surface, &right, border);
}

SDL_Surface* renderTelemetryGraphs(TTF_Font* font, SDL_Color color)
{
    const StreamPipelineTelemetry::GraphSnapshot graphs =
            StreamPipelineTelemetry::graphSnapshot();
    if (!graphs.hasData || font == nullptr) {
        return nullptr;
    }

    struct GraphRow {
        const char* label;
        const StreamPipelineTelemetry::GraphSeries* series;
        bool drawFramePeriod;
    };

    const GraphRow rows[] = {
        {"Host frame interval", &graphs.hostFrameInterval, true},
        {"First packet interval", &graphs.firstPacketInterval, true},
        {"Complete frame interval", &graphs.completeFrameInterval, true},
        {"First packet -> complete", &graphs.firstPacketToComplete, false},
        {"Present interval", &graphs.presentInterval, true},
    };

    const int fontHeight = TTF_FontHeight(font);
    const int titleHeight = fontHeight + 6;
    const int labelHeight = fontHeight + 2;
    const int rowHeight = labelHeight + kTelemetryGraphPlotHeight + kTelemetryGraphRowGap;
    const int width = kTelemetryGraphPanelPadding * 2 +
            static_cast<int>(StreamPipelineTelemetry::kGraphColumns);
    const int height = kTelemetryGraphPanelPadding * 2 + titleHeight +
            static_cast<int>(sizeof(rows) / sizeof(rows[0])) * rowHeight;

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface == nullptr) {
        return nullptr;
    }

    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
    SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 0, 0, 0, 0));

    blitGraphLabel(surface,
                   font,
                   "10 s spike history (0-25 ms, max per bucket)",
                   kTelemetryGraphPanelPadding,
                   kTelemetryGraphPanelPadding,
                   color);

    int y = kTelemetryGraphPanelPadding + titleHeight;
    for (const GraphRow& row : rows) {
        blitGraphLabel(surface,
                       font,
                       row.label,
                       kTelemetryGraphPanelPadding,
                       y,
                       color);
        y += labelHeight;

        drawTelemetryGraph(surface,
                           kTelemetryGraphPanelPadding,
                           y,
                           *row.series,
                           graphs.framePeriodMs,
                           row.drawFramePeriod,
                           color);
        y += kTelemetryGraphPlotHeight + kTelemetryGraphRowGap;
    }

    return surface;
}

SDL_Surface* combineDebugOverlaySurfaces(SDL_Surface* textSurface,
                                         SDL_Surface* graphSurface)
{
    if (graphSurface == nullptr) {
        return textSurface;
    }
    if (textSurface == nullptr) {
        return graphSurface;
    }

    const int width = textSurface->w + kTelemetryGraphSurfaceGap + graphSurface->w;
    const int height = textSurface->h > graphSurface->h ? textSurface->h : graphSurface->h;
    SDL_Surface* combined = SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (combined == nullptr) {
        SDL_FreeSurface(graphSurface);
        return textSurface;
    }

    SDL_SetSurfaceBlendMode(combined, SDL_BLENDMODE_BLEND);
    SDL_FillRect(combined, nullptr, SDL_MapRGBA(combined->format, 0, 0, 0, 0));

    SDL_Rect textDst = {0, 0, textSurface->w, textSurface->h};
    SDL_BlitSurface(textSurface, nullptr, combined, &textDst);

    SDL_Rect graphDst = {textSurface->w + kTelemetryGraphSurfaceGap,
                         0,
                         graphSurface->w,
                         graphSurface->h};
    SDL_BlitSurface(graphSurface, nullptr, combined, &graphDst);

    SDL_FreeSurface(textSurface);
    SDL_FreeSurface(graphSurface);
    return combined;
}

} // namespace
#endif

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

#ifdef HAVE_LATENCY_PROBE
    // OverlayManager is per streaming Session, so this gives the Moonlight-side
    // stream-health counters the same lifetime as the stream rather than the
    // latency benchmark. Common C resets its FEC counters with the RTP queue.
    StreamHealthTelemetry::reset();
#endif

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
#ifdef HAVE_LATENCY_PROBE
    // LatencyProbe is process-global while OverlayManager is per streaming
    // session. Ensure a session ending with the debug OSD still visible cannot
    // leave its SDL event watch/timer or host helper active into the next stream.
    if (m_Overlays[OverlayType::OverlayDebug].enabled) {
        LatencyProbe::instance().setEnabled(false);
    }

    // Stop the async debug-OSD worker before freeing its font or shutting down
    // SDL_ttf. The renderer has already been detached by decoder teardown.
    stopDebugOverlayWorker();
#endif

    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    SDL_utf8strlcpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

#ifdef HAVE_LATENCY_PROBE
void OverlayManager::appendDebugTelemetry(char* text, std::size_t length)
{
    char healthLines[320];
    StreamHealthTelemetry::formatOverlayLines(healthLines, sizeof(healthLines));

    const size_t currentLength = SDL_strlen(text);
    if (currentLength > 0 && text[currentLength - 1] != '\n') {
        SDL_strlcat(text, "\n", length);
    }
    SDL_strlcat(text, healthLines, length);

    char latencyLine[192];
    LatencyProbe::instance().formatOverlayLine(latencyLine, sizeof(latencyLine));
    SDL_strlcat(text, "\n\n", length);
    SDL_strlcat(text, latencyLine, length);

    char pipelineLines[1024];
    StreamPipelineTelemetry::formatOverlayLines(pipelineLines, sizeof(pipelineLines));
    if (pipelineLines[0] != '\0') {
        SDL_strlcat(text, "\n\n", length);
        SDL_strlcat(text, pipelineLines, length);
    }
}

int OverlayManager::debugOverlayThreadEntry(void* opaque)
{
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    static_cast<OverlayManager*>(opaque)->debugOverlayThreadProc();
    return 0;
}

bool OverlayManager::ensureDebugOverlayWorker()
{
    std::lock_guard<std::mutex> lock(m_DebugOverlayMutex);

    if (m_DebugOverlayThread != nullptr) {
        return true;
    }
    if (m_FontData.isEmpty()) {
        return false;
    }

    m_DebugOverlayFont = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                        1,
                                        m_Overlays[OverlayType::OverlayDebug].fontSize);
    if (m_DebugOverlayFont == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL async debug overlay font failed to load: %s",
                    TTF_GetError());
        return false;
    }

    m_DebugOverlayStop = false;
    m_DebugOverlayPending = false;
    m_DebugOverlayThread = SDL_CreateThread(debugOverlayThreadEntry,
                                             "Moonlight debug OSD",
                                             this);
    if (m_DebugOverlayThread == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to create async debug overlay thread: %s",
                    SDL_GetError());
        TTF_CloseFont(m_DebugOverlayFont);
        m_DebugOverlayFont = nullptr;
        return false;
    }

    return true;
}

void OverlayManager::stopDebugOverlayWorker()
{
    SDL_Thread* thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_DebugOverlayMutex);
        if (m_DebugOverlayThread == nullptr) {
            return;
        }

        m_DebugOverlayStop = true;
        m_DebugOverlayPending = false;
        m_DebugOverlayEnabled = false;
        ++m_DebugOverlayGeneration;
        thread = m_DebugOverlayThread;
        m_DebugOverlayCondition.notify_all();
    }

    SDL_WaitThread(thread, nullptr);

    {
        std::lock_guard<std::mutex> lock(m_DebugOverlayMutex);
        m_DebugOverlayThread = nullptr;
        m_DebugOverlayStop = false;
    }

    if (m_DebugOverlayFont != nullptr) {
        TTF_CloseFont(m_DebugOverlayFont);
        m_DebugOverlayFont = nullptr;
    }
}

void OverlayManager::setDebugOverlayWorkerEnabled(bool enabled)
{
    if (enabled && !ensureDebugOverlayWorker()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_DebugOverlayMutex);
    m_DebugOverlayEnabled = enabled;
    m_DebugOverlayPending = false;
    ++m_DebugOverlayGeneration;
    m_DebugOverlayCondition.notify_all();
}

void OverlayManager::invalidateDebugOverlayUpdate()
{
    std::lock_guard<std::mutex> lock(m_DebugOverlayMutex);
    m_DebugOverlayPending = false;
    ++m_DebugOverlayGeneration;
}

bool OverlayManager::queueDebugOverlayUpdate()
{
    std::lock_guard<std::mutex> lock(m_DebugOverlayMutex);
    if (m_DebugOverlayThread == nullptr ||
            m_DebugOverlayStop ||
            !m_DebugOverlayEnabled) {
        return false;
    }

    // The decoder thread does only a bounded text copy and worker wakeup. If the
    // worker is still processing the previous one-second update, overwrite the
    // pending text so updates coalesce instead of building a queue.
    SDL_utf8strlcpy(m_DebugOverlayPendingText,
                    m_Overlays[OverlayType::OverlayDebug].text,
                    sizeof(m_DebugOverlayPendingText));
    m_DebugOverlayPending = true;
    ++m_DebugOverlayGeneration;
    m_DebugOverlayCondition.notify_one();
    return true;
}

void OverlayManager::publishDebugOverlaySurface(SDL_Surface* newSurface,
                                                std::uint64_t generation)
{
    if (newSurface == nullptr) {
        return;
    }

    SDL_Surface* oldSurface = nullptr;
    bool published = false;
    {
        // Serializing renderer callbacks also guarantees that
        // setOverlayRenderer(nullptr) cannot race renderer destruction with this
        // worker callback.
        std::lock_guard<std::mutex> rendererLock(m_RendererMutex);

        bool valid = false;
        {
            std::lock_guard<std::mutex> workerLock(m_DebugOverlayMutex);
            valid = !m_DebugOverlayStop &&
                    m_DebugOverlayEnabled &&
                    generation == m_DebugOverlayGeneration;
        }

        if (valid && m_Renderer != nullptr) {
            oldSurface = (SDL_Surface*)SDL_AtomicSetPtr(
                (void**)&m_Overlays[OverlayType::OverlayDebug].surface,
                newSurface);
            m_Renderer->notifyOverlayUpdated(OverlayType::OverlayDebug);
            published = true;
        }
    }

    if (!published) {
        SDL_FreeSurface(newSurface);
    }
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}

void OverlayManager::debugOverlayThreadProc()
{
    for (;;) {
        char text[sizeof(m_DebugOverlayPendingText)];
        std::uint64_t generation = 0;

        {
            std::unique_lock<std::mutex> lock(m_DebugOverlayMutex);
            m_DebugOverlayCondition.wait(lock, [this]() {
                return m_DebugOverlayStop || m_DebugOverlayPending;
            });

            if (m_DebugOverlayStop) {
                return;
            }

            SDL_utf8strlcpy(text,
                            m_DebugOverlayPendingText,
                            sizeof(text));
            generation = m_DebugOverlayGeneration;
            m_DebugOverlayPending = false;
        }

        // Everything below this point used to execute synchronously on the
        // decoder thread before avcodec_send_packet(). Keep telemetry snapshot,
        // text layout, rasterization, graph generation, and renderer upload off
        // that measured path.
        appendDebugTelemetry(text, sizeof(text));

        SDL_Surface* textSurface = RenderTextOutlinedWrapped(
            m_DebugOverlayFont,
            text,
            m_Overlays[OverlayType::OverlayDebug].color,
            {0, 0, 0, 255},
            4,
            2048);
        SDL_Surface* graphSurface = renderTelemetryGraphs(
            m_DebugOverlayFont,
            m_Overlays[OverlayType::OverlayDebug].color);
        SDL_Surface* newSurface = combineDebugOverlaySurfaces(textSurface, graphSurface);
        publishDebugOverlaySurface(newSurface, generation);
    }
}
#endif

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
#ifdef HAVE_LATENCY_PROBE
    // While telemetry is active, the performance OSD is part of the system being
    // measured. Keep its expensive telemetry scan, SDL_ttf rasterization, and
    // renderer upload off the decoder thread. The worker keeps only the newest
    // pending one-second update.
    if (type == OverlayType::OverlayDebug && m_Overlays[type].enabled) {
        if (StreamPipelineTelemetry::isActiveFast() && queueDebugOverlayUpdate()) {
            return;
        }

        // Prevent a late active-benchmark worker result from overwriting a newer
        // synchronous idle/frozen OSD surface after telemetry has stopped.
        invalidateDebugOverlayUpdate();
        appendDebugTelemetry(m_Overlays[type].text,
                             sizeof(m_Overlays[0].text));
    }
#endif

    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

#ifdef HAVE_LATENCY_PROBE
    if (type == OverlayType::OverlayDebug && stateChanged) {
        setDebugOverlayWorkerEnabled(enabled);

        if (enabled) {
            Session* session = Session::get();
            LatencyBenchmarkControl::configure(session != nullptr ? session->getComputer() : nullptr);
        }
        LatencyProbe::instance().setEnabled(enabled);
    }
#endif

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    // The async debug worker may be inside notifyOverlayUpdated(). Decoder reset
    // detaches the renderer here before deleting it, so serialize the pointer
    // change with worker callbacks to make that lifetime guarantee explicit.
    std::lock_guard<std::mutex> lock(m_RendererMutex);
    m_Renderer = renderer;
}

void OverlayManager::publishOverlaySurface(OverlayType type, SDL_Surface* newSurface)
{
    SDL_Surface* oldSurface = nullptr;
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(m_RendererMutex);
        if (m_Renderer != nullptr) {
            oldSurface = (SDL_Surface*)SDL_AtomicSetPtr(
                (void**)&m_Overlays[type].surface,
                newSurface);
            m_Renderer->notifyOverlayUpdated(type);
            published = true;
        }
    }

    if (!published && newSurface != nullptr) {
        SDL_FreeSurface(newSurface);
    }
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    {
        std::lock_guard<std::mutex> lock(m_RendererMutex);
        if (m_Renderer == nullptr) {
            return;
        }
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    SDL_Surface* newSurface = nullptr;
    if (m_Overlays[type].enabled) {
        // The _Wrapped variant is required for line breaks to work
        newSurface = RenderTextOutlinedWrapped(m_Overlays[type].font,
                                               m_Overlays[type].text,
                                               m_Overlays[type].color,
                                               {0, 0, 0, 255},
                                               4,
                                               2048);
#ifdef HAVE_LATENCY_PROBE
        if (type == OverlayType::OverlayDebug) {
            SDL_Surface* graphSurface = renderTelemetryGraphs(
                    m_Overlays[type].font,
                    m_Overlays[type].color);
            newSurface = combineDebugOverlaySurfaces(newSurface, graphSurface);
        }
#endif
    }

    publishOverlaySurface(type, newSurface);
}

SDL_Surface* OverlayManager::RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    int oldOutline = TTF_GetFontOutline(font);
    TTF_SetFontOutline(font, outlineWidth);

    // Verify that the string won't require wrapping (which could cause the outline and the text
    // to diverge due to different wrapping positions).
    //
    // FIXME: We do this rather than just disabling wrapping entirely (wrapWidth = 0) because we
    // need further testing to ensure that all renderers can handle non-NPOT overlay textures.
    for (const QString& line : QString(text).split('\n')) {
        int extent, count;
        if (TTF_MeasureUTF8(font, line.toUtf8(), wrapWidth, &extent, &count) == 0 && count < line.size()) {
            // If it requires wrapping, render it without the outline
            TTF_SetFontOutline(font, oldOutline);
            return TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
        }
    }

    // Draw text twice, but outline is a bit bigger
    auto outlineSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, outlineColor, wrapWidth);
    TTF_SetFontOutline(font, 0);
    auto textSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
    TTF_SetFontOutline(font, oldOutline);

    if (outlineSurface == nullptr || textSurface == nullptr) {
        SDL_FreeSurface(outlineSurface);
        SDL_FreeSurface(textSurface);
        return nullptr;
    }

    // Merge the texts
    SDL_Rect dst = { outlineWidth, outlineWidth, textSurface->w, textSurface->h };
    SDL_BlitSurface(textSurface, nullptr, outlineSurface, &dst);

    SDL_FreeSurface(textSurface);
    return outlineSurface;
}
