#include "streamhealthtelemetry.h"

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include "gamemodecontrol.h"
#include "threadpriority.h"
#endif

extern "C" {
#include <FecFrameStats.h>
}

#include <atomic>
#include <cstdio>
#include <cstring>

namespace StreamHealthTelemetry {
namespace {

std::atomic<std::uint32_t> g_LastDecodeUnitFrameNumber { 0 };
std::atomic<std::uint64_t> g_NetworkFrameDrops { 0 };
std::atomic<std::uint64_t> g_PacerFrameDrops { 0 };
// This is lifecycle state, not a session counter. Moonlight may construct and
// destroy multiple Vulkan renderer candidates while selecting the live decoder,
// so a single boolean can be cleared by a discarded candidate even while the
// selected extrapolator is still alive.
std::atomic<int> g_FrameExtrapolationActiveInstances { 0 };
std::atomic<std::uint64_t> g_FrameExtrapolationOpportunities { 0 };
std::atomic<std::uint64_t> g_FrameExtrapolationAnalysisBusySkips { 0 };
std::atomic<std::uint64_t> g_FrameExtrapolationRejectNoMotion { 0 };
std::atomic<std::uint64_t> g_FrameExtrapolationRejectTiming { 0 };
std::atomic<std::uint64_t> g_FrameExtrapolationRejectGpuBusy { 0 };
std::atomic<std::uint64_t> g_FrameExtrapolationRejectState { 0 };
std::atomic<std::uint64_t> g_FrameExtrapolated { 0 };

} // namespace

void reset()
{
    g_LastDecodeUnitFrameNumber.store(0, std::memory_order_relaxed);
    g_NetworkFrameDrops.store(0, std::memory_order_relaxed);
    g_PacerFrameDrops.store(0, std::memory_order_relaxed);
    // Do not reset g_FrameExtrapolationActiveInstances here. Extrapolator
    // lifetime is independent of OSD/session-counter reset ordering.
    g_FrameExtrapolationOpportunities.store(0, std::memory_order_relaxed);
    g_FrameExtrapolationAnalysisBusySkips.store(0, std::memory_order_relaxed);
    g_FrameExtrapolationRejectNoMotion.store(0, std::memory_order_relaxed);
    g_FrameExtrapolationRejectTiming.store(0, std::memory_order_relaxed);
    g_FrameExtrapolationRejectGpuBusy.store(0, std::memory_order_relaxed);
    g_FrameExtrapolationRejectState.store(0, std::memory_order_relaxed);
    g_FrameExtrapolated.store(0, std::memory_order_relaxed);
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

void setFrameExtrapolationActive(bool active)
{
    if (active) {
        g_FrameExtrapolationActiveInstances.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Clamp at zero so an unusual teardown/reset ordering can never underflow
    // the diagnostic state and make a later live instance appear active forever.
    int count = g_FrameExtrapolationActiveInstances.load(std::memory_order_relaxed);
    while (count > 0 &&
           !g_FrameExtrapolationActiveInstances.compare_exchange_weak(
                   count,
                   count - 1,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
    }
}

void frameExtrapolationOpportunity()
{
    g_FrameExtrapolationOpportunities.fetch_add(1, std::memory_order_relaxed);
}

void frameExtrapolationAnalysisBusySkip()
{
    g_FrameExtrapolationAnalysisBusySkips.fetch_add(1, std::memory_order_relaxed);
}

void frameExtrapolationRejectNoMotion()
{
    g_FrameExtrapolationRejectNoMotion.fetch_add(1, std::memory_order_relaxed);
}

void frameExtrapolationRejectTiming()
{
    g_FrameExtrapolationRejectTiming.fetch_add(1, std::memory_order_relaxed);
}

void frameExtrapolationRejectGpuBusy()
{
    g_FrameExtrapolationRejectGpuBusy.fetch_add(1, std::memory_order_relaxed);
}

void frameExtrapolationRejectState()
{
    g_FrameExtrapolationRejectState.fetch_add(1, std::memory_order_relaxed);
}

void frameExtrapolated()
{
    g_FrameExtrapolated.fetch_add(1, std::memory_order_relaxed);
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

    const int activeExtrapolators =
            g_FrameExtrapolationActiveInstances.load(std::memory_order_relaxed);

    std::snprintf(output,
                  length,
                  "Stream health\n"
                  "  FEC frames: recovered %u | failed %u | success %s\n"
                  "  Network frame drops: %llu\n"
                  "  Pacer frame drops: %llu\n"
                  "Frame extrapolation: %s | presented %llu\n"
                  "  opportunities %llu | analysis busy skips %llu\n"
                  "  rejects: no motion %llu | timing %llu | GPU busy %llu | state %llu",
                  recoveredFrames,
                  failedFrames,
                  success,
                  static_cast<unsigned long long>(
                      g_NetworkFrameDrops.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_PacerFrameDrops.load(std::memory_order_relaxed)),
                  activeExtrapolators > 0 ? "active" : "inactive",
                  static_cast<unsigned long long>(
                      g_FrameExtrapolated.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_FrameExtrapolationOpportunities.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_FrameExtrapolationAnalysisBusySkips.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_FrameExtrapolationRejectNoMotion.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_FrameExtrapolationRejectTiming.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_FrameExtrapolationRejectGpuBusy.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_FrameExtrapolationRejectState.load(std::memory_order_relaxed)));
#if defined(Q_OS_LINUX)
    if (GameModeControl::isEnabledFast()) {
        const std::size_t used = std::strlen(output);
        std::snprintf(output + used,
                      length - used,
                      "\nGameMode: %s",
                      GameModeControl::stateText());
    }

    if (ThreadPriority::isEnabledFast()) {
        char priorityLines[160];
        ThreadPriority::formatOverlayLines(priorityLines, sizeof(priorityLines));

        // Indent the existing per-thread status lines under a compact section
        // heading without changing their status formatting.
        char indentedPriorityLines[192] = {};
        std::size_t sourceOffset = 0;
        std::size_t destOffset = 0;
        while (priorityLines[sourceOffset] != '\0' &&
               destOffset + 1 < sizeof(indentedPriorityLines)) {
            if ((sourceOffset == 0 || priorityLines[sourceOffset - 1] == '\n') &&
                destOffset + 2 < sizeof(indentedPriorityLines)) {
                indentedPriorityLines[destOffset++] = ' ';
                indentedPriorityLines[destOffset++] = ' ';
            }
            indentedPriorityLines[destOffset++] = priorityLines[sourceOffset++];
        }
        indentedPriorityLines[destOffset] = '\0';

        const std::size_t used = std::strlen(output);
        std::snprintf(output + used,
                      length - used,
                      "\nThread priorities\n%s",
                      indentedPriorityLines);
    }
#endif
}

} // namespace StreamHealthTelemetry
