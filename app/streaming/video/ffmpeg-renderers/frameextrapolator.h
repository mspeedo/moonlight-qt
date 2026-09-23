#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)

#include <algorithm>
#include <cstdint>

#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>

#include "streaming/streamhealthtelemetry.h"

extern "C" {
#include <libavutil/frame.h>
}

class FrameExtrapolator
{
public:
    FrameExtrapolator(pl_log log, pl_gpu gpu);
    ~FrameExtrapolator();

    bool initialize();

    // Queues reduced-resolution analysis for a real frame. This never waits for
    // the GPU; libplacebo tracks the resource dependencies between dispatches.
    bool submitRealFrame(const AVFrame* frame, pl_frame& mappedFrame, uint64_t renderTimeUs);

    // Queue a one-frame-ahead synthetic YUV frame immediately after a real frame.
    // Unlike the deadline path, this deliberately does not wait for analysis to
    // complete: libplacebo orders the warp behind the just-queued motion passes.
    // It returns false rather than building a backlog if the reusable synthetic
    // planes are still in use by an older prepared frame.
    bool buildPreparedSyntheticFrame(pl_frame& currentFrame,
                                     uint64_t frameIntervalUs,
                                     pl_frame* syntheticFrame)
    {
        if (!m_ResourcesReady || !m_HasMotion || m_SyntheticSinceLastReal ||
                m_MotionPairIntervalUs == 0 || frameIntervalUs == 0 ||
                currentFrame.num_planes != m_PlaneCount || syntheticFrame == nullptr) {
            return false;
        }

        const double temporalScale = (double)frameIntervalUs /
                (double)m_MotionPairIntervalUs;
        if (temporalScale < 0.35 || temporalScale > 1.50) {
            return false;
        }

        // Never queue a second prediction behind an older prepared frame. The
        // deadline path must either find an already prepared image or skip this
        // opportunity; background prediction must not become a GPU backlog that
        // delays real-frame rendering.
        for (int i = 0; i < m_PlaneCount; ++i) {
            if (m_SyntheticPlanes[i] == nullptr ||
                    pl_tex_poll(m_Gpu, m_SyntheticPlanes[i], 0)) {
                return false;
            }
        }

        struct AcquireGuard {
            pl_gpu gpu;
            pl_frame* frame;
            bool ok = false;
            bool release = false;

            AcquireGuard(pl_gpu gpu_, pl_frame& frame_) :
                gpu(gpu_), frame(&frame_)
            {
                if (frame_.acquire == nullptr) {
                    ok = true;
                    return;
                }
                if (frame_.release == nullptr) {
                    return;
                }
                ok = frame_.acquire(gpu_, &frame_);
                release = ok;
            }

            ~AcquireGuard()
            {
                if (release) {
                    frame->release(gpu, frame);
                }
            }
        } frameGuard(m_Gpu, currentFrame);

        if (!frameGuard.ok) {
            return false;
        }

        // This prediction represents exactly the next expected real-frame
        // interval. Normalize motion fields spanning more/less than one RTP
        // interval before projecting them forward.
        const float alpha = (float)std::clamp(temporalScale, 0.4, 1.25);

        for (int i = 0; i < currentFrame.num_planes; ++i) {
            pl_tex source = currentFrame.planes[i].texture;
            if (source == nullptr ||
                    source->params.w != m_PlaneWidths[i] ||
                    source->params.h != m_PlaneHeights[i] ||
                    currentFrame.planes[i].components != m_PlaneComponents[i]) {
                return false;
            }

            if (!dispatchWarp(source, m_SyntheticPlanes[i], alpha)) {
                return false;
            }
        }

        *syntheticFrame = currentFrame;
        for (int i = 0; i < syntheticFrame->num_planes; ++i) {
            syntheticFrame->planes[i].texture = m_SyntheticPlanes[i];
        }

        // These textures are owned by FrameExtrapolator rather than FFmpeg.
        syntheticFrame->acquire = nullptr;
        syntheticFrame->release = nullptr;
        syntheticFrame->user_data = nullptr;
        return true;
    }

    // Non-blocking readiness check. If analysis is still executing, this returns
    // false so the pacer preserves normal hold/repeat behavior.
    bool canExtrapolate(uint64_t targetTimeUs, uint64_t frameIntervalUs);

    const AVFrame* latestRealFrame() const { return m_LatestRealFrame; }

    // Legacy on-demand builder retained as a fallback/debug path. The normal
    // Vulkan extrapolation path now presents an ahead-of-time prepared image.
    bool buildSyntheticFrame(pl_frame& currentFrame,
                             uint64_t targetTimeUs,
                             uint64_t frameIntervalUs,
                             pl_frame* syntheticFrame);

    void markSyntheticPresented()
    {
        m_SyntheticSinceLastReal = true;
        StreamHealthTelemetry::frameExtrapolated();
    }

private:
    bool ensureResources(const pl_frame& frame);
    bool createTexture(pl_tex* texture, int width, int height, int components);
    void destroyResources();

    bool dispatchDownsample(pl_tex source, pl_tex target, int scale);
    bool dispatchSceneMetric(pl_tex current, pl_tex previous);
    bool dispatchCoarseMotion(pl_tex current, pl_tex previous);
    bool dispatchFineMotion(pl_tex current, pl_tex previous);
    bool dispatchWarp(pl_tex source, pl_tex target, float alpha);

    bool runCompute(pl_tex target,
                    const char* description,
                    const char* header,
                    const char* body,
                    const pl_shader_desc* descriptors,
                    int descriptorCount,
                    const pl_shader_var* variables = nullptr,
                    int variableCount = 0,
                    int groupSizeX = 8,
                    int groupSizeY = 8);

    pl_log m_Log;
    pl_gpu m_Gpu;
    pl_dispatch m_Dispatch = nullptr;
    AVFrame* m_LatestRealFrame = nullptr;

    uint64_t m_LastRealRenderTimeUs = 0;
    int64_t m_LastRealPts = AV_NOPTS_VALUE;
    uint64_t m_MotionPairIntervalUs = 0;
    uint64_t m_LastTelemetryTargetUs = 0;

    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_FineWidth = 0;
    int m_FineHeight = 0;
    int m_CoarseWidth = 0;
    int m_CoarseHeight = 0;
    int m_PlaneCount = 0;
    int m_PlaneWidths[PL_MAX_PLANES] = {};
    int m_PlaneHeights[PL_MAX_PLANES] = {};
    int m_PlaneComponents[PL_MAX_PLANES] = {};
    uint64_t m_PlaneFormatSignatures[PL_MAX_PLANES] = {};

    pl_tex m_FineLuma[2] = {};
    pl_tex m_CoarseLuma[2] = {};
    pl_tex m_CoarseMotion = nullptr;
    pl_tex m_FineMotion = nullptr;
    pl_tex m_SceneMetric = nullptr;
    pl_tex m_SyntheticPlanes[PL_MAX_PLANES] = {};

    int m_HistoryIndex = 0;
    int m_MotionAgeFrames = 0;
    bool m_HasHistory = false;
    bool m_HasMotion = false;
    bool m_ResourcesReady = false;
    bool m_SyntheticSinceLastReal = false;
};

#endif
