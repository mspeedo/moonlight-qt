#pragma once

#include "SDL_compat.h"
#include "displaypresentlatency.h"
#include "latencybenchmarkcontrol.h"
#include "streampipelinetelemetry.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// End-to-end Moonlight input-to-present latency benchmark.
//
// The probe is normally controlled by debug/performance OSD visibility, but an
// already-started benchmark deliberately remains active if the OSD is hidden.
// Merely showing the OSD does not launch the host helper or sample video. A normal
// physical A press/release is left completely untouched. Holding physical A for
// kBenchmarkHoldMs activates the benchmark: Moonlight first releases A through
// its ordinary SDL/input path, gives that queued release one benchmark timer tick
// to reach Sunshine, sends a best-effort helper START request, then deliberately
// leaves video sampling disabled for kHelperStartupDelayMs. After that fixed
// startup interval it acquires the center-marker baseline and sends one validation
// A pulse that must produce a real black/white transition. That first transition
// is discarded; only subsequent synthetic A pulses contribute latency samples.
// Releasing physical A leaves the run active. Physical B-down stops the run
// (including helper startup) even while the OSD is hidden. Holding physical Y for
// kTelemetryHoldMs toggles manual pipeline-telemetry freeze/resume only while the
// OSD is visible, without altering ordinary short Y presses.
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

    bool needsVideoSampleFast() const
    {
        return m_NeedsVideoSampleFast.load(std::memory_order_relaxed);
    }

    bool setEnabled(bool enabled, bool forceDisable = false)
    {
        bool stateChanged = false;
        bool restoreAState = false;
        bool restorePhysicalA = false;
        bool requestStop = false;
        bool benchmarkWasActive = false;
        SDL_JoystickID restoreController = 0;
        SDL_TimerID holdTimerToRemove = 0;
        SDL_TimerID telemetryHoldTimerToRemove = 0;
        SDL_TimerID benchmarkTimerToRemove = 0;

        SDL_AtomicLock(&m_Lock);
        m_OsdVisible = enabled;

        const bool benchmarkInProgress =
                m_BenchmarkStarting || m_HelperRunning || m_AutoBenchmark;
        if (!enabled && benchmarkInProgress && !forceDisable) {
            // Hiding the OSD must not perturb an already-started benchmark. Keep
            // the event watch, timer, helper, video sampling, and telemetry alive.
            // A/Y diagnostics are visibility-gated below; B remains available to
            // stop the hidden benchmark.
            m_PhysicalYHeld = false;
            SDL_AtomicUnlock(&m_Lock);
            return false;
        }

        if (m_Enabled != enabled) {
            stateChanged = true;

            if (!enabled) {
                // Restore the physical A state, including releasing a synthetic
                // pulse if the user has already released the start button.
                restorePhysicalA = m_PhysicalAHeld;
                restoreAState =
                        (m_BenchmarkStarting || m_HelperRunning || m_AutoBenchmark) &&
                        (m_PhysicalAHeld || m_AutoButtonDown);
                restoreController = m_BenchmarkControllerId;
                requestStop = m_BenchmarkStarting || m_HelperRunning || m_AutoBenchmark;
                benchmarkWasActive = m_AutoBenchmark;
                holdTimerToRemove = m_HoldTimer;
                telemetryHoldTimerToRemove = m_TelemetryHoldTimer;
                benchmarkTimerToRemove = m_BenchmarkTimer;
                m_HoldTimer = 0;
                m_TelemetryHoldTimer = 0;
                m_BenchmarkTimer = 0;
            }

            m_Enabled = enabled;
            m_WaitingForTransition = false;
            m_HasResult = false;
            m_Baseline = VisualState::Unknown;
            m_Expected = VisualState::Unknown;
            m_InputTimestamp = 0;
            m_LastLatencyMs = 0.0;
            m_FrozenStatsTimestamp = 0;

            m_PhysicalAHeld = false;
            m_PhysicalYHeld = false;
            m_StopRequested = false;
            m_BenchmarkStarting = false;
            m_StartRequestPending = false;
            m_StartRequestAfterTick = 0;
            m_BaselineSampleAfterTick = 0;
            m_HelperRunning = false;
            m_ValidationPending = false;
            m_AutoBenchmark = false;
            m_BenchmarkRan = false;
            m_ManualTelemetryFrozen = false;
            m_AutoButtonDown = false;
            m_AutoDownTick = 0;
            m_AutoNextDownTick = 0;
            m_PulseJitterState = 0;
            resetAverageLocked();
            refreshVideoSamplingFastLocked();
        }
        SDL_AtomicUnlock(&m_Lock);

        if (!stateChanged) {
            return false;
        }

        if (enabled) {
            // Pipeline telemetry runs for the whole stream. Enabling the OSD only
            // enables benchmark controls and display work; it must not reset the
            // already accumulated stream statistics.
            SDL_AddEventWatch(controllerEventWatch, this);
        }
        else {
            SDL_DelEventWatch(controllerEventWatch, this);

            if (holdTimerToRemove != 0) {
                SDL_RemoveTimer(holdTimerToRemove);
            }
            if (telemetryHoldTimerToRemove != 0) {
                SDL_RemoveTimer(telemetryHoldTimerToRemove);
            }
            if (benchmarkTimerToRemove != 0) {
                SDL_RemoveTimer(benchmarkTimerToRemove);
            }

            if (restoreAState) {
                pushSyntheticAEvent(restoreController, restorePhysicalA);
            }

            if (requestStop) {
                LatencyBenchmarkControl::stopAsync();
            }

            if (benchmarkWasActive) {
                DisplayPresentLatency::endRun(SDL_GetPerformanceCounter());
            }
        }

        return benchmarkWasActive;
    }

    // Called from the asynchronous Vulkan readback callback. Luma is normalized
    // to 0.0-1.0 after libplacebo has converted the sampled video frame to RGB.
    // submitTimestamp belongs to this exact successfully submitted frame.
    void onVideoSample(uint64_t serial, uint64_t submitTimestamp, float luma)
    {
        const VisualState state = classify(luma);
        if (state == VisualState::Unknown) {
            return;
        }

        SDL_AtomicLock(&m_Lock);

        if (!m_Enabled || !m_HelperRunning || m_BenchmarkStarting) {
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        checkTimeoutLocked(SDL_GetPerformanceCounter());

        // The first classified helper frame establishes the marker state. No
        // synthetic measurement pulse may be sent until this baseline exists.
        if (m_Baseline == VisualState::Unknown && !m_WaitingForTransition) {
            m_Baseline = state;
            refreshVideoSamplingFastLocked();
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        if (m_WaitingForTransition && state == m_Expected) {
            m_Baseline = state;
            completeLocked(serial, submitTimestamp);
        }

        SDL_AtomicUnlock(&m_Lock);
    }

    void formatOverlayLine(char* output, size_t length)
    {
        if (length == 0) {
            return;
        }

        SDL_AtomicLock(&m_Lock);
        const uint64_t now = SDL_GetPerformanceCounter();
        checkTimeoutLocked(now);

        double averageMs = 0.0;
        bool averageValid = false;
        double average10sMs = 0.0;
        double maximum10sMs = 0.0;
        size_t averageCount = 0;
        const bool hasAverage = getStatsLocked(now,
                                               averageMs,
                                               averageValid,
                                               average10sMs,
                                               maximum10sMs,
                                               averageCount);

        if (m_BenchmarkStarting) {
            std::snprintf(output, length,
                          "Input -> present: starting host helper...");
        }
        else if (m_HelperRunning && m_Baseline == VisualState::Unknown) {
            std::snprintf(output, length,
                          "Input -> present: acquiring helper baseline...");
        }
        else if (m_AutoBenchmark) {
            if (m_ValidationPending) {
                std::snprintf(output, length,
                              "Input -> present: validating helper...");
            }
            else if (m_HasResult || hasAverage) {
                std::snprintf(output, length,
                              "Input -> present: benchmark running");
            }
            else {
                std::snprintf(output, length,
                              "Input -> present: auto benchmark starting...");
            }
        }
        else {
            std::snprintf(output, length,
                          "Input -> present: ready (hold A to start)");
        }

        if (m_BenchmarkStarting || m_HelperRunning || m_AutoBenchmark) {
            SDL_strlcat(output, " (B to stop)", length);
        }

        if (m_AutoBenchmark) {
            SDL_strlcat(output, "\nTelemetry: BENCHMARK", length);
        }
        else if (m_BenchmarkRan && !StreamPipelineTelemetry::isActiveFast()) {
            SDL_strlcat(output, "\nTelemetry: FROZEN (benchmark)", length);
        }
        else if (m_ManualTelemetryFrozen) {
            SDL_strlcat(output, "\nTelemetry: FROZEN (hold Y to resume)", length);
        }
        else if (m_BenchmarkStarting || m_HelperRunning) {
            SDL_strlcat(output, "\nTelemetry: LIVE (benchmark starting)", length);
        }
        else {
            SDL_strlcat(output, "\nTelemetry: LIVE (hold Y to freeze)", length);
        }

        if (hasAverage) {
            char result[144];
            if (averageValid) {
                std::snprintf(result, sizeof(result),
                              "\n  AVG %.2f ms | AVG10s %.2f ms | MAX10s %.2f ms (n=%zu)",
                              averageMs, average10sMs, maximum10sMs, averageCount);
            }
            else {
                std::snprintf(result, sizeof(result),
                              "\n  AVG N/A | AVG10s %.2f ms | MAX10s %.2f ms (n=%zu)",
                              average10sMs, maximum10sMs, averageCount);
            }
            SDL_strlcat(output, result, length);
        }
        else if (m_HasResult) {
            SDL_strlcat(output,
                        "\n  AVG N/A | AVG10s N/A | MAX10s N/A (n=0)",
                        length);
        }

        const bool showDisplayPresent = m_AutoBenchmark || m_BenchmarkRan;
        SDL_AtomicUnlock(&m_Lock);

        if (showDisplayPresent) {
            char displayPresentLine[192];
            DisplayPresentLatency::formatOverlayLine(displayPresentLine,
                                                     sizeof(displayPresentLine));
            SDL_strlcat(output, "\n", length);
            SDL_strlcat(output, displayPresentLine, length);
        }
    }

private:
    static constexpr uint64_t kTimeoutMs = 500;

    // Hold A/Y long enough that ordinary gameplay taps never activate benchmark
    // or telemetry-control behavior. Synthetic A-down intervals jitter by one
    // 17 ms timer tick around a 272 ms mean (255/272/289 ms), preventing samples
    // from locking to a repeating stream-cadence phase. The 51 ms release gap is
    // three 17 ms timer ticks.
    static constexpr Uint32 kBenchmarkHoldMs = 750;
    static constexpr Uint32 kTelemetryHoldMs = 750;
    static constexpr Uint32 kHelperStartupDelayMs = 2000;
    static constexpr Uint32 kBenchmarkPeriodMs = 272;
    static constexpr Uint32 kBenchmarkPressMs = 51;
    static constexpr Uint32 kBenchmarkTickMs = 17;
    static constexpr uint64_t kAverageWindowMs = 10000;
    static constexpr uint64_t kShortAverageWindowMs = 1000;
    static constexpr size_t kAverageCapacity = 64;

    // Synthetic events use a timestamp ordinary SDL controller events will not
    // practically produce, so the event watch never mistakes benchmark pulses
    // for the user's physical hold.
    static constexpr Uint32 kSyntheticEventTimestamp = 0xFFFFFFFFu;

    struct LatencySample {
        uint64_t timestamp = 0;
        double latencyMs = 0.0;
    };

    LatencyProbe() = default;

    Uint32 nextBenchmarkPeriodLocked()
    {
        // The benchmark timer itself ticks every 17 ms, so jitter in whole
        // timer ticks avoids fake sub-tick precision while preserving a
        // 272 ms mean interval. Seeded per run to vary the phase walk.
        m_PulseJitterState = m_PulseJitterState * 1664525u + 1013904223u;
        switch ((m_PulseJitterState >> 16) % 3u) {
        case 0:
            return kBenchmarkPeriodMs - kBenchmarkTickMs;
        case 2:
            return kBenchmarkPeriodMs + kBenchmarkTickMs;
        default:
            return kBenchmarkPeriodMs;
        }
    }

    static int SDLCALL controllerEventWatch(void* userdata, SDL_Event* event)
    {
        if ((event->type == SDL_CONTROLLERBUTTONDOWN ||
             event->type == SDL_CONTROLLERBUTTONUP) &&
                event->cbutton.timestamp != kSyntheticEventTimestamp) {
            auto* probe = static_cast<LatencyProbe*>(userdata);
            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                probe->onPhysicalAEvent(event->type == SDL_CONTROLLERBUTTONDOWN,
                                        event->cbutton.which);
            }
            else if (event->cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
                probe->onPhysicalYEvent(event->type == SDL_CONTROLLERBUTTONDOWN,
                                        event->cbutton.which);
            }
            else if (event->type == SDL_CONTROLLERBUTTONDOWN &&
                     event->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                SDL_AtomicLock(&probe->m_Lock);
                if (probe->m_Enabled &&
                        (probe->m_BenchmarkStarting ||
                         probe->m_HelperRunning ||
                         probe->m_AutoBenchmark)) {
                    probe->m_StopRequested = true;
                }
                SDL_AtomicUnlock(&probe->m_Lock);
            }
        }

        return 1;
    }

    static Uint32 SDLCALL holdTimerCallback(Uint32, void* userdata)
    {
        static_cast<LatencyProbe*>(userdata)->holdTimerFired();
        return 0;
    }

    static Uint32 SDLCALL telemetryHoldTimerCallback(Uint32, void* userdata)
    {
        static_cast<LatencyProbe*>(userdata)->telemetryHoldTimerFired();
        return 0;
    }

    static Uint32 SDLCALL benchmarkTimerCallback(Uint32, void* userdata)
    {
        return static_cast<LatencyProbe*>(userdata)->benchmarkTimerTick() ?
                    kBenchmarkTickMs : 0;
    }

    static VisualState classify(float luma)
    {
        if (luma < 0.45f) {
            return VisualState::Dark;
        }
        if (luma > 0.55f) {
            return VisualState::Bright;
        }
        return VisualState::Unknown;
    }

    void refreshVideoSamplingFastLocked()
    {
        const bool needed = m_Enabled && m_HelperRunning && !m_BenchmarkStarting &&
                (m_Baseline == VisualState::Unknown || m_WaitingForTransition);
        m_NeedsVideoSampleFast.store(needed, std::memory_order_release);
    }

    void onPhysicalAEvent(bool pressed, SDL_JoystickID controllerId)
    {
        bool armHoldTimer = false;
        SDL_TimerID holdTimerToRemove = 0;

        SDL_AtomicLock(&m_Lock);

        const bool benchmarkInProgress =
                m_BenchmarkStarting || m_HelperRunning || m_AutoBenchmark;
        if (!m_Enabled || (!m_OsdVisible && !benchmarkInProgress)) {
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        // Keep synthetic pulses bound to the controller that started the run.
        if (benchmarkInProgress && controllerId != m_BenchmarkControllerId) {
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        if (pressed) {
            // A normal press remains purely normal gameplay input. Arm only a
            // one-shot 750 ms hold timer; short presses never create the 17 ms
            // benchmark timer and never touch the render sampling path.
            if (!m_PhysicalAHeld) {
                m_PhysicalAHeld = true;
                m_BenchmarkControllerId = controllerId;
                if (m_OsdVisible &&
                        !m_BenchmarkStarting && !m_HelperRunning && !m_AutoBenchmark &&
                        m_HoldTimer == 0) {
                    m_StopRequested = false;
                    armHoldTimer = true;
                }
            }
        }
        else if (controllerId == m_BenchmarkControllerId) {
            m_PhysicalAHeld = false;
            if (!m_BenchmarkStarting && !m_HelperRunning && !m_AutoBenchmark &&
                    m_HoldTimer != 0) {
                holdTimerToRemove = m_HoldTimer;
                m_HoldTimer = 0;
            }
        }

        SDL_AtomicUnlock(&m_Lock);

        if (holdTimerToRemove != 0) {
            SDL_RemoveTimer(holdTimerToRemove);
        }

        if (armHoldTimer) {
            SDL_TimerID timer = SDL_AddTimer(kBenchmarkHoldMs, holdTimerCallback, this);
            if (timer == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Latency probe: unable to arm hold timer");
                return;
            }

            bool keepTimer = false;
            SDL_AtomicLock(&m_Lock);
            if (m_Enabled && m_OsdVisible && m_PhysicalAHeld &&
                    !m_BenchmarkStarting && !m_HelperRunning && !m_AutoBenchmark &&
                    m_HoldTimer == 0) {
                m_HoldTimer = timer;
                keepTimer = true;
            }
            SDL_AtomicUnlock(&m_Lock);

            if (!keepTimer) {
                SDL_RemoveTimer(timer);
            }
        }
    }

    void onPhysicalYEvent(bool pressed, SDL_JoystickID controllerId)
    {
        bool armHoldTimer = false;
        SDL_TimerID holdTimerToRemove = 0;

        SDL_AtomicLock(&m_Lock);

        if (!m_Enabled || !m_OsdVisible) {
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        if (pressed) {
            if (!m_PhysicalYHeld) {
                m_PhysicalYHeld = true;
                m_TelemetryControllerId = controllerId;
                if (!m_BenchmarkStarting && !m_HelperRunning && !m_AutoBenchmark &&
                        !m_BenchmarkRan && m_TelemetryHoldTimer == 0) {
                    armHoldTimer = true;
                }
            }
        }
        else if (controllerId == m_TelemetryControllerId) {
            m_PhysicalYHeld = false;
            if (m_TelemetryHoldTimer != 0) {
                holdTimerToRemove = m_TelemetryHoldTimer;
                m_TelemetryHoldTimer = 0;
            }
        }

        SDL_AtomicUnlock(&m_Lock);

        if (holdTimerToRemove != 0) {
            SDL_RemoveTimer(holdTimerToRemove);
        }

        if (armHoldTimer) {
            SDL_TimerID timer = SDL_AddTimer(kTelemetryHoldMs,
                                             telemetryHoldTimerCallback,
                                             this);
            if (timer == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Latency probe: unable to arm telemetry hold timer");
                return;
            }

            bool keepTimer = false;
            SDL_AtomicLock(&m_Lock);
            if (m_Enabled && m_OsdVisible && m_PhysicalYHeld &&
                    m_TelemetryControllerId == controllerId &&
                    !m_BenchmarkStarting && !m_HelperRunning && !m_AutoBenchmark &&
                    !m_BenchmarkRan && m_TelemetryHoldTimer == 0) {
                m_TelemetryHoldTimer = timer;
                keepTimer = true;
            }
            SDL_AtomicUnlock(&m_Lock);

            if (!keepTimer) {
                SDL_RemoveTimer(timer);
            }
        }
    }

    void telemetryHoldTimerFired()
    {
        bool freezeTelemetry = false;
        bool resumeTelemetry = false;

        SDL_AtomicLock(&m_Lock);
        m_TelemetryHoldTimer = 0;
        if (m_Enabled && m_OsdVisible && m_PhysicalYHeld &&
                !m_BenchmarkStarting && !m_HelperRunning && !m_AutoBenchmark &&
                !m_BenchmarkRan) {
            if (StreamPipelineTelemetry::isActiveFast()) {
                m_ManualTelemetryFrozen = true;
                freezeTelemetry = true;
            }
            else if (m_ManualTelemetryFrozen) {
                m_ManualTelemetryFrozen = false;
                resumeTelemetry = true;
            }
        }
        SDL_AtomicUnlock(&m_Lock);

        if (freezeTelemetry) {
            StreamPipelineTelemetry::stop();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Pipeline telemetry manually frozen");
        }
        else if (resumeTelemetry) {
            StreamPipelineTelemetry::start();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Pipeline telemetry manually resumed");
        }
    }

    void holdTimerFired()
    {
        SDL_AtomicLock(&m_Lock);
        m_HoldTimer = 0;
        const bool eligible = m_Enabled && m_OsdVisible &&
                m_PhysicalAHeld && !m_StopRequested &&
                !m_BenchmarkStarting && !m_HelperRunning && !m_AutoBenchmark;
        SDL_AtomicUnlock(&m_Lock);

        if (!eligible) {
            return;
        }

        SDL_TimerID timer = SDL_AddTimer(kBenchmarkTickMs, benchmarkTimerCallback, this);
        if (timer == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe: unable to start benchmark timer");
            return;
        }

        bool beginBenchmark = false;
        SDL_JoystickID controllerId = 0;
        const Uint32 nowTick = SDL_GetTicks();

        SDL_AtomicLock(&m_Lock);
        if (m_Enabled && m_OsdVisible && m_PhysicalAHeld && !m_StopRequested &&
                !m_BenchmarkStarting && !m_HelperRunning && !m_AutoBenchmark &&
                m_BenchmarkTimer == 0) {
            m_BenchmarkTimer = timer;
            m_BenchmarkStarting = true;
            m_StartRequestPending = true;
            m_StartRequestAfterTick = nowTick + kBenchmarkTickMs;
            m_BaselineSampleAfterTick = 0;
            m_Baseline = VisualState::Unknown;
            m_Expected = VisualState::Unknown;
            m_WaitingForTransition = false;
            m_InputTimestamp = 0;
            m_HasResult = false;
            m_LastLatencyMs = 0.0;
            m_FrozenStatsTimestamp = 0;
            m_ValidationPending = false;
            resetAverageLocked();
            controllerId = m_BenchmarkControllerId;
            beginBenchmark = true;
            refreshVideoSamplingFastLocked();
        }
        SDL_AtomicUnlock(&m_Lock);

        if (!beginBenchmark) {
            SDL_RemoveTimer(timer);
            return;
        }

        // Crossing the hold threshold is the only benchmark activation point.
        // Release A through the ordinary SDL/input path now; the benchmark timer
        // waits one 17 ms tick before issuing the best-effort host START request.
        pushSyntheticAEvent(controllerId, false);
    }

    bool startMeasurementLocked(uint64_t timestamp)
    {
        if (!m_Enabled || !m_HelperRunning || !m_AutoBenchmark ||
                m_WaitingForTransition || m_Baseline == VisualState::Unknown) {
            return false;
        }

        m_InputTimestamp = timestamp;
        m_Expected = m_Baseline == VisualState::Dark ? VisualState::Bright : VisualState::Dark;
        m_WaitingForTransition = true;
        refreshVideoSamplingFastLocked();
        return true;
    }

    bool benchmarkTimerTick()
    {
        enum class PulseAction : uint8_t {
            None,
            Press,
            Release,
        };

        PulseAction action = PulseAction::None;
        SDL_JoystickID controllerId = 0;
        bool requestStart = false;
        bool requestStop = false;
        bool telemetryStart = false;
        bool telemetryStop = false;
        bool displayMeasurementStarted = false;
        bool displayExpectedBright = false;
        bool displayValidationMeasurement = false;
        const Uint32 nowTick = SDL_GetTicks();
        const uint64_t nowCounter = SDL_GetPerformanceCounter();

        SDL_AtomicLock(&m_Lock);

        if (!m_Enabled) {
            m_BenchmarkTimer = 0;
            refreshVideoSamplingFastLocked();
            SDL_AtomicUnlock(&m_Lock);
            return false;
        }

        checkTimeoutLocked(nowCounter);

        if (m_StopRequested) {
            controllerId = m_BenchmarkControllerId;
            requestStop = m_HelperRunning;
            telemetryStop = m_AutoBenchmark;
            if (m_AutoBenchmark) {
                pruneAverageLocked(nowCounter);
                m_FrozenStatsTimestamp = nowCounter;
            }
            m_StopRequested = false;
            m_BenchmarkStarting = false;
            m_StartRequestPending = false;
            m_StartRequestAfterTick = 0;
            m_BaselineSampleAfterTick = 0;
            m_HelperRunning = false;
            m_AutoBenchmark = false;
            m_AutoButtonDown = false;
            m_ValidationPending = false;
            m_WaitingForTransition = false;
            m_Baseline = VisualState::Unknown;
            m_Expected = VisualState::Unknown;
            m_InputTimestamp = 0;
            action = m_PhysicalAHeld ? PulseAction::Press : PulseAction::Release;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe: automatic benchmark stopped");
        }
        else if (m_BenchmarkStarting) {
            if (m_StartRequestPending &&
                    SDL_TICKS_PASSED(nowTick, m_StartRequestAfterTick)) {
                // START is fire-and-forget. From this point, readiness is governed
                // only by the fixed startup delay and subsequent video validation.
                m_StartRequestPending = false;
                m_HelperRunning = true;
                m_BaselineSampleAfterTick = nowTick + kHelperStartupDelayMs;
                requestStart = true;
            }
            else if (!m_StartRequestPending && m_HelperRunning &&
                     SDL_TICKS_PASSED(nowTick, m_BaselineSampleAfterTick)) {
                m_BenchmarkStarting = false;
            }
        }
        else if (m_HelperRunning && !m_AutoBenchmark) {
            controllerId = m_BenchmarkControllerId;

            if (m_Baseline != VisualState::Unknown) {
                // A classified post-delay baseline is only a candidate helper.
                // The first A-driven transition validates it and is discarded.
                m_AutoBenchmark = true;
                m_BenchmarkRan = true;
                m_ManualTelemetryFrozen = false;
                m_ValidationPending = true;
                m_AutoButtonDown = false;
                m_AutoDownTick = 0;
                m_AutoNextDownTick = nowTick + kBenchmarkPressMs;
                m_PulseJitterState = static_cast<uint32_t>(nowCounter);
                telemetryStart = true;

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Latency probe: automatic benchmark started");
            }
        }
        else if (m_AutoBenchmark) {
            controllerId = m_BenchmarkControllerId;

            if (m_AutoButtonDown) {
                if (SDL_TICKS_PASSED(nowTick, m_AutoDownTick + kBenchmarkPressMs)) {
                    m_AutoButtonDown = false;
                    action = PulseAction::Release;
                }
            }
            else if (SDL_TICKS_PASSED(nowTick, m_AutoNextDownTick)) {
                if (startMeasurementLocked(nowCounter)) {
                    m_AutoButtonDown = true;
                    m_AutoDownTick = nowTick;
                    m_AutoNextDownTick = nowTick + nextBenchmarkPeriodLocked();
                    action = PulseAction::Press;
                    displayMeasurementStarted = true;
                    displayExpectedBright = m_Expected == VisualState::Bright;
                    displayValidationMeasurement = m_ValidationPending;
                }
                else {
                    m_AutoNextDownTick = nowTick + kBenchmarkTickMs;
                }
            }
        }

        const bool keepTimer = m_Enabled &&
                (m_BenchmarkStarting || m_HelperRunning || m_AutoBenchmark);
        if (!keepTimer) {
            m_BenchmarkTimer = 0;
        }
        refreshVideoSamplingFastLocked();
        SDL_AtomicUnlock(&m_Lock);

        if (action == PulseAction::Press) {
            pushSyntheticAEvent(controllerId, true);

            // Publish the exact same t0 value only after the original synthetic
            // input has been queued, so the additive metric cannot delay it.
            if (displayMeasurementStarted) {
                DisplayPresentLatency::measurementStarted(nowCounter,
                                                          displayExpectedBright,
                                                          displayValidationMeasurement);
            }
        }
        else if (action == PulseAction::Release) {
            pushSyntheticAEvent(controllerId, false);
        }

        if (telemetryStart) {
            StreamPipelineTelemetry::start();
            DisplayPresentLatency::beginRun();
        }
        if (telemetryStop) {
            StreamPipelineTelemetry::stop();
            DisplayPresentLatency::endRun(nowCounter);
        }

        if (requestStart) {
            LatencyBenchmarkControl::startAsync();
        }
        else if (requestStop) {
            LatencyBenchmarkControl::stopAsync();
        }

        return keepTimer;
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
            const double latencyMs =
                    (double)(submitTimestamp - m_InputTimestamp) * 1000.0 / (double)frequency;

            if (m_ValidationPending) {
                // The first successful A-driven transition proves that the sampled
                // baseline belongs to the helper. Deliberately discard its timing.
                m_ValidationPending = false;
                m_HasResult = false;
                m_LastLatencyMs = 0.0;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Latency probe: helper transition validated on frame=%llu",
                            (unsigned long long)serial);
            }
            else {
                m_LastLatencyMs = latencyMs;
                m_HasResult = true;

                if (m_AutoBenchmark) {
                    addAverageSampleLocked(submitTimestamp, m_LastLatencyMs);
                }
            }
        }

        m_WaitingForTransition = false;
        m_Expected = VisualState::Unknown;
        refreshVideoSamplingFastLocked();
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
            refreshVideoSamplingFastLocked();
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

    bool getStatsLocked(uint64_t now,
                        double& averageMs,
                        bool& averageValid,
                        double& average10sMs,
                        double& maximum10sMs,
                        size_t& count10s)
    {
        const uint64_t referenceNow =
                m_AutoBenchmark || m_FrozenStatsTimestamp == 0 ?
                now : m_FrozenStatsTimestamp;
        if (m_AutoBenchmark) {
            pruneAverageLocked(referenceNow);
        }

        if (m_AverageCount == 0) {
            averageMs = 0.0;
            averageValid = false;
            average10sMs = 0.0;
            maximum10sMs = 0.0;
            count10s = 0;
            return false;
        }

        const uint64_t frequency = SDL_GetPerformanceFrequency();
        const uint64_t shortWindowTicks =
                frequency == 0 ? 0 : frequency * kShortAverageWindowMs / 1000;
        double sumAverage = 0.0;
        size_t averageCount = 0;
        double sum10s = 0.0;
        maximum10sMs = 0.0;

        for (size_t i = 0; i < m_AverageCount; ++i) {
            const size_t index = (m_AverageStart + i) % kAverageCapacity;
            const LatencySample& sample = m_AverageSamples[index];
            sum10s += sample.latencyMs;
            if (i == 0 || sample.latencyMs > maximum10sMs) {
                maximum10sMs = sample.latencyMs;
            }

            if (frequency != 0 && sample.timestamp <= referenceNow &&
                    referenceNow - sample.timestamp <= shortWindowTicks) {
                sumAverage += sample.latencyMs;
                averageCount++;
            }
        }

        count10s = m_AverageCount;
        average10sMs = sum10s / static_cast<double>(m_AverageCount);
        averageValid = averageCount != 0;
        averageMs = averageValid ?
                sumAverage / static_cast<double>(averageCount) : 0.0;
        return true;
    }

    SDL_SpinLock m_Lock = 0;
    std::atomic<bool> m_NeedsVideoSampleFast { false };
    bool m_Enabled = false;
    bool m_OsdVisible = false;
    bool m_WaitingForTransition = false;
    bool m_HasResult = false;
    VisualState m_Baseline = VisualState::Unknown;
    VisualState m_Expected = VisualState::Unknown;
    uint64_t m_InputTimestamp = 0;
    double m_LastLatencyMs = 0.0;
    uint64_t m_FrozenStatsTimestamp = 0;

    SDL_TimerID m_HoldTimer = 0;
    SDL_TimerID m_TelemetryHoldTimer = 0;
    SDL_TimerID m_BenchmarkTimer = 0;
    bool m_PhysicalAHeld = false;
    bool m_PhysicalYHeld = false;
    bool m_StopRequested = false;
    SDL_JoystickID m_BenchmarkControllerId = 0;
    SDL_JoystickID m_TelemetryControllerId = 0;
    bool m_BenchmarkStarting = false;
    bool m_StartRequestPending = false;
    Uint32 m_StartRequestAfterTick = 0;
    Uint32 m_BaselineSampleAfterTick = 0;
    bool m_HelperRunning = false;
    bool m_ValidationPending = false;
    bool m_AutoBenchmark = false;
    bool m_BenchmarkRan = false;
    bool m_ManualTelemetryFrozen = false;
    bool m_AutoButtonDown = false;
    Uint32 m_AutoDownTick = 0;
    Uint32 m_AutoNextDownTick = 0;
    uint32_t m_PulseJitterState = 0;

    LatencySample m_AverageSamples[kAverageCapacity] = {};
    size_t m_AverageStart = 0;
    size_t m_AverageCount = 0;
};