#pragma once

#include <QtGlobal>

#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)

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
    FrameExtrapolator(pl_log log, pl_gpu gpu, bool enableQualityMeasurement);
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
                                     pl_frame* syntheticFrame);

    // Compare the synthetic candidate and held real frame against the decoded
    // real frame that the synthetic frame replaced. All work and readback are
    // asynchronous; this function never waits for the GPU.
    bool evaluateGroundTruthQuality(pl_frame& groundTruthFrame);
    bool isQualityMeasurementEnabled() const { return m_QualityMeasurementEnabled; }

    // Non-blocking readiness check. If analysis is still executing, this returns
    // false so the pacer preserves normal hold/repeat behavior.
    bool canExtrapolate(uint64_t targetTimeUs, uint64_t frameIntervalUs);

    void markSyntheticPresented();

private:
    bool ensureResources(const pl_frame& frame);
    bool ensureQualityResources();
    bool createTexture(pl_tex* texture, int width, int height, int components);
    bool createQualityMetricTexture(pl_tex* texture);
    void destroyResources();

    bool dispatchDownsample(pl_tex source, pl_tex target, int scale);
    bool dispatchQualityGrid(pl_tex source, pl_tex target);
    bool dispatchQualityMetric();
    bool dispatchAffineDiagnosticMetric();
    bool queueAffineDiagnosticReadback();
    bool dispatchSceneMetric(pl_tex current, pl_tex previous);
    bool dispatchGlobalTranslationCosts(pl_tex current, pl_tex previous);
    bool dispatchGlobalTranslationSelect();
    bool dispatchFineMotion(pl_tex current, pl_tex previous);
    bool dispatchAffineHypotheses();
    bool dispatchAffineModel();
    bool dispatchAffineHoldMask();
    bool dispatchAffineWarp(pl_tex source, pl_tex target, float alpha);

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
    bool m_QualityMeasurementEnabled = false;
    pl_dispatch m_Dispatch = nullptr;
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
    pl_tex m_GlobalTranslationCosts = nullptr;
    pl_tex m_GlobalTranslation = nullptr;
    pl_tex m_FineMotion = nullptr;
    pl_tex m_AffineHypotheses = nullptr;
    pl_tex m_AffineModel = nullptr;
    pl_tex m_AffineHoldMask = nullptr;
    pl_tex m_SceneMetric = nullptr;
    pl_tex m_SyntheticPlanes[PL_MAX_PLANES] = {};

    // Tiny diagnostic-only textures. The baseline grid snapshots the exact real
    // frame used as the source of the prepared candidate. Synthetic and ground
    // truth grids are produced only after a synthetic frame has actually been
    // presented and its matching real frame later arrives.
    pl_tex m_QualityBaselineGrid = nullptr;
    pl_tex m_QualitySyntheticGrid = nullptr;
    pl_tex m_QualityGroundTruthGrid = nullptr;
    pl_tex m_QualityMetric = nullptr;
    pl_tex m_AffineDiagnosticMetric = nullptr;
    int m_QualityGridWidth = 0;
    int m_QualityGridHeight = 0;

    int m_HistoryIndex = 0;
    int m_MotionAgeFrames = 0;
    bool m_HasHistory = false;
    bool m_HasMotion = false;
    bool m_ResourcesReady = false;
    bool m_QualityResourcesReady = false;
    bool m_HasQualityBaseline = false;
    bool m_HasAffineDiagnostic = false;
    bool m_SyntheticSinceLastReal = false;
};

#endif
