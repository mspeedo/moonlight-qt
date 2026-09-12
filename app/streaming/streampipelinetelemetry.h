#pragma once

#include <Limelight.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

struct AVFrame;

namespace StreamPipelineTelemetry {

struct MetricSnapshot {
    bool valid = false;
    double averageMs = 0.0;
    double maximumMs = 0.0;
    bool runValid = false;
    double runMaximumMs = 0.0;
};

struct Snapshot {
    MetricSnapshot hostFrameInterval;
    MetricSnapshot hostProcessing;
    MetricSnapshot firstPacketInterval;
    MetricSnapshot completeFrameInterval;
    MetricSnapshot firstPacketToComplete;
    MetricSnapshot completeToDecodeStart;
    MetricSnapshot decodeStartToDecoded;
    MetricSnapshot decodedToRenderStart;
    MetricSnapshot renderStartToPresent;
    MetricSnapshot presentInterval;
    std::uint64_t frames = 0;
    bool hasData = false;
};

#if defined(_WIN32) || defined(__APPLE__)

// Pacer is built on all platforms, while this downstream telemetry implementation
// is intentionally built only on non-macOS Unix. Keep Pacer source platform-neutral
// by compiling its telemetry hooks to no-ops where no implementation is linked.
inline bool isActiveFast()
{
    return false;
}

inline std::uint32_t prepareRender(AVFrame*)
{
    return 0;
}

inline void armRenderStart(std::uint64_t)
{
}

inline void recordDecodedToRenderStart(std::uint32_t, std::uint64_t)
{
}

inline void renderEnd()
{
}

inline void pacerDrop()
{
}

#else

extern std::atomic<bool> g_Active;

inline bool isActiveFast()
{
    return g_Active.load(std::memory_order_relaxed);
}

// Pacer resolves the frame identity before taking the render-start timestamp,
// then arms this tiny TLS context immediately before the frontend renderer call.
// This keeps ring/table bookkeeping outside the Render start -> present interval.
struct RenderContextState {
    std::uint32_t frameNumber = 0;
    std::uint64_t renderStartUs = 0;
};

extern thread_local RenderContextState g_RenderContext;

std::uint32_t prepareRender(AVFrame* frame);
inline void armRenderStart(std::uint64_t renderStartUs)
{
    g_RenderContext.renderStartUs = renderStartUs;
}
inline bool presentPendingFast()
{
    return g_RenderContext.frameNumber != 0 && g_RenderContext.renderStartUs != 0;
}
void recordDecodedToRenderStart(std::uint32_t frameNumber, std::uint64_t renderStartUs);
void renderEnd();

// Existing Pacer call sites use this bridge so stream-health accounting remains
// aligned with Moonlight's stock pacerDroppedFrames semantics.
void pacerDrop();

#endif

void clear();
void start();
void stop();

// Decoder wrappers use this to associate the DECODE_UNIT returned by Common C
// with the immediately following avcodec_send_packet() call.
void noteDecodeUnit(PDECODE_UNIT du);
void decodeSubmitted(std::uint64_t decodeStartUs, bool accepted);
void decodedFrame(AVFrame* frame, std::uint64_t decodedUs);

#if !defined(_WIN32) && !defined(__APPLE__)
void presentSuccess(std::uint64_t presentUs);
#endif

Snapshot snapshot();
void formatOverlayLines(char* output, std::size_t length);

} // namespace StreamPipelineTelemetry
