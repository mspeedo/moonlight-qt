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

// Format stream-lifetime health counters for the debug OSD. This block is
// independent of the latency benchmark and is shown above benchmark telemetry.
void formatOverlayLines(char* output, std::size_t length);

} // namespace StreamHealthTelemetry
