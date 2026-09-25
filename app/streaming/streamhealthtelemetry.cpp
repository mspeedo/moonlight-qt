#include "streamhealthtelemetry.h"

extern "C" {
#include <FecFrameStats.h>
}

#include <atomic>
#include <cstdio>

namespace StreamHealthTelemetry {
namespace {

std::atomic<std::uint32_t> g_LastDecodeUnitFrameNumber { 0 };
std::atomic<std::uint64_t> g_NetworkFrameDrops { 0 };
std::atomic<std::uint64_t> g_PacerFrameDrops { 0 };

} // namespace

void reset()
{
    g_LastDecodeUnitFrameNumber.store(0, std::memory_order_relaxed);
    g_NetworkFrameDrops.store(0, std::memory_order_relaxed);
    g_PacerFrameDrops.store(0, std::memory_order_relaxed);
}

void noteDecodeUnit(std::uint32_t frameNumber)
{
    const std::uint32_t previousFrameNumber =
            g_LastDecodeUnitFrameNumber.load(std::memory_order_relaxed);

    // Match Moonlight's stock networkDroppedFrames semantics: the first decode
    // unit establishes the baseline, and only later gaps count as network drops.
    // Frames omitted before the first delivered decode unit may have been dropped
    // intentionally while waiting for an IDR/RFI frame rather than by the network.
    if (previousFrameNumber != 0 && frameNumber > previousFrameNumber + 1) {
        g_NetworkFrameDrops.fetch_add(
            static_cast<std::uint64_t>(frameNumber - previousFrameNumber - 1),
            std::memory_order_relaxed);
    }

    // Common C delivers decode units in frame order. Keep the guard so a
    // duplicate or unexpected stale frame can never move our baseline backwards.
    if (frameNumber > previousFrameNumber) {
        g_LastDecodeUnitFrameNumber.store(frameNumber, std::memory_order_relaxed);
    }
}

void pacerFrameDrop()
{
    g_PacerFrameDrops.fetch_add(1, std::memory_order_relaxed);
}

void formatOverlayLines(char* output, std::size_t length)
{
    if (output == nullptr || length == 0) {
        return;
    }

    FEC_FRAME_STATS fecStats {};
    LiGetFecFrameStats(&fecStats);
    const std::uint32_t recoveredFrames = fecStats.recoveredFrames;
    const std::uint32_t failedFrames = fecStats.failedFrames;
    const std::uint64_t fecOutcomeFrames =
            static_cast<std::uint64_t>(recoveredFrames) + failedFrames;

    char success[32];
    if (fecOutcomeFrames != 0) {
        std::snprintf(success,
                      sizeof(success),
                      "%.1f%%",
                      (static_cast<double>(recoveredFrames) * 100.0) /
                          static_cast<double>(fecOutcomeFrames));
    }
    else {
        std::snprintf(success, sizeof(success), "N/A");
    }

    std::snprintf(output,
                  length,
                  "Stream health\n"
                  "  FEC frames: recovered %u | failed %u | success %s\n"
                  "  Network frame drops: %llu\n"
                  "  Pacer frame drops: %llu",
                  recoveredFrames,
                  failedFrames,
                  success,
                  static_cast<unsigned long long>(
                      g_NetworkFrameDrops.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_PacerFrameDrops.load(std::memory_order_relaxed)));
}

} // namespace StreamHealthTelemetry
