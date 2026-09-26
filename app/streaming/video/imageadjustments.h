#pragma once

#include <atomic>
#include <cmath>

namespace ImageAdjustments {

struct State {
    float sharpening;
    float saturation;
    bool enabled;
};

// Store tenths as integers so the render thread gets exact 0.1 steps with
// cheap lock-free atomic reads.
inline std::atomic<int>& sharpeningState()
{
    static std::atomic<int> value { 0 };
    return value;
}

inline std::atomic<int>& saturationState()
{
    static std::atomic<int> value { 10 };
    return value;
}

inline std::atomic<bool>& enabledState()
{
    static std::atomic<bool> value { true };
    return value;
}

inline std::atomic<bool>& hdrStreamActiveState()
{
    static std::atomic<bool> value { false };
    return value;
}

inline std::atomic<bool>& osdOpenState()
{
    static std::atomic<bool> value { false };
    return value;
}

inline std::atomic<int>& selectedRowState()
{
    static std::atomic<int> value { 0 };
    return value;
}

inline int clampInt(int value, int minimum, int maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

inline int toTenths(float value)
{
    return static_cast<int>(std::lround(value * 10.0f));
}

inline float fromTenths(int value)
{
    return static_cast<float>(value) * 0.1f;
}

inline void initialize(float sharpening, float saturation, bool enabled)
{
    sharpeningState().store(clampInt(toTenths(sharpening), 0, 10),
                            std::memory_order_relaxed);
    saturationState().store(clampInt(toTenths(saturation), 10, 20),
                           std::memory_order_relaxed);
    enabledState().store(enabled, std::memory_order_relaxed);
    hdrStreamActiveState().store(false, std::memory_order_relaxed);
    osdOpenState().store(false, std::memory_order_release);
    selectedRowState().store(0, std::memory_order_relaxed);
}

inline State snapshot()
{
    State state;
    state.enabled = enabledState().load(std::memory_order_relaxed);
    state.sharpening = fromTenths(
            sharpeningState().load(std::memory_order_relaxed));
    state.saturation = fromTenths(
            saturationState().load(std::memory_order_relaxed));
    return state;
}

inline void setSharpening(float value)
{
    sharpeningState().store(clampInt(toTenths(value), 0, 10),
                            std::memory_order_relaxed);
}

inline void setSaturation(float value)
{
    saturationState().store(clampInt(toTenths(value), 10, 20),
                           std::memory_order_relaxed);
}

inline void setEnabled(bool enabled)
{
    enabledState().store(enabled, std::memory_order_relaxed);
}

inline bool isHdrStreamActive()
{
    return hdrStreamActiveState().load(std::memory_order_relaxed);
}

inline void setHdrStreamActive(bool active)
{
    hdrStreamActiveState().store(active, std::memory_order_relaxed);
}

inline bool isOsdOpen()
{
    return osdOpenState().load(std::memory_order_acquire);
}

inline void setOsdOpen(bool open)
{
    osdOpenState().store(open, std::memory_order_release);
}

inline int selectedRow()
{
    return selectedRowState().load(std::memory_order_relaxed);
}

inline void setSelectedRow(int row)
{
    selectedRowState().store(row == 0 ? 0 : 1, std::memory_order_relaxed);
}

} // namespace ImageAdjustments
