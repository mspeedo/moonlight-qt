#pragma once

#include "SDL_compat.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Sidecar metric for the existing input latency benchmark.
//
// The original LatencyProbe remains authoritative for input -> swapchain-submit
// latency. This class observes the same benchmark input timestamp and the same
// sampled marker frame, but completes the measurement only when Vulkan WSI
// confirms the tagged presentation through VK_KHR_present_wait. Missing
// presentation confirmation is represented as N/A; it never falls back to the
// submit timestamp.
namespace DisplayPresentLatency {

static constexpr uint64_t kTimeoutMs = 500;
static constexpr uint64_t kAverageWindowMs = 10000;
static constexpr size_t kAverageCapacity = 64;

enum class VisualState : uint8_t {
    Unknown,
    Dark,
    Bright,
};

struct LatencySample {
    uint64_t timestamp = 0;
    double latencyMs = 0.0;
};

inline SDL_SpinLock g_Lock = 0;
inline bool g_RunActive = false;
inline bool g_WaitingForTransition = false;
inline bool g_ValidationMeasurement = false;
inline bool g_HasResult = false;
inline VisualState g_Expected = VisualState::Unknown;
inline uint64_t g_InputTimestamp = 0;
inline double g_RunMaximumMs = 0.0;
inline LatencySample g_AverageSamples[kAverageCapacity] = {};
inline size_t g_AverageStart = 0;
inline size_t g_AverageCount = 0;

inline VisualState classify(float luma)
{
    if (luma < 0.45f) {
        return VisualState::Dark;
    }
    if (luma > 0.55f) {
        return VisualState::Bright;
    }
    return VisualState::Unknown;
}

inline void resetAverageLocked()
{
    g_AverageStart = 0;
    g_AverageCount = 0;
}

inline void pruneAverageLocked(uint64_t now)
{
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) {
        return;
    }

    const uint64_t windowTicks = frequency * kAverageWindowMs / 1000;
    while (g_AverageCount != 0) {
        const LatencySample& oldest = g_AverageSamples[g_AverageStart];
        if (now >= oldest.timestamp && now - oldest.timestamp > windowTicks) {
            g_AverageStart = (g_AverageStart + 1) % kAverageCapacity;
            g_AverageCount--;
        }
        else {
            break;
        }
    }
}

inline void addAverageSampleLocked(uint64_t timestamp, double latencyMs)
{
    pruneAverageLocked(timestamp);

    if (g_AverageCount == kAverageCapacity) {
        g_AverageStart = (g_AverageStart + 1) % kAverageCapacity;
        g_AverageCount--;
    }

    const size_t index = (g_AverageStart + g_AverageCount) % kAverageCapacity;
    g_AverageSamples[index].timestamp = timestamp;
    g_AverageSamples[index].latencyMs = latencyMs;
    g_AverageCount++;
}

inline bool getAverageLocked(uint64_t now,
                             double& averageMs,
                             double& maximumMs,
                             size_t& count)
{
    if (g_RunActive) {
        pruneAverageLocked(now);
    }

    if (g_AverageCount == 0) {
        averageMs = 0.0;
        maximumMs = 0.0;
        count = 0;
        return false;
    }

    double sum = 0.0;
    maximumMs = 0.0;
    for (size_t i = 0; i < g_AverageCount; ++i) {
        const size_t index = (g_AverageStart + i) % kAverageCapacity;
        const double latencyMs = g_AverageSamples[index].latencyMs;
        sum += latencyMs;
        if (i == 0 || latencyMs > maximumMs) {
            maximumMs = latencyMs;
        }
    }

    count = g_AverageCount;
    averageMs = sum / static_cast<double>(g_AverageCount);
    return true;
}

inline void beginRun()
{
    SDL_AtomicLock(&g_Lock);
    g_RunActive = true;
    g_WaitingForTransition = false;
    g_ValidationMeasurement = false;
    g_HasResult = false;
    g_Expected = VisualState::Unknown;
    g_InputTimestamp = 0;
    g_RunMaximumMs = 0.0;
    resetAverageLocked();
    SDL_AtomicUnlock(&g_Lock);
}

inline void endRun(uint64_t now)
{
    SDL_AtomicLock(&g_Lock);
    if (g_RunActive) {
        pruneAverageLocked(now);
    }
    g_RunActive = false;
    g_WaitingForTransition = false;
    g_ValidationMeasurement = false;
    g_Expected = VisualState::Unknown;
    g_InputTimestamp = 0;
    SDL_AtomicUnlock(&g_Lock);
}

inline void measurementStarted(uint64_t inputTimestamp,
                               bool expectedBright,
                               bool validationMeasurement)
{
    SDL_AtomicLock(&g_Lock);
    if (g_RunActive) {
        g_InputTimestamp = inputTimestamp;
        g_Expected = expectedBright ? VisualState::Bright : VisualState::Dark;
        g_ValidationMeasurement = validationMeasurement;
        g_WaitingForTransition = true;
    }
    SDL_AtomicUnlock(&g_Lock);
}

inline void onVideoSample(uint64_t presentTimestamp, float luma)
{
    const VisualState state = classify(luma);
    if (state == VisualState::Unknown) {
        return;
    }

    SDL_AtomicLock(&g_Lock);

    if (!g_RunActive || !g_WaitingForTransition || g_InputTimestamp == 0 ||
            presentTimestamp == 0 || presentTimestamp < g_InputTimestamp) {
        SDL_AtomicUnlock(&g_Lock);
        return;
    }

    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) {
        SDL_AtomicUnlock(&g_Lock);
        return;
    }

    const uint64_t timeoutTicks = frequency * kTimeoutMs / 1000;
    if (presentTimestamp - g_InputTimestamp >= timeoutTicks) {
        g_WaitingForTransition = false;
        g_ValidationMeasurement = false;
        g_Expected = VisualState::Unknown;
        g_InputTimestamp = 0;
        SDL_AtomicUnlock(&g_Lock);
        return;
    }

    if (state != g_Expected) {
        SDL_AtomicUnlock(&g_Lock);
        return;
    }

    const double latencyMs =
            static_cast<double>(presentTimestamp - g_InputTimestamp) * 1000.0 /
            static_cast<double>(frequency);

    if (!g_ValidationMeasurement) {
        if (!g_HasResult || latencyMs > g_RunMaximumMs) {
            g_RunMaximumMs = latencyMs;
        }
        g_HasResult = true;
        addAverageSampleLocked(presentTimestamp, latencyMs);
    }

    g_WaitingForTransition = false;
    g_ValidationMeasurement = false;
    g_Expected = VisualState::Unknown;
    g_InputTimestamp = 0;

    SDL_AtomicUnlock(&g_Lock);
}

inline void formatOverlayLine(char* output, size_t length)
{
    if (length == 0) {
        return;
    }

    SDL_AtomicLock(&g_Lock);

    double averageMs = 0.0;
    double maximumMs = 0.0;
    size_t averageCount = 0;
    const bool hasAverage = getAverageLocked(SDL_GetPerformanceCounter(),
                                             averageMs,
                                             maximumMs,
                                             averageCount);

    if (hasAverage) {
        std::snprintf(output, length,
                      "Input -> display present:\n"
                      "  AVG10s %.2f ms | MAX10s %.2f ms (n=%zu) | MAX %.2f ms",
                      averageMs, maximumMs, averageCount, g_RunMaximumMs);
    }
    else if (g_HasResult) {
        std::snprintf(output, length,
                      "Input -> display present:\n"
                      "  AVG10s N/A | MAX10s N/A (n=0) | MAX %.2f ms",
                      g_RunMaximumMs);
    }
    else {
        std::snprintf(output, length,
                      "Input -> display present:\n"
                      "  AVG10s N/A | MAX10s N/A (n=0) | MAX N/A");
    }

    SDL_AtomicUnlock(&g_Lock);
}

} // namespace DisplayPresentLatency