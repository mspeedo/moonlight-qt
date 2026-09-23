#pragma once

#include <cstddef>
#include <cstdint>

namespace StreamHealthTelemetry {

// Reset Moonlight-side counters for a new streaming session. Common C resets
// its FEC frame counters when the video RTP queue is initialized.
void reset();

// Called once for each decode unit delivered by Common C. Frame-number gaps are
// counted as network frame drops for the lifetime of the current stream.
void noteDecodeUnit(std::uint32_t frameNumber);

// Called at the same two Pacer catch-up drop sites used by stock
// VIDEO_STATS::pacerDroppedFrames.
void pacerFrameDrop();

// Frame-extrapolation diagnostics. These are relaxed session counters used only
// by the debug OSD and must never block or affect the render path.
void setFrameExtrapolationActive(bool active);
void frameExtrapolationDeadlineMiss();
void frameExtrapolationDeadlineWake(std::uint64_t latenessUs);
void frameExtrapolationSubmitDelay(std::uint64_t delayUs);
void frameExtrapolationCancelledByReal();
void frameExtrapolationOpportunity();
void frameExtrapolationAnalysisBusySkip();
void frameExtrapolationRejectNoMotion();
void frameExtrapolationRejectTiming();
void frameExtrapolationRejectGpuBusy();
void frameExtrapolationRejectState();

// Ground-truth quality diagnostics. The generation token prevents an
// asynchronous GPU readback from a previous session from contaminating the
// current session counters. Errors are normalized luma MAE in [0, 1].
std::uint64_t frameExtrapolationQualityGeneration();
void frameExtrapolationQualitySample(std::uint64_t generation,
                                     float syntheticMae,
                                     float holdMae,
                                     float betterFraction);
void frameExtrapolationQualitySkip();

// Called only after a synthetic frame has been successfully submitted by the
// Vulkan extrapolation path. Failed trigger/analysis attempts are not counted.
void frameExtrapolated();

// Format stream-lifetime health counters for the debug OSD. This block is
// independent of the latency benchmark and is shown above benchmark telemetry.
void formatOverlayLines(char* output, std::size_t length);

} // namespace StreamHealthTelemetry
