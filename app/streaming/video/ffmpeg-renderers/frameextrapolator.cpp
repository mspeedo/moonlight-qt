#include "frameextrapolator.h"

#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)

#include <algorithm>

#include <libplacebo/shaders/custom.h>

FrameExtrapolator::FrameExtrapolator(pl_log log, pl_gpu gpu) :
    m_Log(log),
    m_Gpu(gpu)
{
}

FrameExtrapolator::~FrameExtrapolator()
{
    StreamHealthTelemetry::setFrameExtrapolationActive(false);

    // Destroy GPU resources before releasing the decoded surface reference.
    // pl_tex_destroy() handles any outstanding GPU use of these resources.
    destroyResources();
    pl_dispatch_destroy(&m_Dispatch);
    av_frame_free(&m_LatestRealFrame);
}

bool FrameExtrapolator::initialize()
{
    m_Dispatch = pl_dispatch_create(m_Log, m_Gpu);
    m_LatestRealFrame = av_frame_alloc();
    const bool initialized = m_Dispatch != nullptr && m_LatestRealFrame != nullptr;
    StreamHealthTelemetry::setFrameExtrapolationActive(initialized);
    return initialized;
}

bool FrameExtrapolator::createTexture(pl_tex* texture, int width, int height, int components)
{
    // Three-component storage images are not universally available, so use
    // RGBA for three-component video planes while preserving pl_plane metadata.
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

void FrameExtrapolator::destroyResources()
{
    if (m_Gpu == nullptr) {
        return;
    }

    for (int i = 0; i < 2; ++i) {
        pl_tex_destroy(m_Gpu, &m_FineLuma[i]);
        pl_tex_destroy(m_Gpu, &m_CoarseLuma[i]);
    }

    pl_tex_destroy(m_Gpu, &m_CoarseMotion);
    pl_tex_destroy(m_Gpu, &m_FineMotion);
    pl_tex_destroy(m_Gpu, &m_SceneMetric);

    for (int i = 0; i < PL_MAX_PLANES; ++i) {
        pl_tex_destroy(m_Gpu, &m_SyntheticPlanes[i]);
    }

    m_ResourcesReady = false;
    m_HasHistory = false;
    m_HasMotion = false;
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

    if (!createTexture(&m_FineLuma[0], m_FineWidth, m_FineHeight, 1) ||
            !createTexture(&m_FineLuma[1], m_FineWidth, m_FineHeight, 1) ||
            !createTexture(&m_CoarseLuma[0], m_CoarseWidth, m_CoarseHeight, 1) ||
            !createTexture(&m_CoarseLuma[1], m_CoarseWidth, m_CoarseHeight, 1) ||
            !createTexture(&m_CoarseMotion, (m_CoarseWidth + 3) / 4, (m_CoarseHeight + 3) / 4, 4) ||
            !createTexture(&m_FineMotion, (m_FineWidth + 3) / 4, (m_FineHeight + 3) / 4, 4) ||
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
    pl_shader shader = pl_dispatch_begin(m_Dispatch);
    if (shader == nullptr) {
        return false;
    }

    struct pl_custom_shader params = {};
    params.description = description;
    params.header = header;
    params.body = body;
    params.input = PL_SHADER_SIG_NONE;
    params.output = PL_SHADER_SIG_COLOR;
    params.descriptors = descriptors;
    params.num_descriptors = descriptorCount;
    params.variables = variables;
    params.num_variables = variableCount;
    params.compute = true;
    params.compute_group_size[0] = groupSizeX;
    params.compute_group_size[1] = groupSizeY;
    params.output_w = target->params.w;
    params.output_h = target->params.h;

    if (!pl_shader_custom(shader, &params)) {
        // pl_dispatch_begin() transfers an active shader to the caller. If
        // shader construction fails, return it explicitly instead of leaking
        // an unfinished dispatch object into the next frame.
        pl_dispatch_abort(m_Dispatch, &shader);
        return false;
    }

    struct pl_dispatch_params dispatch = {};
    dispatch.shader = &shader;
    dispatch.target = target;
    return pl_dispatch_finish(m_Dispatch, &dispatch);
}

bool FrameExtrapolator::dispatchDownsample(pl_tex source, pl_tex target, int scale)
{
    pl_shader_desc descriptor = {};
    descriptor.desc.name = "src_tex";
    descriptor.desc.type = PL_DESC_SAMPLED_TEX;
    descriptor.binding.object = (void*)source;
    descriptor.binding.sample_mode =
            (source->params.format->caps & PL_FMT_CAP_LINEAR) ?
                PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;

    const char* body4 = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 dstSize = (textureSize(src_tex, 0) + ivec2(3)) / 4;
        vec2 uv = (vec2(p) + vec2(0.5)) / vec2(dstSize);
        float y = textureLod(src_tex, uv, 0.0).r;
        color = vec4(y, 0.0, 0.0, 1.0);
    )";

    const char* body2 = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 dstSize = (textureSize(src_tex, 0) + ivec2(1)) / 2;
        vec2 uv = (vec2(p) + vec2(0.5)) / vec2(dstSize);
        float y = textureLod(src_tex, uv, 0.0).r;
        color = vec4(y, 0.0, 0.0, 1.0);
    )";

    return runCompute(target,
                      scale == 4 ? "frame extrapolation luma quarter-res" :
                                   "frame extrapolation luma coarse-res",
                      nullptr,
                      scale == 4 ? body4 : body2,
                      &descriptor,
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

bool FrameExtrapolator::dispatchCoarseMotion(pl_tex current, pl_tex previous)
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
        ivec2 block = ivec2(gl_GlobalInvocationID.xy);
        ivec2 size = textureSize(current_luma, 0);
        ivec2 base = block * 4;
        float bestCost = 1e20;
        ivec2 bestVector = ivec2(0);

        // Backward vector convention: for a current-frame block at p, v points
        // to the matching location p+v in the previous frame. Search the full
        // +/-8 range hierarchically instead of evaluating all 17x17 offsets.
        // This keeps the same maximum displacement while reducing the coarse
        // pass from 289 candidate SADs per block to at most 36.
        for (int level = 0; level < 4; ++level) {
            int step = level == 0 ? 8 : (level == 1 ? 4 : (level == 2 ? 2 : 1));
            ivec2 center = bestVector;

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    ivec2 candidate = clamp(center + ivec2(dx, dy) * step,
                                            ivec2(-8), ivec2(8));
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
                        bestCost = sad;
                        bestVector = candidate;
                    }
                }
            }
        }

        color = vec4(vec2(bestVector), bestCost / 16.0, 1.0);
    )";

    return runCompute(m_CoarseMotion,
                      "frame extrapolation coarse block motion",
                      nullptr,
                      body,
                      descriptors,
                      2);
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
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[2].desc.name = "coarse_motion";
    descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[2].binding.object = (void*)m_CoarseMotion;
    descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const char* body = R"(
        ivec2 block = ivec2(gl_GlobalInvocationID.xy);
        ivec2 size = textureSize(current_luma, 0);
        ivec2 base = block * 4;

        ivec2 coarseSize = textureSize(coarse_motion, 0);
        ivec2 coarseBlock = clamp(block / 2, ivec2(0), coarseSize - ivec2(1));
        ivec2 seed = ivec2(round(texelFetch(coarse_motion, coarseBlock, 0).rg * 2.0));

        float bestCost = 1e20;
        float secondCost = 1e20;
        ivec2 bestVector = seed;

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

        float meanCost = bestCost / 16.0;
        float separation = max(secondCost - bestCost, 0.0) / max(secondCost, 1e-5);
        float uniqueness = clamp(separation * 5.0, 0.0, 1.0);
        float quality = 1.0 - smoothstep(0.08, 0.22, meanCost);
        float confidence = uniqueness * quality;

        color = vec4(vec2(bestVector), confidence, meanCost);
    )";

    return runCompute(m_FineMotion,
                      "frame extrapolation fine block motion",
                      nullptr,
                      body,
                      descriptors,
                      3);
}

bool FrameExtrapolator::dispatchWarp(pl_tex source, pl_tex target, float alpha)
{
    pl_shader_desc descriptors[3] = {};
    descriptors[0].desc.name = "src_plane";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)source;
    descriptors[0].binding.sample_mode =
            (source->params.format->caps & PL_FMT_CAP_LINEAR) ?
                PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "motion_field";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)m_FineMotion;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
    descriptors[2].desc.name = "scene_metric";
    descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[2].binding.object = (void*)m_SceneMetric;
    descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    pl_shader_var variable = {};
    variable.var = pl_var_float("extrapolation_alpha");
    variable.data = &alpha;
    variable.dynamic = true;

    const char* body = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 planeSize = textureSize(src_plane, 0);
        vec2 uv = (vec2(p) + vec2(0.5)) / vec2(planeSize);

        vec4 motion = textureLod(motion_field, uv, 0.0);
        float sceneDiff = textureLod(scene_metric, vec2(0.5), 0.0).r;

        // Scene cuts and ambiguous blocks smoothly collapse to the current real
        // image instead of being warped. No CPU readback is required.
        float sceneConfidence = 1.0 - smoothstep(0.12, 0.18, sceneDiff);
        float confidence = clamp(motion.b * sceneConfidence, 0.0, 1.0);

        // motion.rg is a backward current->previous displacement measured in
        // quarter-resolution luma pixels. A future output location therefore
        // samples current at x + alpha*v. Normalized motion applies correctly
        // to luma and chroma planes regardless of chroma subsampling.
        vec2 fineSize = vec2(textureSize(motion_field, 0)) * 4.0;
        vec2 motionUv = motion.rg / max(fineSize, vec2(1.0));
        vec2 warpedUv = clamp(uv + extrapolation_alpha * motionUv,
                              vec2(0.0), vec2(1.0));

        vec4 original = textureLod(src_plane, uv, 0.0);
        vec4 warped = textureLod(src_plane, warpedUv, 0.0);
        color = mix(original, warped, confidence);
    )";

    return runCompute(target,
                      "frame extrapolation full-resolution warp",
                      nullptr,
                      body,
                      descriptors,
                      3,
                      &variable,
                      1);
}

bool FrameExtrapolator::submitRealFrame(const AVFrame* frame,
                                        const pl_frame& mappedFrame,
                                        uint64_t renderTimeUs)
{
    if (!ensureResources(mappedFrame)) {
        return false;
    }

    const int currentIndex = m_HasHistory ? 1 - m_HistoryIndex : 0;

    // Never build a queue of analysis work behind presentation. If any of the
    // reusable analysis resources are still busy, skip this frame's analysis
    // completely. This preserves the latest real AVFrame for future use but
    // deliberately invalidates motion until a fresh pair has completed.
    if (m_HasHistory &&
            (pl_tex_poll(m_Gpu, m_FineLuma[m_HistoryIndex], 0) ||
             pl_tex_poll(m_Gpu, m_CoarseLuma[m_HistoryIndex], 0) ||
             pl_tex_poll(m_Gpu, m_FineLuma[currentIndex], 0) ||
             pl_tex_poll(m_Gpu, m_CoarseLuma[currentIndex], 0) ||
             pl_tex_poll(m_Gpu, m_CoarseMotion, 0) ||
             pl_tex_poll(m_Gpu, m_FineMotion, 0) ||
             pl_tex_poll(m_Gpu, m_SceneMetric, 0))) {
        StreamHealthTelemetry::frameExtrapolationAnalysisBusySkip();
        m_HasMotion = false;
        m_MotionPairIntervalUs = 0;
        m_LastRealRenderTimeUs = renderTimeUs;
        m_SyntheticSinceLastReal = false;

        av_frame_unref(m_LatestRealFrame);
        if (av_frame_ref(m_LatestRealFrame, frame) < 0) {
            return false;
        }

        // Keep m_LastRealPts and m_HistoryIndex pointing at the last frame that
        // actually entered the GPU history. The next successful analysis may
        // therefore span multiple RTP intervals and will normalize that span.
        return true;
    }

    pl_dispatch_reset_frame(m_Dispatch);

    pl_tex luma = mappedFrame.planes[0].texture;

    if (!dispatchDownsample(luma, m_FineLuma[currentIndex], 4) ||
            !dispatchDownsample(m_FineLuma[currentIndex], m_CoarseLuma[currentIndex], 2)) {
        m_HasMotion = false;
        return false;
    }

    if (m_HasHistory) {
        if (!dispatchSceneMetric(m_CoarseLuma[currentIndex], m_CoarseLuma[m_HistoryIndex]) ||
                !dispatchCoarseMotion(m_CoarseLuma[currentIndex], m_CoarseLuma[m_HistoryIndex]) ||
                !dispatchFineMotion(m_FineLuma[currentIndex], m_FineLuma[m_HistoryIndex])) {
            m_HasMotion = false;
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

    m_HistoryIndex = currentIndex;
    m_HasHistory = true;
    m_LastRealRenderTimeUs = renderTimeUs;
    m_LastRealPts = frame->pts;
    m_SyntheticSinceLastReal = false;

    av_frame_unref(m_LatestRealFrame);
    if (av_frame_ref(m_LatestRealFrame, frame) < 0) {
        m_HasMotion = false;
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
            m_LatestRealFrame == nullptr || m_LatestRealFrame->width <= 0 ||
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
    if (pl_tex_poll(m_Gpu, m_FineMotion, 0) ||
            pl_tex_poll(m_Gpu, m_SceneMetric, 0)) {
        if (firstCheckForTarget) {
            StreamHealthTelemetry::frameExtrapolationRejectGpuBusy();
        }
        return false;
    }

    return true;
}

bool FrameExtrapolator::buildSyntheticFrame(const pl_frame& currentFrame,
                                             uint64_t targetTimeUs,
                                             uint64_t frameIntervalUs,
                                             pl_frame* syntheticFrame)
{
    if (!canExtrapolate(targetTimeUs, frameIntervalUs) ||
            currentFrame.num_planes != m_PlaneCount) {
        return false;
    }

    const double predictionAlpha =
            (double)(targetTimeUs - m_LastRealRenderTimeUs) /
            (double)frameIntervalUs;
    const double temporalScale =
            (double)frameIntervalUs / (double)m_MotionPairIntervalUs;
    const float alpha = (float)(
            std::clamp(predictionAlpha, 0.75, 1.25) *
            std::clamp(temporalScale, 0.4, 1.25));

    pl_dispatch_reset_frame(m_Dispatch);

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

    return true;
}

#endif
