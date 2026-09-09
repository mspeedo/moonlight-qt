#pragma once

#include "SDL_compat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Shared state for the optional end-to-end input latency probe.
//
// The probe is enabled only while the debug/performance OSD is visible. While
// enabled, an SDL event watch timestamps physical controller A-button presses
// as soon as Moonlight receives them. The ordinary physical gamepad path remains
// unchanged and forwards the same input to Sunshine. Holding A starts an
// automated benchmark that injects short A pulses through that same SDL/input
// path. The Vulkan hook samples incoming frames until the helper's black/white
// center-marker transition is observed and associates that frame with its
// presentation submission time.
class LatencyProbe
{
public:
    enum class VisualState : uint8_t {
        Unknown,
        Dark,
        Bright,
    };

    static LatencyProbe& instance()
    {
        static LatencyProbe probe;
        return probe;
    }

    bool isEnabled() const
    {
        return m_EnabledFast.load(std::memory_order_relaxed);
    }

    void setEnabled(bool enabled)
    {
        bool stateChanged = false;
        bool restorePhysicalA = false;
        SDL_JoystickID restoreController = 0;
        SDL_TimerID timerToRemove = 0;

        SDL_AtomicLock(&m_Lock);
        if (m_Enabled != enabled) {
            stateChanged = true;

            if (!enabled) {
                // If automatic benchmarking temporarily released a physically
                // held A button, restore the real held state before disabling
                // the probe so the normal input path remains semantically intact.
                restorePhysicalA = m_AutoBenchmark && m_PhysicalAHeld;
                restoreController = m_BenchmarkControllerId;
                timerToRemove = m_BenchmarkTimer;
                m_BenchmarkTimer = 0;
            }

            m_Enabled = enabled;
            m_EnabledFast.store(enabled, std::memory_order_release);
            m_WaitingForTransition = false;
            m_HasResult = false;
            m_Baseline = VisualState::Unknown;
            m_Expected = VisualState::Unknown;
            m_InputTimestamp = 0;
            m_LastLatencyMs = 0.0;

            m_PhysicalAHeld = false;
            m_PhysicalADownTick = 0;
            m_AutoBenchmark = false;
            m_AutoButtonDown = false;
            m_AutoDownTick = 0;
            m_AutoNextDownTick = 0;
            resetAverageLocked();
        }
        SDL_AtomicUnlock(&m_Lock);

        if (!stateChanged) {
            return;
        }

        // Register the controller watcher and benchmark timer only while the
        // OSD is visible. Disabled streams therefore pay no probe overhead.
        if (enabled) {
            SDL_AddEventWatch(controllerEventWatch, this);

            SDL_TimerID timer = SDL_AddTimer(kBenchmarkTickMs, benchmarkTimerCallback, this);
            if (timer == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Latency probe: unable to start benchmark timer");
            }

            SDL_AtomicLock(&m_Lock);
            if (m_Enabled) {
                m_BenchmarkTimer = timer;
                timer = 0;
            }
            SDL_AtomicUnlock(&m_Lock);

            if (timer != 0) {
                SDL_RemoveTimer(timer);
            }
        }
        else {
            SDL_DelEventWatch(controllerEventWatch, this);

            if (timerToRemove != 0) {
                SDL_RemoveTimer(timerToRemove);
            }

            if (restorePhysicalA) {
                pushSyntheticAEvent(restoreController, true);
            }
        }
    }

    bool needsVideoSample()
    {
        SDL_AtomicLock(&m_Lock);
        checkTimeoutLocked(SDL_GetPerformanceCounter());
        const bool needed = m_Enabled &&
                (m_Baseline == VisualState::Unknown || m_WaitingForTransition);
        SDL_AtomicUnlock(&m_Lock);
        return needed;
    }

    // Called at the SDL controller event boundary. This timestamp therefore
    // includes Moonlight's own event handling/queueing before the ordinary
    // controller packet reaches Sunshine, which is appropriate for a total
    // Moonlight-input-to-present metric.
    void onControllerATrigger(uint64_t timestamp)
    {
        SDL_AtomicLock(&m_Lock);
        checkTimeoutLocked(timestamp);
        startMeasurementLocked(timestamp);
        SDL_AtomicUnlock(&m_Lock);
    }

    // Called from the asynchronous Vulkan readback callback. Luma is normalized
    // to 0.0-1.0 after libplacebo has converted the sampled video frame to RGB.
    // The submit timestamp belongs to this exact sampled frame.
    void onVideoSample(uint64_t serial, uint64_t submitTimestamp, float luma)
    {
        const VisualState state = classify(luma);
        if (state == VisualState::Unknown) {
            return;
        }

        SDL_AtomicLock(&m_Lock);

        if (!m_Enabled) {
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        checkTimeoutLocked(SDL_GetPerformanceCounter());

        // The first valid sample after enabling the OSD establishes the helper
        // state. No input measurement is started until this baseline exists.
        if (m_Baseline == VisualState::Unknown && !m_WaitingForTransition) {
            m_Baseline = state;
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        if (m_WaitingForTransition && state == m_Expected) {
            m_Baseline = state;
            completeLocked(serial, submitTimestamp);
        }

        SDL_AtomicUnlock(&m_Lock);
    }

    // Appends one OSD line. The caller only invokes this for an enabled debug
    // OSD, so disabled streams do not pay formatting/locking overhead.
    void formatOverlayLine(char* output, size_t length)
    {
        if (length == 0) {
            return;
        }

        SDL_AtomicLock(&m_Lock);
        const uint64_t now = SDL_GetPerformanceCounter();
        checkTimeoutLocked(now);

        double averageMs = 0.0;
        size_t averageCount = 0;
        const bool hasAverage = getAverageLocked(now, averageMs, averageCount);

        if (m_WaitingForTransition) {
            if (hasAverage) {
                std::snprintf(output, length,
                              "Input to present latency: measuring... | AVG10s %.2f ms (n=%zu)",
                              averageMs, averageCount);
            }
            else {
                std::snprintf(output, length, "Input to present latency: measuring...");
            }
        }
        else if (m_HasResult) {
            if (hasAverage) {
                std::snprintf(output, length,
                              "Input to present latency: %.2f ms | AVG10s %.2f ms (n=%zu)",
                              m_LastLatencyMs, averageMs, averageCount);
            }
            else {
                std::snprintf(output, length,
                              "Input to present latency: %.2f ms", m_LastLatencyMs);
            }
        }
        else if (m_Baseline == VisualState::Unknown) {
            if (hasAverage) {
                std::snprintf(output, length,
                              "Input to present latency: acquiring baseline... | AVG10s %.2f ms (n=%zu)",
                              averageMs, averageCount);
            }
            else {
                std::snprintf(output, length,
                              "Input to present latency: acquiring baseline...");
            }
        }
        else if (m_AutoBenchmark) {
            if (hasAverage) {
                std::snprintf(output, length,
                              "Input to present latency: auto benchmark | AVG10s %.2f ms (n=%zu)",
                              averageMs, averageCount);
            }
            else {
                std::snprintf(output, length,
                              "Input to present latency: auto benchmark starting...");
            }
        }
        else if (hasAverage) {
            // Freeze the final rolling window after A is released so the result
            // remains readable until the next automatic benchmark starts.
            std::snprintf(output, length,
                          "Input to present latency: ready (press/hold A) | AVG10s %.2f ms (n=%zu)",
                          averageMs, averageCount);
        }
        else {
            std::snprintf(output, length,
                          "Input to present latency: ready (press/hold A)");
        }

        SDL_AtomicUnlock(&m_Lock);
    }

private:
    static constexpr uint64_t kTimeoutMs = 500;

    // Hold A for this long to start an automatic run. Automatic A-down edges
    // are 272 ms apart (~3.68 Hz), which deliberately walks across common
    // 117/120/144 Hz phase relationships instead of repeatedly landing at the
    // same stream/display phase. A 17 ms scheduler tick divides the 272 ms
    // period exactly, and the 51 ms release gap is three scheduler ticks.
    static constexpr Uint32 kBenchmarkHoldMs = 750;
    static constexpr Uint32 kBenchmarkPeriodMs = 272;
    static constexpr Uint32 kBenchmarkPressMs = 51;
    static constexpr Uint32 kBenchmarkTickMs = 17;
    static constexpr uint64_t kAverageWindowMs = 10000;
    static constexpr size_t kAverageCapacity = 64;

    // Synthetic controller events use a timestamp value that ordinary SDL
    // controller events will not practically produce. This prevents our event
    // watch from interpreting injected benchmark pulses as physical hold input.
    static constexpr Uint32 kSyntheticEventTimestamp = 0xFFFFFFFFu;

    struct LatencySample {
        uint64_t timestamp = 0;
        double latencyMs = 0.0;
    };

    LatencyProbe() = default;

    static int SDLCALL controllerEventWatch(void* userdata, SDL_Event* event)
    {
        if ((event->type == SDL_CONTROLLERBUTTONDOWN ||
             event->type == SDL_CONTROLLERBUTTONUP) &&
                event->cbutton.button == SDL_CONTROLLER_BUTTON_A) {
            // Synthetic benchmark pulses are already timestamped explicitly by
            // benchmarkTimerTick(), so do not treat them as physical input.
            if (event->cbutton.timestamp == kSyntheticEventTimestamp) {
                return 1;
            }

            static_cast<LatencyProbe*>(userdata)->onPhysicalAEvent(
                        event->type == SDL_CONTROLLERBUTTONDOWN,
                        event->cbutton.which,
                        SDL_GetPerformanceCounter());
        }

        // Event-watch return values are ignored, but return 1 for consistency
        // with SDL event-filter conventions.
        return 1;
    }

    static Uint32 SDLCALL benchmarkTimerCallback(Uint32, void* userdata)
    {
        static_cast<LatencyProbe*>(userdata)->benchmarkTimerTick();
        return kBenchmarkTickMs;
    }

    static VisualState classify(float luma)
    {
        // The host helper forces the center marker to pure black or pure white.
        // Keep a wide threshold gap so codec quantization cannot cause false
        // edges around the detector boundary.
        if (luma < 0.45f) {
            return VisualState::Dark;
        }
        if (luma > 0.55f) {
            return VisualState::Bright;
        }
        return VisualState::Unknown;
    }

    void onPhysicalAEvent(bool pressed, SDL_JoystickID controllerId, uint64_t timestamp)
    {
        SDL_AtomicLock(&m_Lock);

        if (!m_Enabled) {
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        checkTimeoutLocked(timestamp);

        if (pressed) {
            m_PhysicalAHeld = true;
            m_BenchmarkControllerId = controllerId;
            m_PhysicalADownTick = SDL_GetTicks();

            // Preserve the existing short-press behavior: every physical A-down
            // immediately clears the previous single result and starts one fresh
            // measurement if the detector baseline is ready.
            startMeasurementLocked(timestamp);
        }
        else if (controllerId == m_BenchmarkControllerId) {
            // Automatic mode is stopped by the benchmark timer on its next tick.
            // The real A-up event itself continues through Moonlight normally and
            // guarantees the host cannot be left with A held down.
            m_PhysicalAHeld = false;
        }

        SDL_AtomicUnlock(&m_Lock);
    }

    bool startMeasurementLocked(uint64_t timestamp)
    {
        if (!m_Enabled || m_WaitingForTransition || m_Baseline == VisualState::Unknown) {
            return false;
        }

        m_InputTimestamp = timestamp;
        m_Expected = m_Baseline == VisualState::Dark ? VisualState::Bright : VisualState::Dark;
        m_WaitingForTransition = true;
        m_HasResult = false;
        m_LastLatencyMs = 0.0;
        return true;
    }

    void benchmarkTimerTick()
    {
        enum class PulseAction : uint8_t {
            None,
            Press,
            Release,
        };

        PulseAction action = PulseAction::None;
        SDL_JoystickID controllerId = 0;
        const Uint32 nowTick = SDL_GetTicks();
        const uint64_t nowCounter = SDL_GetPerformanceCounter();

        SDL_AtomicLock(&m_Lock);

        if (!m_Enabled) {
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        checkTimeoutLocked(nowCounter);

        if (m_PhysicalAHeld && !m_AutoBenchmark &&
                SDL_TICKS_PASSED(nowTick, m_PhysicalADownTick + kBenchmarkHoldMs)) {
            // A new automatic benchmark is a fresh dataset. Keep the current
            // detector baseline but discard any previous single/average result.
            m_AutoBenchmark = true;
            m_AutoButtonDown = false;
            m_AutoDownTick = 0;
            m_AutoNextDownTick = nowTick + kBenchmarkPressMs;
            m_WaitingForTransition = false;
            m_Expected = VisualState::Unknown;
            m_InputTimestamp = 0;
            m_HasResult = false;
            m_LastLatencyMs = 0.0;
            resetAverageLocked();

            controllerId = m_BenchmarkControllerId;
            action = PulseAction::Release;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe: automatic benchmark started");
        }
        else if (m_AutoBenchmark) {
            controllerId = m_BenchmarkControllerId;

            if (!m_PhysicalAHeld) {
                // Prune once at stop, then freeze the final 10-second window on
                // screen so the user can read/compare it after releasing A.
                pruneAverageLocked(nowCounter);
                m_AutoBenchmark = false;
                m_AutoButtonDown = false;
                action = PulseAction::Release;

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Latency probe: automatic benchmark stopped");
            }
            else if (m_AutoButtonDown) {
                if (SDL_TICKS_PASSED(nowTick, m_AutoDownTick + kBenchmarkPressMs)) {
                    m_AutoButtonDown = false;
                    action = PulseAction::Release;
                }
            }
            else if (SDL_TICKS_PASSED(nowTick, m_AutoNextDownTick)) {
                // Never overlap measurements. If capture/decode is temporarily
                // delayed, leave A released and retry on a later timer tick.
                if (startMeasurementLocked(nowCounter)) {
                    m_AutoButtonDown = true;
                    m_AutoDownTick = nowTick;
                    m_AutoNextDownTick = nowTick + kBenchmarkPeriodMs;
                    action = PulseAction::Press;
                }
                else {
                    m_AutoNextDownTick = nowTick + kBenchmarkTickMs;
                }
            }
        }

        SDL_AtomicUnlock(&m_Lock);

        if (action == PulseAction::Press) {
            pushSyntheticAEvent(controllerId, true);
        }
        else if (action == PulseAction::Release) {
            pushSyntheticAEvent(controllerId, false);
        }
    }

    static void pushSyntheticAEvent(SDL_JoystickID controllerId, bool pressed)
    {
        SDL_Event event = {};
        event.type = pressed ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        event.cbutton.type = event.type;
        event.cbutton.timestamp = kSyntheticEventTimestamp;
        event.cbutton.which = controllerId;
        event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
        event.cbutton.state = pressed ? SDL_PRESSED : SDL_RELEASED;

        if (SDL_PushEvent(&event) < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe: failed to inject benchmark A event: %s",
                        SDL_GetError());
        }
    }

    void completeLocked(uint64_t serial, uint64_t submitTimestamp)
    {
        if (!m_WaitingForTransition || submitTimestamp == 0 || submitTimestamp < m_InputTimestamp) {
            return;
        }

        const uint64_t frequency = SDL_GetPerformanceFrequency();
        if (frequency != 0) {
            m_LastLatencyMs = (double)(submitTimestamp - m_InputTimestamp) * 1000.0 / (double)frequency;
            m_HasResult = true;

            if (m_AutoBenchmark) {
                addAverageSampleLocked(submitTimestamp, m_LastLatencyMs);
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe: frame=%llu input-to-present=%.3f ms",
                        (unsigned long long)serial,
                        m_LastLatencyMs);
        }

        m_WaitingForTransition = false;
        m_Expected = VisualState::Unknown;
    }

    void checkTimeoutLocked(uint64_t now)
    {
        if (!m_WaitingForTransition || m_InputTimestamp == 0) {
            return;
        }

        const uint64_t frequency = SDL_GetPerformanceFrequency();
        if (frequency == 0) {
            return;
        }

        const uint64_t timeoutTicks = frequency * kTimeoutMs / 1000;
        if (now - m_InputTimestamp >= timeoutTicks) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe timed out waiting for host transition");
            m_WaitingForTransition = false;
            m_Baseline = VisualState::Unknown;
            m_Expected = VisualState::Unknown;
            m_InputTimestamp = 0;
        }
    }

    void resetAverageLocked()
    {
        m_AverageStart = 0;
        m_AverageCount = 0;
    }

    void addAverageSampleLocked(uint64_t timestamp, double latencyMs)
    {
        pruneAverageLocked(timestamp);

        if (m_AverageCount == kAverageCapacity) {
            m_AverageStart = (m_AverageStart + 1) % kAverageCapacity;
            m_AverageCount--;
        }

        const size_t index = (m_AverageStart + m_AverageCount) % kAverageCapacity;
        m_AverageSamples[index].timestamp = timestamp;
        m_AverageSamples[index].latencyMs = latencyMs;
        m_AverageCount++;
    }

    void pruneAverageLocked(uint64_t now)
    {
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        if (frequency == 0) {
            return;
        }

        const uint64_t windowTicks = frequency * kAverageWindowMs / 1000;
        while (m_AverageCount != 0) {
            const LatencySample& oldest = m_AverageSamples[m_AverageStart];
            if (now >= oldest.timestamp && now - oldest.timestamp > windowTicks) {
                m_AverageStart = (m_AverageStart + 1) % kAverageCapacity;
                m_AverageCount--;
            }
            else {
                break;
            }
        }
    }

    bool getAverageLocked(uint64_t now, double& averageMs, size_t& count)
    {
        // While the benchmark is running this is a true rolling 10-second
        // window. Once stopped, it is intentionally frozen for comparison.
        if (m_AutoBenchmark) {
            pruneAverageLocked(now);
        }

        if (m_AverageCount == 0) {
            averageMs = 0.0;
            count = 0;
            return false;
        }

        double sum = 0.0;
        for (size_t i = 0; i < m_AverageCount; ++i) {
            const size_t index = (m_AverageStart + i) % kAverageCapacity;
            sum += m_AverageSamples[index].latencyMs;
        }

        count = m_AverageCount;
        averageMs = sum / (double)m_AverageCount;
        return true;
    }

    SDL_SpinLock m_Lock = 0;
    std::atomic<bool> m_EnabledFast { false };
    bool m_Enabled = false;
    bool m_WaitingForTransition = false;
    bool m_HasResult = false;
    VisualState m_Baseline = VisualState::Unknown;
    VisualState m_Expected = VisualState::Unknown;
    uint64_t m_InputTimestamp = 0;
    double m_LastLatencyMs = 0.0;

    SDL_TimerID m_BenchmarkTimer = 0;
    bool m_PhysicalAHeld = false;
    SDL_JoystickID m_BenchmarkControllerId = 0;
    Uint32 m_PhysicalADownTick = 0;
    bool m_AutoBenchmark = false;
    bool m_AutoButtonDown = false;
    Uint32 m_AutoDownTick = 0;
    Uint32 m_AutoNextDownTick = 0;

    LatencySample m_AverageSamples[kAverageCapacity] = {};
    size_t m_AverageStart = 0;
    size_t m_AverageCount = 0;
};
