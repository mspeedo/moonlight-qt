#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)

#include <cstdint>

#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>

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
    bool submitRealFrame(const AVFrame* frame, const pl_frame& mappedFrame, uint64_t renderTimeUs);

    // Non-blocking readiness check. If analysis is still executing, this returns
    // false so the pacer preserves normal hold/repeat behavior.
    bool canExtrapolate(uint64_t targetTimeUs, uint64_t frameIntervalUs);

    const AVFrame* latestRealFrame() const { return m_LatestRealFrame; }

    // Produces a YUV-compatible synthetic pl_frame using the current real frame
    // as the source. The returned frame borrows this object's synthetic textures.
    bool buildSyntheticFrame(const pl_frame& currentFrame,
                             uint64_t targetTimeUs,
                             uint64_t frameIntervalUs,
                             pl_frame* syntheticFrame);

    void markSyntheticPresented() { m_SyntheticSinceLastReal = true; }

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
    bool m_HasHistory = false;
    bool m_HasMotion = false;
    bool m_ResourcesReady = false;
    bool m_SyntheticSinceLastReal = false;
};

#endif
