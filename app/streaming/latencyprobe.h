#pragma once

#include "SDL_compat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Shared state for the optional end-to-end input latency probe.
//
// The probe is enabled only while the debug/performance OSD is visible. While
// enabled, an SDL event watch timestamps the physical controller A-button as
// soon as Moonlight receives it. The ordinary Moonlight gamepad path remains
// completely untouched and forwards the same input to Sunshine. The Vulkan
// hook then samples incoming frames until the helper's dark/bright transition
// is observed and associates that frame with its presentation submission time.
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

        SDL_AtomicLock(&m_Lock);
        if (m_Enabled != enabled) {
            stateChanged = true;
            m_Enabled = enabled;
            m_EnabledFast.store(enabled, std::memory_order_release);
            m_WaitingForTransition = false;
            m_HasResult = false;
            m_Baseline = VisualState::Unknown;
            m_Expected = VisualState::Unknown;
            m_InputTimestamp = 0;
            m_LastLatencyMs = 0.0;
        }
        SDL_AtomicUnlock(&m_Lock);

        if (!stateChanged) {
            return;
        }

        // Register the controller watcher only while the OSD is visible. This
        // keeps the normal input path completely free of probe work otherwise.
        if (enabled) {
            SDL_AddEventWatch(controllerEventWatch, this);
        }
        else {
            SDL_DelEventWatch(controllerEventWatch, this);
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
        if (!m_Enabled || m_WaitingForTransition || m_Baseline == VisualState::Unknown) {
            SDL_AtomicUnlock(&m_Lock);
            return;
        }

        m_InputTimestamp = timestamp;
        m_Expected = m_Baseline == VisualState::Dark ? VisualState::Bright : VisualState::Dark;
        m_WaitingForTransition = true;
        m_HasResult = false;
        m_LastLatencyMs = 0.0;

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
        checkTimeoutLocked(SDL_GetPerformanceCounter());

        if (m_WaitingForTransition) {
            std::snprintf(output, length, "Input to present latency: measuring...");
        }
        else if (m_HasResult) {
            std::snprintf(output, length, "Input to present latency: %.2f ms", m_LastLatencyMs);
        }
        else if (m_Baseline == VisualState::Unknown) {
            std::snprintf(output, length, "Input to present latency: acquiring baseline...");
        }
        else {
            std::snprintf(output, length, "Input to present latency: ready (press A)");
        }

        SDL_AtomicUnlock(&m_Lock);
    }

private:
    static constexpr uint64_t kTimeoutMs = 500;

    LatencyProbe() = default;

    static int SDLCALL controllerEventWatch(void* userdata, SDL_Event* event)
    {
        if (event->type == SDL_CONTROLLERBUTTONDOWN &&
                event->cbutton.button == SDL_CONTROLLER_BUTTON_A) {
            static_cast<LatencyProbe*>(userdata)->onControllerATrigger(SDL_GetPerformanceCounter());
        }

        // Event-watch return values are ignored, but return 1 for consistency
        // with SDL event-filter conventions.
        return 1;
    }

    static VisualState classify(float luma)
    {
        // The host helper deliberately leaves a wide luminance gap between its
        // dark-noise and bright-noise distributions. Keep the client threshold
        // similarly generous so codec quantization cannot cause false edges.
        if (luma < 0.45f) {
            return VisualState::Dark;
        }
        if (luma > 0.55f) {
            return VisualState::Bright;
        }
        return VisualState::Unknown;
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

    SDL_SpinLock m_Lock = 0;
    std::atomic<bool> m_EnabledFast { false };
    bool m_Enabled = false;
    bool m_WaitingForTransition = false;
    bool m_HasResult = false;
    VisualState m_Baseline = VisualState::Unknown;
    VisualState m_Expected = VisualState::Unknown;
    uint64_t m_InputTimestamp = 0;
    double m_LastLatencyMs = 0.0;
};
