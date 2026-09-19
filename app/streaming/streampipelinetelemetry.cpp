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
#include <mutex>

namespace StreamPipelineTelemetry {
namespace {

constexpr std::uint64_t kWindowUs = 10'000'000;
constexpr std::uint64_t kAverageWindowUs = 1'000'000;
constexpr std::size_t kRingCapacity = 2048;
constexpr std::size_t kDecodeQueueCapacity = 64;
constexpr int kMetricLabelWidth = 34;
std::mutex g_StateChangeMutex;
std::mutex g_StateCallbackMutex;
std::atomic<std::uint64_t> g_DisplayRevision {0};
StateChangedCallback g_StateCallback = nullptr;
void* g_StateCallbackContext = nullptr;

void notifyStateChanged()
{
    g_DisplayRevision.fetch_add(1, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_StateCallbackMutex);
    if (g_StateCallback != nullptr) {
        g_StateCallback(g_StateCallbackContext);
    }
}

struct SampleSlot {
    std::atomic<std::uint64_t> serial {0};
    std::atomic<std::uint64_t> completedUs {0};
    std::atomic<std::uint64_t> durationUs {0};
};

struct GraphWindow {
    std::uint64_t firstBucket = 0;
    std::uint64_t lastBucket = 0;
    std::size_t firstColumn = 0;
};

std::uint64_t graphBucketForTime(std::uint64_t timestampUs)
{
    // Decompose before multiplying so this stays overflow-safe for the full
    // uint64_t clock range while retaining the exact 480-buckets-per-10s grid.
    const std::uint64_t wholeWindows = timestampUs / kWindowUs;
    const std::uint64_t offsetUs = timestampUs % kWindowUs;
    return wholeWindows * static_cast<std::uint64_t>(kGraphColumns) +
            (offsetUs * static_cast<std::uint64_t>(kGraphColumns)) / kWindowUs;
}

GraphWindow graphWindowForTime(std::uint64_t nowUs)
{
    // Treat graph buckets as half-open time ranges. At an exact bucket boundary,
    // keep the just-completed bucket at the right edge instead of showing a new
    // zero-width future bucket.
    const std::uint64_t lastTimeUs = nowUs == 0 ? 0 : nowUs - 1;
    const std::uint64_t lastBucket = graphBucketForTime(lastTimeUs);

    GraphWindow window;
    window.lastBucket = lastBucket;
    if (lastBucket >= kGraphColumns - 1) {
        window.firstBucket = lastBucket - (kGraphColumns - 1);
    }
    else {
        // This only matters during the first ~10 seconds of system uptime. Keep
        // the available history right-aligned just like a mature 10-second view.
        window.firstBucket = 0;
        window.firstColumn = static_cast<std::size_t>(
                (kGraphColumns - 1) - lastBucket);
    }
    return window;
}

bool graphColumnForBucket(const GraphWindow& window,
                          std::uint64_t absoluteBucket,
                          std::size_t& column)
{
    if (absoluteBucket < window.firstBucket ||
            absoluteBucket > window.lastBucket) {
        return false;
    }

    const std::uint64_t relativeBucket = absoluteBucket - window.firstBucket;
    const std::uint64_t mappedColumn =
            static_cast<std::uint64_t>(window.firstColumn) + relativeBucket;
    if (mappedColumn >= kGraphColumns) {
        return false;
    }

    column = static_cast<std::size_t>(mappedColumn);
    return true;
}

class MetricRing {
public:
    void reset()
    {
        m_NextSerial.store(0, std::memory_order_relaxed);
        for (auto& slot : m_Slots) {
            slot.serial.store(0, std::memory_order_relaxed);
            slot.completedUs.store(0, std::memory_order_relaxed);
            slot.durationUs.store(0, std::memory_order_relaxed);
        }
    }

    void add(std::uint64_t completedUs, std::uint64_t durationUs)
    {
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
        long double totalAverageUs = 0.0;
        long double total10sUs = 0.0;
        std::uint64_t maximum10sUs = 0;
        std::uint64_t averageCount = 0;
        std::uint64_t count10s = 0;

        for (const auto& slot : m_Slots) {
            const std::uint64_t serialBefore = slot.serial.load(std::memory_order_acquire);
            if (serialBefore == 0) {
                continue;
            }

            const std::uint64_t completedUs = slot.completedUs.load(std::memory_order_relaxed);
            const std::uint64_t durationUs = slot.durationUs.load(std::memory_order_relaxed);
            const std::uint64_t serialAfter = slot.serial.load(std::memory_order_acquire);

            if (serialBefore != serialAfter || serialAfter == 0 ||
                    completedUs > nowUs || nowUs - completedUs > kWindowUs) {
                continue;
            }

            total10sUs += durationUs;
            count10s++;
            if (count10s == 1 || durationUs > maximum10sUs) {
                maximum10sUs = durationUs;
            }

            if (nowUs - completedUs <= kAverageWindowUs) {
                totalAverageUs += durationUs;
                averageCount++;
            }
        }

        if (averageCount != 0) {
            result.averageValid = true;
            result.averageMs =
                    static_cast<double>(totalAverageUs / averageCount) / 1000.0;
        }
        if (count10s != 0) {
            result.windowValid = true;
            result.average10sMs =
                    static_cast<double>(total10sUs / count10s) / 1000.0;
            result.maximum10sMs = static_cast<double>(maximum10sUs) / 1000.0;
        }

        return result;
    }

    bool fillGraph(std::uint64_t nowUs, GraphSeries& graph, bool fillDurationSpan = true) const
    {
        graph = {};
        const GraphWindow window = graphWindowForTime(nowUs);
        bool hasData = false;

        // Samples keep their original completion timestamp and duration. Map both
        // endpoints onto an absolute time grid, then fill every bucket overlapped
        // by the completed interval. This makes a long hitch backfill the blank
        // columns that elapsed while it was still in progress, while fixed bucket
        // boundaries keep historical holes stable as the graph scrolls.
        for (const auto& slot : m_Slots) {
            const std::uint64_t serialBefore = slot.serial.load(std::memory_order_acquire);
            if (serialBefore == 0) {
                continue;
            }

            const std::uint64_t completedUs = slot.completedUs.load(std::memory_order_relaxed);
            const std::uint64_t durationUs = slot.durationUs.load(std::memory_order_relaxed);
            const std::uint64_t serialAfter = slot.serial.load(std::memory_order_acquire);

            if (serialBefore != serialAfter || serialAfter == 0 || completedUs > nowUs) {
                continue;
            }

            std::uint64_t firstBucket;
            std::uint64_t lastBucket;
            if (!fillDurationSpan || durationUs == 0) {
                const std::uint64_t pointUs = completedUs == 0 ? 0 : completedUs - 1;
                firstBucket = lastBucket = graphBucketForTime(pointUs);
            }
            else {
                const std::uint64_t startUs =
                        completedUs > durationUs ? completedUs - durationUs : 0;
                const std::uint64_t endUs = completedUs == 0 ? 0 : completedUs - 1;
                firstBucket = graphBucketForTime(startUs);
                lastBucket = graphBucketForTime(endUs);
            }

            if (lastBucket < window.firstBucket ||
                    firstBucket > window.lastBucket) {
                continue;
            }
            if (firstBucket < window.firstBucket) {
                firstBucket = window.firstBucket;
            }
            if (lastBucket > window.lastBucket) {
                lastBucket = window.lastBucket;
            }

            std::size_t firstColumn;
            std::size_t lastColumn;
            if (!graphColumnForBucket(window, firstBucket, firstColumn) ||
                    !graphColumnForBucket(window, lastBucket, lastColumn)) {
                continue;
            }

            const float durationMs = static_cast<float>(durationUs) / 1000.0f;
            for (std::size_t column = firstColumn; column <= lastColumn; ++column) {
                if (!graph.valid[column] || durationMs > graph.maximumMs[column]) {
                    graph.maximumMs[column] = durationMs;
                    graph.valid[column] = 1;
                }
            }
            hasData = true;
        }

        return hasData;
    }

private:
    std::array<SampleSlot, kRingCapacity> m_Slots;
    std::atomic<std::uint64_t> m_NextSerial {0};
};

struct EventSlot {
    std::atomic<std::uint64_t> serial {0};
    std::atomic<std::uint64_t> timestampUs {0};
};

class EventRing {
public:
    void reset()
    {
        m_NextSerial.store(0, std::memory_order_relaxed);
        for (auto& slot : m_Slots) {
            slot.serial.store(0, std::memory_order_relaxed);
            slot.timestampUs.store(0, std::memory_order_relaxed);
        }
    }

    void add(std::uint64_t timestampUs)
    {
        const std::uint64_t serial = m_NextSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        EventSlot& slot = m_Slots[(serial - 1) % kRingCapacity];

        slot.serial.store(0, std::memory_order_relaxed);
        slot.timestampUs.store(timestampUs, std::memory_order_relaxed);
        slot.serial.store(serial, std::memory_order_release);
    }

    bool applyGraphGaps(std::uint64_t nowUs, GraphSeries& graph) const
    {
        const GraphWindow window = graphWindowForTime(nowUs);
        bool hasGap = false;

        // Apply these after measured HOST spans. A frame-number discontinuity
        // deliberately has no host cadence sample, so its bucket must remain blank
        // even when another measured interval overlaps the same 20.8 ms column.
        for (const auto& slot : m_Slots) {
            const std::uint64_t serialBefore = slot.serial.load(std::memory_order_acquire);
            if (serialBefore == 0) {
                continue;
            }

            const std::uint64_t timestampUs = slot.timestampUs.load(std::memory_order_relaxed);
            const std::uint64_t serialAfter = slot.serial.load(std::memory_order_acquire);
            if (serialBefore != serialAfter || serialAfter == 0 || timestampUs > nowUs) {
                continue;
            }

            const std::uint64_t pointUs = timestampUs == 0 ? 0 : timestampUs - 1;
            const std::uint64_t bucket = graphBucketForTime(pointUs);
            std::size_t column;
            if (!graphColumnForBucket(window, bucket, column)) {
                continue;
            }

            graph.valid[column] = 0;
            graph.maximumMs[column] = 0.0f;
            hasGap = true;
        }

        return hasGap;
    }

private:
    std::array<EventSlot, kRingCapacity> m_Slots;
    std::atomic<std::uint64_t> m_NextSerial {0};
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
MetricRing g_NetworkBufferReserve;
EventRing g_HostFrameDiscontinuities;
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
thread_local std::uint64_t g_PresentGeneration = 0;

std::atomic<std::uint64_t> g_Generation {1};
std::atomic<std::uint64_t> g_FrozenNowUs {0};
std::atomic<std::uint64_t> g_Frames {0};
std::atomic<std::uint64_t> g_NetworkBufferReserveUs {0};
std::atomic<std::uint64_t> g_NetworkBufferConfiguredUs {0};

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

void resetTimingState()
{
    for (auto& slot : g_DecodeStarts) {
        slot.frameNumber.store(0, std::memory_order_relaxed);
        slot.startUs.store(0, std::memory_order_relaxed);
    }
    for (auto& slot : g_DecodedTimestamps) {
        slot.frameNumber.store(0, std::memory_order_relaxed);
        slot.decodedUs.store(0, std::memory_order_relaxed);
    }

    g_CurrentDecodeUnit = {};
    g_DecodeQueue.clear();
    g_RenderContext = {};
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
    g_NetworkBufferReserve.reset();
    g_HostFrameDiscontinuities.reset();
    g_FrozenNowUs.store(0, std::memory_order_relaxed);
    g_Frames.store(0, std::memory_order_relaxed);
    g_NetworkBufferReserveUs.store(0, std::memory_order_relaxed);
    g_NetworkBufferConfiguredUs.store(0, std::memory_order_relaxed);
    resetTimingState();
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

    if (metric.windowValid) {
        if (metric.averageValid) {
            std::snprintf(output + used, length - used,
                          "%-*s %7.2f %7.2f %7.2f ms\n",
                          kMetricLabelWidth, label,
                          metric.averageMs,
                          metric.average10sMs,
                          metric.maximum10sMs);
        }
        else {
            std::snprintf(output + used, length - used,
                          "%-*s %7s %7.2f %7.2f ms\n",
                          kMetricLabelWidth, label,
                          "N/A",
                          metric.average10sMs,
                          metric.maximum10sMs);
        }
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

void setStateChangedCallback(StateChangedCallback callback, void* context)
{
    std::lock_guard<std::mutex> lock(g_StateCallbackMutex);
    g_StateCallback = callback;
    g_StateCallbackContext = context;
}

std::uint64_t displayRevision()
{
    return g_DisplayRevision.load(std::memory_order_acquire);
}

std::uint64_t snapshotTimeUs()
{
    return g_Active.load(std::memory_order_acquire) ? LiGetMicroseconds() :
            g_FrozenNowUs.load(std::memory_order_acquire);
}

static void resetTelemetry()
{
    g_Active.store(false, std::memory_order_release);
    // Invalidate decoder-thread TLS lazily, without involving sample writers
    // in OSD synchronization.
    g_Generation.fetch_add(1, std::memory_order_acq_rel);
    resetClientState();
}

void clear()
{
    std::lock_guard<std::mutex> lock(g_StateChangeMutex);
    g_DisplayRevision.fetch_add(1, std::memory_order_acq_rel);
    resetTelemetry();
    notifyStateChanged();
}

void start()
{
    std::lock_guard<std::mutex> lock(g_StateChangeMutex);
    g_DisplayRevision.fetch_add(1, std::memory_order_acq_rel);
    resetTelemetry();
    g_Active.store(true, std::memory_order_release);
    notifyStateChanged();
}

void resume()
{
    std::lock_guard<std::mutex> lock(g_StateChangeMutex);
    g_DisplayRevision.fetch_add(1, std::memory_order_acq_rel);
    if (!g_Active.load(std::memory_order_acquire)) {
        // Preserve samples and counters, but don't measure intervals across the
        // frozen pause or reuse pending frame timings from before it.
        g_Generation.fetch_add(1, std::memory_order_acq_rel);
        resetTimingState();
        g_FrozenNowUs.store(0, std::memory_order_relaxed);
        g_Active.store(true, std::memory_order_release);
    }
    notifyStateChanged();
}

void stop()
{
    std::lock_guard<std::mutex> lock(g_StateChangeMutex);
    g_DisplayRevision.fetch_add(1, std::memory_order_acq_rel);
    if (g_Active.exchange(false, std::memory_order_acq_rel)) {
        g_FrozenNowUs.store(LiGetMicroseconds(), std::memory_order_release);
    }
    g_CurrentDecodeUnit = {};
    g_DecodeQueue.clear();
    g_RenderContext = {};
    notifyStateChanged();
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
    // sample so network loss cannot masquerade as a host pacing hitch. Keep a
    // graph-only marker so that unknown HOST interval stays visibly blank.
    if (networkGapBeforeCurrent) {
        g_HostFrameSequenceContinuous = false;
        if (du->enqueueTimeUs != 0) {
            g_HostFrameDiscontinuities.add(du->enqueueTimeUs);
        }
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
    // Pacer calls this alongside its catch-up pacerDroppedFrames counter.
    // Keep this session-lifetime and independent of
    // benchmark start/stop.
    StreamHealthTelemetry::pacerFrameDrop();
}

void recordNetworkBufferReserve(std::uint64_t completedUs,
                                std::uint64_t reserveUs,
                                std::uint64_t configuredUs)
{
    if (!g_Active.load(std::memory_order_acquire)) {
        return;
    }

    g_NetworkBufferReserve.add(completedUs, reserveUs);
    g_NetworkBufferReserveUs.store(reserveUs, std::memory_order_relaxed);
    g_NetworkBufferConfiguredUs.store(configuredUs, std::memory_order_relaxed);
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

    // Rebase on the render thread after a reset or resume from frozen telemetry.
    // An already-live benchmark start preserves the interval baseline too.
    const std::uint64_t generation = g_Generation.load(std::memory_order_acquire);
    if (g_PresentGeneration != generation) {
        g_LastPresentUs = 0;
        g_PresentGeneration = generation;
    }
    g_Frames.fetch_add(1, std::memory_order_relaxed);
    if (g_LastPresentUs != 0 &&
            presentUs > g_LastPresentUs) {
        g_PresentInterval.add(presentUs, presentUs - g_LastPresentUs);
    }
    g_LastPresentUs = presentUs;

    // Consume the context at the first successful real swapchain submit. A
    // renderer that performs another submit inside the same renderFrame() must
    // not double-count this video frame.
    g_RenderContext = {};
}

Snapshot snapshot(std::uint64_t nowUs)
{
    Snapshot result;
    if (nowUs == 0) {
        nowUs = snapshotTimeUs();
        if (nowUs == 0) {
            nowUs = LiGetMicroseconds();
        }
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
    result.hasData = result.hostFrameInterval.windowValid ||
                     result.hostProcessing.windowValid ||
                     result.firstPacketInterval.windowValid ||
                     result.completeFrameInterval.windowValid ||
                     result.firstPacketToComplete.windowValid ||
                     result.completeToDecodeStart.windowValid ||
                     result.decodeStartToDecoded.windowValid ||
                     result.decodedToRenderStart.windowValid ||
                     result.renderStartToPresent.windowValid ||
                     result.presentInterval.windowValid;

    result.frames = g_Frames.load(std::memory_order_relaxed);
    return result;
}

GraphSnapshot graphSnapshot(std::uint64_t nowUs)
{
    GraphSnapshot result;
    const bool active = g_Active.load(std::memory_order_acquire);
    const std::uint64_t frozenNowUs = g_FrozenNowUs.load(std::memory_order_acquire);
    if (!active && frozenNowUs == 0) {
        return result;
    }

    if (nowUs == 0) {
        nowUs = active ? LiGetMicroseconds() : frozenNowUs;
    }

    const bool hostData = g_HostFrameInterval.fillGraph(nowUs, result.hostFrameInterval);
    const bool hostGap =
            g_HostFrameDiscontinuities.applyGraphGaps(nowUs, result.hostFrameInterval);
    result.hasData |= hostData || hostGap;
    result.hasData |= g_FirstPacketInterval.fillGraph(nowUs, result.firstPacketInterval);
    result.hasData |= g_CompleteFrameInterval.fillGraph(nowUs, result.completeFrameInterval);
    result.hasData |= g_FirstPacketToComplete.fillGraph(nowUs, result.firstPacketToComplete);
    result.hasData |= g_PresentInterval.fillGraph(nowUs, result.presentInterval);
    result.hasData |= g_NetworkBufferReserve.fillGraph(
            nowUs, result.networkBufferReserve, false);
    result.networkBufferReserveMs =
            static_cast<double>(g_NetworkBufferReserveUs.load(std::memory_order_relaxed)) / 1000.0;
    result.networkBufferConfiguredMs =
            static_cast<double>(g_NetworkBufferConfiguredUs.load(std::memory_order_relaxed)) / 1000.0;

    const MetricSnapshot hostCadence = g_HostFrameInterval.snapshot(nowUs);
    if (hostCadence.windowValid) {
        result.framePeriodMs = hostCadence.average10sMs;
    }

    return result;
}

void formatOverlayLines(char* output, std::size_t length, std::uint64_t nowUs)
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

    const Snapshot stats = snapshot(nowUs);
    if (!g_Active.load(std::memory_order_acquire) && !stats.hasData) {
        return;
    }

    std::snprintf(output, length,
                  "%-*s %7s %7s %7s\n"
                  "\nHOST\n",
                  kMetricLabelWidth, "Stream pipeline",
                  "AVG", "AVG10s", "MAX10s");
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
