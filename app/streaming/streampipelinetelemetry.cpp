#include "streampipelinetelemetry.h"
#include "streamhealthtelemetry.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <string>

namespace StreamPipelineTelemetry {
namespace {

constexpr std::uint64_t kWindowUs = 10'000'000;
constexpr std::size_t kRingCapacity = 2048;
constexpr std::size_t kDecodeQueueCapacity = 64;
constexpr int kMetricLabelWidth = 34;

struct SampleSlot {
    std::atomic<std::uint64_t> serial {0};
    std::atomic<std::uint64_t> completedUs {0};
    std::atomic<std::uint64_t> durationUs {0};
};

class MetricRing {
public:
    void reset()
    {
        m_NextSerial.store(0, std::memory_order_relaxed);
        m_RunMaximumUs.store(0, std::memory_order_relaxed);
        for (auto& slot : m_Slots) {
            slot.serial.store(0, std::memory_order_relaxed);
            slot.completedUs.store(0, std::memory_order_relaxed);
            slot.durationUs.store(0, std::memory_order_relaxed);
        }
    }

    void add(std::uint64_t completedUs, std::uint64_t durationUs)
    {
        // Keep an independent maximum for the whole benchmark run. Most samples
        // only pay the relaxed load; the CAS loop runs only when a new record is
        // observed. This survives rolling-window expiry and ring wrap.
        std::uint64_t runMaximumUs = m_RunMaximumUs.load(std::memory_order_relaxed);
        while (durationUs > runMaximumUs &&
               !m_RunMaximumUs.compare_exchange_weak(runMaximumUs,
                                                      durationUs,
                                                      std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
        }

        const std::uint64_t serial = m_NextSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        SampleSlot& slot = m_Slots[(serial - 1) % kRingCapacity];

        // Publish serial last. Readers validate it before and after copying the
        // atomics, so reuse of a wrapped slot can only cause that slot to be
        // skipped for one snapshot, never produce a torn sample.
        slot.serial.store(0, std::memory_order_relaxed);
        slot.completedUs.store(completedUs, std::memory_order_relaxed);
        slot.durationUs.store(durationUs, std::memory_order_relaxed);
        slot.serial.store(serial, std::memory_order_release);
    }

    MetricSnapshot snapshot(std::uint64_t nowUs) const
    {
        MetricSnapshot result;
        long double totalUs = 0.0;
        std::uint64_t maxUs = 0;
        std::uint64_t count = 0;

        for (const auto& slot : m_Slots) {
            const std::uint64_t serialBefore = slot.serial.load(std::memory_order_acquire);
            if (serialBefore == 0) {
                continue;
            }

            const std::uint64_t completedUs = slot.completedUs.load(std::memory_order_relaxed);
            const std::uint64_t durationUs = slot.durationUs.load(std::memory_order_relaxed);
            const std::uint64_t serialAfter = slot.serial.load(std::memory_order_acquire);

            if (serialBefore != serialAfter || serialAfter == 0) {
                continue;
            }
            if (completedUs > nowUs || nowUs - completedUs > kWindowUs) {
                continue;
            }

            totalUs += durationUs;
            count++;
            if (!result.valid || durationUs > maxUs) {
                maxUs = durationUs;
            }
            result.valid = true;
        }

        if (result.valid) {
            result.averageMs = static_cast<double>(totalUs / count) / 1000.0;
            result.maximumMs = static_cast<double>(maxUs) / 1000.0;
        }

        result.runValid = m_NextSerial.load(std::memory_order_relaxed) != 0;
        if (result.runValid) {
            result.runMaximumMs =
                    static_cast<double>(m_RunMaximumUs.load(std::memory_order_relaxed)) / 1000.0;
        }

        return result;
    }

private:
    std::array<SampleSlot, kRingCapacity> m_Slots;
    std::atomic<std::uint64_t> m_NextSerial {0};
    std::atomic<std::uint64_t> m_RunMaximumUs {0};
};

struct DecodeStartSlot {
    std::atomic<std::uint32_t> frameNumber {0};
    std::atomic<std::uint64_t> startUs {0};
};

struct CurrentDecodeUnit {
    bool valid = false;
    std::uint32_t frameNumber = 0;
    std::uint64_t receiveTimeUs = 0;
    std::uint64_t enqueueTimeUs = 0;
};

struct DecodeQueue {
    std::array<std::uint32_t, kDecodeQueueCapacity> frames {};
    std::size_t read = 0;
    std::size_t count = 0;

    void clear()
    {
        read = 0;
        count = 0;
    }

    void push(std::uint32_t frameNumber)
    {
        if (count == kDecodeQueueCapacity) {
            read = (read + 1) % kDecodeQueueCapacity;
            count--;
        }
        frames[(read + count) % kDecodeQueueCapacity] = frameNumber;
        count++;
    }

    bool pop(std::uint32_t& frameNumber)
    {
        if (count == 0) {
            return false;
        }
        frameNumber = frames[read];
        read = (read + 1) % kDecodeQueueCapacity;
        count--;
        return true;
    }
};

struct DecodedTimestampSlot {
    std::atomic<std::uint32_t> frameNumber {0};
    std::atomic<std::uint64_t> decodedUs {0};
};

MetricRing g_HostFrameInterval;
MetricRing g_HostProcessing;
MetricRing g_FirstPacketInterval;
MetricRing g_CompleteFrameInterval;
MetricRing g_FirstPacketToComplete;
MetricRing g_CompleteToDecodeStart;
MetricRing g_DecodeStartToDecoded;
MetricRing g_DecodedToRenderStart;
MetricRing g_RenderStartToPresent;
MetricRing g_PresentInterval;
std::array<DecodeStartSlot, kRingCapacity> g_DecodeStarts;
std::array<DecodedTimestampSlot, kRingCapacity> g_DecodedTimestamps;

thread_local CurrentDecodeUnit g_CurrentDecodeUnit;
thread_local DecodeQueue g_DecodeQueue;
thread_local std::uint32_t g_LastDecodeUnitFrameNumber = 0;
thread_local std::uint32_t g_LastAcceptedNetworkFrameNumber = 0;
thread_local std::uint64_t g_LastFirstPacketUs = 0;
thread_local std::uint64_t g_LastCompleteFrameUs = 0;
thread_local std::uint64_t g_LastRealPresentationTimeUs = 0;
thread_local bool g_HaveLastRealPresentationTime = false;
thread_local bool g_HostFrameSequenceContinuous = true;
thread_local std::uint64_t g_DecodeGeneration = 0;
thread_local std::uint64_t g_LastPresentUs = 0;

std::atomic<std::uint64_t> g_Generation {1};
std::atomic<std::uint64_t> g_FrozenNowUs {0};
std::atomic<std::uint64_t> g_Frames {0};

void syncDecodeGeneration()
{
    const std::uint64_t generation = g_Generation.load(std::memory_order_acquire);
    if (g_DecodeGeneration != generation) {
        g_CurrentDecodeUnit = {};
        g_DecodeQueue.clear();
        g_LastDecodeUnitFrameNumber = 0;
        g_LastAcceptedNetworkFrameNumber = 0;
        g_LastFirstPacketUs = 0;
        g_LastCompleteFrameUs = 0;
        g_LastRealPresentationTimeUs = 0;
        g_HaveLastRealPresentationTime = false;
        g_HostFrameSequenceContinuous = true;
        g_DecodeGeneration = generation;
    }
}

std::uint32_t frameNumberFromAvFrame(const AVFrame* frame)
{
    if (frame == nullptr || frame->opaque == nullptr) {
        return 0;
    }

    const auto encoded = reinterpret_cast<std::uintptr_t>(frame->opaque);
    return encoded == 0 ? 0 : static_cast<std::uint32_t>(encoded - 1);
}

void resetClientState()
{
    g_HostFrameInterval.reset();
    g_HostProcessing.reset();
    g_FirstPacketInterval.reset();
    g_CompleteFrameInterval.reset();
    g_FirstPacketToComplete.reset();
    g_CompleteToDecodeStart.reset();
    g_DecodeStartToDecoded.reset();
    g_DecodedToRenderStart.reset();
    g_RenderStartToPresent.reset();
    g_PresentInterval.reset();

    for (auto& slot : g_DecodeStarts) {
        slot.frameNumber.store(0, std::memory_order_relaxed);
        slot.startUs.store(0, std::memory_order_relaxed);
    }
    for (auto& slot : g_DecodedTimestamps) {
        slot.frameNumber.store(0, std::memory_order_relaxed);
        slot.decodedUs.store(0, std::memory_order_relaxed);
    }

    g_FrozenNowUs.store(0, std::memory_order_relaxed);
    g_Frames.store(0, std::memory_order_relaxed);

    g_CurrentDecodeUnit = {};
    g_DecodeQueue.clear();
    g_RenderContext = {};
}

void appendMetric(char* output,
                  std::size_t length,
                  const char* label,
                  const MetricSnapshot& metric)
{
    if (length == 0) {
        return;
    }

    const std::size_t used = std::char_traits<char>::length(output);
    if (used >= length - 1) {
        return;
    }

    if (metric.valid && metric.runValid) {
        std::snprintf(output + used, length - used,
                      "%-*s %7.2f %7.2f %7.2f ms\n",
                      kMetricLabelWidth, label,
                      metric.averageMs, metric.maximumMs, metric.runMaximumMs);
    }
    else if (metric.runValid) {
        std::snprintf(output + used, length - used,
                      "%-*s %7s %7s %7.2f ms\n",
                      kMetricLabelWidth, label,
                      "N/A", "N/A", metric.runMaximumMs);
    }
    else {
        std::snprintf(output + used, length - used,
                      "%-*s %7s %7s %7s\n",
                      kMetricLabelWidth, label,
                      "N/A", "N/A", "N/A");
    }
}

} // namespace

std::atomic<bool> g_Active {false};
thread_local RenderContextState g_RenderContext;

void clear()
{
    g_Active.store(false, std::memory_order_release);
    // Invalidate decoder-thread TLS from any previous run. The decoder thread
    // observes this lazily on its next active telemetry hook, avoiding locks or
    // cross-thread queue manipulation at benchmark start/stop.
    g_Generation.fetch_add(1, std::memory_order_acq_rel);
    resetClientState();
}

void start()
{
    clear();
    g_Active.store(true, std::memory_order_release);
}

void stop()
{
    if (g_Active.exchange(false, std::memory_order_acq_rel)) {
        g_FrozenNowUs.store(LiGetMicroseconds(), std::memory_order_release);
    }
    g_CurrentDecodeUnit = {};
    g_DecodeQueue.clear();
    g_RenderContext = {};
}

void noteDecodeUnit(PDECODE_UNIT du)
{
    syncDecodeGeneration();

    // The wrapper's relaxed active gate happens immediately before this call,
    // but physical-A release can race that tiny interval on another thread.
    // Recheck here so benchmark-only timing state can never change after freeze.
    if (!g_Active.load(std::memory_order_relaxed)) {
        g_CurrentDecodeUnit = {};
        return;
    }

    if (du == nullptr) {
        g_CurrentDecodeUnit = {};
        return;
    }

    const std::uint32_t frameNumber = du->frameNumber;
    const std::uint64_t hostProcessingUs =
            static_cast<std::uint64_t>(du->frameHostProcessingLatency) * 100;
    const bool networkGapBeforeCurrent =
            g_LastDecodeUnitFrameNumber != 0 &&
            frameNumber > g_LastDecodeUnitFrameNumber + 1;

    // A missing decode-unit frame means we cannot know whether an unseen host
    // timestamp existed in the gap. Invalidate only the next host-frame interval
    // sample so network loss cannot masquerade as a host pacing hitch. The
    // session-lifetime drop count is maintained separately by StreamHealthTelemetry.
    if (networkGapBeforeCurrent) {
        g_HostFrameSequenceContinuous = false;
    }

    // A nonzero host-processing value identifies a real captured frame rather
    // than Sunshine's repeated-frame fallback. presentationTimeUs follows the
    // host frame timestamp, so consecutive real values expose host frame pacing.
    // Repeated frames are deliberately bridged: the next real frame then exposes
    // the full time since the previous real capture event.
    if (hostProcessingUs != 0) {
        if (g_HaveLastRealPresentationTime &&
                g_HostFrameSequenceContinuous &&
                du->presentationTimeUs > g_LastRealPresentationTimeUs) {
            g_HostFrameInterval.add(
                        du->enqueueTimeUs,
                        du->presentationTimeUs - g_LastRealPresentationTimeUs);
        }
        g_LastRealPresentationTimeUs = du->presentationTimeUs;
        g_HaveLastRealPresentationTime = true;
        g_HostFrameSequenceContinuous = true;

        // Sunshine already carries host processing latency in every non-repeated
        // video frame in 0.1 ms units. Reuse that in-band value instead of polling
        // a separate telemetry endpoint. Timestamp the sample with the local frame
        // completion time only for the rolling-window age calculation.
        g_HostProcessing.add(du->enqueueTimeUs, hostProcessingUs);
    }

    g_LastDecodeUnitFrameNumber = frameNumber;

    g_CurrentDecodeUnit.valid = true;
    g_CurrentDecodeUnit.frameNumber = frameNumber;
    g_CurrentDecodeUnit.receiveTimeUs = du->receiveTimeUs;
    g_CurrentDecodeUnit.enqueueTimeUs = du->enqueueTimeUs;
}

void decodeSubmitted(std::uint64_t decodeStartUs, bool accepted)
{
    // The timestamp was taken immediately before the real avcodec_send_packet()
    // call, so the Complete -> decode start endpoint is exact. The small fixed
    // bookkeeping below happens after send_packet() returns and remains
    // allocation-free to minimize benchmark perturbation.
    syncDecodeGeneration();
    const CurrentDecodeUnit du = g_CurrentDecodeUnit;
    g_CurrentDecodeUnit = {};

    // If the benchmark stopped while the decoder call was in flight, freeze at
    // release and discard this boundary frame. Rejected submissions are not
    // normal decoded frames and are deliberately excluded too.
    if (!accepted || !g_Active.load(std::memory_order_relaxed) ||
            !du.valid || du.frameNumber == 0) {
        return;
    }

    // Measure cadence at the two existing network boundaries without taking any
    // additional clocks. Only compare consecutive accepted frame numbers: a lost
    // frame is handled by the stream-health drop counter and must not create an
    // artificial 2x interval maximum here.
    if (g_LastAcceptedNetworkFrameNumber != 0 &&
            du.frameNumber == g_LastAcceptedNetworkFrameNumber + 1) {
        if (du.receiveTimeUs > g_LastFirstPacketUs) {
            g_FirstPacketInterval.add(
                        du.receiveTimeUs,
                        du.receiveTimeUs - g_LastFirstPacketUs);
        }
        if (du.enqueueTimeUs > g_LastCompleteFrameUs) {
            g_CompleteFrameInterval.add(
                        du.enqueueTimeUs,
                        du.enqueueTimeUs - g_LastCompleteFrameUs);
        }
    }
    g_LastAcceptedNetworkFrameNumber = du.frameNumber;
    g_LastFirstPacketUs = du.receiveTimeUs;
    g_LastCompleteFrameUs = du.enqueueTimeUs;

    if (du.enqueueTimeUs >= du.receiveTimeUs) {
        g_FirstPacketToComplete.add(
                                    du.enqueueTimeUs,
                                    du.enqueueTimeUs - du.receiveTimeUs);
    }
    if (decodeStartUs >= du.enqueueTimeUs) {
        g_CompleteToDecodeStart.add(
                                    decodeStartUs,
                                    decodeStartUs - du.enqueueTimeUs);
    }

    DecodeStartSlot& slot = g_DecodeStarts[du.frameNumber % kRingCapacity];
    slot.startUs.store(decodeStartUs, std::memory_order_relaxed);
    slot.frameNumber.store(du.frameNumber, std::memory_order_release);
    g_DecodeQueue.push(du.frameNumber);
}

void decodedFrame(AVFrame* frame, std::uint64_t decodedUs)
{
    syncDecodeGeneration();
    if (!g_Active.load(std::memory_order_relaxed)) {
        return;
    }

    std::uint32_t frameNumber = 0;
    if (frame == nullptr || !g_DecodeQueue.pop(frameNumber) || frameNumber == 0) {
        return;
    }

    DecodeStartSlot& slot = g_DecodeStarts[frameNumber % kRingCapacity];
    if (slot.frameNumber.load(std::memory_order_acquire) == frameNumber) {
        const std::uint64_t startUs = slot.startUs.load(std::memory_order_relaxed);
        if (decodedUs >= startUs) {
            g_DecodeStartToDecoded.add(decodedUs, decodedUs - startUs);
        }
        slot.frameNumber.store(0, std::memory_order_release);
    }

    // Keep the exact decoder-output timestamp for the next interval. Do not
    // reuse pkt_dts here: Moonlight writes its pacing timestamp only after this
    // wrapper returns, which would create a small gap between adjacent metrics.
    DecodedTimestampSlot& decodedSlot = g_DecodedTimestamps[frameNumber % kRingCapacity];
    decodedSlot.decodedUs.store(decodedUs, std::memory_order_relaxed);
    decodedSlot.frameNumber.store(frameNumber, std::memory_order_release);

    // AVFrame::opaque is application-owned. Moonlight does not otherwise use it,
    // so carry the Common C frame number through Pacer without touching PTS/DTS.
    frame->opaque = reinterpret_cast<void*>(static_cast<std::uintptr_t>(frameNumber) + 1);
}

std::uint32_t prepareRender(AVFrame* frame)
{
    const std::uint32_t frameNumber = frameNumberFromAvFrame(frame);
    g_RenderContext.frameNumber = frameNumber;
    g_RenderContext.renderStartUs = 0;
    return frameNumber;
}

void recordDecodedToRenderStart(std::uint32_t frameNumber, std::uint64_t renderStartUs)
{
    // Do this after the frontend renderer returns. The sample's completion time
    // remains the exact renderStartUs captured immediately before that call, but
    // table lookup and ring writes no longer perturb Render start -> present.
    if (!g_Active.load(std::memory_order_relaxed) || frameNumber == 0) {
        return;
    }

    DecodedTimestampSlot& decodedSlot = g_DecodedTimestamps[frameNumber % kRingCapacity];
    if (decodedSlot.frameNumber.load(std::memory_order_acquire) == frameNumber) {
        const std::uint64_t decodedUs = decodedSlot.decodedUs.load(std::memory_order_relaxed);
        if (renderStartUs >= decodedUs) {
            g_DecodedToRenderStart.add(
                                       renderStartUs,
                                       renderStartUs - decodedUs);
        }
        decodedSlot.frameNumber.store(0, std::memory_order_release);
    }
}

void renderEnd()
{
    g_RenderContext = {};
}

void pacerDrop()
{
    // Pacer calls this at the same two catch-up drop sites as its stock
    // pacerDroppedFrames counter. Keep this session-lifetime and independent of
    // benchmark start/stop.
    StreamHealthTelemetry::pacerFrameDrop();
}

void presentSuccess(std::uint64_t presentUs)
{
    if (!g_Active.load(std::memory_order_relaxed) ||
            g_RenderContext.frameNumber == 0 ||
            g_RenderContext.renderStartUs == 0 ||
            presentUs < g_RenderContext.renderStartUs) {
        g_RenderContext = {};
        return;
    }

    g_RenderStartToPresent.add(
                               presentUs,
                               presentUs - g_RenderContext.renderStartUs);

    // Present interval is entirely client-side: consecutive successful real
    // swapchain-submit return timestamps. The first present of each benchmark
    // run is deliberately used only as the baseline, so a stopped/restarted run
    // can never bridge to the previous run's last presentation.
    const std::uint64_t previousFrameCount =
            g_Frames.fetch_add(1, std::memory_order_relaxed);
    if (previousFrameCount != 0 &&
            g_LastPresentUs != 0 &&
            presentUs > g_LastPresentUs) {
        g_PresentInterval.add(presentUs, presentUs - g_LastPresentUs);
    }
    g_LastPresentUs = presentUs;

    // Consume the context at the first successful real swapchain submit. A
    // renderer that performs another submit inside the same renderFrame() must
    // not double-count this video frame.
    g_RenderContext = {};
}

Snapshot snapshot()
{
    Snapshot result;
    const bool active = g_Active.load(std::memory_order_acquire);
    std::uint64_t nowUs = active ? LiGetMicroseconds() : g_FrozenNowUs.load(std::memory_order_acquire);
    if (nowUs == 0) {
        nowUs = LiGetMicroseconds();
    }

    result.hostFrameInterval = g_HostFrameInterval.snapshot(nowUs);
    result.hostProcessing = g_HostProcessing.snapshot(nowUs);
    result.firstPacketInterval = g_FirstPacketInterval.snapshot(nowUs);
    result.completeFrameInterval = g_CompleteFrameInterval.snapshot(nowUs);
    result.firstPacketToComplete = g_FirstPacketToComplete.snapshot(nowUs);
    result.completeToDecodeStart = g_CompleteToDecodeStart.snapshot(nowUs);
    result.decodeStartToDecoded = g_DecodeStartToDecoded.snapshot(nowUs);
    result.decodedToRenderStart = g_DecodedToRenderStart.snapshot(nowUs);
    result.renderStartToPresent = g_RenderStartToPresent.snapshot(nowUs);
    result.presentInterval = g_PresentInterval.snapshot(nowUs);
    result.hasData = result.hostFrameInterval.valid || result.hostFrameInterval.runValid ||
                     result.hostProcessing.valid || result.hostProcessing.runValid ||
                     result.firstPacketInterval.valid || result.firstPacketInterval.runValid ||
                     result.completeFrameInterval.valid || result.completeFrameInterval.runValid ||
                     result.firstPacketToComplete.valid || result.firstPacketToComplete.runValid ||
                     result.completeToDecodeStart.valid || result.completeToDecodeStart.runValid ||
                     result.decodeStartToDecoded.valid || result.decodeStartToDecoded.runValid ||
                     result.decodedToRenderStart.valid || result.decodedToRenderStart.runValid ||
                     result.renderStartToPresent.valid || result.renderStartToPresent.runValid ||
                     result.presentInterval.valid || result.presentInterval.runValid;

    result.frames = g_Frames.load(std::memory_order_relaxed);
    return result;
}

void formatOverlayLines(char* output, std::size_t length)
{
    if (length == 0) {
        return;
    }
    output[0] = '\0';

    // Do not even sample the clock while the OSD is merely visible and no
    // automatic benchmark has run. After a run, g_FrozenNowUs is the immutable
    // snapshot time so stopped OSD refreshes also require no new clock reads.
    if (!g_Active.load(std::memory_order_acquire) &&
            g_FrozenNowUs.load(std::memory_order_acquire) == 0) {
        return;
    }

    const Snapshot stats = snapshot();
    if (!g_Active.load(std::memory_order_acquire) && !stats.hasData) {
        return;
    }

    std::snprintf(output, length,
                  "%-*s %7s %7s %7s\n"
                  "\nHOST\n",
                  kMetricLabelWidth, "Stream pipeline",
                  "AVG10s", "MAX10s", "MAX");
    appendMetric(output, length, "  Host frame interval", stats.hostFrameInterval);
    appendMetric(output, length, "  Host processing latency", stats.hostProcessing);

    std::size_t used = std::char_traits<char>::length(output);
    if (used < length - 1) {
        std::snprintf(output + used, length - used, "\nNETWORK\n");
    }
    appendMetric(output, length, "  First packet -> complete frame", stats.firstPacketToComplete);

    used = std::char_traits<char>::length(output);
    if (used < length - 1) {
        std::snprintf(output + used, length - used, "\nCLIENT\n");
    }
    appendMetric(output, length, "  Complete frame -> decode start", stats.completeToDecodeStart);
    appendMetric(output, length, "  Decode start -> decoded frame", stats.decodeStartToDecoded);
    appendMetric(output, length, "  Decoded frame -> render start", stats.decodedToRenderStart);
    appendMetric(output, length, "  Render start -> present", stats.renderStartToPresent);
    appendMetric(output, length, "  First packet interval", stats.firstPacketInterval);
    appendMetric(output, length, "  Complete frame interval", stats.completeFrameInterval);
    appendMetric(output, length, "  Present interval", stats.presentInterval);

    used = std::char_traits<char>::length(output);
    if (used < length - 1) {
        std::snprintf(output + used, length - used,
                      "\nFrames: %llu",
                      static_cast<unsigned long long>(stats.frames));
    }
}

} // namespace StreamPipelineTelemetry
