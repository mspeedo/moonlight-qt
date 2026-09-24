#include "frameextrapolator.h"

#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)

#include <algorithm>
#include <cstdio>

#include <libplacebo/shaders/custom.h>

namespace {

struct QualityReadback
{
    std::uint64_t generation = 0;
    alignas(16) float values[4] = {};
};

struct AffineDiagnosticReadback
{
    std::uint64_t generation = 0;
    alignas(16) float values[4] = {};
};

void qualityReadbackComplete(void* opaque)
{
    QualityReadback* readback = static_cast<QualityReadback*>(opaque);
    if (readback == nullptr) {
        return;
    }

    StreamHealthTelemetry::frameExtrapolationQualitySample(
            readback->generation,
            readback->values[0],
            readback->values[1],
            readback->values[2],
            readback->values[3]);
    delete readback;
}

void affineDiagnosticReadbackComplete(void* opaque)
{
    AffineDiagnosticReadback* readback = static_cast<AffineDiagnosticReadback*>(opaque);
    if (readback == nullptr) {
        return;
    }

    StreamHealthTelemetry::frameExtrapolationAffineSample(
            readback->generation,
            readback->values[0],
            readback->values[1],
            readback->values[2],
            readback->values[3]);
    delete readback;
}

class FrameAcquireGuard
{
public:
    FrameAcquireGuard(pl_gpu gpu, pl_frame& frame) :
        m_Gpu(gpu),
        m_Frame(&frame)
    {
        if (frame.acquire == nullptr) {
            m_Ok = true;
            return;
        }

        // A frame that needs explicit acquisition must also provide the
        // matching release callback so ownership can be returned safely.
        if (frame.release == nullptr) {
            return;
        }

        m_Ok = frame.acquire(gpu, &frame);
        m_NeedsRelease = m_Ok;
    }

    ~FrameAcquireGuard()
    {
        if (m_NeedsRelease) {
            m_Frame->release(m_Gpu, m_Frame);
        }
    }

    bool ok() const { return m_Ok; }

private:
    pl_gpu m_Gpu = nullptr;
    pl_frame* m_Frame = nullptr;
    bool m_Ok = false;
    bool m_NeedsRelease = false;
};

}

FrameExtrapolator::FrameExtrapolator(pl_log log, pl_gpu gpu, bool enableQualityMeasurement) :
    m_Log(log),
    m_Gpu(gpu),
    m_QualityMeasurementEnabled(enableQualityMeasurement)
{
}

FrameExtrapolator::~FrameExtrapolator()
{
    StreamHealthTelemetry::setFrameExtrapolationActive(false);

    // pl_tex_destroy() handles any outstanding GPU use of these resources.
    destroyResources();
    pl_dispatch_destroy(&m_Dispatch);
}

bool FrameExtrapolator::initialize()
{
    m_Dispatch = pl_dispatch_create(m_Log, m_Gpu);
    const bool initialized = m_Dispatch != nullptr;
    StreamHealthTelemetry::setFrameExtrapolationActive(initialized);
    return initialized;
}

bool FrameExtrapolator::createTexture(pl_tex* texture, int width, int height, int components)
{
    // Three-component storage images are not universally available, so use
    // RGBA for three-component video planes while preserving pl_plane metadata.
    // Custom compute writes these through PL_DESC_STORAGE_IMG, so renderable
    // capability is neither required nor requested.
    const int storageComponents = components == 3 ? 4 : components;
    const enum pl_fmt_caps caps = (enum pl_fmt_caps)
            (PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_STORABLE);
    pl_fmt format = pl_find_fmt(m_Gpu, PL_FMT_FLOAT, storageComponents, 16, 0, caps);
    if (format == nullptr) {
        return false;
    }

    struct pl_tex_params params = {};
    params.w = width;
    params.h = height;
    params.format = format;
    params.sampleable = true;
    params.storable = true;

    *texture = pl_tex_create(m_Gpu, &params);
    return *texture != nullptr;
}

bool FrameExtrapolator::createQualityMetricTexture(pl_tex* texture)
{
    // Require an exact float32 host representation so the asynchronous 1x1
    // readback can be interpreted directly as four floats without conversion.
    const enum pl_fmt_caps caps = (enum pl_fmt_caps)
            (PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE);
    pl_fmt format = pl_find_fmt(m_Gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
    if (format == nullptr) {
        return false;
    }

    struct pl_tex_params params = {};
    params.w = 1;
    params.h = 1;
    params.format = format;
    params.storable = true;
    params.host_readable = true;

    *texture = pl_tex_create(m_Gpu, &params);
    return *texture != nullptr;
}

bool FrameExtrapolator::ensureQualityResources()
{
    if (m_QualityResourcesReady) {
        return true;
    }

    if (!m_QualityMeasurementEnabled || m_SourceWidth <= 0 ||
            m_SourceHeight <= 0 || m_Gpu == nullptr ||
            !m_Gpu->limits.callbacks) {
        return false;
    }

    // A tiny aspect-aware grid is enough to answer the important question:
    // did the synthetic frame move luma toward the real future frame, or was
    // holding the previous real frame just as good?
    m_QualityGridWidth = 32;
    m_QualityGridHeight = std::clamp(
            (m_QualityGridWidth * m_SourceHeight + m_SourceWidth / 2) / m_SourceWidth,
            12,
            32);

    if (!createTexture(&m_QualityBaselineGrid,
                       m_QualityGridWidth, m_QualityGridHeight, 1) ||
            !createTexture(&m_QualitySyntheticGrid,
                           m_QualityGridWidth, m_QualityGridHeight, 1) ||
            !createTexture(&m_QualityGroundTruthGrid,
                           m_QualityGridWidth, m_QualityGridHeight, 1) ||
            !createQualityMetricTexture(&m_QualityMetric) ||
            !createQualityMetricTexture(&m_AffineDiagnosticMetric)) {
        pl_tex_destroy(m_Gpu, &m_QualityBaselineGrid);
        pl_tex_destroy(m_Gpu, &m_QualitySyntheticGrid);
        pl_tex_destroy(m_Gpu, &m_QualityGroundTruthGrid);
        pl_tex_destroy(m_Gpu, &m_QualityMetric);
        pl_tex_destroy(m_Gpu, &m_AffineDiagnosticMetric);
        m_QualityGridWidth = 0;
        m_QualityGridHeight = 0;
        return false;
    }

    m_QualityResourcesReady = true;
    return true;
}

void FrameExtrapolator::destroyResources()
{
    if (m_Gpu == nullptr) {
        return;
    }

    for (int i = 0; i < 2; ++i) {
        pl_tex_destroy(m_Gpu, &m_FineLuma[i]);
        pl_tex_destroy(m_Gpu, &m_CoarseLuma[i]);
    }

    pl_tex_destroy(m_Gpu, &m_GlobalTranslationCosts);
    pl_tex_destroy(m_Gpu, &m_GlobalTranslation);
    pl_tex_destroy(m_Gpu, &m_FineMotion);
    pl_tex_destroy(m_Gpu, &m_AffineHypotheses);
    pl_tex_destroy(m_Gpu, &m_AffineModel);
    pl_tex_destroy(m_Gpu, &m_AffineHoldMask);
    pl_tex_destroy(m_Gpu, &m_SceneMetric);

    for (int i = 0; i < PL_MAX_PLANES; ++i) {
        pl_tex_destroy(m_Gpu, &m_SyntheticPlanes[i]);
    }

    pl_tex_destroy(m_Gpu, &m_QualityBaselineGrid);
    pl_tex_destroy(m_Gpu, &m_QualitySyntheticGrid);
    pl_tex_destroy(m_Gpu, &m_QualityGroundTruthGrid);
    pl_tex_destroy(m_Gpu, &m_QualityMetric);
    pl_tex_destroy(m_Gpu, &m_AffineDiagnosticMetric);

    m_ResourcesReady = false;
    m_QualityResourcesReady = false;
    m_HasQualityBaseline = false;
    m_HasAffineDiagnostic = false;
    m_QualityGridWidth = 0;
    m_QualityGridHeight = 0;
    m_HasHistory = false;
    m_HasMotion = false;
    m_MotionAgeFrames = 0;
}

bool FrameExtrapolator::ensureResources(const pl_frame& frame)
{
    if (frame.num_planes <= 0 || frame.num_planes > PL_MAX_PLANES ||
            frame.planes[0].texture == nullptr) {
        return false;
    }

    const int sourceWidth = frame.planes[0].texture->params.w;
    const int sourceHeight = frame.planes[0].texture->params.h;

    if (m_ResourcesReady) {
        if (sourceWidth != m_SourceWidth || sourceHeight != m_SourceHeight ||
                frame.num_planes != m_PlaneCount) {
            m_HasMotion = false;
            m_MotionAgeFrames = 0;
            return false;
        }

        for (int i = 0; i < frame.num_planes; ++i) {
            pl_tex source = frame.planes[i].texture;
            if (source == nullptr ||
                    source->params.w != m_PlaneWidths[i] ||
                    source->params.h != m_PlaneHeights[i] ||
                    frame.planes[i].components != m_PlaneComponents[i] ||
                    source->params.format == nullptr ||
                    source->params.format->signature != m_PlaneFormatSignatures[i]) {
                m_HasMotion = false;
                m_MotionAgeFrames = 0;
                return false;
            }
        }

        return true;
    }

    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_FineWidth = (sourceWidth + 3) / 4;
    m_FineHeight = (sourceHeight + 3) / 4;
    m_CoarseWidth = (m_FineWidth + 1) / 2;
    m_CoarseHeight = (m_FineHeight + 1) / 2;
    m_PlaneCount = frame.num_planes;

    const int fineMotionWidth = (m_FineWidth + 3) / 4;
    const int fineMotionHeight = (m_FineHeight + 3) / 4;
    if (!createTexture(&m_FineLuma[0], m_FineWidth, m_FineHeight, 1) ||
            !createTexture(&m_FineLuma[1], m_FineWidth, m_FineHeight, 1) ||
            !createTexture(&m_CoarseLuma[0], m_CoarseWidth, m_CoarseHeight, 1) ||
            !createTexture(&m_CoarseLuma[1], m_CoarseWidth, m_CoarseHeight, 1) ||
            !createTexture(&m_GlobalTranslationCosts, 17, 17, 1) ||
            !createTexture(&m_GlobalTranslation, 1, 1, 4) ||
            !createTexture(&m_FineMotion, fineMotionWidth, fineMotionHeight, 4) ||
            !createTexture(&m_AffineHypotheses, 16, 2, 4) ||
            !createTexture(&m_AffineModel, 2, 1, 4) ||
            !createTexture(&m_AffineHoldMask, fineMotionWidth, fineMotionHeight, 1) ||
            !createTexture(&m_SceneMetric, 1, 1, 1)) {
        destroyResources();
        return false;
    }

    for (int i = 0; i < frame.num_planes; ++i) {
        pl_tex source = frame.planes[i].texture;
        if (source == nullptr || source->params.format == nullptr ||
                frame.planes[i].components <= 0) {
            destroyResources();
            return false;
        }

        m_PlaneWidths[i] = source->params.w;
        m_PlaneHeights[i] = source->params.h;
        m_PlaneComponents[i] = frame.planes[i].components;
        m_PlaneFormatSignatures[i] = source->params.format->signature;

        if (!createTexture(&m_SyntheticPlanes[i],
                           m_PlaneWidths[i],
                           m_PlaneHeights[i],
                           m_PlaneComponents[i])) {
            destroyResources();
            return false;
        }
    }

    m_ResourcesReady = true;
    // Quality measurement is optional diagnostics. Extrapolation remains fully
    // functional if the GPU lacks asynchronous host-readable storage textures.
    if (m_QualityMeasurementEnabled && !ensureQualityResources()) {
        // Quality telemetry is optional. If this GPU cannot provide the tiny
        // asynchronous readback resources, disable only the diagnostic path.
        m_QualityMeasurementEnabled = false;
        StreamHealthTelemetry::frameExtrapolationQualitySkip();
    }
    return true;
}

bool FrameExtrapolator::runCompute(pl_tex target,
                                   const char* description,
                                   const char* header,
                                   const char* body,
                                   const pl_shader_desc* descriptors,
                                   int descriptorCount,
                                   const pl_shader_var* variables,
                                   int variableCount,
                                   int groupSizeX,
                                   int groupSizeY)
{
    if (target == nullptr || !target->params.storable ||
            descriptorCount < 0 || descriptorCount > 3) {
        return false;
    }

    pl_shader shader = pl_dispatch_begin(m_Dispatch);
    if (shader == nullptr) {
        return false;
    }

    // Mirror libplacebo's own side-effect compute pattern: the destination is
    // an explicit storage image and the shader has no conventional color
    // output. pl_dispatch_compute() then dispatches the shader without a render
    // target while libplacebo owns all Vulkan layout/barrier transitions.
    pl_shader_desc computeDescriptors[4] = {};
    for (int i = 0; i < descriptorCount; ++i) {
        computeDescriptors[i] = descriptors[i];
    }
    computeDescriptors[descriptorCount].desc.name = "out_image";
    computeDescriptors[descriptorCount].desc.type = PL_DESC_STORAGE_IMG;
    computeDescriptors[descriptorCount].desc.access = PL_DESC_ACCESS_WRITEONLY;
    computeDescriptors[descriptorCount].binding.object = (void*)target;

    // Existing pass bodies calculate into `color`. Declare it explicitly,
    // then make imageStore() the only observable output. The bounds guard is
    // required because libplacebo rounds compute workgroups up to cover the
    // requested width and height.
    char wrappedBody[16384];
    const int wrappedLength = std::snprintf(
                wrappedBody,
                sizeof(wrappedBody),
                "ivec2 output_pos = ivec2(gl_GlobalInvocationID.xy);\n"
                "if (all(lessThan(output_pos, imageSize(out_image)))) {\n"
                "vec4 color = vec4(0.0);\n"
                "%s\n"
                "imageStore(out_image, output_pos, color);\n"
                "}\n",
                body != nullptr ? body : "");
    if (wrappedLength < 0 || wrappedLength >= (int)sizeof(wrappedBody)) {
        pl_dispatch_abort(m_Dispatch, &shader);
        return false;
    }

    struct pl_custom_shader params = {};
    params.description = description;
    params.header = header;
    params.body = wrappedBody;
    params.input = PL_SHADER_SIG_NONE;
    params.output = PL_SHADER_SIG_NONE;
    params.descriptors = computeDescriptors;
    params.num_descriptors = descriptorCount + 1;
    params.variables = variables;
    params.num_variables = variableCount;
    params.compute = true;
    params.compute_group_size[0] = groupSizeX;
    params.compute_group_size[1] = groupSizeY;

    if (!pl_shader_custom(shader, &params)) {
        // pl_dispatch_begin() transfers an active shader to the caller. If
        // shader construction fails, return it explicitly instead of leaking
        // an unfinished dispatch object into the next frame.
        pl_dispatch_abort(m_Dispatch, &shader);
        return false;
    }

    struct pl_dispatch_compute_params dispatch = {};
    dispatch.shader = &shader;
    dispatch.width = target->params.w;
    dispatch.height = target->params.h;
    return pl_dispatch_compute(m_Dispatch, &dispatch);
}

bool FrameExtrapolator::dispatchDownsample(pl_tex source, pl_tex target, int scale)
{
    pl_shader_desc descriptor = {};
    descriptor.desc.name = "src_tex";
    descriptor.desc.type = PL_DESC_SAMPLED_TEX;
    descriptor.binding.object = (void*)source;
    descriptor.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    // Use exact box reductions for motion analysis. This is deliberately more
    // expensive than a single filtered lookup, but it prevents high-frequency
    // detail from aliasing into false motion vectors.
    const char* body4 = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 srcSize = textureSize(src_tex, 0);
        ivec2 base = p * 4;
        float y = 0.0;
        for (int oy = 0; oy < 4; ++oy) {
            for (int ox = 0; ox < 4; ++ox) {
                ivec2 sp = clamp(base + ivec2(ox, oy), ivec2(0), srcSize - ivec2(1));
                y += texelFetch(src_tex, sp, 0).r;
            }
        }
        color = vec4(y * (1.0 / 16.0), 0.0, 0.0, 1.0);
    )";

    const char* body2 = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 srcSize = textureSize(src_tex, 0);
        ivec2 base = p * 2;
        float y = 0.0;
        for (int oy = 0; oy < 2; ++oy) {
            for (int ox = 0; ox < 2; ++ox) {
                ivec2 sp = clamp(base + ivec2(ox, oy), ivec2(0), srcSize - ivec2(1));
                y += texelFetch(src_tex, sp, 0).r;
            }
        }
        color = vec4(y * 0.25, 0.0, 0.0, 1.0);
    )";

    return runCompute(target,
                      scale == 4 ? "frame extrapolation luma quarter-res" :
                                   "frame extrapolation luma coarse-res",
                      nullptr,
                      scale == 4 ? body4 : body2,
                      &descriptor,
                      1);
}

bool FrameExtrapolator::dispatchQualityGrid(pl_tex source, pl_tex target)
{
    if (source == nullptr || target == nullptr || source->params.format == nullptr) {
        return false;
    }

    pl_shader_desc descriptor = {};
    descriptor.desc.name = "source_luma";
    descriptor.desc.type = PL_DESC_SAMPLED_TEX;
    descriptor.binding.object = (void*)source;
    descriptor.binding.sample_mode =
            (source->params.format->caps & PL_FMT_CAP_LINEAR) ?
                PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;

    // Each output texel represents one coarse cell of the full image. A 4x4
    // stratified average covers the whole cell rather than sampling a single
    // point, so fast edges and fine benchmark texture don't dominate the score.
    const char* body = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 gridSize = imageSize(out_image);
        vec2 cellMin = vec2(p) / vec2(gridSize);
        vec2 cellSize = vec2(1.0) / vec2(gridSize);
        float sum = 0.0;
        for (int oy = 0; oy < 4; ++oy) {
            for (int ox = 0; ox < 4; ++ox) {
                vec2 sub = (vec2(ox, oy) + vec2(0.5)) * 0.25;
                vec2 uv = clamp(cellMin + sub * cellSize,
                                vec2(0.0), vec2(1.0));
                sum += textureLod(source_luma, uv, 0.0).r;
            }
        }
        color = vec4(sum * (1.0 / 16.0), 0.0, 0.0, 1.0);
    )";

    return runCompute(target,
                      "frame extrapolation quality grid",
                      nullptr,
                      body,
                      &descriptor,
                      1);
}

bool FrameExtrapolator::dispatchQualityMetric()
{
    if (m_QualityBaselineGrid == nullptr || m_QualitySyntheticGrid == nullptr ||
            m_QualityGroundTruthGrid == nullptr || m_QualityMetric == nullptr) {
        return false;
    }

    pl_shader_desc descriptors[3] = {};
    descriptors[0].desc.name = "baseline_grid";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)m_QualityBaselineGrid;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "synthetic_grid";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)m_QualitySyntheticGrid;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[2].desc.name = "truth_grid";
    descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[2].binding.object = (void*)m_QualityGroundTruthGrid;
    descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const char* body = R"(
        ivec2 gridSize = textureSize(truth_grid, 0);
        float syntheticError = 0.0;
        float holdError = 0.0;
        float syntheticHoldError = 0.0;
        float betterMotionCells = 0.0;
        float motionCells = 0.0;
        int samples = 0;

        for (int y = 0; y < gridSize.y; ++y) {
            for (int x = 0; x < gridSize.x; ++x) {
                ivec2 p = ivec2(x, y);
                float baseline = texelFetch(baseline_grid, p, 0).r;
                float synthetic = texelFetch(synthetic_grid, p, 0).r;
                float truth = texelFetch(truth_grid, p, 0).r;
                float synthDiff = abs(synthetic - truth);
                float holdDiff = abs(baseline - truth);
                syntheticError += synthDiff;
                holdError += holdDiff;
                syntheticHoldError += abs(synthetic - baseline);

                // Static cells where both candidates are effectively identical
                // should not dilute the spatial success ratio. Only count cells
                // where holding the old real frame has measurable error.
                if (holdDiff >= (1.0 / 1024.0)) {
                    motionCells += 1.0;
                    betterMotionCells += synthDiff < holdDiff ? 1.0 : 0.0;
                }
                samples++;
            }
        }

        float invSamples = 1.0 / max(float(samples), 1.0);
        float betterFraction = motionCells > 0.0 ?
                betterMotionCells / motionCells : 0.0;
        color = vec4(syntheticError * invSamples,
                     holdError * invSamples,
                     betterFraction,
                     syntheticHoldError * invSamples);
    )";

    return runCompute(m_QualityMetric,
                      "frame extrapolation ground-truth quality metric",
                      nullptr,
                      body,
                      descriptors,
                      3,
                      nullptr,
                      0,
                      1,
                      1);
}

bool FrameExtrapolator::dispatchAffineDiagnosticMetric()
{
    if (m_AffineModel == nullptr || m_AffineHoldMask == nullptr ||
            m_AffineDiagnosticMetric == nullptr) {
        return false;
    }

    pl_shader_desc descriptors[2] = {};
    descriptors[0].desc.name = "affine_model";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)m_AffineModel;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "hold_mask";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)m_AffineHoldMask;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;

    const float sourceWidth = (float)m_SourceWidth;
    const float sourceHeight = (float)m_SourceHeight;
    pl_shader_var variables[2] = {};
    variables[0].var = pl_var_float("source_width");
    variables[0].data = &sourceWidth;
    variables[0].dynamic = true;
    variables[1].var = pl_var_float("source_height");
    variables[1].data = &sourceHeight;
    variables[1].dynamic = true;

    const char* body = R"(
        vec4 row0 = texelFetch(affine_model, ivec2(0, 0), 0);
        vec4 row1 = texelFetch(affine_model, ivec2(1, 0), 0);
        float support = clamp(row1.z, 0.0, 1.0);
        float motionSum = 0.0;
        float holdSum = 0.0;

        const int gridX = 32;
        const int gridY = 18;
        for (int y = 0; y < gridY; ++y) {
            for (int x = 0; x < gridX; ++x) {
                vec2 uv = (vec2(x, y) + vec2(0.5)) / vec2(gridX, gridY);
                vec2 p = uv * 2.0 - vec2(1.0);
                vec2 d = vec2(row0.x + row0.y * p.x + row0.z * p.y,
                              row0.w + row1.x * p.x + row1.y * p.y);
                vec2 dPx = d * vec2(source_width, source_height);
                motionSum += length(dPx);
                holdSum += textureLod(hold_mask, uv, 0.0).r;
            }
        }

        const float invSamples = 1.0 / float(gridX * gridY);
        float holdFraction = clamp(holdSum * invSamples, 0.0, 1.0);
        float modelValid = support >= 0.35 ? 1.0 : 0.0;
        float extrapolatedFraction = (1.0 - holdFraction) * modelValid;
        color = vec4(motionSum * invSamples,
                     support,
                     extrapolatedFraction,
                     1.0 - extrapolatedFraction);
    )";

    return runCompute(m_AffineDiagnosticMetric,
                      "frame extrapolation affine diagnostic metric",
                      nullptr,
                      body,
                      descriptors,
                      2,
                      variables,
                      2,
                      1,
                      1);
}

bool FrameExtrapolator::dispatchSceneMetric(pl_tex current, pl_tex previous)
{
    pl_shader_desc descriptors[2] = {};
    descriptors[0].desc.name = "current_luma";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)current;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
    descriptors[1].desc.name = "previous_luma";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)previous;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;

    const char* body = R"(
        float sumDiff = 0.0;
        const int gridX = 32;
        const int gridY = 18;
        for (int y = 0; y < gridY; ++y) {
            for (int x = 0; x < gridX; ++x) {
                vec2 uv = (vec2(x, y) + vec2(0.5)) / vec2(gridX, gridY);
                float a = textureLod(current_luma, uv, 0.0).r;
                float b = textureLod(previous_luma, uv, 0.0).r;
                sumDiff += abs(a - b);
            }
        }
        color = vec4(sumDiff / float(gridX * gridY), 0.0, 0.0, 1.0);
    )";

    return runCompute(m_SceneMetric,
                      "frame extrapolation scene metric",
                      nullptr,
                      body,
                      descriptors,
                      2,
                      nullptr,
                      0,
                      1,
                      1);
}

bool FrameExtrapolator::dispatchGlobalTranslationCosts(pl_tex current, pl_tex previous)
{
    pl_shader_desc descriptors[2] = {};
    descriptors[0].desc.name = "current_luma";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)current;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "previous_luma";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)previous;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const char* body = R"(
        ivec2 candidate = ivec2(gl_GlobalInvocationID.xy) - ivec2(8);
        ivec2 size = textureSize(current_luma, 0);
        float cost = 0.0;
        const int gridX = 24;
        const int gridY = 14;

        // Stay away from the border by the full search radius so clamping never
        // makes a candidate look artificially good.
        vec2 minPos = vec2(8.0);
        vec2 maxPos = max(vec2(size - ivec2(9)), minPos);
        for (int y = 0; y < gridY; ++y) {
            for (int x = 0; x < gridX; ++x) {
                vec2 uv = (vec2(x, y) + vec2(0.5)) / vec2(gridX, gridY);
                ivec2 cp = ivec2(round(mix(minPos, maxPos, uv)));
                ivec2 pp = cp + candidate;
                cost += abs(texelFetch(current_luma, cp, 0).r -
                            texelFetch(previous_luma, pp, 0).r);
            }
        }

        color = vec4(cost / float(gridX * gridY), 0.0, 0.0, 1.0);
    )";

    return runCompute(m_GlobalTranslationCosts,
                      "frame extrapolation global translation costs",
                      nullptr,
                      body,
                      descriptors,
                      2);
}

bool FrameExtrapolator::dispatchGlobalTranslationSelect()
{
    pl_shader_desc descriptor = {};
    descriptor.desc.name = "translation_costs";
    descriptor.desc.type = PL_DESC_SAMPLED_TEX;
    descriptor.binding.object = (void*)m_GlobalTranslationCosts;
    descriptor.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const char* body = R"(
        float bestCost = 1e20;
        float secondCost = 1e20;
        ivec2 best = ivec2(0);
        for (int y = 0; y < 17; ++y) {
            for (int x = 0; x < 17; ++x) {
                float c = texelFetch(translation_costs, ivec2(x, y), 0).r;
                if (c < bestCost) {
                    secondCost = bestCost;
                    bestCost = c;
                    best = ivec2(x - 8, y - 8);
                }
                else if (c < secondCost) {
                    secondCost = c;
                }
            }
        }

        vec2 refined = vec2(best);
        ivec2 bp = best + ivec2(8);
        if (bp.x > 0 && bp.x < 16) {
            float cm = texelFetch(translation_costs, bp - ivec2(1, 0), 0).r;
            float c0 = texelFetch(translation_costs, bp, 0).r;
            float cp = texelFetch(translation_costs, bp + ivec2(1, 0), 0).r;
            float denom = cm - 2.0 * c0 + cp;
            if (abs(denom) > 1e-6) {
                refined.x += clamp(0.5 * (cm - cp) / denom, -0.5, 0.5);
            }
        }
        if (bp.y > 0 && bp.y < 16) {
            float cm = texelFetch(translation_costs, bp - ivec2(0, 1), 0).r;
            float c0 = texelFetch(translation_costs, bp, 0).r;
            float cp = texelFetch(translation_costs, bp + ivec2(0, 1), 0).r;
            float denom = cm - 2.0 * c0 + cp;
            if (abs(denom) > 1e-6) {
                refined.y += clamp(0.5 * (cm - cp) / denom, -0.5, 0.5);
            }
        }

        float uniqueness = clamp((secondCost - bestCost) /
                                 max(secondCost, 1e-5) * 5.0,
                                 0.0, 1.0);
        color = vec4(refined, bestCost, uniqueness);
    )";

    return runCompute(m_GlobalTranslation,
                      "frame extrapolation global translation select",
                      nullptr,
                      body,
                      &descriptor,
                      1,
                      nullptr,
                      0,
                      1,
                      1);
}

bool FrameExtrapolator::dispatchFineMotion(pl_tex current, pl_tex previous)
{
    pl_shader_desc descriptors[3] = {};
    descriptors[0].desc.name = "current_luma";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)current;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "previous_luma";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)previous;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
    descriptors[2].desc.name = "global_translation";
    descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[2].binding.object = (void*)m_GlobalTranslation;
    descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const char* body = R"(
        ivec2 block = ivec2(gl_GlobalInvocationID.xy);
        ivec2 size = textureSize(current_luma, 0);
        ivec2 base = block * 4;
        vec2 globalCoarse = texelFetch(global_translation, ivec2(0), 0).rg;
        ivec2 seed = ivec2(round(globalCoarse * 2.0));

        float bestCost = 1e20;
        float secondCost = 1e20;
        ivec2 bestVector = seed;

        // The global translation removes the large common camera component.
        // Search only a small local residual around it. Independently moving
        // objects that disagree with the camera model still land near the edge
        // of this window, which is enough for conservative outlier detection.
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                ivec2 candidate = seed + ivec2(dx, dy);
                float sad = 0.0;
                for (int by = 0; by < 4; ++by) {
                    for (int bx = 0; bx < 4; ++bx) {
                        ivec2 cp = clamp(base + ivec2(bx, by), ivec2(0), size - ivec2(1));
                        ivec2 pp = clamp(cp + candidate, ivec2(0), size - ivec2(1));
                        sad += abs(texelFetch(current_luma, cp, 0).r -
                                   texelFetch(previous_luma, pp, 0).r);
                    }
                }

                if (sad < bestCost) {
                    secondCost = bestCost;
                    bestCost = sad;
                    bestVector = candidate;
                }
                else if (sad < secondCost) {
                    secondCost = sad;
                }
            }
        }

        vec2 bestVectorF = vec2(bestVector);
        vec2 sizeF = vec2(size);
        for (int ry = -1; ry <= 1; ++ry) {
            for (int rx = -1; rx <= 1; ++rx) {
                if (rx == 0 && ry == 0) {
                    continue;
                }

                vec2 candidate = vec2(bestVector) + vec2(rx, ry) * 0.5;
                float sad = 0.0;
                for (int by = 0; by < 4; ++by) {
                    for (int bx = 0; bx < 4; ++bx) {
                        ivec2 cp = clamp(base + ivec2(bx, by), ivec2(0), size - ivec2(1));
                        vec2 pp = clamp(vec2(cp) + candidate,
                                        vec2(0.0), sizeF - vec2(1.0));
                        float a = texelFetch(current_luma, cp, 0).r;
                        float b = textureLod(previous_luma,
                                             (pp + vec2(0.5)) / sizeF,
                                             0.0).r;
                        sad += abs(a - b);
                    }
                }

                if (sad < bestCost) {
                    secondCost = bestCost;
                    bestCost = sad;
                    bestVectorF = candidate;
                }
                else if (sad < secondCost) {
                    secondCost = sad;
                }
            }
        }

        float meanCost = bestCost / 16.0;
        float separation = max(secondCost - bestCost, 0.0) / max(secondCost, 1e-5);
        float uniqueness = clamp(separation * 5.0, 0.0, 1.0);
        float quality = 1.0 - smoothstep(0.06, 0.18, meanCost);
        float confidence = uniqueness * quality;
        color = vec4(bestVectorF, confidence, meanCost);
    )";

    return runCompute(m_FineMotion,
                      "frame extrapolation camera-seeded local motion",
                      nullptr,
                      body,
                      descriptors,
                      3);
}

bool FrameExtrapolator::dispatchAffineHypotheses()
{
    pl_shader_desc descriptor = {};
    descriptor.desc.name = "motion_field";
    descriptor.desc.type = PL_DESC_SAMPLED_TEX;
    descriptor.binding.object = (void*)m_FineMotion;
    descriptor.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const float sourceWidth = (float)m_SourceWidth;
    const float sourceHeight = (float)m_SourceHeight;
    pl_shader_var variables[2] = {};
    variables[0].var = pl_var_float("source_width");
    variables[0].data = &sourceWidth;
    variables[0].dynamic = true;
    variables[1].var = pl_var_float("source_height");
    variables[1].data = &sourceHeight;
    variables[1].dynamic = true;

    const char* body = R"(
        int h = int(gl_GlobalInvocationID.x);
        int row = int(gl_GlobalInvocationID.y);
        float hf = float(h);

        vec2 uv0 = vec2(0.08) + vec2(0.84) * fract(vec2(0.137, 0.271) + hf * vec2(0.331, 0.619));
        vec2 uv1 = vec2(0.08) + vec2(0.84) * fract(vec2(0.563, 0.193) + hf * vec2(0.417, 0.473));
        vec2 uv2 = vec2(0.08) + vec2(0.84) * fract(vec2(0.307, 0.719) + hf * vec2(0.293, 0.437));

        vec4 m0 = textureLod(motion_field, uv0, 0.0);
        vec4 m1 = textureLod(motion_field, uv1, 0.0);
        vec4 m2 = textureLod(motion_field, uv2, 0.0);

        vec2 p0 = uv0 * 2.0 - vec2(1.0);
        vec2 p1 = uv1 * 2.0 - vec2(1.0);
        vec2 p2 = uv2 * 2.0 - vec2(1.0);
        mat3 design = mat3(vec3(1.0, 1.0, 1.0),
                           vec3(p0.x, p1.x, p2.x),
                           vec3(p0.y, p1.y, p2.y));

        float det = determinant(design);
        vec3 cx = vec3(0.0);
        vec3 cy = vec3(0.0);
        float score = 0.0;
        if (abs(det) > 0.03 && m0.a < 0.20 && m1.a < 0.20 && m2.a < 0.20) {
            vec2 scale = vec2(4.0 / source_width, 4.0 / source_height);
            vec2 d0 = m0.rg * scale;
            vec2 d1 = m1.rg * scale;
            vec2 d2 = m2.rg * scale;
            mat3 invDesign = inverse(design);
            cx = invDesign * vec3(d0.x, d1.x, d2.x);
            cy = invDesign * vec3(d0.y, d1.y, d2.y);

            float support = 0.0;
            float total = 0.0;
            const int gridX = 24;
            const int gridY = 14;
            for (int y = 0; y < gridY; ++y) {
                for (int x = 0; x < gridX; ++x) {
                    vec2 uv = (vec2(x, y) + vec2(0.5)) / vec2(gridX, gridY);
                    vec4 m = textureLod(motion_field, uv, 0.0);
                    float quality = 1.0 - smoothstep(0.06, 0.18, m.a);
                    float weight = quality * mix(0.35, 1.0, clamp(m.b, 0.0, 1.0));
                    vec2 p = uv * 2.0 - vec2(1.0);
                    vec2 predicted = vec2(dot(cx, vec3(1.0, p.x, p.y)),
                                          dot(cy, vec3(1.0, p.x, p.y)));
                    vec2 observed = m.rg * scale;
                    float residualPx = length((observed - predicted) *
                                              vec2(source_width, source_height));
                    support += weight * (1.0 - smoothstep(4.0, 8.0, residualPx));
                    total += weight;
                }
            }
            score = total > 1e-5 ? support / total : 0.0;

            float maxMotionPx = 0.0;
            for (int yy = -1; yy <= 1; yy += 2) {
                for (int xx = -1; xx <= 1; xx += 2) {
                    vec2 p = vec2(xx, yy);
                    vec2 d = vec2(dot(cx, vec3(1.0, p.x, p.y)),
                                  dot(cy, vec3(1.0, p.x, p.y)));
                    maxMotionPx = max(maxMotionPx,
                                      length(d * vec2(source_width, source_height)));
                }
            }
            score *= 1.0 - smoothstep(96.0, 144.0, maxMotionPx);
        }

        if (row == 0) {
            color = vec4(cx, cy.x);
        }
        else {
            color = vec4(cy.y, cy.z, score, 1.0);
        }
    )";

    return runCompute(m_AffineHypotheses,
                      "frame extrapolation affine hypotheses",
                      nullptr,
                      body,
                      &descriptor,
                      1,
                      variables,
                      2,
                      1,
                      1);
}

bool FrameExtrapolator::dispatchAffineModel()
{
    pl_shader_desc descriptors[3] = {};
    descriptors[0].desc.name = "motion_field";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)m_FineMotion;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "hypotheses";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)m_AffineHypotheses;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[2].desc.name = "scene_metric";
    descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[2].binding.object = (void*)m_SceneMetric;
    descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const float sourceWidth = (float)m_SourceWidth;
    const float sourceHeight = (float)m_SourceHeight;
    pl_shader_var variables[2] = {};
    variables[0].var = pl_var_float("source_width");
    variables[0].data = &sourceWidth;
    variables[0].dynamic = true;
    variables[1].var = pl_var_float("source_height");
    variables[1].data = &sourceHeight;
    variables[1].dynamic = true;

    const char* body = R"(
        int outputRow = int(gl_GlobalInvocationID.x);
        int bestIndex = 0;
        float bestScore = -1.0;
        vec4 best0 = vec4(0.0);
        vec4 best1 = vec4(0.0);
        for (int i = 0; i < 16; ++i) {
            vec4 r0 = texelFetch(hypotheses, ivec2(i, 0), 0);
            vec4 r1 = texelFetch(hypotheses, ivec2(i, 1), 0);
            if (r1.z > bestScore) {
                bestScore = r1.z;
                bestIndex = i;
                best0 = r0;
                best1 = r1;
            }
        }

        vec3 initialX = best0.xyz;
        vec3 initialY = vec3(best0.w, best1.x, best1.y);
        vec2 scale = vec2(4.0 / source_width, 4.0 / source_height);

        float s00 = 0.0, s01 = 0.0, s02 = 0.0;
        float s11 = 0.0, s12 = 0.0, s22 = 0.0;
        vec3 bx = vec3(0.0);
        vec3 by = vec3(0.0);
        float totalBase = 0.0;
        float robustSupport = 0.0;

        const int gridX = 32;
        const int gridY = 18;
        for (int y = 0; y < gridY; ++y) {
            for (int x = 0; x < gridX; ++x) {
                vec2 uv = (vec2(x, y) + vec2(0.5)) / vec2(gridX, gridY);
                vec4 m = textureLod(motion_field, uv, 0.0);
                float quality = 1.0 - smoothstep(0.06, 0.18, m.a);
                float baseWeight = quality * mix(0.35, 1.0, clamp(m.b, 0.0, 1.0));
                vec2 p = uv * 2.0 - vec2(1.0);
                vec3 a = vec3(1.0, p.x, p.y);
                vec2 observed = m.rg * scale;
                vec2 initial = vec2(dot(initialX, a), dot(initialY, a));
                float residualPx = length((observed - initial) *
                                          vec2(source_width, source_height));
                float robust = 1.0 - smoothstep(4.0, 10.0, residualPx);
                float w = baseWeight * robust;

                s00 += w;
                s01 += w * a.x * a.y;
                s02 += w * a.x * a.z;
                s11 += w * a.y * a.y;
                s12 += w * a.y * a.z;
                s22 += w * a.z * a.z;
                bx += w * observed.x * a;
                by += w * observed.y * a;
                totalBase += baseWeight;
                robustSupport += w;
            }
        }

        s00 += 1e-5;
        s11 += 1e-5;
        s22 += 1e-5;
        mat3 ata = mat3(vec3(s00, s01, s02),
                        vec3(s01, s11, s12),
                        vec3(s02, s12, s22));
        vec3 refinedX = initialX;
        vec3 refinedY = initialY;
        float det = determinant(ata);
        if (abs(det) > 1e-9 && robustSupport > 1e-4) {
            mat3 invAta = inverse(ata);
            refinedX = invAta * bx;
            refinedY = invAta * by;
        }

        float support = totalBase > 1e-5 ? robustSupport / totalBase : 0.0;
        float sceneDiff = textureLod(scene_metric, vec2(0.5), 0.0).r;
        float sceneConfidence = 1.0 - smoothstep(0.10, 0.16, sceneDiff);
        support *= sceneConfidence;

        // Reject implausibly aggressive affine deformation. Translation itself
        // can still be large; this limit targets scale/rotation/shear changes.
        float linearMagnitude = max(max(abs(refinedX.y), abs(refinedX.z)),
                                    max(abs(refinedY.y), abs(refinedY.z)));
        support *= 1.0 - smoothstep(0.06, 0.10, linearMagnitude);

        if (outputRow == 0) {
            color = vec4(refinedX, refinedY.x);
        }
        else {
            color = vec4(refinedY.y, refinedY.z, clamp(support, 0.0, 1.0), bestScore);
        }
    )";

    return runCompute(m_AffineModel,
                      "frame extrapolation robust affine model",
                      nullptr,
                      body,
                      descriptors,
                      3,
                      variables,
                      2,
                      1,
                      1);
}

bool FrameExtrapolator::dispatchAffineHoldMask()
{
    pl_shader_desc descriptors[2] = {};
    descriptors[0].desc.name = "motion_field";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)m_FineMotion;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "affine_model";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)m_AffineModel;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const float sourceWidth = (float)m_SourceWidth;
    const float sourceHeight = (float)m_SourceHeight;
    pl_shader_var variables[2] = {};
    variables[0].var = pl_var_float("source_width");
    variables[0].data = &sourceWidth;
    variables[0].dynamic = true;
    variables[1].var = pl_var_float("source_height");
    variables[1].data = &sourceHeight;
    variables[1].dynamic = true;

    const char* body = R"(
        ivec2 block = ivec2(gl_GlobalInvocationID.xy);
        ivec2 motionSize = textureSize(motion_field, 0);
        vec4 row0 = texelFetch(affine_model, ivec2(0, 0), 0);
        vec4 row1 = texelFetch(affine_model, ivec2(1, 0), 0);
        float support = clamp(row1.z, 0.0, 1.0);

        float good = 0.0;
        float outliers = 0.0;
        vec2 scale = vec2(4.0 / source_width, 4.0 / source_height);
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                ivec2 b = clamp(block + ivec2(ox, oy),
                                ivec2(0), motionSize - ivec2(1));
                vec4 m = texelFetch(motion_field, b, 0);
                float quality = 1.0 - smoothstep(0.06, 0.18, m.a);
                if (quality > 0.20) {
                    vec2 uv = (vec2(b) + vec2(0.5)) / vec2(motionSize);
                    vec2 p = uv * 2.0 - vec2(1.0);
                    vec2 predicted = vec2(row0.x + row0.y * p.x + row0.z * p.y,
                                          row0.w + row1.x * p.x + row1.y * p.y);
                    vec2 observed = m.rg * scale;
                    float residualPx = length((observed - predicted) *
                                              vec2(source_width, source_height));
                    good += 1.0;
                    outliers += residualPx > 6.0 ? 1.0 : 0.0;
                }
            }
        }

        float outlierRatio = good >= 4.0 ? outliers / good : 0.0;
        float hold = smoothstep(0.78, 0.92, outlierRatio);
        if (support < 0.35) {
            hold = 1.0;
        }
        color = vec4(hold, 0.0, 0.0, 1.0);
    )";

    return runCompute(m_AffineHoldMask,
                      "frame extrapolation affine outlier hold mask",
                      nullptr,
                      body,
                      descriptors,
                      2,
                      variables,
                      2);
}

bool FrameExtrapolator::dispatchAffineWarp(pl_tex source, pl_tex target, float alpha)
{
    pl_shader_desc descriptors[3] = {};
    descriptors[0].desc.name = "src_plane";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)source;
    descriptors[0].binding.sample_mode =
            (source->params.format->caps & PL_FMT_CAP_LINEAR) ?
                PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "affine_model";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)m_AffineModel;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[2].desc.name = "hold_mask";
    descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[2].binding.object = (void*)m_AffineHoldMask;
    descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;

    pl_shader_var variable = {};
    variable.var = pl_var_float("extrapolation_alpha");
    variable.data = &alpha;
    variable.dynamic = true;

    const char* body = R"(
        ivec2 pOut = ivec2(gl_GlobalInvocationID.xy);
        ivec2 planeSize = textureSize(src_plane, 0);
        vec2 uv = (vec2(pOut) + vec2(0.5)) / vec2(planeSize);

        vec4 row0 = texelFetch(affine_model, ivec2(0, 0), 0);
        vec4 row1 = texelFetch(affine_model, ivec2(1, 0), 0);
        float support = clamp(row1.z, 0.0, 1.0);
        vec2 p = uv * 2.0 - vec2(1.0);
        vec2 displacement = vec2(row0.x + row0.y * p.x + row0.z * p.y,
                                 row0.w + row1.x * p.x + row1.y * p.y);
        vec2 warpedRaw = uv + extrapolation_alpha * displacement;
        vec2 warpedUv = clamp(warpedRaw, vec2(0.0), vec2(1.0));

        vec4 original = textureLod(src_plane, uv, 0.0);
        vec4 warped = textureLod(src_plane, warpedUv, 0.0);

        // The mask is produced at motion-block resolution. Linear sampling plus
        // this soft threshold feathers the transition without allowing a local
        // deformation field to bend rigid geometry.
        float hold = textureLod(hold_mask, uv, 0.0).r;
        hold = smoothstep(0.20, 0.80, hold);
        bool inside = all(greaterThanEqual(warpedRaw, vec2(0.0))) &&
                      all(lessThanEqual(warpedRaw, vec2(1.0)));
        if (support < 0.35 || !inside) {
            hold = 1.0;
        }

        color = mix(warped, original, hold);
    )";

    return runCompute(target,
                      "frame extrapolation global affine hybrid warp",
                      nullptr,
                      body,
                      descriptors,
                      3,
                      &variable,
                      1);
}

bool FrameExtrapolator::submitRealFrame(const AVFrame* frame,
                                        pl_frame& mappedFrame,
                                        uint64_t renderTimeUs)
{
    // A newly submitted real frame supersedes any diagnostic baseline belonging
    // to the previous speculative candidate. If that candidate was actually
    // presented, Pacer evaluates its exact matching ground-truth frame before
    // this method is reached.
    m_HasQualityBaseline = false;
    m_HasAffineDiagnostic = false;

    // pl_render_image() has already released a mapped Vulkan AVFrame by the
    // time this post-render analysis runs. Re-acquire it before directly
    // sampling its textures so FFmpeg/libplacebo semaphore and layout ownership
    // remains correct. DRM PRIME/VAAPI mappings have no callback and are no-op.
    FrameAcquireGuard frameGuard(m_Gpu, mappedFrame);
    if (!frameGuard.ok()) {
        return false;
    }

    if (!ensureResources(mappedFrame)) {
        return false;
    }

    const int currentIndex = m_HasHistory ? 1 - m_HistoryIndex : 0;

    // Never build a queue of analysis work behind presentation. If the previous
    // analysis is still using the reusable textures, skip analysis for this real
    // frame. Keep a valid motion field for at most one skipped frame: applying
    // the immediately preceding motion estimate to the newest real image is a
    // useful constant-velocity fallback, while allowing it to age further would
    // make extrapolation increasingly speculative. The zero-time readiness poll
    // in canExtrapolate() still guarantees that unfinished GPU work is never used.
    if (m_HasHistory &&
            (pl_tex_poll(m_Gpu, m_FineLuma[m_HistoryIndex], 0) ||
             pl_tex_poll(m_Gpu, m_CoarseLuma[m_HistoryIndex], 0) ||
             pl_tex_poll(m_Gpu, m_FineLuma[currentIndex], 0) ||
             pl_tex_poll(m_Gpu, m_CoarseLuma[currentIndex], 0) ||
             pl_tex_poll(m_Gpu, m_GlobalTranslationCosts, 0) ||
             pl_tex_poll(m_Gpu, m_GlobalTranslation, 0) ||
             pl_tex_poll(m_Gpu, m_FineMotion, 0) ||
             pl_tex_poll(m_Gpu, m_AffineHypotheses, 0) ||
             pl_tex_poll(m_Gpu, m_AffineModel, 0) ||
             pl_tex_poll(m_Gpu, m_AffineHoldMask, 0) ||
             pl_tex_poll(m_Gpu, m_SceneMetric, 0))) {
        StreamHealthTelemetry::frameExtrapolationAnalysisBusySkip();

        const bool currentIsIntra =
                (frame->flags & AV_FRAME_FLAG_KEY) ||
                frame->pict_type == AV_PICTURE_TYPE_I;
        const bool keepRecentMotion =
                m_HasMotion &&
                m_MotionPairIntervalUs != 0 &&
                m_MotionAgeFrames == 0 &&
                !currentIsIntra;

        if (keepRecentMotion) {
            m_MotionAgeFrames = 1;
        }
        else {
            m_HasMotion = false;
            m_MotionPairIntervalUs = 0;
            m_MotionAgeFrames = 0;
        }

        m_LastRealRenderTimeUs = renderTimeUs;
        m_SyntheticSinceLastReal = false;

        // Keep m_LastRealPts and m_HistoryIndex pointing at the last frame that
        // actually entered GPU history. The next successful analysis may span
        // multiple RTP intervals and will normalize that span.
        return true;
    }

    pl_dispatch_reset_frame(m_Dispatch);

    pl_tex luma = mappedFrame.planes[0].texture;

    if (!dispatchDownsample(luma, m_FineLuma[currentIndex], 4) ||
            !dispatchDownsample(m_FineLuma[currentIndex], m_CoarseLuma[currentIndex], 2)) {
        m_HasMotion = false;
        m_MotionPairIntervalUs = 0;
        m_MotionAgeFrames = 0;
        return false;
    }

    if (m_HasHistory) {
        if (!dispatchSceneMetric(m_CoarseLuma[currentIndex], m_CoarseLuma[m_HistoryIndex]) ||
                !dispatchGlobalTranslationCosts(m_CoarseLuma[currentIndex], m_CoarseLuma[m_HistoryIndex]) ||
                !dispatchGlobalTranslationSelect() ||
                !dispatchFineMotion(m_FineLuma[currentIndex], m_FineLuma[m_HistoryIndex]) ||
                !dispatchAffineHypotheses() ||
                !dispatchAffineModel() ||
                !dispatchAffineHoldMask()) {
            m_HasMotion = false;
            m_MotionPairIntervalUs = 0;
            m_MotionAgeFrames = 0;
            return false;
        }

        // Record the temporal span represented by this motion field. RTP PTS
        // is 90 kHz; unsigned subtraction keeps the delta correct across the
        // 32-bit RTP timestamp wrap. A later synthetic warp scales this motion
        // to the pacer's current expected interval, so pairs such as 100 -> 102
        // do not over-project the next frame.
        m_MotionPairIntervalUs = 0;
        if (m_LastRealPts != AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE) {
            const uint32_t deltaPts =
                    (uint32_t)frame->pts - (uint32_t)m_LastRealPts;
            const uint64_t deltaUs = (uint64_t)deltaPts *
                    1000000ULL / 90000ULL;
            if (deltaPts != 0 && deltaUs >= 2000ULL && deltaUs <= 100000ULL) {
                m_MotionPairIntervalUs = deltaUs;
                m_HasMotion = true;
            }
            else {
                m_HasMotion = false;
            }
        }
        else {
            m_HasMotion = false;
        }

        // Encoder keyframes/I-frames are cheap CPU-visible scene-cut hints.
        // The reduced GPU scene metric additionally suppresses unflagged cuts.
        if ((frame->flags & AV_FRAME_FLAG_KEY) ||
                frame->pict_type == AV_PICTURE_TYPE_I) {
            m_HasMotion = false;
        }
    }
    else {
        m_HasMotion = false;
        m_MotionPairIntervalUs = 0;
    }

    m_MotionAgeFrames = 0;
    m_HistoryIndex = currentIndex;
    m_HasHistory = true;
    m_LastRealRenderTimeUs = renderTimeUs;
    m_LastRealPts = frame->pts;
    m_SyntheticSinceLastReal = false;

    return true;
}

bool FrameExtrapolator::buildPreparedSyntheticFrame(pl_frame& currentFrame,
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
    // deadline path must find a completed candidate rather than create GPU
    // backlog that could delay normal real-frame rendering.
    for (int i = 0; i < m_PlaneCount; ++i) {
        if (m_SyntheticPlanes[i] == nullptr ||
                pl_tex_poll(m_Gpu, m_SyntheticPlanes[i], 0)) {
            return false;
        }
    }

    FrameAcquireGuard frameGuard(m_Gpu, currentFrame);
    if (!frameGuard.ok()) {
        return false;
    }

    // Predict exactly one current Pacer interval ahead, normalizing motion
    // fields that span a different number of RTP ticks. The warp is queued
    // immediately after analysis and libplacebo tracks texture dependencies
    // without any CPU/GPU synchronization.
    const float alpha = (float)std::clamp(temporalScale, 0.4, 1.25);

    for (int i = 0; i < currentFrame.num_planes; ++i) {
        pl_tex source = currentFrame.planes[i].texture;
        if (source == nullptr ||
                source->params.w != m_PlaneWidths[i] ||
                source->params.h != m_PlaneHeights[i] ||
                currentFrame.planes[i].components != m_PlaneComponents[i]) {
            return false;
        }

        if (!dispatchAffineWarp(source, m_SyntheticPlanes[i], alpha)) {
            return false;
        }
    }

    // While the real source frame is already acquired, opportunistically queue
    // a tiny aspect-aware luma signature for ground-truth scoring. This avoids
    // an extra acquire/release cycle and adds only a 32-wide diagnostic grid.
    // If the reusable grid is still busy, extrapolation proceeds unchanged and
    // this candidate simply won't have a quality sample.
    if (m_QualityMeasurementEnabled && m_QualityResourcesReady &&
            !pl_tex_poll(m_Gpu, m_QualityBaselineGrid, 0) &&
            dispatchQualityGrid(currentFrame.planes[0].texture,
                                m_QualityBaselineGrid)) {
        m_HasQualityBaseline = true;
    }

    // Snapshot affine support and how much of the frame is actually being
    // extrapolated versus conservatively held. Readback is deferred until the
    // candidate is presented, so timing tests remain unaffected when quality
    // telemetry is disabled.
    m_HasAffineDiagnostic = false;
    if (m_QualityMeasurementEnabled && m_QualityResourcesReady &&
            !pl_tex_poll(m_Gpu, m_AffineDiagnosticMetric, 0)) {
        m_HasAffineDiagnostic = dispatchAffineDiagnosticMetric();
    }

    *syntheticFrame = currentFrame;
    for (int i = 0; i < syntheticFrame->num_planes; ++i) {
        syntheticFrame->planes[i].texture = m_SyntheticPlanes[i];
    }

    // These textures are owned wholly by FrameExtrapolator.
    syntheticFrame->acquire = nullptr;
    syntheticFrame->release = nullptr;
    syntheticFrame->user_data = nullptr;
    return true;
}

bool FrameExtrapolator::queueAffineDiagnosticReadback()
{
    if (!m_QualityMeasurementEnabled || !m_QualityResourcesReady ||
            !m_HasAffineDiagnostic || m_AffineDiagnosticMetric == nullptr) {
        return false;
    }

    // Consume this candidate's metric exactly once. The transfer is queued
    // asynchronously after presentation submission and never waits for the GPU.
    m_HasAffineDiagnostic = false;

    AffineDiagnosticReadback* readback = new AffineDiagnosticReadback();
    readback->generation = StreamHealthTelemetry::frameExtrapolationQualityGeneration();

    pl_tex_transfer_params transfer = {};
    transfer.tex = m_AffineDiagnosticMetric;
    transfer.ptr = readback->values;
    transfer.no_import = true;
    transfer.callback = affineDiagnosticReadbackComplete;
    transfer.priv = readback;
    if (!pl_tex_download(m_Gpu, &transfer)) {
        delete readback;
        return false;
    }

    return true;
}

void FrameExtrapolator::markSyntheticPresented()
{
    m_SyntheticSinceLastReal = true;
    StreamHealthTelemetry::frameExtrapolated();

    if (m_QualityMeasurementEnabled && !queueAffineDiagnosticReadback()) {
        StreamHealthTelemetry::frameExtrapolationAffineSkip();
    }
}

bool FrameExtrapolator::evaluateGroundTruthQuality(pl_frame& groundTruthFrame)
{
    if (!m_QualityMeasurementEnabled) {
        return false;
    }

    const bool canMeasure = m_SyntheticSinceLastReal &&
            m_HasQualityBaseline &&
            m_QualityResourcesReady &&
            groundTruthFrame.num_planes > 0 &&
            groundTruthFrame.planes[0].texture != nullptr &&
            m_SyntheticPlanes[0] != nullptr;

    // Consume the baseline on the first exact ground-truth opportunity whether
    // measurement succeeds or not. A later real frame would no longer represent
    // the timestamp that the synthetic frame replaced.
    m_HasQualityBaseline = false;

    if (!canMeasure) {
        StreamHealthTelemetry::frameExtrapolationQualitySkip();
        return false;
    }

    pl_tex groundTruth = groundTruthFrame.planes[0].texture;
    if (groundTruth->params.w != m_PlaneWidths[0] ||
            groundTruth->params.h != m_PlaneHeights[0]) {
        StreamHealthTelemetry::frameExtrapolationQualitySkip();
        return false;
    }

    // Reusable diagnostic targets must be completely idle. This makes the
    // quality path opportunistic rather than allowing readbacks to accumulate
    // and perturb later real-frame rendering.
    if (pl_tex_poll(m_Gpu, m_SyntheticPlanes[0], 0) ||
            pl_tex_poll(m_Gpu, m_QualityBaselineGrid, 0) ||
            pl_tex_poll(m_Gpu, m_QualitySyntheticGrid, 0) ||
            pl_tex_poll(m_Gpu, m_QualityGroundTruthGrid, 0) ||
            pl_tex_poll(m_Gpu, m_QualityMetric, 0)) {
        StreamHealthTelemetry::frameExtrapolationQualitySkip();
        return false;
    }

    FrameAcquireGuard frameGuard(m_Gpu, groundTruthFrame);
    if (!frameGuard.ok()) {
        StreamHealthTelemetry::frameExtrapolationQualitySkip();
        return false;
    }

    // This is a separate diagnostic frame from the normal motion-analysis
    // dispatches. All commands are queued asynchronously and no GPU wait occurs.
    pl_dispatch_reset_frame(m_Dispatch);

    if (!dispatchQualityGrid(m_SyntheticPlanes[0], m_QualitySyntheticGrid) ||
            !dispatchQualityGrid(groundTruth, m_QualityGroundTruthGrid) ||
            !dispatchQualityMetric()) {
        StreamHealthTelemetry::frameExtrapolationQualitySkip();
        return false;
    }

    QualityReadback* readback = new QualityReadback();
    readback->generation = StreamHealthTelemetry::frameExtrapolationQualityGeneration();

    pl_tex_transfer_params transfer = {};
    transfer.tex = m_QualityMetric;
    transfer.ptr = readback->values;
    // Force libplacebo to use its staging path rather than importing this tiny
    // heap allocation as host memory. The callback keeps the transfer fully
    // asynchronous while avoiding host-pointer alignment/import edge cases.
    transfer.no_import = true;
    transfer.callback = qualityReadbackComplete;
    transfer.priv = readback;
    if (!pl_tex_download(m_Gpu, &transfer)) {
        delete readback;
        StreamHealthTelemetry::frameExtrapolationQualitySkip();
        return false;
    }

    return true;
}

bool FrameExtrapolator::canExtrapolate(uint64_t targetTimeUs,
                                        uint64_t frameIntervalUs)
{
    const bool firstCheckForTarget = targetTimeUs != m_LastTelemetryTargetUs;
    if (firstCheckForTarget) {
        m_LastTelemetryTargetUs = targetTimeUs;
        StreamHealthTelemetry::frameExtrapolationOpportunity();
    }

    if (!m_ResourcesReady || m_SyntheticSinceLastReal ||
            m_LastRealRenderTimeUs == 0 || m_MotionPairIntervalUs == 0 ||
            frameIntervalUs == 0 || targetTimeUs <= m_LastRealRenderTimeUs) {
        if (firstCheckForTarget) {
            StreamHealthTelemetry::frameExtrapolationRejectState();
        }
        return false;
    }

    if (!m_HasMotion) {
        if (firstCheckForTarget) {
            StreamHealthTelemetry::frameExtrapolationRejectNoMotion();
        }
        return false;
    }

    const uint64_t elapsedUs = targetTimeUs - m_LastRealRenderTimeUs;
    const double predictionAlpha = (double)elapsedUs / (double)frameIntervalUs;
    const double temporalScale = (double)frameIntervalUs /
            (double)m_MotionPairIntervalUs;
    if (predictionAlpha < 0.75 || predictionAlpha > 1.25 ||
            temporalScale < 0.35 || temporalScale > 1.50) {
        if (firstCheckForTarget) {
            StreamHealthTelemetry::frameExtrapolationRejectTiming();
        }
        return false;
    }

    // A zero-time poll must never stall presentation. Skip this opportunity if
    // analysis is still in flight rather than synchronizing the CPU with GPU.
    if (pl_tex_poll(m_Gpu, m_AffineModel, 0) ||
            pl_tex_poll(m_Gpu, m_AffineHoldMask, 0) ||
            pl_tex_poll(m_Gpu, m_SceneMetric, 0)) {
        if (firstCheckForTarget) {
            StreamHealthTelemetry::frameExtrapolationRejectGpuBusy();
        }
        return false;
    }

    return true;
}


#endif