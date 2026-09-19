#include "overlaymanager.h"
#include "path.h"

#include <chrono>
#include <memory>

#if defined(HAVE_LIBPLACEBO_VULKAN) && defined(Q_OS_LINUX)
#include "streaming/displaypresentlatency.h"
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

constexpr int kTelemetryGraphPlotHeight = 88;
constexpr int kTelemetryGraphRowGap = 8;
constexpr int kTelemetryGraphPanelPadding = 8;
constexpr int kTelemetryGraphSurfaceGap = 24;
constexpr float kTelemetryGraphScaleStepMs = 5.0f;
constexpr float kTelemetryGraphMinScaleMs = 5.0f;
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

float graphScaleMaxMs(const StreamPipelineTelemetry::GraphSeries& series)
{
    float maximumMs = 0.0f;
    for (std::size_t i = 0; i < StreamPipelineTelemetry::kGraphColumns; ++i) {
        if (series.valid[i] && series.maximumMs[i] > maximumMs) {
            maximumMs = series.maximumMs[i];
        }
    }

    if (maximumMs <= kTelemetryGraphMinScaleMs) {
        return kTelemetryGraphMinScaleMs;
    }

    const int completeSteps = static_cast<int>(maximumMs / kTelemetryGraphScaleStepMs);
    float scaleMaxMs = static_cast<float>(completeSteps) * kTelemetryGraphScaleStepMs;
    if (scaleMaxMs < maximumMs) {
        scaleMaxMs += kTelemetryGraphScaleStepMs;
    }
    return scaleMaxMs;
}

int graphValueToY(float valueMs, int plotY, float scaleMaxMs)
{
    if (valueMs < 0.0f) {
        valueMs = 0.0f;
    }
    if (valueMs > scaleMaxMs) {
        valueMs = scaleMaxMs;
    }

    const int usableHeight = kTelemetryGraphPlotHeight - 3;
    const int scaled = static_cast<int>(
            (valueMs / scaleMaxMs) * usableHeight + 0.5f);
    return plotY + kTelemetryGraphPlotHeight - 2 - scaled;
}

void drawTelemetryGraph(SDL_Surface* surface,
                        int plotX,
                        int plotY,
                        const StreamPipelineTelemetry::GraphSeries& series,
                        float scaleMaxMs,
                        double framePeriodMs,
                        bool drawFramePeriod,
                        SDL_Color color,
                        bool backgroundOnly)
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
    if (backgroundOnly) {
        SDL_FillRect(surface, &plot, background);

        for (float ms = kTelemetryGraphScaleStepMs;
             ms < scaleMaxMs;
             ms += kTelemetryGraphScaleStepMs) {
            const int y = graphValueToY(ms, plotY, scaleMaxMs);
            SDL_Rect line = {plotX, y,
                             static_cast<int>(StreamPipelineTelemetry::kGraphColumns), 1};
            SDL_FillRect(surface, &line, grid);
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
    else {
        if (drawFramePeriod && framePeriodMs > 0.0 &&
                framePeriodMs < scaleMaxMs) {
            const int y = graphValueToY(static_cast<float>(framePeriodMs), plotY, scaleMaxMs);
            SDL_Rect line = {plotX + 1, y,
                             static_cast<int>(StreamPipelineTelemetry::kGraphColumns) - 2, 1};
            SDL_FillRect(surface, &line, reference);
        }

        const int bottom = plotY + kTelemetryGraphPlotHeight - 2;
        for (std::size_t i = 1; i + 1 < StreamPipelineTelemetry::kGraphColumns; ++i) {
            if (!series.valid[i]) {
                continue;
            }

            const int y = graphValueToY(series.maximumMs[i], plotY, scaleMaxMs);
            SDL_Rect bar = {plotX + static_cast<int>(i),
                            y,
                            1,
                            bottom - y + 1};
            SDL_FillRect(surface, &bar, trace);
        }
    }
}

using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;

struct GraphCache {
    SurfacePtr background {nullptr, SDL_FreeSurface};
    std::array<float, 6> scales {};
};

SDL_Surface* renderTelemetryGraphs(TTF_Font* font, SDL_Color color,
                                   const StreamPipelineTelemetry::GraphSnapshot& graphs,
                                   GraphCache& cache)
{
    if (font == nullptr) {
        return nullptr;
    }

    struct GraphRow {
        const char* label;
        const StreamPipelineTelemetry::GraphSeries* series;
        bool drawFramePeriod;
    };

    StreamPipelineTelemetry::GraphSeries displayPresent;
    DisplayPresentLatency::graphSnapshot(displayPresent);
    const GraphRow rows[] = {
        {"Host frame interval", &graphs.hostFrameInterval, true},
        {"First packet interval", &graphs.firstPacketInterval, true},
        {"Complete frame interval", &graphs.completeFrameInterval, true},
        {"Present interval", &graphs.presentInterval, true},
        {"First packet -> complete", &graphs.firstPacketToComplete, false},
        {"Input -> display present", &displayPresent, false},
    };
    const std::size_t rowCount = sizeof(rows) / sizeof(rows[0]);

    const int fontHeight = TTF_FontHeight(font);
    const int titleHeight = fontHeight + 6;
    const int labelHeight = fontHeight + 2;
    const int rowHeight = labelHeight + kTelemetryGraphPlotHeight + kTelemetryGraphRowGap;
    const int width = kTelemetryGraphPanelPadding * 2 +
            static_cast<int>(StreamPipelineTelemetry::kGraphColumns);
    const int height = kTelemetryGraphPanelPadding * 2 + titleHeight +
            static_cast<int>(rowCount) * rowHeight;

    std::array<float, 6> scales {};
    for (std::size_t i = 0; i < rowCount; ++i) {
        scales[i] = graphScaleMaxMs(*rows[i].series);
    }
    if (!cache.background || cache.scales != scales) {
        cache.background.reset(SDL_CreateRGBSurfaceWithFormat(
                0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
        if (!cache.background) {
            return nullptr;
        }
        cache.scales = scales;
        SDL_Surface* background = cache.background.get();
        // Copy pixels exactly, including alpha, into each upload-owned surface.
        SDL_SetSurfaceBlendMode(background, SDL_BLENDMODE_NONE);
        SDL_FillRect(background, nullptr, 0);
        blitGraphLabel(background, font, "10s history",
                       kTelemetryGraphPanelPadding, kTelemetryGraphPanelPadding, color);
        int y = kTelemetryGraphPanelPadding + titleHeight;
        for (std::size_t i = 0; i < rowCount; ++i) {
            char label[96];
            SDL_snprintf(label, sizeof(label), "%s (0-%.0f ms)",
                         rows[i].label, static_cast<double>(scales[i]));
            blitGraphLabel(background, font, label, kTelemetryGraphPanelPadding, y, color);
            y += labelHeight;
            drawTelemetryGraph(background, kTelemetryGraphPanelPadding, y,
                               *rows[i].series, scales[i], 0, false, color, true);
            y += kTelemetryGraphPlotHeight + kTelemetryGraphRowGap;
        }
    }

    // Upload takes ownership asynchronously, so never overwrite a surface still
    // being read by the GPU transfer. Only this small panel is allocated at 20 Hz.
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return nullptr;
    }
    SDL_BlitSurface(cache.background.get(), nullptr, surface, nullptr);
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
    int y = kTelemetryGraphPanelPadding + titleHeight + labelHeight;
    for (std::size_t i = 0; i < rowCount; ++i) {
        drawTelemetryGraph(surface, kTelemetryGraphPanelPadding, y,
                           *rows[i].series, scales[i], graphs.framePeriodMs,
                           rows[i].drawFramePeriod, color, false);
        y += rowHeight;
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
            0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
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
    // OverlayManager is per streaming Session. Stream-health counters and
    // pipeline telemetry therefore begin fresh for every stream, independently
    // of whether the debug OSD is visible.
    StreamHealthTelemetry::reset();
    StreamPipelineTelemetry::start();
    StreamPipelineTelemetry::setStateChangedCallback(telemetryStateChanged, this);
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
    // session. Force cleanup even if an active benchmark deliberately survived
    // hiding the OSD, so no timer/event watch/helper can leak into the next stream.
    StreamPipelineTelemetry::setStateChangedCallback(nullptr, nullptr);
    LatencyProbe::instance().setEnabled(false, true);

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
void OverlayManager::appendDebugTelemetry(char* text, std::size_t length, std::uint64_t nowUs)
{
    char healthLines[320];
    StreamHealthTelemetry::formatOverlayLines(healthLines, sizeof(healthLines));

    const size_t currentLength = SDL_strlen(text);
    if (currentLength > 0 && text[currentLength - 1] != '\n') {
        SDL_strlcat(text, "\n", length);
    }
    SDL_strlcat(text, healthLines, length);

    char latencyLine[512];
    LatencyProbe::instance().formatOverlayLine(latencyLine, sizeof(latencyLine));

    // Keep the input-latency block contiguous. The probe owns the detailed
    // telemetry state because it knows whether we're live, manually frozen, or
    // in a benchmark state, but display that state immediately above the
    // stream-pipeline table below.
    char telemetryStatus[64] = {};
    char* telemetryLine = SDL_strstr(latencyLine, "\nTelemetry: ");
    if (telemetryLine != nullptr) {
        char* statusStart = telemetryLine + SDL_strlen("\nTelemetry: ");
        char* nextLine = SDL_strchr(statusStart, '\n');
        const size_t statusLength = nextLine != nullptr ?
                static_cast<size_t>(nextLine - statusStart) : SDL_strlen(statusStart);
        const size_t copyLength = statusLength < sizeof(telemetryStatus) - 1 ?
                statusLength : sizeof(telemetryStatus) - 1;
        SDL_memcpy(telemetryStatus, statusStart, copyLength);
        telemetryStatus[copyLength] = '\0';

        if (nextLine != nullptr) {
            SDL_memmove(telemetryLine, nextLine, SDL_strlen(nextLine) + 1);
        }
        else {
            *telemetryLine = '\0';
        }
    }

    SDL_strlcat(text, "\n\n", length);
    SDL_strlcat(text, latencyLine, length);

    char pipelineLines[1024];
    StreamPipelineTelemetry::formatOverlayLines(pipelineLines, sizeof(pipelineLines), nowUs);
    if (pipelineLines[0] != '\0') {
        SDL_strlcat(text, "\n\n", length);
        if (telemetryStatus[0] != '\0') {
            SDL_strlcat(text, "Telemetry: ", length);
            SDL_strlcat(text, telemetryStatus, length);
            SDL_strlcat(text, "\n", length);
        }
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

    // Same lock order as publication: disable cannot race an upload/publication.
    std::lock_guard<std::mutex> rendererLock(m_RendererMutex);
    std::lock_guard<std::mutex> lock(m_DebugOverlayMutex);
    m_DebugOverlayReady = false;
    m_DebugOverlayStatePending = false;
    m_DebugOverlayEnabled = enabled;
    m_DebugOverlayPending = false;
    ++m_DebugOverlayGeneration;
    m_DebugOverlayCondition.notify_all();
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
    m_DebugOverlayCondition.notify_one();
    return true;
}

void OverlayManager::telemetryStateChanged(void* opaque)
{
    auto* manager = static_cast<OverlayManager*>(opaque);
    std::lock_guard<std::mutex> lock(manager->m_DebugOverlayMutex);
    if (manager->m_DebugOverlayEnabled) {
        manager->m_DebugOverlayStatePending = true;
        ++manager->m_DebugOverlayGeneration;
        manager->m_DebugOverlayCondition.notify_one();
    }
}

bool OverlayManager::publishDebugOverlaySurfaces(SDL_Surface* text, SDL_Surface* graph,
                                                std::uint64_t generation,
                                                std::uint64_t revision)
{
    SurfacePtr textOwner(text, SDL_FreeSurface);
    SurfacePtr graphOwner(graph, SDL_FreeSurface);
    std::lock_guard<std::mutex> rendererLock(m_RendererMutex);
    {
        std::lock_guard<std::mutex> workerLock(m_DebugOverlayMutex);
        if (m_DebugOverlayStop || !m_DebugOverlayEnabled ||
                generation != m_DebugOverlayGeneration ||
                revision != StreamPipelineTelemetry::displayRevision() ||
                m_Renderer == nullptr) {
            return false;
        }
    }

    bool published = true;
    if (m_Renderer->supportsDebugGraph()) {
        published = m_Renderer->updateDebugOverlay(textOwner.release(), graphOwner.release());
    }
    else {
        // Other Linux renderers retain their single, low-rate debug surface.
        SDL_Surface* combined = combineDebugOverlaySurfaces(textOwner.release(), graphOwner.release());
        SDL_Surface* old = (SDL_Surface*)SDL_AtomicSetPtr(
                (void**)&m_Overlays[OverlayDebug].surface, combined);
        m_Renderer->notifyOverlayUpdated(OverlayDebug);
        SDL_FreeSurface(old);
    }
    {
        std::lock_guard<std::mutex> workerLock(m_DebugOverlayMutex);
        if (generation == m_DebugOverlayGeneration) {
            m_DebugOverlayReady = published;
        }
    }
    return published;
}

void OverlayManager::debugOverlayThreadProc()
{
    using Clock = std::chrono::steady_clock;
    constexpr auto graphInterval = std::chrono::milliseconds(50);
    auto nextGraph = Clock::time_point::min();
    bool graphLive = false;
    std::uint64_t lastRevision = ~std::uint64_t(0);
    std::uint64_t cacheGeneration = ~std::uint64_t(0);
    StreamPipelineTelemetry::GraphSnapshot graphs;
    GraphCache cache;
    char frozenText[sizeof(m_DebugOverlayPendingText)] = {};
    std::uint64_t frozenTextRevision = ~std::uint64_t(0);

    for (;;) {
        char text[sizeof(m_DebugOverlayPendingText)];
        std::uint64_t generation;
        bool textUpdate;
        bool split;
        bool ready;
        {
            std::unique_lock<std::mutex> lock(m_DebugOverlayMutex);
            const auto waitGeneration = m_DebugOverlayGeneration;
            const auto workPending = [this, waitGeneration]() {
                return m_DebugOverlayStop || m_DebugOverlayGeneration != waitGeneration ||
                        (m_DebugOverlayEnabled && m_DebugOverlayAttached &&
                         (m_DebugOverlayPending || m_DebugOverlayStatePending));
            };
            if (m_DebugOverlayEnabled && m_DebugOverlayAttached &&
                    m_DebugOverlayReady && m_DebugOverlaySplit && graphLive) {
                m_DebugOverlayCondition.wait_until(lock, nextGraph, workPending);
            }
            else {
                m_DebugOverlayCondition.wait(lock, workPending);
            }
            if (m_DebugOverlayStop) {
                return;
            }
            if (!m_DebugOverlayEnabled || !m_DebugOverlayAttached) {
                continue;
            }
            if (!m_DebugOverlayPending && !m_DebugOverlayStatePending && !m_DebugOverlayReady) {
                // Enable invalidates the old surface before benchmark visibility
                // is updated. Wait for its explicit first-text request.
                continue;
            }
            textUpdate = m_DebugOverlayPending || m_DebugOverlayStatePending || !m_DebugOverlayReady;
            ready = m_DebugOverlayReady;
            split = m_DebugOverlaySplit;
            generation = m_DebugOverlayGeneration;
            SDL_utf8strlcpy(text, m_DebugOverlayPendingText, sizeof(text));
            m_DebugOverlayPending = false;
            m_DebugOverlayStatePending = false;
        }

        const std::uint64_t revision = StreamPipelineTelemetry::displayRevision();
        if (revision & 1) {
            // The control-plane callback will wake us when the transition ends.
            graphLive = false;
            continue;
        }
        const bool live = StreamPipelineTelemetry::isActiveFast();
        const bool stateChanged = revision != lastRevision;
        const bool reopened = generation != cacheGeneration;
        if (reopened) {
            cache.background.reset();
        }
        if (!live && !stateChanged && !reopened && ready) {
            // Frozen text and graph stay byte-for-byte unchanged. The existing
            // one-second stats notification cannot start a graph timer or upload.
            graphLive = false;
            continue;
        }
        const bool graphUpdate = !ready || !split || stateChanged || reopened ||
                (live && Clock::now() >= nextGraph);
        if (!textUpdate && !graphUpdate) {
            continue;
        }
        const std::uint64_t nowUs = StreamPipelineTelemetry::snapshotTimeUs();
        if (graphUpdate) {
            if (live || stateChanged) {
                graphs = StreamPipelineTelemetry::graphSnapshot(nowUs);
            }
        }
        SurfacePtr textSurface(nullptr, SDL_FreeSurface);
        if (textUpdate) {
            if (!live && frozenTextRevision == revision) {
                SDL_utf8strlcpy(text, frozenText, sizeof(text));
            }
            else {
                appendDebugTelemetry(text, sizeof(text), nowUs);
                if (!live) {
                    SDL_utf8strlcpy(frozenText, text, sizeof(frozenText));
                    frozenTextRevision = revision;
                }
            }
            textSurface.reset(RenderTextOutlinedWrapped(m_DebugOverlayFont, text,
                    m_Overlays[OverlayDebug].color, {0, 0, 0, 255}, 4, 0));
            if (!textSurface) {
                graphLive = false;
                continue;
            }
        }
        SurfacePtr graphSurface(nullptr, SDL_FreeSurface);
        if (graphUpdate) {
            graphSurface.reset(renderTelemetryGraphs(m_DebugOverlayFont,
                    m_Overlays[OverlayDebug].color, graphs, cache));
            if (!graphSurface) {
                graphLive = false;
                continue;
            }
        }
        // A reset/freeze during snapshot or rasterization invalidates the whole
        // pair. Publication rechecks both control revision and renderer generation.
        const bool published = publishDebugOverlaySurfaces(textSurface.release(),
                graphSurface.release(), generation, revision);
        if (published) {
            lastRevision = revision;
            cacheGeneration = generation;
        }
        graphLive = published && live;
        if (graphUpdate) {
            // Completion-relative deadline: never queue or catch up missed ticks.
            nextGraph = Clock::now() + graphInterval;
        }
    }
}
#endif

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
#ifdef HAVE_LATENCY_PROBE
    // Keep debug OSD updates off the decoder thread in active, idle, and frozen
    // states. The worker keeps only the newest pending one-second update.
    if (type == OverlayType::OverlayDebug && m_Overlays[type].enabled) {
        if (queueDebugOverlayUpdate()) {
            return;
        }

        // Worker failure must not move telemetry/rasterization onto the decoder.
        return;
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
        const bool benchmarkWasActive = LatencyProbe::instance().setEnabled(enabled);

        if (!enabled &&
                (benchmarkWasActive || !StreamPipelineTelemetry::isActiveFast())) {
            // Leave continuous stream telemetry untouched when the OSD was only
            // viewed. Restart only after an active benchmark, or after B froze
            // a completed benchmark run.
            StreamPipelineTelemetry::start();
        }
    }
#endif

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

#ifdef HAVE_LATENCY_PROBE
        // On debug-OSD enable, let the async worker build and publish the first
        // complete text+graph surface. Rendering synchronously here can expose a
        // graph-only surface because graph history exists before telemetry text
        // has been appended. Queueing it atomically makes both appear together.
        if (type == OverlayType::OverlayDebug && enabled) {
            queueDebugOverlayUpdate();
            return;
        }
#endif

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
#ifdef HAVE_LATENCY_PROBE
    std::lock_guard<std::mutex> workerLock(m_DebugOverlayMutex);
    m_DebugOverlayAttached = renderer != nullptr;
    m_DebugOverlaySplit = renderer != nullptr && renderer->supportsDebugGraph();
    m_DebugOverlayReady = false;
    m_DebugOverlayStatePending = m_DebugOverlayEnabled;
    ++m_DebugOverlayGeneration;
    m_DebugOverlayCondition.notify_one();
#endif
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
        // The _Wrapped variant is required for line breaks to work. The telemetry
        // debug OSD uses wrapLength 0 so SDL_ttf sizes the surface to the actual
        // longest line instead of padding multiline text to 2048 pixels. This
        // keeps the graph panel adjacent to the visible text rather than off-screen.
        int wrapWidth = 2048;
#ifdef HAVE_LATENCY_PROBE
        if (type == OverlayType::OverlayDebug) {
            wrapWidth = 0;
        }
#endif
        newSurface = RenderTextOutlinedWrapped(m_Overlays[type].font,
                                               m_Overlays[type].text,
                                               m_Overlays[type].color,
                                               {0, 0, 0, 255},
                                               4,
                                               wrapWidth);
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
    // to diverge due to different wrapping positions). With wrapWidth 0, SDL_ttf is newline-aware
    // but does not wrap by width, so this check is unnecessary.
    //
    // FIXME: We do this rather than just disabling wrapping entirely (wrapWidth = 0) because we
    // need further testing to ensure that all renderers can handle non-NPOT overlay textures.
    if (wrapWidth > 0) {
        for (const QString& line : QString(text).split('\n')) {
            int extent, count;
            if (TTF_MeasureUTF8(font, line.toUtf8(), wrapWidth, &extent, &count) == 0 && count < line.size()) {
                // If it requires wrapping, render it without the outline
                TTF_SetFontOutline(font, oldOutline);
                return TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
            }
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
